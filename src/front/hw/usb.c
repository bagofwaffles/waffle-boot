/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery's USB device (recovery/usb.h): a polled driver for the BF70x USB
 * controller in peripheral mode, the processor moving every byte (no USB
 * DMA), interrupts never enabled at the SEC or the core. The register use
 * is the full bootloader's (drivers/bf70x/usb.c, usb_dcd.c, after the
 * ADSP-BF70x HRM chapter 28), trimmed to one CDC ACM function:
 * - the PHY enabled with USB_PLL_OSC left at reset (S-13a.080), high speed
 *   enabled with the pull-up (S-10a.030): high speed behind a high-speed
 *   host, else full speed, as USB_POWER.HSMODE reports;
 * - endpoint 0's control transfers through USB_EP0_CSR, setup, data and
 *   status, with the address taken after the status stage;
 * - endpoint 1 IN (notification, never sent), 9 IN and 10 OUT (bulk),
 *   single-buffered FIFO blocks after endpoint 0's first 64 bytes.
 * The controller's interrupt flags are read, which clears them, on every
 * poll; the endpoints' state is read from their CSRs.
 */
#include "front/recovery/usb.h"

#include "waffle/drivers/bf70x/usb.h"
#include "waffle/drivers/mmr.h"

#define DETACH_US   5000u	/* S-10a.030: a hub reports the disconnect */
#define RING        2048u	/* each direction; a power of two */
#define REPLY_MAX   128u	/* the largest descriptor or reply sent */

#define EPN_NOTIFY  (FR_USB_EP_NOTIFY & 0x0Fu)
#define EPN_IN      (FR_USB_EP_IN & 0x0Fu)
#define EPN_OUT     (FR_USB_EP_OUT & 0x0Fu)

enum ep0_state {
	EP0_IDLE,	/* waiting for a SETUP */
	EP0_IN,		/* sending reply[], a packet at a time */
	EP0_OUT,	/* receiving a SET_LINE_CODING's data */
	EP0_STATUS,	/* the last packet given: the status stage to end */
};

static const struct fr_clock *clock;
static const struct fr_dev *usb_dev;	/* the identity given (fr_usb_device()) */
static uint8_t state;		/* enum ep0_state */
static uint8_t reply[REPLY_MAX];
static uint16_t reply_len, reply_pos;
static bool reply_zlp;		/* short of wLength and a multiple of 64 */
static uint8_t out_buf[FR_USB_EP0];
static uint16_t out_len, out_got;
static uint8_t new_addr;
static bool addr_pending;
static uint8_t config;		/* 0 or 1 */
static bool halt_in, halt_out, halt_notify;
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };
static uint8_t line_state;	/* DTR bit 0, RTS bit 1 */

static uint8_t rx_ring[RING], tx_ring[RING];
static uint32_t rx_head, rx_tail, tx_head, tx_tail;	/* free-running */
static bool tx_zlp;		/* a full packet ended what was queued */
static uint8_t pkt[FR_USB_BULK_HS];

/* --- Registers ------------------------------------------------------- */

static void csr0(uint16_t v)
{
	wb_mmr_write16(USB0_EP0_CSR, v);
}

static bool high_speed(void)
{
	return (wb_mmr_read8(USB0_POWER) & USB_POWER_HSMODE) != 0;
}

static unsigned bulk_mps(void)
{
	return high_speed() ? FR_USB_BULK_HS : FR_USB_BULK;
}

/* Words through the 32-bit FIFO register, the tail through its byte form
 * (usb_dcd.c). */
static void fifo_read(unsigned epn, uint8_t *dst, unsigned n)
{
	const wb_mmr_addr a = USB0_FIFO(epn);

	for (; n >= 4u; n -= 4u, dst += 4) {
		const uint32_t w = wb_mmr_read32(a);

		dst[0] = (uint8_t)w;
		dst[1] = (uint8_t)(w >> 8);
		dst[2] = (uint8_t)(w >> 16);
		dst[3] = (uint8_t)(w >> 24);
	}
	for (; n; n--)
		*dst++ = wb_mmr_read8(a);
}

