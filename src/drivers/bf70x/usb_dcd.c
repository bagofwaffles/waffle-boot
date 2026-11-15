/* SPDX-License-Identifier: GPL-2.0-or-later AND MIT */
/*
 * TinyUSB device controller driver for the ADSP-BF70x USB controller
 * (waffle/drivers/bf70x_usb.h, HRM chapter 28).
 *
 * The endpoint 0 control-transfer state machine and the endpoint transfer
 * logic follow TinyUSB's portable/mentor/musb/dcd_musb.c (0.21.0), whose
 * notice is:
 *
 *   The MIT License (MIT)
 *
 *   Copyright (c) 2021 Koji KITAYAMA
 *   Copyright (c) 2024, Brent Kowal (Analog Devices, Inc)
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * What differs from upstream, and why:
 * - Every register access goes through the MMR accessors (anomaly 19000058:
 *   upstream reads through plain volatile pointers), at the widths of HRM
 *   Table A-77, using the directly mapped endpoint registers.
 * - FIFO sizing (wb_usb_fifo_alloc) uses USB_TXFIFOSZ/USB_RXFIFOSZ.DPB for
 *   double buffering. Upstream's dynamic-FIFO path also read-modify-writes
 *   offsets 0x340/0x342, which the BF70x HRM does not list; nothing here
 *   touches them.
 * - The pull-up is not turned on by dcd_init(): the board attaches the
 *   device when held mode starts a descriptor set (S-10a.030).
 * - Opening an endpoint always resets its data toggle (S-10a.032).
 * - A transmit interrupt with no transfer armed completes nothing.
 * - Each SETUP is first offered to the chapter 9 filter
 *   (wb_usb_set_setup_filter), which may answer it here, or have a
 *   SET_CONFIGURATION of the current value taken as SET_CONFIGURATION(0)
 *   and then itself (S-10a.032).
 * - The bus reset, resume and suspend interrupts are enabled explicitly
 *   (USB_IEN resets to 0 on BF70x), and the disconnect interrupt (the end of
 *   the session, in peripheral mode) reports an unplug.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "waffle/drivers/bf70x_sec.h"
#include "waffle/drivers/bf70x_usb.h"
#include "waffle/drivers/mmr.h"

#include "tusb_option.h"
#include "device/dcd.h"

enum {
	PIPE0_IDLE,
	PIPE0_DATA_IN,
	PIPE0_DATA_OUT,
	PIPE0_STATUS_IN,
	PIPE0_STATUS_OUT,
	PIPE0_STATUS_OUT_PENDING_XFER,	/* status OUT armed, IRQ to come */
	PIPE0_STATUS_OUT_PENDING_IRQ,	/* IRQ seen, status OUT to be armed */
	PIPE0_OWN_DATA_IN,		/* the filter's reply being sent */
	PIPE0_OWN_STATUS,		/* the filter's status stage */
};

struct pipe0 {
	uint8_t *buf;
	uint16_t xact_len;
	uint16_t remain_wlength;
	uint8_t state;
	uint8_t pending_addr;
	bool rxrdy_consumed;	/* RXPKTRDY left set for a drained packet */
	bool deferred_valid;
	uint8_t deferred[8];
	bool reselect_valid;	/* the filter's RESELECT: the request held */
	uint8_t reselect[8];
};

struct pipe {
	uint8_t *buf;
	uint16_t length;
	uint16_t remaining;
	uint16_t mps;
	bool armed;
	bool iso;
};

static struct pipe0 pipe0;
static struct pipe pipes[2][WB_USB_EP_MAX];	/* [is_in][epn] */
static uint16_t txe_shadow = 1u, rxe_shadow;
static uint8_t ien_shadow;
static uint16_t opened[2];	/* [is_in] bit n: endpoint n open */
static unsigned fifo_next = WB_USB_EP0_SIZE;
static bool irq_on;

static wb_usb_setup_filter setup_filter;
static void *setup_filter_ctx;

void wb_usb_set_setup_filter(wb_usb_setup_filter fn, void *ctx)
{
	setup_filter = fn;
	setup_filter_ctx = ctx;
}

void wb_usb_dcd_reset_state(void)
{
	unsigned i;

	pipe0 = (struct pipe0){ 0 };
	for (i = 0; i < WB_USB_EP_MAX; i++)
		pipes[0][i] = pipes[1][i] = (struct pipe){ 0 };
	txe_shadow = 1u;
	rxe_shadow = 0;
	ien_shadow = 0;
	opened[0] = opened[1] = 0;
	fifo_next = WB_USB_EP0_SIZE;
	irq_on = false;
}

/* --- FIFO RAM ---------------------------------------------------------- */