static void fifo_write(unsigned epn, const uint8_t *src, unsigned n)
{
	const wb_mmr_addr a = USB0_FIFO(epn);

	for (; n >= 4u; n -= 4u, src += 4)
		wb_mmr_write32(a, (uint32_t)src[0] | (uint32_t)src[1] << 8 |
			       (uint32_t)src[2] << 16 | (uint32_t)src[3] << 24);
	for (; n; n--)
		wb_mmr_write8(a, *src++);
}

/* An endpoint's FIFO block: size 8 << sz bytes at byte address addr. */
static void fifo_block(unsigned epn, bool rx, unsigned addr, uint8_t sz)
{
	wb_mmr_write8(USB0_INDEX, (uint8_t)epn);
	if (rx) {
		wb_mmr_write8(USB0_RXFIFOSZ, sz);
		wb_mmr_write16(USB0_RXFIFOADDR, (uint16_t)(addr / 8u));
	} else {
		wb_mmr_write8(USB0_TXFIFOSZ, sz);
		wb_mmr_write16(USB0_TXFIFOADDR, (uint16_t)(addr / 8u));
	}
}

/* Empty an endpoint's FIFO; twice, as usb_dcd.c does. */
static void flush(unsigned epn, bool in)
{
	unsigned i;

	for (i = 0; i < 2u; i++) {
		if (in) {
			if (wb_mmr_read16(USB0_EP_TXCSR(epn)) & USB_TXCSR_NEFIFO)
				wb_mmr_write16(USB0_EP_TXCSR(epn),
					       USB_TXCSR_FLUSHFIFO);
		} else if (wb_mmr_read16(USB0_EP_RXCSR(epn)) &
			   USB_RXCSR_RXPKTRDY) {
			wb_mmr_write16(USB0_EP_RXCSR(epn), USB_RXCSR_FLUSHFIFO);
		}
	}
}

/* Data toggle to DATA0, no halt, the FIFO empty (S-10a.032, S-10a.033). */
static void ep_reset(unsigned epn, bool in)
{
	flush(epn, in);
	if (in)
		wb_mmr_write16(USB0_EP_TXCSR(epn), USB_TXCSR_CLRDATATGL);
	else
		wb_mmr_write16(USB0_EP_RXCSR(epn), USB_RXCSR_CLRDATATGL);
}

static void ep_halt(unsigned epn, bool in)
{
	if (in)
		wb_mmr_write16(USB0_EP_TXCSR(epn), USB_TXCSR_SENDSTALL);
	else
		wb_mmr_write16(USB0_EP_RXCSR(epn), USB_RXCSR_SENDSTALL);
}

static void rings_reset(void)
{
	rx_head = rx_tail = tx_head = tx_tail = 0;
	tx_zlp = false;
}

/* SET_CONFIGURATION(1): FIFO blocks 64 (16 bytes), 128 and 640 (512 bytes
 * each, enough for either speed), the sizes of the current speed. */
static void endpoints_open(void)
{
	const uint16_t mps = (uint16_t)bulk_mps();

	fifo_block(EPN_NOTIFY, false, 64u, 1u);
	fifo_block(EPN_IN, false, 128u, 6u);
	fifo_block(EPN_OUT, true, 640u, 6u);
	wb_mmr_write16(USB0_EP_TXMAXP(EPN_NOTIFY), FR_USB_NOTIFY_MPS);
	wb_mmr_write16(USB0_EP_TXMAXP(EPN_IN), mps);
	wb_mmr_write16(USB0_EP_RXMAXP(EPN_OUT), mps);
	ep_reset(EPN_NOTIFY, true);
	ep_reset(EPN_IN, true);
	ep_reset(EPN_OUT, false);
	halt_in = halt_out = halt_notify = false;
	rings_reset();
	line_state = 0;
}

static void endpoints_close(void)
{
	flush(EPN_NOTIFY, true);
	flush(EPN_IN, true);
	flush(EPN_OUT, false);
	wb_mmr_write16(USB0_EP_TXMAXP(EPN_NOTIFY), 0);
	wb_mmr_write16(USB0_EP_TXMAXP(EPN_IN), 0);
	wb_mmr_write16(USB0_EP_RXMAXP(EPN_OUT), 0);
	wb_mmr_write16(USB0_EP_TXCSR(EPN_NOTIFY), 0);
	wb_mmr_write16(USB0_EP_TXCSR(EPN_IN), 0);
	wb_mmr_write16(USB0_EP_RXCSR(EPN_OUT), 0);
	halt_in = halt_out = halt_notify = false;
	rings_reset();
	line_state = 0;
}

/* --- Endpoint 0 ------------------------------------------------------ */

/* The next IN packet of reply[]; the last one carries DATAEND. */
static void ep0_send(void)
{
	const unsigned left = (unsigned)(reply_len - reply_pos);
	const unsigned n = left < FR_USB_EP0 ? left : FR_USB_EP0;
	const bool last = reply_pos + n == reply_len &&
			  (n < FR_USB_EP0 || !reply_zlp);

	fifo_write(0, reply + reply_pos, n);
	reply_pos = (uint16_t)(reply_pos + n);
	if (last) {
		csr0(USB_CSR0_TXPKTRDY | USB_CSR0_DATAEND);
		state = EP0_STATUS;
	} else {
		csr0(USB_CSR0_TXPKTRDY);
	}
}

static void ep0_accept(void)
{
	csr0(USB_CSR0_SPKTRDY | USB_CSR0_DATAEND);
	state = EP0_STATUS;
}

/* Answer with reply[0, len), cut to wLength (USB 2.0 9.4.3). */
static void ep0_reply(unsigned len, unsigned wlength)
{
	if (wlength == 0) {
		ep0_accept();	/* no data stage */
		return;
	}
	if (len > wlength)
		len = wlength;
	reply_len = (uint16_t)len;
	reply_pos = 0;
	reply_zlp = len < wlength && len % FR_USB_EP0 == 0u;
	csr0(USB_CSR0_SPKTRDY);
	state = EP0_IN;
	ep0_send();
}

static void ep0_stall(void)
{
	csr0(USB_CSR0_SPKTRDY | USB_CSR0_SENDSTALL);
	state = EP0_IDLE;
}

/* The status stage is over: a new address takes effect (USB 2.0 9.4.6). */
static void ep0_done(void)
{
	if (addr_pending) {
		wb_mmr_write8(USB0_FADDR, new_addr);
		addr_pending = false;
	}
	state = EP0_IDLE;
}

/* The halt flag of an endpoint of the configuration, NULL for none. */
static bool *halt_of(unsigned addr)
{
	if (!config)
		return NULL;
	switch (addr) {
	case FR_USB_EP_NOTIFY:
		return &halt_notify;
	case FR_USB_EP_IN:
		return &halt_in;
	case FR_USB_EP_OUT:
		return &halt_out;
	default:
		return NULL;
	}
}

static bool get_descriptor(unsigned type, unsigned index, unsigned *len)
{
	size_t n;

	switch (type) {
	case 0x01:
		n = fr_usb_desc_device(usb_dev, reply, sizeof reply);
		break;
	case 0x02:
		if (index != 0)
			return false;
		n = fr_usb_desc_config(high_speed(), reply, sizeof reply);
		break;
	case 0x03:
		n = fr_usb_desc_string(usb_dev, index, NULL, reply, sizeof reply);
		break;
	case 0x06:
		n = fr_usb_desc_qualifier(reply, sizeof reply);
		break;
	default:	/* OTHER_SPEED_CONFIGURATION, BOS and the rest */
		return false;
	}
	if (n == 0 || n > sizeof reply)
		return false;
	*len = (unsigned)n;
	return true;
}