void wb_usb_fifo_reset(void)
{
	fifo_next = WB_USB_EP0_SIZE;
}

unsigned wb_usb_fifo_used(void)
{
	return fifo_next;
}

bool wb_usb_fifo_alloc(unsigned epn, bool rx, unsigned mps, bool dbl)
{
	unsigned sz = 0, bytes;
	uint8_t reg;

	if (epn == 0 || epn >= WB_USB_EP_MAX || mps == 0 || mps > 8192u)
		return false;
	while ((8u << sz) < mps)
		sz++;
	bytes = (8u << sz) * (dbl ? 2u : 1u);
	if (fifo_next + bytes > WB_USB_FIFO_BYTES)
		return false;
	reg = (uint8_t)(sz | (dbl ? USB_FIFOSZ_DPB : 0u));
	wb_mmr_write8(USB0_INDEX, (uint8_t)epn);
	if (rx) {
		wb_mmr_write8(USB0_RXFIFOSZ, reg);
		wb_mmr_write16(USB0_RXFIFOADDR, (uint16_t)(fifo_next / 8u));
	} else {
		wb_mmr_write8(USB0_TXFIFOSZ, reg);
		wb_mmr_write16(USB0_TXFIFOADDR, (uint16_t)(fifo_next / 8u));
	}
	fifo_next += bytes;
	return true;
}