static void standard(const uint8_t s[8], unsigned value, unsigned index,
		     unsigned wlength)
{
	const unsigned rt = s[0];
	unsigned len;
	bool *halt;

	switch (s[1]) {
	case 0x00:	/* GET_STATUS */
		reply[0] = reply[1] = 0;
		if (rt == 0x80) {
			reply[0] = 0x01;	/* self-powered, as 0xC0 says */
		} else if (rt == 0x81) {
			if (!config || index > 1u)
				break;
		} else if (rt == 0x82) {
			halt = halt_of(index);
			if (halt)
				reply[0] = *halt ? 1u : 0u;
			else if ((index & ~0x80u) != 0)
				break;
		} else {
			break;
		}
		ep0_reply(2, wlength);
		return;
	case 0x01:	/* CLEAR_FEATURE */
	case 0x03:	/* SET_FEATURE */
		if (rt != 0x02 || value != 0 || wlength != 0)
			break;		/* only ENDPOINT_HALT */
		if ((index & ~0x80u) == 0) {
			ep0_accept();	/* endpoint 0 is never halted */
			return;
		}
		halt = halt_of(index);
		if (!halt)
			break;
		*halt = s[1] == 0x03;
		if (*halt) {
			ep_halt(index & 0x0Fu, (index & 0x80u) != 0);
		} else {
			ep_reset(index & 0x0Fu, (index & 0x80u) != 0);
			if (index == FR_USB_EP_IN)
				tx_zlp = false;
		}
		ep0_accept();
		return;
	case 0x05:	/* SET_ADDRESS */
		if (rt != 0x00 || value > 127u || wlength != 0)
			break;
		new_addr = (uint8_t)value;
		addr_pending = true;
		ep0_accept();
		return;
	case 0x06:	/* GET_DESCRIPTOR */
		if (rt != 0x80 || !get_descriptor(value >> 8, value & 0xFFu,
						  &len))
			break;
		ep0_reply(len, wlength);
		return;
	case 0x08:	/* GET_CONFIGURATION */
		if (rt != 0x80)
			break;
		reply[0] = config;
		ep0_reply(1, wlength);
		return;
	case 0x09:	/* SET_CONFIGURATION */
		if (rt != 0x00 || value > 1u || wlength != 0)
			break;
		/* The same value again resets the endpoints too (S-10a.032). */
		if (value)
			endpoints_open();
		else if (config)
			endpoints_close();
		config = (uint8_t)value;
		ep0_accept();
		return;
	case 0x0A:	/* GET_INTERFACE */
		if (rt != 0x81 || !config || index > 1u)
			break;
		reply[0] = 0;
		ep0_reply(1, wlength);
		return;
	case 0x0B:	/* SET_INTERFACE: alternate 0 resets its endpoints */
		if (rt != 0x01 || !config || index > 1u || value != 0)
			break;
		if (index == 0) {
			ep_reset(EPN_NOTIFY, true);
			halt_notify = false;
		} else {
			ep_reset(EPN_IN, true);
			ep_reset(EPN_OUT, false);
			halt_in = halt_out = false;
			tx_zlp = false;
		}
		ep0_accept();
		return;
	default:
		break;
	}
	ep0_stall();
}

/* CDC ACM requests to the communications interface (S-10a.060). */
static void cdc(const uint8_t s[8], unsigned value, unsigned index,
		unsigned wlength)
{
	if (index != 0 || !config) {
		ep0_stall();
		return;
	}
	switch (s[1]) {
	case 0x20:	/* SET_LINE_CODING: the data stage follows */
		if (s[0] != 0x21 || wlength == 0 || wlength > FR_USB_EP0)
			break;
		out_len = (uint16_t)wlength;
		out_got = 0;
		csr0(USB_CSR0_SPKTRDY);
		state = EP0_OUT;
		return;
	case 0x21:	/* GET_LINE_CODING */
		if (s[0] != 0xA1)
			break;
		fr_copy(reply, line_coding, sizeof line_coding);
		ep0_reply(sizeof line_coding, wlength);
		return;
	case 0x22:	/* SET_CONTROL_LINE_STATE */
		if (s[0] != 0x21 || wlength != 0)
			break;
		line_state = (uint8_t)(value & 3u);
		ep0_accept();
		return;
	case 0x23:	/* SEND_BREAK: no line to break */
		if (s[0] != 0x21 || wlength != 0)
			break;
		ep0_accept();
		return;
	default:
		break;
	}
	ep0_stall();
}

static void ep0_setup(void)
{
	uint8_t s[8];
	unsigned value, index, wlength;

	if (wb_mmr_read16(USB0_EP0_CNT) != 8u) {
		/* Not a SETUP: throw it away. */
		csr0(USB_CSR0_SPKTRDY | USB_CSR0_SENDSTALL);
		state = EP0_IDLE;
		return;
	}
	fifo_read(0, s, 8);
	value = fr_le16(s + 2);
	index = fr_le16(s + 4);
	wlength = fr_le16(s + 6);
	switch (s[0] & 0x60u) {
	case 0x00:
		standard(s, value, index, wlength);
		break;
	case 0x20:
		if ((s[0] & 0x1Fu) == 0x01) {
			cdc(s, value, index, wlength);
			break;
		}
		ep0_stall();
		break;
	default:
		ep0_stall();
		break;
	}
}