/* Words through the 32-bit FIFO register, the tail through its byte form. */
static void fifo_read(unsigned epn, uint8_t *dst, unsigned n)
{
	const wb_mmr_addr a = USB0_FIFO(epn);

	for (; n >= 4u; n -= 4u, dst += 4) {
		uint32_t w = wb_mmr_read32(a);

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

static void set_txe(uint16_t v)
{
	txe_shadow = v;
	wb_mmr_write16(USB0_INTRTXE, v);
}

static void set_rxe(uint16_t v)
{
	rxe_shadow = v;
	wb_mmr_write16(USB0_INTRRXE, v);
}

static uint16_t bit16(unsigned n)
{
	return (uint16_t)(1u << n);
}

/* --- Interrupts -------------------------------------------------------- */

void dcd_int_enable(uint8_t rhport)
{
	(void)rhport;
	irq_on = true;
	(void)wb_sec_enable(WB_SID_USB0_STAT);
	(void)wb_sec_enable(WB_SID_USB0_DATA);
}

void dcd_int_disable(uint8_t rhport)
{
	(void)rhport;
	(void)wb_sec_disable(WB_SID_USB0_STAT);
	(void)wb_sec_disable(WB_SID_USB0_DATA);
	irq_on = false;
}

/* Mask the USB sources around a change the handler also makes. */
static bool irq_save(void)
{
	bool was = irq_on;

	if (was)
		dcd_int_disable(0);
	return was;
}

static void irq_restore(bool was)
{
	if (was)
		dcd_int_enable(0);
}

void wb_usb_irq_stat(void *ctx, uint32_t sid)
{
	(void)ctx;
	(void)sid;
	dcd_int_handler(0);
}

/* No USB DMA channel is used; acknowledge anything pending. */
void wb_usb_irq_data(void *ctx, uint32_t sid)
{
	(void)ctx;
	(void)sid;
	(void)wb_mmr_read8(USB0_DMA_IRQ);
}

/* --- Endpoint 0 -------------------------------------------------------- */

static void csr0(uint16_t v)
{
	wb_mmr_write16(USB0_EP0_CSR, v);
}

static bool pipe0_data_stage_done(uint16_t xfer_len)
{
	return pipe0.remain_wlength == 0 || xfer_len < WB_USB_EP0_SIZE;
}

static bool pipe0_read_setup(uint8_t setup[8])
{
	if (wb_mmr_read16(USB0_EP0_CNT) != 8u)
		return false;
	fifo_read(0, setup, 8);
	return true;
}

static uint16_t setup_wlength(const uint8_t setup[8])
{
	return (uint16_t)(setup[6] | setup[7] << 8);
}

static void pipe0_start_setup(uint8_t rhport, const uint8_t setup[8],
			      bool is_isr)
{
	uint16_t wlength = setup_wlength(setup);

	pipe0.remain_wlength = wlength;
	if (wlength == 0) {
		/* RXPKTRDY stays set; the status IN arm acks it with DATAEND. */
		pipe0.state = PIPE0_STATUS_IN;
		pipe0.rxrdy_consumed = true;
	} else if (setup[0] & 0x80u) {
		pipe0.state = PIPE0_DATA_IN;
		if (!pipe0.rxrdy_consumed)
			csr0(USB_CSR0_SPKTRDY);
	} else {
		/* Acked when the data OUT stage is armed (NAK flow control). */
		pipe0.state = PIPE0_DATA_OUT;
		pipe0.rxrdy_consumed = true;
	}
	dcd_event_setup_received(rhport, setup, is_isr);
}

/*
 * Offer a SETUP to the filter. Returns true when the filter answered it
 * here; RXPKTRDY has then been serviced.
 */
static bool pipe0_filter(uint8_t rhport, const uint8_t setup[8],
			 bool is_isr)
{
	uint8_t reply[WB_USB_EP0_SIZE];
	uint16_t len = 0, wlength = setup_wlength(setup);
	enum wb_usb_setup_action a;

	if (!setup_filter)
		return false;
	a = setup_filter(setup_filter_ctx, setup, reply, &len);
	switch (a) {
	case WB_USB_SETUP_FORWARD:
		return false;
	case WB_USB_SETUP_RESELECT: {
		uint8_t zero[8];

		if (wlength != 0 || setup[0] != 0x00u || setup[1] != 9u)
			break;
		/* TinyUSB sees SET_CONFIGURATION(0); its status stage hands
		 * it the request itself (edpt0_xfer). */
		memcpy(pipe0.reselect, setup, sizeof(pipe0.reselect));
		pipe0.reselect_valid = true;
		memcpy(zero, setup, sizeof(zero));
		zero[2] = 0;
		zero[3] = 0;
		pipe0_start_setup(rhport, zero, is_isr);
		return true;
	}
	case WB_USB_SETUP_ACCEPT:
		if (wlength != 0)
			break;
		pipe0.state = PIPE0_OWN_STATUS;
		pipe0.rxrdy_consumed = false;
		csr0(USB_CSR0_SPKTRDY | USB_CSR0_DATAEND);
		return true;
	case WB_USB_SETUP_REPLY:
		/* One packet, shorter than a full one: no ZLP is ever owed. */
		if (!(setup[0] & 0x80u) || wlength == 0 ||
		    len >= WB_USB_EP0_SIZE)
			break;
		if (len > wlength)
			len = wlength;
		pipe0.state = PIPE0_OWN_DATA_IN;
		pipe0.rxrdy_consumed = false;
		csr0(USB_CSR0_SPKTRDY);
		fifo_write(0, reply, len);
		csr0(USB_CSR0_TXPKTRDY | USB_CSR0_DATAEND);
		return true;
	case WB_USB_SETUP_STALL:
	default:
		break;
	}
	pipe0.state = PIPE0_IDLE;
	pipe0.rxrdy_consumed = false;
	csr0(USB_CSR0_SPKTRDY | USB_CSR0_SENDSTALL);
	return true;
}

/* Every SETUP, a coalesced one too, goes past the filter first. */
static void pipe0_setup(uint8_t rhport, const uint8_t setup[8], bool is_isr)
{
	if (!pipe0_filter(rhport, setup, is_isr))
		pipe0_start_setup(rhport, setup, is_isr);
}

static void pipe0_try_deferred_setup(uint8_t rhport, bool is_isr)
{
	if (!pipe0.deferred_valid)
		return;
	pipe0.deferred_valid = false;
	pipe0_setup(rhport, pipe0.deferred, is_isr);
}

static bool edpt0_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer,
		       uint16_t total_bytes, bool is_isr)
{
	const bool dir_in = (ep_addr & 0x80u) != 0;

	if (total_bytes > WB_USB_EP0_SIZE)
		return false;
	switch (pipe0.state) {
	case PIPE0_DATA_IN:
		if (!dir_in)
			return false;
		pipe0.xact_len = total_bytes;
		if (pipe0.rxrdy_consumed) {
			csr0(USB_CSR0_SPKTRDY);
			pipe0.rxrdy_consumed = false;
		}
		fifo_write(0, buffer, total_bytes);
		pipe0.remain_wlength = (uint16_t)(pipe0.remain_wlength -
						  total_bytes);
		csr0(pipe0_data_stage_done(total_bytes) ?
		     USB_CSR0_TXPKTRDY | USB_CSR0_DATAEND : USB_CSR0_TXPKTRDY);
		break;
	case PIPE0_DATA_OUT:
		if (dir_in)
			return false;
		pipe0.xact_len = total_bytes;
		pipe0.buf = buffer;
		csr0(USB_CSR0_SPKTRDY);
		pipe0.rxrdy_consumed = false;
		break;
	case PIPE0_STATUS_IN:
		if (!dir_in || total_bytes)
			return false;
		if (pipe0.reselect_valid) {
			/* SET_CONFIGURATION(0) answered: now the request. */
			pipe0.reselect_valid = false;
			dcd_event_setup_received(rhport, pipe0.reselect,
						 is_isr);
			break;
		}
		csr0(USB_CSR0_SPKTRDY | USB_CSR0_DATAEND);
		pipe0.rxrdy_consumed = false;
		break;
	case PIPE0_STATUS_OUT:
		if (dir_in || total_bytes)
			return false;
		pipe0.state = PIPE0_STATUS_OUT_PENDING_XFER;
		break;
	case PIPE0_STATUS_OUT_PENDING_IRQ:
		pipe0.state = PIPE0_IDLE;
		dcd_event_xfer_complete(rhport, ep_addr, 0, XFER_RESULT_SUCCESS,
					is_isr);
		pipe0_try_deferred_setup(rhport, is_isr);
		break;
	default:
		break;
	}
	return true;
}

/* A tail event of the transfer in progress: CSR0 read 0, or a coalesced SETUP. */
static void pipe0_tail_isr(uint8_t rhport)
{
	switch (pipe0.state) {
	case PIPE0_DATA_IN:
		if (pipe0_data_stage_done(pipe0.xact_len))
			pipe0.state = (uint8_t)(pipe0.deferred_valid ?
				PIPE0_STATUS_OUT_PENDING_IRQ :
				PIPE0_STATUS_OUT);
		dcd_event_xfer_complete(rhport, 0x80, pipe0.xact_len,
					XFER_RESULT_SUCCESS, true);
		break;
	case PIPE0_STATUS_OUT:
		pipe0.state = PIPE0_STATUS_OUT_PENDING_IRQ;
		break;
	case PIPE0_STATUS_OUT_PENDING_XFER:
		pipe0.state = PIPE0_IDLE;
		dcd_event_xfer_complete(rhport, 0x00, 0, XFER_RESULT_SUCCESS,
					true);
		pipe0_try_deferred_setup(rhport, true);
		break;
	case PIPE0_STATUS_IN:
		if (pipe0.pending_addr) {
			wb_mmr_write8(USB0_FADDR, pipe0.pending_addr);
			pipe0.pending_addr = 0;
		}
		pipe0.state = PIPE0_IDLE;
		dcd_event_xfer_complete(rhport, 0x80, 0, XFER_RESULT_SUCCESS,
					true);
		pipe0_try_deferred_setup(rhport, true);
		break;
	case PIPE0_OWN_DATA_IN:
		pipe0.state = PIPE0_OWN_STATUS;
		break;
	case PIPE0_OWN_STATUS:
		pipe0.state = PIPE0_IDLE;
		break;
	case PIPE0_STATUS_OUT_PENDING_IRQ:
	case PIPE0_IDLE:
	case PIPE0_DATA_OUT:
	default:
		break;
	}
}

static void setup_isr(uint8_t rhport)
{
	uint8_t setup[8];

	if (pipe0_read_setup(setup))
		pipe0_setup(rhport, setup, true);
}

static void process_ep0_isr(uint8_t rhport)
{
	const uint16_t csr = wb_mmr_read16(USB0_EP0_CSR);

	if (csr & USB_CSR0_SENTSTALL) {
		csr0(0);
		pipe0.state = PIPE0_IDLE;
		pipe0.deferred_valid = false;
		pipe0.reselect_valid = false;
		pipe0.rxrdy_consumed = false;
		return;
	}
	if (csr & USB_CSR0_SETUPEND) {
		/* The host ended the transfer (a new SETUP, or an early status). */
		csr0(USB_CSR0_SSETUPEND);
		pipe0.state = PIPE0_IDLE;
		pipe0.deferred_valid = false;
		pipe0.reselect_valid = false;
		pipe0.rxrdy_consumed = false;
		if (!(csr & USB_CSR0_RXPKTRDY))
			return;
	}
	if (csr & USB_CSR0_RXPKTRDY) {
		if (pipe0.rxrdy_consumed)
			return;	/* stale: this packet was drained already */
		switch (pipe0.state) {
		case PIPE0_IDLE:
			setup_isr(rhport);
			break;
		case PIPE0_DATA_OUT: {
			const uint16_t n = wb_mmr_read16(USB0_EP0_CNT);

			if (n) {
				if (!pipe0.buf || n > pipe0.xact_len) {
					csr0(USB_CSR0_SPKTRDY |
					     USB_CSR0_SENDSTALL);
					pipe0.state = PIPE0_IDLE;
					return;
				}
				fifo_read(0, pipe0.buf, n);
				pipe0.remain_wlength = (uint16_t)(
					pipe0.remain_wlength -
					(n < pipe0.remain_wlength ?
					 n : pipe0.remain_wlength));
			}
			pipe0.rxrdy_consumed = true;
			if (pipe0_data_stage_done(n))
				pipe0.state = PIPE0_STATUS_IN;
			dcd_event_xfer_complete(rhport, 0x00, n,
						XFER_RESULT_SUCCESS, true);
			break;
		}
		case PIPE0_OWN_DATA_IN:
		case PIPE0_OWN_STATUS:
			/* Our own transfer is over; this is a new SETUP. */
			pipe0.state = PIPE0_IDLE;
			setup_isr(rhport);
			break;
		case PIPE0_DATA_IN:
		case PIPE0_STATUS_OUT:
		case PIPE0_STATUS_OUT_PENDING_XFER:
		case PIPE0_STATUS_OUT_PENDING_IRQ:
		case PIPE0_STATUS_IN:
			/* A SETUP coalesced with the old transfer's tail. */
			if (!pipe0_read_setup(pipe0.deferred))
				return;
			pipe0.deferred_valid = true;
			pipe0.rxrdy_consumed = true;
			pipe0_tail_isr(rhport);
			break;
		default:
			break;
		}
		return;
	}
	if (csr & USB_CSR0_DATAEND)
		return;
	pipe0_tail_isr(rhport);
}

/* --- Endpoints 1-11 ---------------------------------------------------- */

static struct pipe *pipe_get(unsigned epn, bool in)
{
	return &pipes[in ? 1 : 0][epn];
}

static void pipe_write(struct pipe *p, unsigned epn)
{
	const uint16_t n = (uint16_t)(p->remaining < p->mps ?
		p->remaining : p->mps);

	if (n) {
		fifo_write(epn, p->buf, n);
		p->buf += n;
		p->remaining = (uint16_t)(p->remaining - n);
	}
	wb_mmr_write16(USB0_EP_TXCSR(epn), (uint16_t)(USB_TXCSR_TXPKTRDY |
		       (p->iso ? USB_TXCSR_ISO : 0u)));
}

static void process_epin_isr(uint8_t rhport, unsigned epn)
{
	const uint16_t csr = wb_mmr_read16(USB0_EP_TXCSR(epn));
	struct pipe *p = pipe_get(epn, true);
	uint16_t len;

	if (csr & USB_TXCSR_SENTSTALL) {
		/* Clear SENTSTALL and URUNERR (W0C); keep SENDSTALL. */
		wb_mmr_write16(USB0_EP_TXCSR(epn), (uint16_t)(csr &
			~(USB_TXCSR_SENTSTALL | USB_TXCSR_URUNERR |
			  USB_TXCSR_TXPKTRDY | USB_TXCSR_FLUSHFIFO |
			  USB_TXCSR_CLRDATATGL)));
		return;
	}
	if (!p->armed)
		return;
	if (p->remaining) {
		pipe_write(p, epn);
		return;
	}
	/* Double buffered: wait until the last packet has left the FIFO. */
	if (csr & USB_TXCSR_NEFIFO)
		return;
	len = p->length;
	p->buf = NULL;
	p->armed = false;
	dcd_event_xfer_complete(rhport, (uint8_t)(0x80u | epn), len,
				XFER_RESULT_SUCCESS, true);
}

/* Drain one packet; returns true when it was short. */
static bool pipe_read(struct pipe *p, unsigned epn)
{
	const uint16_t cnt = wb_mmr_read16(USB0_EP_RXCNT(epn));
	uint16_t n = (uint16_t)(p->remaining < p->mps ?
		p->remaining : p->mps);

	if (cnt < n)
		n = cnt;
	if (n) {
		fifo_read(epn, p->buf, n);
		p->buf += n;
		p->remaining = (uint16_t)(p->remaining - n);
	}
	/* Clear RXPKTRDY (W0C): the FIFO slot is free again. */
	wb_mmr_write16(USB0_EP_RXCSR(epn),
		       (uint16_t)(p->iso ? USB_RXCSR_ISO : 0u));
	return n < p->mps;
}

static void process_epout_isr(uint8_t rhport, unsigned epn, bool is_isr)
{
	const uint16_t csr = wb_mmr_read16(USB0_EP_RXCSR(epn));
	struct pipe *p = pipe_get(epn, false);

	if (csr & USB_RXCSR_SENTSTALL) {
		/* Clear SENTSTALL and ORUNERR (W0C); RXPKTRDY written as read. */
		wb_mmr_write16(USB0_EP_RXCSR(epn), (uint16_t)(csr &
			~(USB_RXCSR_SENTSTALL | USB_RXCSR_ORUNERR |
			  USB_RXCSR_FLUSHFIFO | USB_RXCSR_CLRDATATGL)));
		return;
	}
	if (!(csr & USB_RXCSR_RXPKTRDY))
		return;
	if (!p->armed) {
		/*
		 * The packet stays in the FIFO, so the controller NAKs the
		 * host until a transfer is armed; mask the endpoint meanwhile.
		 */
		set_rxe((uint16_t)(rxe_shadow & ~bit16(epn)));
		return;
	}
	if (pipe_read(p, epn) || p->remaining == 0) {
		const uint16_t len = (uint16_t)(p->length - p->remaining);

		p->buf = NULL;
		p->armed = false;
		dcd_event_xfer_complete(rhport, (uint8_t)epn, len,
					XFER_RESULT_SUCCESS, is_isr);
	}
}

/* --- Bus events -------------------------------------------------------- */

static void process_bus_reset_isr(uint8_t rhport)
{
	unsigned i;

	/* The controller has cleared FADDR and the endpoint registers. */
	wb_usb_fifo_reset();
	pipe0 = (struct pipe0){ 0 };
	for (i = 0; i < WB_USB_EP_MAX; i++)
		pipes[0][i] = pipes[1][i] = (struct pipe){ 0 };
	opened[0] = opened[1] = 0;
	set_txe(1u);
	set_rxe(0);
	dcd_event_bus_reset(rhport, (wb_mmr_read8(USB0_POWER) &
				     USB_POWER_HSMODE) ?
			    TUSB_SPEED_HIGH : TUSB_SPEED_FULL, true);
}

void dcd_int_handler(uint8_t rhport)
{
	const uint8_t irq = (uint8_t)(wb_mmr_read8(USB0_IRQ) & ien_shadow);
	uint16_t tx = (uint16_t)(wb_mmr_read16(USB0_INTRTX) & txe_shadow);
	uint16_t rx = (uint16_t)(wb_mmr_read16(USB0_INTRRX) & rxe_shadow);

	if (irq & USB_IRQ_DISCON)
		dcd_event_bus_signal(rhport, DCD_EVENT_UNPLUGGED, true);
	if (irq & USB_IRQ_SOF)
		dcd_event_bus_signal(rhport, DCD_EVENT_SOF, true);
	if (irq & USB_IRQ_RSTBABBLE)
		process_bus_reset_isr(rhport);
	if (irq & USB_IRQ_RESUME)
		dcd_event_bus_signal(rhport, DCD_EVENT_RESUME, true);
	if (irq & USB_IRQ_SUSPEND)
		dcd_event_bus_signal(rhport, DCD_EVENT_SUSPEND, true);

	while (tx) {
		const unsigned n = (unsigned)__builtin_ctz(tx);

		if (n == 0)
			process_ep0_isr(rhport);
		else
			process_epin_isr(rhport, n);
		tx = (uint16_t)(tx & ~bit16(n));
		/* A double-buffered endpoint, or a SETUP, may be pending. */
		tx = (uint16_t)(tx | (wb_mmr_read16(USB0_INTRTX) & txe_shadow));
	}
	while (rx) {
		const unsigned n = (unsigned)__builtin_ctz(rx);

		process_epout_isr(rhport, n, true);
		rx = (uint16_t)(rx & ~bit16(n));
		rx = (uint16_t)(rx | (wb_mmr_read16(USB0_INTRRX) & rxe_shadow));
	}
}

/* --- Device API -------------------------------------------------------- */

bool dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
	(void)rhport;
	(void)rh_init;
	wb_usb_dcd_reset_state();
	ien_shadow = (uint8_t)(USB_IRQ_SUSPEND | USB_IRQ_RESUME |
			       USB_IRQ_RSTBABBLE | USB_IRQ_DISCON);
	wb_mmr_write8(USB0_IEN, ien_shadow);
	set_txe(1u);
	set_rxe(0);
	/* Clear what is pending from before; the reads clear. */
	(void)wb_mmr_read8(USB0_IRQ);
	(void)wb_mmr_read16(USB0_INTRTX);
	(void)wb_mmr_read16(USB0_INTRRX);
	return true;
}

bool dcd_deinit(uint8_t rhport)
{
	dcd_int_disable(rhport);
	dcd_disconnect(rhport);
	ien_shadow = 0;
	wb_mmr_write8(USB0_IEN, 0);
	return true;
}

void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
	(void)rhport;
	/* FADDR changes once the status stage has completed (USB 2.0 9.4.6). */
	pipe0.pending_addr = dev_addr;
	pipe0.buf = NULL;
	pipe0.xact_len = 0;
	pipe0.state = PIPE0_STATUS_IN;
	csr0(USB_CSR0_SPKTRDY | USB_CSR0_DATAEND);
	pipe0.rxrdy_consumed = false;
}

/* The device never signals remote wakeup (S-10a.081); nothing to do. */
void dcd_remote_wakeup(uint8_t rhport)
{
	(void)rhport;
}

void dcd_connect(uint8_t rhport)
{
	(void)rhport;
	wb_mmr_write8(USB0_POWER, (uint8_t)(wb_mmr_read8(USB0_POWER) |
		      USB_POWER_HSEN | USB_POWER_SOFTCONN));
}

void dcd_disconnect(uint8_t rhport)
{
	(void)rhport;
	wb_mmr_write8(USB0_POWER, (uint8_t)(wb_mmr_read8(USB0_POWER) &
		      ~USB_POWER_SOFTCONN));
}