/* SET_LINE_CODING's data: the first bytes replace the line coding's. */
static void ep0_data_out(void)
{
	const unsigned n = wb_mmr_read16(USB0_EP0_CNT);
	unsigned i;

	if (n > (unsigned)(out_len - out_got)) {
		ep0_stall();
		return;
	}
	fifo_read(0, out_buf + out_got, n);
	out_got = (uint16_t)(out_got + n);
	if (out_got < out_len && n == FR_USB_EP0) {
		csr0(USB_CSR0_SPKTRDY);
		return;
	}
	for (i = 0; i < out_got && i < sizeof line_coding; i++)
		line_coding[i] = out_buf[i];
	ep0_accept();
}

/*
 * Endpoint 0, on every poll. event: its interrupt flag was set, which
 * also marks a sent packet or the end of a status stage (CSR0 shows
 * neither then).
 */
static void ep0_poll(bool event)
{
	const uint16_t csr = wb_mmr_read16(USB0_EP0_CSR);

	if (csr & USB_CSR0_SENTSTALL) {
		csr0(0);
		state = EP0_IDLE;
		return;
	}
	if (csr & USB_CSR0_SETUPEND) {
		/* The host ended the transfer early. */
		csr0(USB_CSR0_SSETUPEND);
		if (state == EP0_STATUS)
			ep0_done();
		state = EP0_IDLE;
	}
	if (csr & USB_CSR0_RXPKTRDY) {
		if (state == EP0_OUT) {
			ep0_data_out();
			return;
		}
		/* A new SETUP; a status stage before it has ended. */
		if (state == EP0_STATUS)
			ep0_done();
		ep0_setup();
		return;
	}
	if (!event || (csr & USB_CSR0_TXPKTRDY))
		return;
	if (state == EP0_IN)
		ep0_send();
	else if (state == EP0_STATUS)
		ep0_done();
}

/* --- Bulk ------------------------------------------------------------ */

/* An OUT packet into the receive ring, when it fits whole; until then it
 * waits in the FIFO and the controller NAKs the host. */
static void bulk_out_poll(void)
{
	const uint16_t csr = wb_mmr_read16(USB0_EP_RXCSR(EPN_OUT));
	unsigned n, i;

	if (csr & USB_RXCSR_SENTSTALL) {
		wb_mmr_write16(USB0_EP_RXCSR(EPN_OUT), halt_out ?
			       USB_RXCSR_SENDSTALL : 0u);
		return;
	}
	if (halt_out || !(csr & USB_RXCSR_RXPKTRDY))
		return;
	n = wb_mmr_read16(USB0_EP_RXCNT(EPN_OUT));
	if (n > sizeof pkt) {
		wb_mmr_write16(USB0_EP_RXCSR(EPN_OUT), USB_RXCSR_FLUSHFIFO);
		return;
	}
	if (n > RING - (rx_head - rx_tail))
		return;
	fifo_read(EPN_OUT, pkt, n);
	/* RXPKTRDY written 0: the FIFO is free for the next packet. */
	wb_mmr_write16(USB0_EP_RXCSR(EPN_OUT), 0);
	for (i = 0; i < n; i++)
		rx_ring[rx_head++ & (RING - 1u)] = pkt[i];
}

/* The next IN packet from the transmit ring once the FIFO is empty; a
 * zero-length packet after a full packet that emptied the ring. */
static void bulk_in_poll(void)
{
	const uint16_t csr = wb_mmr_read16(USB0_EP_TXCSR(EPN_IN));
	const unsigned mps = bulk_mps();
	unsigned n, i;

	if (csr & USB_TXCSR_SENTSTALL) {
		wb_mmr_write16(USB0_EP_TXCSR(EPN_IN), halt_in ?
			       USB_TXCSR_SENDSTALL : 0u);
		return;
	}
	if (halt_in || (csr & (USB_TXCSR_TXPKTRDY | USB_TXCSR_NEFIFO)))
		return;
	n = tx_head - tx_tail;
	if (n == 0 && !tx_zlp)
		return;
	if (n > mps)
		n = mps;
	for (i = 0; i < n; i++)
		pkt[i] = tx_ring[tx_tail++ & (RING - 1u)];
	fifo_write(EPN_IN, pkt, n);
	wb_mmr_write16(USB0_EP_TXCSR(EPN_IN), USB_TXCSR_TXPKTRDY);
	tx_zlp = n == mps && tx_head == tx_tail;
}