void dcd_sof_enable(uint8_t rhport, bool en)
{
	(void)rhport;
	ien_shadow = (uint8_t)(en ? ien_shadow | USB_IRQ_SOF :
			       ien_shadow & ~USB_IRQ_SOF);
	wb_mmr_write8(USB0_IEN, ien_shadow);
}

/* --- Endpoint API ------------------------------------------------------ */

static void ep_flush(unsigned epn, bool in)
{
	unsigned i;

	/* Twice: a double-buffered FIFO may hold two packets. */
	for (i = 0; i < 2u; i++) {
		if (in) {
			if (wb_mmr_read16(USB0_EP_TXCSR(epn)) & USB_TXCSR_NEFIFO)
				wb_mmr_write16(USB0_EP_TXCSR(epn),
					       USB_TXCSR_FLUSHFIFO);
		} else {
			if (wb_mmr_read16(USB0_EP_RXCSR(epn)) &
			    USB_RXCSR_RXPKTRDY)
				wb_mmr_write16(USB0_EP_RXCSR(epn),
					       USB_RXCSR_FLUSHFIFO);
		}
	}
}

static bool ep_open(unsigned epn, bool in, uint16_t mps, uint8_t type,
		    bool alloc)
{
	struct pipe *p;
	const bool iso = type == TUSB_XFER_ISOCHRONOUS;
	const bool was = irq_save();

	if (epn == 0 || epn >= WB_USB_EP_MAX || mps == 0) {
		irq_restore(was);
		return false;
	}
	if (alloc && !wb_usb_fifo_alloc(epn, !in, mps,
					type == TUSB_XFER_BULK || iso)) {
		irq_restore(was);
		return false;
	}
	p = pipe_get(epn, in);
	*p = (struct pipe){ .mps = mps, .iso = iso };
	if (in) {
		wb_mmr_write16(USB0_EP_TXMAXP(epn), mps);
		ep_flush(epn, true);
		wb_mmr_write16(USB0_EP_TXCSR(epn), (uint16_t)(
			USB_TXCSR_CLRDATATGL | (iso ? USB_TXCSR_ISO : 0u)));
		set_txe((uint16_t)(txe_shadow | bit16(epn)));
	} else {
		wb_mmr_write16(USB0_EP_RXMAXP(epn), mps);
		ep_flush(epn, false);
		wb_mmr_write16(USB0_EP_RXCSR(epn), (uint16_t)(
			USB_RXCSR_CLRDATATGL | (iso ? USB_RXCSR_ISO : 0u)));
		set_rxe((uint16_t)(rxe_shadow | bit16(epn)));
	}
	opened[in ? 1 : 0] = (uint16_t)(opened[in ? 1 : 0] | bit16(epn));
	irq_restore(was);
	return true;
}

bool dcd_edpt_open(uint8_t rhport, const tusb_desc_endpoint_t *ep_desc)
{
	(void)rhport;
	return ep_open(ep_desc->bEndpointAddress & 0x0Fu,
		       (ep_desc->bEndpointAddress & 0x80u) != 0,
		       (uint16_t)(tu_edpt_packet_size(ep_desc)),
		       ep_desc->bmAttributes.xfer, true);
}

bool dcd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr,
			uint16_t largest_packet_size)
{
	(void)rhport;
	return wb_usb_fifo_alloc(ep_addr & 0x0Fu, !(ep_addr & 0x80u),
				 largest_packet_size, true);
}

bool dcd_edpt_iso_activate(uint8_t rhport, const tusb_desc_endpoint_t *desc)
{
	(void)rhport;
	return ep_open(desc->bEndpointAddress & 0x0Fu,
		       (desc->bEndpointAddress & 0x80u) != 0,
		       (uint16_t)(tu_edpt_packet_size(desc)),
		       TUSB_XFER_ISOCHRONOUS, false);
}

static void ep_close(unsigned epn, bool in)
{
	if (in) {
		set_txe((uint16_t)(txe_shadow & ~bit16(epn)));
		ep_flush(epn, true);
		wb_mmr_write16(USB0_EP_TXMAXP(epn), 0);
		wb_mmr_write16(USB0_EP_TXCSR(epn), 0);
	} else {
		set_rxe((uint16_t)(rxe_shadow & ~bit16(epn)));
		ep_flush(epn, false);
		wb_mmr_write16(USB0_EP_RXMAXP(epn), 0);
		wb_mmr_write16(USB0_EP_RXCSR(epn), 0);
	}
	*pipe_get(epn, in) = (struct pipe){ 0 };
	opened[in ? 1 : 0] = (uint16_t)(opened[in ? 1 : 0] & ~bit16(epn));
}