/* --- Bus ------------------------------------------------------------- */

/* A bus reset: the controller has cleared FADDR and the endpoints. */
static void bus_reset(void)
{
	state = EP0_IDLE;
	addr_pending = false;
	config = 0;
	halt_in = halt_out = halt_notify = false;
	rings_reset();
	line_state = 0;
	wb_mmr_write16(USB0_INTRTXE, 1u);
	wb_mmr_write16(USB0_INTRRXE, 0);
}

void fr_usb_device(const struct fr_dev *dev)
{
	usb_dev = dev;
}

void fr_usb_start(const struct fr_clock *c)
{
	clock = c;
	/* Off the bus first, long enough for a hub to see it (bounded). */
	wb_mmr_write8(USB0_POWER, (uint8_t)(wb_mmr_read8(USB0_POWER) &
		      ~USB_POWER_SOFTCONN));
	wb_mmr_sync();
	wb_mmr_write8(USB0_PHY_CTL, USB_PHY_CTL_EN);	/* S-13a.080 */
	wb_mmr_sync();
	for (unsigned t = 0; t < DETACH_US; t += 100u) {
		clock->delay_us(clock->ctx, 100u);
		fr_serve();		/* the PSoC's link meanwhile */
	}
	/* The flags the polls read; nothing reaches the core (no SEC). */
	wb_mmr_write8(USB0_IEN, (uint8_t)(USB_IRQ_SUSPEND | USB_IRQ_RESUME |
		      USB_IRQ_RSTBABBLE | USB_IRQ_DISCON));
	bus_reset();
	(void)wb_mmr_read8(USB0_IRQ);
	(void)wb_mmr_read16(USB0_INTRTX);
	(void)wb_mmr_read16(USB0_INTRRX);
	wb_mmr_write8(USB0_POWER, (uint8_t)(wb_mmr_read8(USB0_POWER) |
		      USB_POWER_HSEN | USB_POWER_SOFTCONN));
	wb_mmr_sync();
}

/* No loop here waits on the controller: each call does what is ready. */
void fr_usb_poll(void)
{
	const uint8_t irq = wb_mmr_read8(USB0_IRQ);
	const uint16_t tx = wb_mmr_read16(USB0_INTRTX);

	(void)wb_mmr_read16(USB0_INTRRX);
	if (irq & (USB_IRQ_RSTBABBLE | USB_IRQ_DISCON))
		bus_reset();
	if (irq & USB_IRQ_RSTBABBLE)
		return;		/* a SETUP comes only after the reset */
	ep0_poll((tx & 1u) != 0);
	if (config) {
		bulk_out_poll();
		bulk_in_poll();
	}
}

bool fr_usb_configured(void)
{
	return config != 0;
}

uint32_t fr_usb_ma(void)
{
	return config ? FR_USB_MAX_MA : 0u;
}

/* --- The serial port ------------------------------------------------- */

static size_t serial_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t n = 0;

	(void)ctx;
	while (n < max && rx_tail != rx_head)
		buf[n++] = rx_ring[rx_tail++ & (RING - 1u)];
	if (config)
		bulk_out_poll();	/* a packet may wait for the room */
	return n;
}

static size_t serial_write(void *ctx, const uint8_t *buf, size_t n)
{
	size_t i;

	(void)ctx;
	if (!config)
		return 0;
	for (i = 0; i < n && tx_head - tx_tail < RING; i++)
		tx_ring[tx_head++ & (RING - 1u)] = buf[i];
	bulk_in_poll();
	return i;
}

static bool serial_idle(void *ctx)
{
	(void)ctx;
	if (!config)
		return true;
	return tx_head == tx_tail && !tx_zlp &&
	       !(wb_mmr_read16(USB0_EP_TXCSR(EPN_IN)) &
		 (USB_TXCSR_TXPKTRDY | USB_TXCSR_NEFIFO));
}

const struct fr_bytes fr_usb_serial = {
	.read = serial_read,
	.write = serial_write,
	.idle = serial_idle,
	.ctx = NULL,
};