void dcd_edpt_close_all(uint8_t rhport)
{
	const bool was = irq_save();
	unsigned n, d;

	(void)rhport;
	for (d = 0; d < 2u; d++)
		for (n = 1; n < WB_USB_EP_MAX; n++)
			if (opened[d] & bit16(n))
				ep_close(n, d != 0);
	wb_usb_fifo_reset();
	irq_restore(was);
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer,
		   uint16_t total_bytes, bool is_isr)
{
	const unsigned epn = ep_addr & 0x0Fu;
	const bool in = (ep_addr & 0x80u) != 0;
	const bool was = irq_save();
	bool ret = true;

	if (epn == 0) {
		ret = edpt0_xfer(rhport, ep_addr, buffer, total_bytes, is_isr);
	} else if (epn >= WB_USB_EP_MAX) {
		ret = false;
	} else {
		struct pipe *p = pipe_get(epn, in);

		p->buf = buffer;
		p->length = total_bytes;
		p->remaining = total_bytes;
		p->armed = true;
		if (in) {
			pipe_write(p, epn);
		} else {
			set_rxe((uint16_t)(rxe_shadow | bit16(epn)));
			/* A packet may already wait in the FIFO. */
			if (wb_mmr_read16(USB0_EP_RXCSR(epn)) &
			    USB_RXCSR_RXPKTRDY)
				process_epout_isr(rhport, epn, is_isr);
		}
	}
	irq_restore(was);
	return ret;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
	const unsigned epn = ep_addr & 0x0Fu;
	const bool was = irq_save();

	if (epn == 0) {
		/* EP0 IN is stalled through EP0 OUT: one SENDSTALL. */
		if (ep_addr == 0x00) {
			pipe0.state = PIPE0_IDLE;
			pipe0.buf = NULL;
			if (pipe0.deferred_valid) {
				/*
				 * The stalled transfer is over on the wire and
				 * the next SETUP was acked: answer that one.
				 */
				pipe0_try_deferred_setup(rhport, false);
			} else {
				/* Service a SETUP still parked in the FIFO. */
				csr0((uint16_t)(USB_CSR0_SENDSTALL |
					(pipe0.rxrdy_consumed ?
					 USB_CSR0_SPKTRDY : 0u)));
				pipe0.rxrdy_consumed = false;
			}
		}
	} else if (epn < WB_USB_EP_MAX) {
		struct pipe *p = pipe_get(epn, (ep_addr & 0x80u) != 0);

		if (ep_addr & 0x80u)
			wb_mmr_write16(USB0_EP_TXCSR(epn), (uint16_t)(
				USB_TXCSR_SENDSTALL |
				(p->iso ? USB_TXCSR_ISO : 0u)));
		else
			wb_mmr_write16(USB0_EP_RXCSR(epn), (uint16_t)(
				USB_RXCSR_SENDSTALL |
				(p->iso ? USB_RXCSR_ISO : 0u)));
		p->armed = false;
	}
	irq_restore(was);
}

/*
 * Clear the halt and reset the data toggle to DATA0 (USB 2.0 9.4.5). usbd
 * also calls this for CLEAR_FEATURE(HALT) on an endpoint that is not halted
 * and for SET_INTERFACE, and clears the endpoint's busy flag either way, so
 * the transfer in progress ends here: its packets leave the FIFO and the
 * class driver arms a new one.
 */
void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
	const unsigned epn = ep_addr & 0x0Fu;
	const bool was = irq_save();

	(void)rhport;
	if (epn && epn < WB_USB_EP_MAX) {
		struct pipe *p = pipe_get(epn, (ep_addr & 0x80u) != 0);

		ep_flush(epn, (ep_addr & 0x80u) != 0);
		p->buf = NULL;
		p->armed = false;
		if (ep_addr & 0x80u)
			wb_mmr_write16(USB0_EP_TXCSR(epn), (uint16_t)(
				USB_TXCSR_CLRDATATGL |
				(p->iso ? USB_TXCSR_ISO : 0u)));
		else
			wb_mmr_write16(USB0_EP_RXCSR(epn), (uint16_t)(
				USB_RXCSR_CLRDATATGL |
				(p->iso ? USB_RXCSR_ISO : 0u)));
	}
	irq_restore(was);
}
