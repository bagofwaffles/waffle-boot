/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10a on TinyUSB's device core: src/core/usb/device.c with the real usbd and a
 * device controller fake below that answers a SETUP as the BF70x driver
 * does (s10a_usb_setup_filter() first, RESELECT included). Checks the
 * enumeration of every set at both speeds (S-10a.001 to S-10a.021), the
 * configurations and the input-current target (S-10a.032), bus events
 * (S-10a.080, S-10a.081), the medium vectors and the BOT 1.0 6.7 cases
 * carried over bulk endpoints (S-10a.040 to S-10a.052), the CDC port
 * (S-10a.060 to S-10a.062), MIDI through S-10c, and S-10b's responder on
 * the MTP sets: framing, the class requests and bus events.
 */
#include <stdint.h>
#include <string.h>

#include "tusb.h"
#include "device/dcd.h"

#include "waffle/boot/device.h"
#include "waffle/usb/msc.h"
#include "waffle/usb/device.h"
#include "waffle/mtp/mtp.h"
#include "waffle/host/commands.h"
#include "waffle/host/sysex.h"

#include "support/check.h"
#include "s15_sha256.h"
#include "vectors/test_s10a_vectors.h"

#define B(r) (s10av_blob + (r).off)
#define BLOCKS 454656u			/* S-12.001 */

/* --- The device controller fake ------------------------------------------ */

static struct {
	bool open[2][16];
	bool armed[2][16];
	bool stalled[2][16];
	uint8_t *buf[2][16];
	uint16_t len[2][16];
	uint16_t mps[2][16];		/* at the speed of the bus */
	uint16_t got[2][16];		/* of an OUT transfer, so far */
	bool dir_in;			/* the control request's direction */
	uint8_t ep0_in[1024];
	size_t ep0_in_len;
	bool ep0_stalled, ep0_status;
	const uint8_t *ep0_out;
	uint16_t ep0_out_len;
	bool reselect_owed;
	uint8_t reselect[8];
	unsigned clears;
} hw;

#define D(ep) (((ep) & 0x80u) ? 1 : 0)
#define N(ep) ((ep) & 0x0Fu)

bool dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
	(void)rhport;
	(void)rh_init;
	return true;
}

uint32_t tusb_time_millis_api(void)
{
	return 0;
}

void dcd_int_handler(uint8_t rhport)
{
	(void)rhport;
}

void dcd_int_enable(uint8_t rhport)
{
	(void)rhport;
}

void dcd_int_disable(uint8_t rhport)
{
	(void)rhport;
}

void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
	(void)dev_addr;
	(void)dcd_edpt_xfer(rhport, 0x80, NULL, 0, false);
}

void dcd_remote_wakeup(uint8_t rhport)
{
	(void)rhport;
	CHECK(0);	/* S-10a.081: never */
}

void dcd_sof_enable(uint8_t rhport, bool en)
{
	(void)rhport;
	(void)en;
}

bool dcd_edpt_open(uint8_t rhport, const tusb_desc_endpoint_t *ep)
{
	const uint8_t a = ep->bEndpointAddress;

	(void)rhport;
	CHECK(!hw.open[D(a)][N(a)]);
	hw.open[D(a)][N(a)] = true;
	hw.mps[D(a)][N(a)] = tu_edpt_packet_size(ep);
	return true;
}

bool dcd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t size)
{
	(void)rhport;
	(void)ep_addr;
	(void)size;
	return false;
}

bool dcd_edpt_iso_activate(uint8_t rhport, const tusb_desc_endpoint_t *ep)
{
	(void)rhport;
	(void)ep;
	return false;
}

void dcd_edpt_close_all(uint8_t rhport)
{
	unsigned n;

	(void)rhport;
	for (n = 1; n < 16; n++) {
		hw.open[0][n] = hw.open[1][n] = false;
		hw.armed[0][n] = hw.armed[1][n] = false;
		hw.stalled[0][n] = hw.stalled[1][n] = false;
	}
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep, uint8_t *buf, uint16_t len,
		   bool is_isr)
{
	(void)is_isr;
	if (ep == 0x80) {
		if (len == 0 && !hw.dir_in && hw.reselect_owed) {
			/* As the BF70x driver: the real request now. */
			hw.reselect_owed = false;
			dcd_event_setup_received(rhport, hw.reselect, false);
			return true;
		}
		if (len == 0 && !hw.dir_in)
			hw.ep0_status = true;
		CHECK(hw.ep0_in_len + len <= sizeof(hw.ep0_in));
		if (len && hw.ep0_in_len + len <= sizeof(hw.ep0_in)) {
			memcpy(hw.ep0_in + hw.ep0_in_len, buf, len);
			hw.ep0_in_len += len;
		}
		dcd_event_xfer_complete(rhport, ep, len, XFER_RESULT_SUCCESS,
					false);
		return true;
	}
	if (ep == 0x00) {
		uint16_t n = (uint16_t)(len < hw.ep0_out_len ? len :
					hw.ep0_out_len);

		if (len == 0) {
			hw.ep0_status = true;
		} else {
			memcpy(buf, hw.ep0_out, n);
			hw.ep0_out += n;
			hw.ep0_out_len = (uint16_t)(hw.ep0_out_len - n);
		}
		dcd_event_xfer_complete(rhport, ep, n, XFER_RESULT_SUCCESS,
					false);
		return true;
	}
	CHECK(hw.open[D(ep)][N(ep)] && !hw.armed[D(ep)][N(ep)]);
	hw.buf[D(ep)][N(ep)] = buf;
	hw.len[D(ep)][N(ep)] = len;
	hw.got[D(ep)][N(ep)] = 0;
	hw.armed[D(ep)][N(ep)] = true;
	return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep)
{
	(void)rhport;
	if (N(ep) == 0) {
		hw.ep0_stalled = true;
		return;
	}
	hw.stalled[D(ep)][N(ep)] = true;
	hw.armed[D(ep)][N(ep)] = false;
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep)
{
	(void)rhport;
	/* As the BF70x driver: the transfer in progress ends too. */
	hw.clears++;
	hw.stalled[D(ep)][N(ep)] = false;
	hw.armed[D(ep)][N(ep)] = false;
}

/* --- The host's side of endpoint 0 --------------------------------------- */

/* One control transfer; true when it completed (no STALL). */
static bool ctl(uint8_t bm, uint8_t req, uint16_t value, uint16_t index,
		uint16_t length, const uint8_t *out)
{
	const uint8_t s[8] = { bm, req, (uint8_t)value, (uint8_t)(value >> 8),
			       (uint8_t)index, (uint8_t)(index >> 8),
			       (uint8_t)length, (uint8_t)(length >> 8) };
	uint8_t reply[64];
	uint16_t len = 0;

	hw.ep0_in_len = 0;
	hw.ep0_stalled = hw.ep0_status = false;
	hw.ep0_out = out;
	hw.ep0_out_len = (uint16_t)(out ? length : 0u);
	hw.dir_in = (bm & 0x80u) != 0;
	switch (s10a_usb_setup_filter(NULL, s, reply, &len)) {
	case WB_USB_SETUP_STALL:
		return false;
	case WB_USB_SETUP_ACCEPT:
		return true;
	case WB_USB_SETUP_REPLY:
		hw.ep0_in_len = (size_t)(len < length ? len : length);
		memcpy(hw.ep0_in, reply, hw.ep0_in_len);
		return true;
	case WB_USB_SETUP_RESELECT:
		memcpy(hw.reselect, s, 8);
		hw.reselect_owed = true;
		{
			uint8_t zero[8];

			memcpy(zero, s, 8);
			zero[2] = zero[3] = 0;
			dcd_event_setup_received(0, zero, false);
		}
		break;
	case WB_USB_SETUP_FORWARD:
	default:
		dcd_event_setup_received(0, s, false);
		break;
	}
	tud_task();
	CHECK(!hw.reselect_owed);
	return !hw.ep0_stalled && hw.ep0_status;
}

static bool get_descriptor(uint8_t type, uint8_t index, uint16_t length)
{
	return ctl(0x80, 6, (uint16_t)(type << 8 | index), 0, length, NULL);
}

static bool set_configuration(unsigned value)
{
	return ctl(0x00, 9, (uint16_t)value, 0, 0, NULL);
}

static bool clear_halt(uint8_t ep)
{
	return ctl(0x02, 1, 0, ep, 0, NULL);
}

/* What the controller forgets at a bus reset or a new session. */
static void hw_forget(void)
{
	unsigned n;

	for (n = 1; n < 16; n++) {
		hw.open[0][n] = hw.open[1][n] = false;
		hw.armed[0][n] = hw.armed[1][n] = false;
		hw.stalled[0][n] = hw.stalled[1][n] = false;
	}
}

static void bus_reset(bool high)
{
	hw_forget();
	dcd_event_bus_reset(0, high ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL,
			    false);
	tud_task();
}

/* A bus reset, the device descriptor, an address. */
static void enumerate(bool high)
{
	bus_reset(high);
	CHECK(get_descriptor(1, 0, 64));
	CHECK(ctl(0x00, 5, 7, 0, 0, NULL));
}

/* --- Bulk endpoints ------------------------------------------------------- */

/*
 * The host sends n bytes in packets of the endpoint's size, as the BF70x
 * driver takes them: a transfer ends on a short packet or once its count is
 * full, and a packet waits (NAK) while nothing is armed. False when the
 * endpoint is halted or not armed for a packet that is still to go.
 */
static bool host_out(uint8_t ep, const uint8_t *p, uint32_t n)
{
	const unsigned e = N(ep);
	uint32_t off = 0;

	do {
		uint32_t pkt = n - off, k;

		if (hw.stalled[0][e] || !hw.armed[0][e])
			return false;
		if (pkt > hw.mps[0][e])
			pkt = hw.mps[0][e];
		k = hw.len[0][e] - hw.got[0][e];
		if (k > pkt)
			k = pkt;
		memcpy(hw.buf[0][e] + hw.got[0][e], p + off, k);
		hw.got[0][e] = (uint16_t)(hw.got[0][e] + k);
		off += pkt;
		if (pkt < hw.mps[0][e] || hw.got[0][e] == hw.len[0][e]) {
			hw.armed[0][e] = false;
			dcd_event_xfer_complete(0, ep, hw.got[0][e],
						XFER_RESULT_SUCCESS, false);
			tud_task();
		}
	} while (off < n);
	return true;
}

/* The host takes the transfer armed on IN, up to `room` bytes. */
static uint32_t host_in(uint8_t ep, uint8_t *dst, uint32_t room)
{
	uint32_t n;

	if (hw.stalled[1][N(ep)] || !hw.armed[1][N(ep)])
		return 0;
	n = hw.len[1][N(ep)];
	CHECK(n <= room);
	if (n > room)
		n = room;
	memcpy(dst, hw.buf[1][N(ep)], n);
	hw.armed[1][N(ep)] = false;
	dcd_event_xfer_complete(0, ep, n, XFER_RESULT_SUCCESS, false);
	tud_task();
	return n;
}

/* --- The board ------------------------------------------------------------ */

static struct {
	unsigned ma, ma_calls;
	unsigned suspends, resumes;
	unsigned released;
	unsigned cdc_restarts;
	unsigned activity;
} brd;

static void board_current(void *ctx, unsigned ma)
{
	(void)ctx;
	brd.ma = ma;
	brd.ma_calls++;
}

static void board_suspend(void *ctx, bool suspended)
{
	(void)ctx;
	if (suspended)
		brd.suspends++;
	else
		brd.resumes++;
}

static void board_released(void *ctx)
{
	(void)ctx;
	brd.released++;
}

static void board_activity(void *ctx)
{
	(void)ctx;
	brd.activity++;
}

static const struct s10a_usb_board board = {
	.input_current = board_current,
	.suspend = board_suspend,
	.disk_released = board_released,
	.activity = board_activity,
};

static void cdc_rx_restart(void *ctx)
{
	(void)ctx;
	brd.cdc_restarts++;
}

static void nothing(void *ctx)
{
	(void)ctx;
}

static void feed_nothing(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	(void)p;
	(void)n;
}

static const struct s10c_ops ops = {
	.send_sysex = s10a_usb_send_sysex,
	.cdc_open = s10a_usb_cdc_open,
	.cdc_rx_restart = cdc_rx_restart,
	.midi_rx_restart = nothing,
	.midi_rx_feed = feed_nothing,
};

static struct s10c_host host;

/* --- The disk: sparse, every block 0 until written ------------------------ */

#define SLOTS 64
static struct {
	uint32_t lba[SLOTS];
	uint8_t data[SLOTS][S10A_BLOCK];
	unsigned used;
} disk;

static uint8_t *slot(uint32_t lba, bool make)
{
	unsigned i;

	for (i = 0; i < disk.used; i++)
		if (disk.lba[i] == lba)
			return disk.data[i];
	if (!make || disk.used == SLOTS)
		return NULL;
	disk.lba[disk.used] = lba;
	memset(disk.data[disk.used], 0, S10A_BLOCK);
	return disk.data[disk.used++];
}

static int disk_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	uint8_t *p = buf;

	(void)ctx;
	if (lba + count > BLOCKS)
		return -1;
	for (; count; count--, lba++, p += S10A_BLOCK) {
		const uint8_t *s = slot(lba, false);

		if (s)
			memcpy(p, s, S10A_BLOCK);
		else
			memset(p, 0, S10A_BLOCK);
	}
	return 0;
}

static int disk_write(void *ctx, uint32_t lba, const void *buf,
		      uint32_t count)
{
	const uint8_t *p = buf;

	(void)ctx;
	if (lba + count > BLOCKS)
		return -1;
	for (; count; count--, lba++, p += S10A_BLOCK) {
		uint8_t *s = slot(lba, true);

		if (!s)
			return -1;
		memcpy(s, p, S10A_BLOCK);
	}
	return 0;
}

static const struct wb_blockdev dev = {
	.n_sectors = BLOCKS, .read = disk_read, .write = disk_write,
};

/* --- Set-up --------------------------------------------------------------- */

static void start(enum s10a_set set, const char *serial, bool high)
{
	static bool inited;

	memset(&hw, 0, sizeof(hw));
	memset(&brd, 0, sizeof(brd));
	if (!inited) {
		static const tusb_rhport_init_t rh = {
			.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH,
		};

		s10c_host_init(&host, &ops, NULL, &wb_device_opxy, WB_DEV_BY_TABLE);
		s10a_usb_init(&board, &host, &wb_device_opxy);
		CHECK(tud_rhport_init(0, &rh));
		inited = true;
	}
	/* As wb_usb_attach() after a detach: a new session. */
	dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, false);
	tud_task();
	hw_forget();
	s10a_usb_select(set, serial, set == S10A_SET_DISK ||
			set == S10A_SET_DISK_CDC ? &dev : NULL);
	memset(&brd, 0, sizeof(brd));
	enumerate(high);
}

/* --- S-10a.001 to S-10a.021: what each set enumerates as ----------------- */

static void check_descriptors(enum s10a_set set, const char *serial,
			      bool high)
{
	uint8_t want[S10A_CONFIG_MAX];
	const bool has = serial[0] != '\0';
	unsigned i;
	size_t n;

	start(set, serial, high);
	CHECK(get_descriptor(1, 0, 255));
	s10a_desc_device(&wb_device_opxy, set, has, want);
	CHECK(hw.ep0_in_len == 18 && memcmp(hw.ep0_in, want, 18) == 0);
	CHECK(get_descriptor(6, 0, 10));
	s10a_desc_qualifier(set, want);
	CHECK(hw.ep0_in_len == 10 && memcmp(hw.ep0_in, want, 10) == 0);
	for (i = 0; i < 4; i++) {
		n = s10a_desc_config(set, has, i, high, false, want);
		CHECK(get_descriptor(2, (uint8_t)i, 1024) == (n != 0));
		CHECK(hw.ep0_in_len == n && memcmp(hw.ep0_in, want, n) == 0);
		n = s10a_desc_config(set, has, i, high, true, want);
		CHECK(get_descriptor(7, (uint8_t)i, 1024) == (n != 0));
		CHECK(hw.ep0_in_len == n && memcmp(hw.ep0_in, want, n) == 0);
		/* Cut to wLength 9: the header only. */
		if (n) {
			CHECK(get_descriptor(2, (uint8_t)i, 9));
			CHECK(hw.ep0_in_len == 9);
		}
	}
	for (i = 0; i < 16; i++) {
		n = s10a_desc_string(&wb_device_opxy, set, serial, i, want);
		CHECK(ctl(0x80, 6, (uint16_t)(0x0300 | i), i ? 0x0409u : 0u,
			  255, NULL) == (n != 0));
		CHECK(hw.ep0_in_len == n && memcmp(hw.ep0_in, want, n) == 0);
	}
	/* Every configuration selects (each function claims its interfaces)
	 * and sets the target; SET_CONFIGURATION(0) sets 100 mA. */
	for (i = 1; i <= s10a_desc_num_configs(set); i++) {
		CHECK(set_configuration(i));
		CHECK(tud_mounted());
		CHECK(brd.ma == 2u * s10a_desc_max_power(set, i));
		CHECK(ctl(0x80, 8, 0, 0, 1, NULL) && hw.ep0_in[0] == i);
	}
	CHECK(set_configuration(0));
	CHECK(!tud_mounted() && brd.ma == 100u);
}

static void test_descriptors(void)
{
	static const char *const serials[] = { "", "XY0A12BC" };
	unsigned set, k, high;

	for (set = 0; set < S10A_SET_COUNT; set++)
		for (k = 0; k < 2; k++)
			for (high = 0; high < 2; high++) {
				const unsigned before = check_failures;

				check_descriptors((enum s10a_set)set,
						  serials[k], high != 0);
				if (check_failures != before)
					printf("  set %u serial %u high %u\n",
					       set, k, high);
			}
}

/* --- S-10a.032, S-10a.080, S-10a.081: configurations and bus events ------- */

static void test_configuration_events(void)
{
	start(S10A_SET_MIDI, "", true);
	CHECK(brd.ma == 100u);		/* the bus reset */
	/* USB activity: the reset; a configuration is not. */
	CHECK(brd.activity == 1u);
	CHECK(set_configuration(3));
	CHECK(tud_mounted() && brd.ma == 2u && brd.activity == 1u);
	/* Another value: no 100 mA in between. */
	brd.ma_calls = 0;
	CHECK(set_configuration(1));
	CHECK(brd.ma == 500u && brd.ma_calls == 1u);
	/* The same value: selected afresh, the target kept. */
	brd.ma_calls = 0;
	CHECK(set_configuration(1));
	CHECK(tud_mounted() && brd.ma == 500u && brd.ma_calls == 1u);
	CHECK(!set_configuration(4) && tud_mounted());
	CHECK(brd.activity == 1u);
	/* SET_INTERFACE leaves the target and is not activity. */
	brd.ma_calls = 0;
	CHECK(ctl(0x01, 11, 0, 1, 0, NULL));
	CHECK(brd.ma_calls == 0 && brd.activity == 1u);
	/* Nor is SET_CONFIGURATION(0). */
	CHECK(set_configuration(0) && brd.activity == 1u);
	CHECK(set_configuration(1) && brd.activity == 1u);
	/* A bus reset: Default state, 100 mA, activity. */
	bus_reset(true);
	CHECK(!tud_mounted() && brd.ma == 100u && brd.activity == 2u);
	CHECK(!ctl(0x81, 10, 0, 0, 1, NULL));	/* no interface now */
	CHECK(ctl(0x80, 8, 0, 0, 1, NULL) && hw.ep0_in[0] == 0);
	/* Suspend and resume reach the board and change nothing else. */
	CHECK(ctl(0x00, 5, 7, 0, 0, NULL));
	CHECK(set_configuration(2));
	dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false);
	tud_task();
	dcd_event_bus_signal(0, DCD_EVENT_RESUME, false);
	tud_task();
	CHECK(brd.suspends == 1u && brd.resumes == 1u);
	CHECK(tud_mounted() && brd.ma == 100u && brd.activity == 4u);
	/* A bus reset behind a SET_CONFIGURATION still queued: the reset's
	 * 100 mA comes after the configuration's 2 mA. */
	{
		static const uint8_t s3[8] = { 0x00, 9, 3, 0, 0, 0, 0, 0 };
		uint8_t reply[64];
		uint16_t len = 0;

		CHECK(set_configuration(1) && brd.ma == 500u);
		CHECK(s10a_usb_setup_filter(NULL, s3, reply, &len) ==
		      WB_USB_SETUP_FORWARD);
		hw.dir_in = false;
		dcd_event_setup_received(0, s3, false);
		hw_forget();
		dcd_event_bus_reset(0, TUSB_SPEED_HIGH, false);
		tud_task();
		CHECK(!tud_mounted() && brd.ma == 100u);
		/* The stale request opened endpoints behind the controller's
		 * reset; usbd's reset does not close them (a hardware-only
		 * case: the driver's next open of them takes FIFO again). */
		hw_forget();
		CHECK(ctl(0x00, 5, 7, 0, 0, NULL));
	}
	/* The loss of VBUS: 100 mA, activity. */
	CHECK(set_configuration(1));
	brd.activity = 0;
	dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, false);
	tud_task();
	hw_forget();
	CHECK(!tud_mounted() && brd.ma == 100u && brd.activity == 1u);
	/* A disconnect before any configuration is activity too. */
	enumerate(true);
	CHECK(brd.activity == 2u);
	dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, false);
	tud_task();
	hw_forget();
	CHECK(brd.activity == 3u);
}

/* --- Mass storage over the bulk endpoints ---------------------------------- */

#define IN S10A_EP_BULK_IN
#define OUT S10A_EP_BULK_OUT

static uint8_t rx[256 * S10A_BLOCK];
static bool saw_in_halt, saw_out_halt;

static void put_cbw(uint8_t cbw[31], uint32_t len, bool in,
		    const uint8_t *cdb, size_t cdb_len)
{
	memset(cbw, 0, 31);
	memcpy(cbw, "USBC", 4);
	cbw[4] = 0x5A;
	cbw[8] = (uint8_t)len;
	cbw[9] = (uint8_t)(len >> 8);
	cbw[10] = (uint8_t)(len >> 16);
	cbw[12] = (uint8_t)(in ? 0x80u : 0u);
	cbw[14] = (uint8_t)cdb_len;
	memcpy(cbw + 15, cdb, cdb_len);
}

/* One command as BOT 1.0 has the host run it (5.3). */
static void host_command(const uint8_t *cbw, const uint8_t *data_out,
			 uint32_t *moved, uint8_t csw[13])
{
	const uint32_t host_len = (uint32_t)cbw[8] | (uint32_t)cbw[9] << 8 |
				  (uint32_t)cbw[10] << 16 |
				  (uint32_t)cbw[11] << 24;
	const bool dir_in = (cbw[12] & 0x80u) != 0;
	unsigned guard;

	*moved = 0;
	saw_in_halt = saw_out_halt = false;
	memset(csw, 0, 13);
	CHECK(host_out(OUT, cbw, 31));
	for (guard = 0; guard < 64 && host_len && *moved < host_len; guard++) {
		if (dir_in) {
			const uint32_t n = host_in(IN, rx + *moved,
						   host_len - *moved);

			if (!n)
				break;
			*moved += n;
		} else {
			uint32_t k = host_len - *moved;

			if (hw.armed[0][N(OUT)] && k > hw.len[0][N(OUT)])
				k = hw.len[0][N(OUT)];
			if (!host_out(OUT, data_out + *moved, k))
				break;
			*moved += k;
		}
	}
	if (hw.stalled[0][N(OUT)] && !dir_in && host_len) {
		saw_out_halt = true;
		CHECK(clear_halt(OUT));
	}
	if (hw.stalled[1][N(IN)]) {
		saw_in_halt = true;
		CHECK(clear_halt(IN));
	}
	CHECK(host_in(IN, csw, 13) == 13);
}

static const struct s10a_lu *lu;

static void new_unit(const char *serial, bool offered)
{
	memset(&disk, 0, sizeof(disk));
	start(S10A_SET_DISK, serial, true);
	CHECK(set_configuration(1));
	CHECK(tud_mounted() && hw.armed[0][N(OUT)]);
	if (offered)
		s10a_usb_disk_offer();
	lu = s10a_usb_disk();
}

static void hex_of(const uint8_t *b, size_t n, char *out)
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = hex[b[i] >> 4];
		out[2 * i + 1] = hex[b[i] & 15];
	}
	out[2 * n] = '\0';
}

/* A medium vector step as a BOT command moving exactly what the step
 * moves, so the CSW status is the step's (GOOD or failed). */
static void command_step(const struct s10av_step *s)
{
	const bool out = s->data_out.len != 0;
	const uint32_t len = out ? (uint32_t)s->data_out.len : s->data_in_len;
	uint8_t cbw[31], csw[13];
	uint32_t moved;

	put_cbw(cbw, len, !out, B(s->cdb), s->cdb.len);
	host_command(cbw, B(s->data_out), &moved, csw);
	CHECK(moved == len);
	CHECK(csw[12] == (s->status ? 1 : 0));
	CHECK(!saw_in_halt && !saw_out_halt);
	if (s->data_in.present) {
		CHECK(s->data_in_len == s->data_in.len &&
		      memcmp(rx, B(s->data_in), s->data_in.len) == 0);
	} else if (!out && len) {
		struct s15_sha256 h;
		char hash[65], want[65];
		uint32_t i;

		s15_sha256_init(&h);
		for (i = 0; i < len; i++)
			s15_sha256_byte(&h, rx[i]);
		s15_sha256_hex(&h, hash);
		hex_of(B(s->data_in_sha256), 32, want);
		CHECK(strcmp(hash, want) == 0);
	}
	if (s->has_sense) {
		CHECK(lu->key == s->sense[0]);
		CHECK(lu->asc == s->sense[1]);
		CHECK(lu->ascq == s->sense[2]);
	}
}

/* Each event as the bus carries it. */
static void test_medium_vectors(void)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(s10av_steps); i++) {
		const struct s10av_step *s = &s10av_steps[i];
		const unsigned before = check_failures;

		if (s->event == 0 && !tud_mounted())
			CHECK(set_configuration(1));
		switch (s->event) {
		case 0:
			command_step(s);
			break;
		case 1:
			s10a_usb_disk_withdraw(S10A_WITHDRAW_SCAN);
			break;
		case 9:
			s10a_usb_disk_withdraw(S10A_WITHDRAW_OWN_WRITE);
			break;
		case 2:
			s10a_usb_disk_offer();
			break;
		case 3:
			bus_reset(true);
			CHECK(ctl(0x00, 5, 7, 0, 0, NULL));
			CHECK(set_configuration(1));
			break;
		case 4:
			CHECK(set_configuration(1));
			break;
		case 5:
			CHECK(set_configuration(0));
			break;
		case 6:
			CHECK(lu->releases == (uint32_t)s->releases);
			CHECK(brd.released == (unsigned)s->releases);
			break;
		case 7:
		case 8:
			new_unit(s->serial, s->event == 7);
			break;
		default:
			CHECK(0);
			break;
		}
		if (check_failures != before)
			printf("  step %s: %s\n", s->seq, s->name);
	}
}

static void test_bot_cases(void)
{
	static const uint8_t tur[6] = { 0 };
	size_t i;

	for (i = 0; i < ARRAY_LEN(s10av_bots); i++) {
		const struct s10av_bot *v = &s10av_bots[i];
		const unsigned before = check_failures;
		uint8_t cbw[31], csw[13], blk[S10A_BLOCK];
		uint32_t moved;
		int stall;

		new_unit("XY0A12BC", true);
		put_cbw(cbw, 0, false, tur, 6);	/* the unit attention */
		host_command(cbw, rx, &moved, csw);
		host_command(B(v->cbw), B(v->data_out), &moved, csw);
		stall = saw_in_halt ? 1 : saw_out_halt ? 2 : 0;
		CHECK(moved == v->moved);
		if (v->data_in.len)
			CHECK(memcmp(rx, B(v->data_in), v->data_in.len) == 0);
		CHECK(memcmp(csw, B(v->csw), 13) == 0);
		CHECK(stall == v->stall);
		CHECK(disk_read(NULL, 5, blk, 1) == 0);
		CHECK(memcmp(blk, B(v->block5), S10A_BLOCK) == 0);
		CHECK(hw.armed[0][N(OUT)] && !s10a_usb_disk_busy());
		if (check_failures != before)
			printf("  BOT case %d: %s\n", v->bot_case, v->name);
	}
}

/* S-10a.033, S-10a.041: a refused CBW holds both halts until the reset. */
static void test_reset_recovery(void)
{
	static const uint8_t tur[6] = { 0 };
	uint8_t cbw[31], csw[13];
	uint32_t moved;

	new_unit("XY0A12BC", true);
	put_cbw(cbw, 0, false, tur, 6);
	CHECK(host_out(OUT, cbw, 30));	/* 30 bytes: not a CBW */
	CHECK(hw.stalled[1][N(IN)] && hw.stalled[0][N(OUT)]);
	hw.clears = 0;
	CHECK(clear_halt(IN) && clear_halt(OUT));
	CHECK(hw.stalled[1][N(IN)] && hw.stalled[0][N(OUT)]);
	CHECK(hw.clears == 0);	/* answered before usbd (S-10a.033) */
	CHECK(ctl(0x82, 0, 0, IN, 2, NULL));	/* GET_STATUS: halted */
	CHECK(hw.ep0_in_len == 2 && hw.ep0_in[0] == 1);
	CHECK(ctl(0x21, 0xFF, 0, 0, 0, NULL));	/* Bulk-Only Reset */
	CHECK(clear_halt(IN) && clear_halt(OUT));
	CHECK(!hw.stalled[1][N(IN)] && !hw.stalled[0][N(OUT)]);
	host_command(cbw, rx, &moved, csw);
	CHECK(csw[12] == 1);	/* the unit attention */
	/* Get Max LUN, and the rest refused. */
	CHECK(ctl(0xA1, 0xFE, 0, 0, 1, NULL) && hw.ep0_in_len == 1 &&
	      hw.ep0_in[0] == 0);
	CHECK(!ctl(0xA1, 0xFE, 0, 0, 2, NULL));
	CHECK(!ctl(0xA1, 0x01, 0, 0, 1, NULL));
	/* SET_INTERFACE(0) abandons a command and expects a CBW. */
	put_cbw(cbw, 512, true, (const uint8_t[]){ 0x28, 0, 0, 0, 0, 3, 0,
						    0, 1, 0 }, 10);
	CHECK(host_out(OUT, cbw, 31));
	CHECK(hw.armed[1][N(IN)] && s10a_usb_disk_busy());
	CHECK(ctl(0x01, 11, 0, 0, 0, NULL));
	CHECK(!hw.armed[1][N(IN)] && hw.armed[0][N(OUT)]);
	CHECK(!s10a_usb_disk_busy());
	/* A reselection abandons it too and releases the disk, which the
	 * first read completed acquires. */
	host_command(cbw, rx, &moved, csw);
	CHECK(moved == 512 && csw[12] == 0 && lu->acquired);
	CHECK(host_out(OUT, cbw, 31));
	CHECK(s10a_usb_disk_busy());
	CHECK(set_configuration(1));
	CHECK(!s10a_usb_disk_busy() && !lu->acquired && brd.released == 1u);
	CHECK(hw.armed[0][N(OUT)] && !hw.armed[1][N(IN)]);
}

/* S-10a.049, S-10a.050: a withdrawal during a command's data phase. */
static void test_withdraw_in_command(void)
{
	static const uint8_t tur[6] = { 0 };
	static const uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 3, 0, 0, 1, 0 };
	uint8_t cbw[31], csw[13];
	uint32_t moved;

	new_unit("XY0A12BC", true);
	put_cbw(cbw, 0, false, tur, 6);
	host_command(cbw, rx, &moved, csw);	/* the unit attention */
	put_cbw(cbw, 512, true, read10, 10);
	host_command(cbw, rx, &moved, csw);
	CHECK(csw[12] == 0 && lu->acquired && brd.released == 0);
	/* A scan's withdrawal releases the held disk at once; the READ whose
	 * CBW has arrived completes as before. */
	CHECK(host_out(OUT, cbw, 31) && s10a_usb_disk_busy());
	s10a_usb_disk_withdraw(S10A_WITHDRAW_SCAN);
	CHECK(brd.released == 1u && !lu->acquired && s10a_usb_disk_busy());
	CHECK(host_in(IN, rx, 512) == 512);
	CHECK(host_in(IN, csw, 13) == 13 && csw[12] == 0);
	CHECK(!s10a_usb_disk_busy() && !lu->acquired);
	put_cbw(cbw, 0, false, tur, 6);
	host_command(cbw, rx, &moved, csw);
	CHECK(csw[12] == 1 && lu->key == S10A_KEY_NOT_READY &&
	      lu->asc == 0x3A);
	/* An own write's withdrawal ends the hold without a release. */
	s10a_usb_disk_offer();
	host_command(cbw, rx, &moved, csw);	/* the unit attention */
	put_cbw(cbw, 512, true, read10, 10);
	host_command(cbw, rx, &moved, csw);
	CHECK(csw[12] == 0 && lu->acquired);
	s10a_usb_disk_withdraw(S10A_WITHDRAW_OWN_WRITE);
	CHECK(brd.released == 1u && !lu->acquired);
}

/* Whether blocks [lba, lba + n) all hold byte v (0: never written). */
static bool blocks_hold(uint32_t lba, uint32_t n, uint8_t v)
{
	uint8_t blk[S10A_BLOCK];
	uint32_t i, k;

	for (i = 0; i < n; i++) {
		if (disk_read(NULL, lba + i, blk, 1) != 0)
			return false;
		for (k = 0; k < S10A_BLOCK; k++)
			if (blk[k] != v)
				return false;
	}
	return true;
}

/*
 * S-10a.049: a WRITE in its data phase when the disk is withdrawn stores
 * nothing more, even with the disk offered again before its data comes:
 * the next data fails it with NOT READY 3Ah/00h. With more still to send
 * the host meets a halted OUT; the blocks before the withdrawal are kept.
 */
static void test_withdraw_in_write(void)
{
	static const uint8_t tur[6] = { 0 };
	static uint8_t data[3 * S10A_BOT_BUF];
	uint8_t write10[10] = { 0x2A, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	uint8_t cbw[31], csw[13];
	uint32_t moved;

	for (int more = 0; more < 2; more++) {
		const uint32_t len = (more ? 3u : 2u) * S10A_BOT_BUF;

		new_unit("XY0A12BC", true);
		put_cbw(cbw, 0, false, tur, 6);
		host_command(cbw, rx, &moved, csw);	/* the unit attention */
		memset(data, 0x5A, sizeof(data));
		write10[8] = (uint8_t)(len / S10A_BLOCK);
		put_cbw(cbw, len, false, write10, 10);
		CHECK(host_out(OUT, cbw, 31));
		CHECK(host_out(OUT, data, S10A_BOT_BUF));
		CHECK(blocks_hold(0, 8, 0x5A));
		s10a_usb_disk_withdraw(S10A_WITHDRAW_OWN_WRITE);
		s10a_usb_disk_offer();
		CHECK(s10a_usb_disk_busy());
		CHECK(host_out(OUT, data + S10A_BOT_BUF, S10A_BOT_BUF));
		CHECK(blocks_hold(8, 8, 0x00));
		CHECK(hw.stalled[0][N(OUT)] == (more != 0));
		if (more)
			CHECK(clear_halt(OUT));
		CHECK(host_in(IN, csw, 13) == 13);
		CHECK(csw[12] == 1);
		CHECK(((uint32_t)csw[8] | (uint32_t)csw[9] << 8 |
		       (uint32_t)csw[10] << 16) == len - S10A_BOT_BUF);
		CHECK(lu->key == S10A_KEY_NOT_READY && lu->asc == 0x3A);
		CHECK(!s10a_usb_disk_busy() && hw.armed[0][N(OUT)]);
		CHECK(blocks_hold(0, 8, 0x5A) && blocks_hold(8, 16, 0x00));
		/* the offer's unit attention, then the disk as usual */
		put_cbw(cbw, 0, false, tur, 6);
		host_command(cbw, rx, &moved, csw);
		CHECK(csw[12] == 1 && lu->key == S10A_KEY_UNIT_ATTENTION);
		host_command(cbw, rx, &moved, csw);
		CHECK(csw[12] == 0);
	}
}

/* --- S-10a.060 to S-10a.062: the CDC port ----------------------------------- */

static void test_cdc(void)
{
	static const uint8_t coding[7] = { 0x80, 0x25, 0, 0, 0, 1, 7 };
	static const uint8_t note[10] = { 0xA1, 0x20, 0, 0, 2, 0, 2, 0, 3, 0 };
	static uint8_t big[1200];
	uint8_t buf[600];
	unsigned i;

	start(S10A_SET_MIDI_CDC, "XY0A12BC", true);
	CHECK(s10a_usb_cdc_room() == 0);
	CHECK(set_configuration(1));
	CHECK(s10a_usb_cdc_room() == 2048u);
	CHECK(!s10a_usb_cdc_open(NULL));
	CHECK(ctl(0x21, 0x20, 0, 2, 7, coding));
	CHECK(ctl(0xA1, 0x21, 0, 2, 7, NULL) && hw.ep0_in_len == 7 &&
	      memcmp(hw.ep0_in, coding, 7) == 0);
	CHECK(!ctl(0x21, 0x20, 0, 2, 6, coding));
	CHECK(!ctl(0xA1, 0x21, 0, 3, 7, NULL));	/* the data interface */
	CHECK(!ctl(0x21, 0x23, 0, 2, 0, NULL));	/* SEND_BREAK */

	/* DTR: open, one SERIAL_STATE, S-10c told. */
	CHECK(ctl(0x21, 0x22, 1, 2, 0, NULL));
	CHECK(s10a_usb_cdc_open(NULL));
	CHECK(hw.armed[1][1] && hw.len[1][1] == 10 &&
	      memcmp(hw.buf[1][1], note, 10) == 0);
	CHECK(brd.cdc_restarts == 0);
	s10a_usb_poll();
	CHECK(brd.cdc_restarts == 1u);
	/* Two more with a line set while the first is unread: in order. */
	CHECK(ctl(0x21, 0x22, 2, 2, 0, NULL));
	CHECK(ctl(0x21, 0x22, 0, 2, 0, NULL));
	CHECK(ctl(0x21, 0x22, 3, 2, 0, NULL));
	CHECK(host_in(0x81, buf, 16) == 10);
	CHECK(host_in(0x81, buf, 16) == 10);
	CHECK(host_in(0x81, buf, 16) == 10 && memcmp(buf, note, 10) == 0);
	CHECK(host_in(0x81, buf, 16) == 0);
	s10a_usb_poll();
	CHECK(brd.cdc_restarts == 4u);

	/* Data both ways; a transfer ending on a full packet gets a ZLP. */
	CHECK(host_out(0x0A, (const uint8_t *)"hello", 5));
	CHECK(s10a_usb_cdc_read(buf, sizeof(buf)) == 5 &&
	      memcmp(buf, "hello", 5) == 0);
	CHECK(hw.armed[0][0x0A]);
	/* S-10c.032: the restart falls where the request came in the
	 * stream, after the bytes before it. */
	CHECK(host_out(0x0A, (const uint8_t *)"abc", 3));
	CHECK(ctl(0x21, 0x22, 1, 2, 0, NULL));
	CHECK(host_out(0x0A, (const uint8_t *)"def", 3));
	CHECK(s10a_usb_cdc_read(buf, sizeof(buf)) == 3 &&
	      memcmp(buf, "abc", 3) == 0);
	CHECK(s10a_usb_cdc_read(buf, sizeof(buf)) == 0);
	CHECK(brd.cdc_restarts == 4u);
	s10a_usb_poll();
	CHECK(brd.cdc_restarts == 5u);
	CHECK(s10a_usb_cdc_read(buf, sizeof(buf)) == 3 &&
	      memcmp(buf, "def", 3) == 0);
	(void)host_in(0x81, buf, 16);
	for (i = 0; i < sizeof(big); i++)
		big[i] = (uint8_t)i;
	CHECK(s10a_usb_cdc_write(big, 1024) == 1024);
	CHECK(host_in(0x89, buf, 512) == 512 && memcmp(buf, big, 512) == 0);
	CHECK(host_in(0x89, buf, 512) == 512 &&
	      memcmp(buf, big + 512, 512) == 0);
	CHECK(hw.armed[1][9] && hw.len[1][9] == 0);
	CHECK(host_in(0x89, buf, 512) == 0 && !hw.armed[1][9]);
	CHECK(s10a_usb_cdc_pending() == 0);
	/* A full receive ring stops arming until it is read. */
	for (i = 0; i < 4; i++)
		CHECK(host_out(0x0A, big, 512));
	CHECK(!hw.armed[0][0x0A]);
	CHECK(s10a_usb_cdc_read(buf, 100) == 100 && !hw.armed[0][0x0A]);
	CHECK(s10a_usb_cdc_read(buf, 500) == 500 && hw.armed[0][0x0A]);

	/* SET_INTERFACE on the data interface: the transfer ends, a new one
	 * starts; the port stays open. */
	CHECK(s10a_usb_cdc_write(big, 10) == 10);
	CHECK(hw.armed[1][9]);
	CHECK(ctl(0x01, 11, 0, 3, 0, NULL));
	CHECK(!hw.armed[1][9] && hw.armed[0][0x0A]);
	CHECK(s10a_usb_cdc_open(NULL));

	/* Reselecting closes the port (S-10a.061) and keeps the coding. */
	CHECK(set_configuration(1));
	CHECK(!s10a_usb_cdc_open(NULL));
	CHECK(ctl(0xA1, 0x21, 0, 2, 7, NULL) &&
	      memcmp(hw.ep0_in, coding, 7) == 0);
	CHECK(ctl(0x21, 0x22, 1, 2, 0, NULL) && s10a_usb_cdc_open(NULL));
	bus_reset(true);
	CHECK(!s10a_usb_cdc_open(NULL));
	CHECK(s10a_usb_cdc_write(big, 10) == 0);

	/* At full speed a 64-byte transfer ends with a ZLP. */
	enumerate(false);
	CHECK(set_configuration(1));
	CHECK(s10a_usb_cdc_write(big, 64) == 64);
	CHECK(host_in(0x89, buf, 64) == 64);
	CHECK(hw.armed[1][9] && hw.len[1][9] == 0);
	/* Four full packets and no short one: the armed count ends it. */
	CHECK(hw.armed[0][0x0A] && hw.len[0][0x0A] == 64u);
	CHECK(host_out(0x0A, big, 256));
	CHECK(s10a_usb_cdc_read(buf, sizeof(buf)) == 256 &&
	      memcmp(buf, big, 256) == 0);
}

/* --- MIDI and S-10a.070 --------------------------------------------------- */

static uint8_t packed[512];
static size_t packed_len;

static void pack(void *ctx, const uint8_t p[4])
{
	(void)ctx;
	memcpy(packed + packed_len, p, 4);
	packed_len += 4;
}

static void test_midi(void)
{
	static const uint8_t identity[8] = { 0x04, 0xF0, 0x7E, 0x7F,
					     0x07, 0x06, 0x01, 0xF7 };
	static uint8_t msg[S10C_SYSEX_MAX_SENT];
	uint8_t buf[512], reply[S10C_IDENTITY_REPLY_LEN];
	unsigned i, sent;

	start(S10A_SET_MIDI, "", true);
	CHECK(s10a_usb_midi_room() == 0);
	CHECK(set_configuration(1));
	CHECK(hw.armed[0][8]);
	CHECK(s10a_usb_midi_room() == 1024u);
	CHECK(s10a_usb_sysex_size(0) == 0 && s10a_usb_sysex_size(1) == 4u &&
	      s10a_usb_sysex_size(3) == 4u && s10a_usb_sysex_size(4) == 8u);
	/* S-10a.070: no audio request is answered. */
	CHECK(!ctl(0xA1, 0x81, 0x0100, 0x0000, 2, NULL));
	CHECK(!ctl(0xA1, 0x81, 0x0100, 0x0301, 2, NULL));
	CHECK(!ctl(0x21, 0x01, 0x0100, 0x0D00, 2, (const uint8_t *)"\0\0"));
	CHECK(!ctl(0xA2, 0x81, 0x0100, 0x0087, 2, NULL));

	/* The identity request in, its reply out (S-10c.010). */
	CHECK(host_out(0x08, identity, sizeof(identity)));
	CHECK(!hw.armed[1][7]);		/* nothing before the poll */
	s10a_usb_poll();
	packed_len = 0;
	s10c_identity_reply(&wb_device_opxy, 0xFFFFu, reply);
	s10c_usbmidi_send(reply, S10C_IDENTITY_REPLY_LEN, pack, NULL);
	CHECK(host_in(0x87, buf, sizeof(buf)) == packed_len &&
	      memcmp(buf, packed, packed_len) == 0);
	CHECK(hw.armed[0][8]);

	/* A host that reads nothing: whole messages are dropped, never part
	 * of one. */
	for (i = 0; i < sizeof(msg); i++)
		msg[i] = (uint8_t)(i & 0x7Fu);
	msg[0] = 0xF0;
	msg[sizeof(msg) - 1] = 0xF7;
	for (sent = 0; sent < 8; sent++)
		s10a_usb_send_sysex(NULL, msg, sizeof(msg));
	/* Dropped exactly when the room said it would not fit. */
	CHECK(s10a_usb_midi_room() < s10a_usb_sysex_size(sizeof(msg)));
	packed_len = 0;
	s10c_usbmidi_send(msg, sizeof(msg), pack, NULL);
	{
		size_t got = 0, n;

		while ((n = host_in(0x87, buf, sizeof(buf))) != 0) {
			CHECK(memcmp(buf, packed + got % packed_len,
				     n < packed_len - got % packed_len ?
				     n : packed_len - got % packed_len) == 0);
			got += n;
		}
		CHECK(got % packed_len == 0 && got / packed_len >= 2u);
	}
	/* Nothing queued while not configured. */
	CHECK(set_configuration(0));
	CHECK(s10a_usb_midi_room() == 0);
	s10a_usb_send_sysex(NULL, msg, sizeof(msg));
	CHECK(set_configuration(1));
	CHECK(!hw.armed[1][7]);

	/* At full speed: eight requests in one 64-byte transfer, each
	 * answered. */
	enumerate(false);
	CHECK(set_configuration(1));
	CHECK(hw.armed[0][8] && hw.len[0][8] == 64u);
	for (i = 0; i < 8; i++)
		memcpy(buf + i * sizeof(identity), identity, sizeof(identity));
	CHECK(host_out(0x08, buf, 64));
	s10a_usb_poll();
	packed_len = 0;
	s10c_usbmidi_send(reply, S10C_IDENTITY_REPLY_LEN, pack, NULL);
	{
		size_t got = 0, n;

		while ((n = host_in(0x87, buf + got,
					  (uint32_t)(sizeof(buf) - got))) != 0)
			got += n;
		CHECK(got == 8u * packed_len);
		for (i = 0; i < 8; i++)
			CHECK(memcmp(buf + i * packed_len, packed,
				     packed_len) == 0);
	}
}

/* --- S-10b on the MTP sets ----------------------------------------------- */

/* The MTP function's bulk pair and interface: 0x89, 0x0A and 0 alone,
 * 0x87, 0x08 and 2 beside the CDC port (S-10a.013). */
static uint8_t MTP_IN = 0x89u, MTP_OUT = 0x0Au;
static uint16_t mtp_itf;
#define MTP_EVENT	0x83u

/* S-10b.003's sizes: ZLPs at 500 and 1012 bytes at both packet sizes, at 52
 * and 116 at 64 only, at 1024 at neither. */
static const char *const mtp_files[] = { "a52", "b116", "c500", "d1012",
					 "e1024" };
static const uint32_t mtp_sizes[] = { 52, 116, 500, 1012, 1024 };

static uint8_t mtp_byte(size_t f, uint32_t off)
{
	return (uint8_t)(off * 7u + f * 31u + 1u);
}

static bool mtp_space(void *ctx, uint64_t *total, uint64_t *free_bytes)
{
	(void)ctx;
	*total = 1u << 20;
	*free_bytes = 1u << 19;
	return true;
}

static bool mtp_list(void *ctx, const char *path, s10b_entry_fn fn, void *arg)
{
	size_t i;

	(void)ctx;
	if (strcmp(path, "/") != 0)
		return false;
	for (i = 0; i < ARRAY_LEN(mtp_files); i++) {
		const struct s10b_entry e = {
			.name = (const uint8_t *)mtp_files[i],
			.name_len = strlen(mtp_files[i]),
			.kind = S10B_KIND_FILE, .size = mtp_sizes[i],
			.mtime = 0x60000000u, .ctime = 0x60000000u,
		};

		fn(arg, &e);
	}
	return true;
}

/*
 * A bus event served from inside a request of the responder, as the board's
 * waits on the bridge serve the bus: fired once by the next read of a file,
 * the one GetObject makes as it executes (n 0) or one of its data's (n > 0).
 * nested_intact says whether the responder's state was left as it was.
 */
static void (*nested)(void);
static bool nested_on_data, nested_intact;
static struct s10b mtp;

static void mtp_nested_fire(size_t n)
{
	static struct s10b before;
	void (*f)(void) = nested;

	if (f == NULL || (n != 0) != nested_on_data)
		return;
	nested = NULL;
	memcpy(&before, &mtp, sizeof(before));
	f();
	nested_intact = memcmp(&before, &mtp, sizeof(before)) == 0;
}

static long mtp_read(void *ctx, const char *path, uint32_t off, uint8_t *buf,
		     size_t n)
{
	size_t i, k;

	(void)ctx;
	mtp_nested_fire(n);
	for (i = 0; i < ARRAY_LEN(mtp_files); i++) {
		if (path[0] != '/' || strcmp(path + 1, mtp_files[i]) != 0)
			continue;
		if (off >= mtp_sizes[i])
			return 0;
		if (n > mtp_sizes[i] - off)
			n = mtp_sizes[i] - off;
		for (k = 0; k < n; k++)
			buf[k] = mtp_byte(i, off + (uint32_t)k);
		return (long)n;
	}
	return -1;
}

static unsigned mtp_battery(void *ctx)
{
	(void)ctx;
	return 50;
}

static uint32_t mtp_clock_read(void *ctx)
{
	(void)ctx;
	return 0;
}

static void mtp_clock_write(void *ctx, uint32_t count)
{
	(void)ctx;
	(void)count;
}

static struct s10b_object mtp_objects[16];
static uint8_t mtp_pool[256];

static void mtp_engine(void)
{
	static const struct s10b_volume_ops vol = {
		.space = mtp_space, .list = mtp_list, .read = mtp_read,
	};
	static const struct s10b_device_ops device = {
		.battery = mtp_battery, .clock_read = mtp_clock_read,
		.clock_write = mtp_clock_write,
	};
	const struct s10b_config cfg = {
		.vol = &vol, .dev = &device, .manufacturer = "m", .model = "p",
		.version = "1.0", .serial = "s", .objects = mtp_objects,
		.max_objects = ARRAY_LEN(mtp_objects), .names = mtp_pool,
		.names_size = sizeof(mtp_pool),
	};

	s10b_init(&mtp, &cfg);
	s10a_usb_mtp(&mtp);
}

static void le(uint8_t *p, uint32_t v, unsigned n)
{
	while (n--) {
		*p++ = (uint8_t)v;
		v >>= 8;
	}
}

static uint32_t rd(const uint8_t *p, unsigned n)
{
	uint32_t v = 0;

	while (n--)
		v = v << 8 | p[n];
	return v;
}

static void container(uint8_t *c, uint32_t len, unsigned type, unsigned code,
		      uint32_t tid)
{
	le(c, len, 4);
	le(c + 4, type, 2);
	le(c + 6, code, 2);
	le(c + 8, tid, 4);
}

static uint32_t mtp_tid;
static unsigned mtp_op;
static uint8_t mtp_data[2048];
static struct {
	unsigned code, transfers;
	bool data, zlp;
	uint32_t len;
} mtp_got;

/* The host sends a command; false when the device does not take it. */
static bool mtp_send(unsigned op, const uint32_t *p, unsigned np)
{
	uint8_t c[32] = { 0 };
	unsigned k;

	mtp_op = op;
	mtp_tid++;
	container(c, 12u + 4u * np, 1, op, mtp_tid);
	for (k = 0; k < np; k++)
		le(c + 12 + 4 * k, p[k], 4);
	return host_out(MTP_OUT, c, 12u + 4u * np);
}

/* The next IN transfer, polling for it; false when none comes. */
static bool mtp_in(uint8_t *dst, uint32_t *n)
{
	unsigned tries;

	for (tries = 0; tries < 4; tries++) {
		if (hw.armed[1][N(MTP_IN)]) {
			*n = host_in(MTP_IN, dst, 512);
			return true;
		}
		s10a_usb_poll();
	}
	return false;
}

/* The device's answer: a data phase, maybe, then the response, each
 * transfer one packet at most (S-10b.003). */
static bool mtp_answer(uint32_t mps)
{
	uint8_t pkt[512] = { 0 };
	uint32_t n;

	memset(&mtp_got, 0, sizeof(mtp_got));
	if (!mtp_in(pkt, &n))
		return false;
	mtp_got.transfers++;
	if (n >= 12 && rd(pkt + 4, 2) == 2) {
		const uint32_t total = rd(pkt, 4);

		CHECK(rd(pkt + 6, 2) == mtp_op && rd(pkt + 8, 4) == mtp_tid);
		mtp_got.data = true;
		for (;;) {
			CHECK(n <= mps);
			if (mtp_got.len + n <= sizeof(mtp_data))
				memcpy(mtp_data + mtp_got.len, pkt, n);
			mtp_got.len += n;
			if (n < mps || mtp_got.len >= total)
				break;
			if (!mtp_in(pkt, &n))
				return false;
			mtp_got.transfers++;
		}
		if (n == mps) {
			if (!mtp_in(pkt, &n) || n != 0)
				return false;
			mtp_got.zlp = true;
			mtp_got.transfers++;
		}
		if (!mtp_in(pkt, &n))
			return false;
		mtp_got.transfers++;
	}
	CHECK(n >= 12 && rd(pkt + 4, 2) == 3 && rd(pkt + 8, 4) == mtp_tid);
	mtp_got.code = rd(pkt + 6, 2);
	return n >= 12;
}

static unsigned mtp_txn(unsigned op, const uint32_t *p, unsigned np,
			uint32_t mps)
{
	if (!mtp_send(op, p, np) || !mtp_answer(mps))
		return 0;
	return mtp_got.code;
}

/* Whether the dataset holds the text as UTF-16LE. */
static bool holds_text(const char *t)
{
	const size_t n = strlen(t);
	size_t i, k;

	for (i = 0; i + 2 * n <= mtp_got.len && i + 2 * n <= sizeof(mtp_data);
	     i++) {
		for (k = 0; k < n; k++)
			if (mtp_data[i + 2 * k] != (uint8_t)t[k] ||
			    mtp_data[i + 2 * k + 1] != 0)
				break;
		if (k == n)
			return true;
	}
	return false;
}

static unsigned device_status(void)
{
	if (!ctl(0xA1, 0x67, 0, mtp_itf, 4, NULL) || hw.ep0_in_len != 4 ||
	    rd(hw.ep0_in, 2) != 4)
		return 0;
	return rd(hw.ep0_in + 2, 2);
}

/* A GetObject of the 1024-byte file, its first packet taken. */
static void get_object_started(uint32_t mps)
{
	const uint32_t h = s10b_handle(4);
	uint8_t pkt[512] = { 0 };
	uint32_t n = 0;

	CHECK(mtp_send(0x1009, &h, 1));
	CHECK(mtp_in(pkt, &n) && n == mps);
	CHECK(device_status() == S10B_DEVICE_BUSY);
}

static void mtp_session(enum s10a_set set, bool high)
{
	const bool cdc = set == S10A_SET_MTP_CDC ||
			 set == S10A_SET_MTP_VENDOR_CDC;

	MTP_IN = cdc ? 0x87u : 0x89u;
	MTP_OUT = cdc ? 0x08u : 0x0Au;
	mtp_itf = cdc ? 2u : 0u;
	const uint32_t mps = high ? 512u : 64u;
	const uint32_t open[1] = { 1 };
	uint8_t c[512] = { 0 };
	uint32_t h;
	size_t i, k;

	start(set, "XY0A12BC", high);
	CHECK(set_configuration(1));
	CHECK(hw.open[1][N(MTP_EVENT)] && hw.open[1][N(MTP_IN)] &&
	      hw.open[0][N(MTP_OUT)]);
	CHECK(hw.armed[0][N(MTP_OUT)] && hw.len[0][N(MTP_OUT)] == mps);
	CHECK(!hw.armed[1][N(MTP_IN)]);

	/* DeviceInfo outside a session, with the set's strings (S-10b.020). */
	CHECK(mtp_txn(0x1001, NULL, 0, mps) == S10B_OK && mtp_got.data);
	CHECK(holds_text("teenage engineering") && holds_text("OP-XY") &&
	      holds_text("XY0A12BC"));
	h = s10b_handle(0);
	CHECK(mtp_txn(0x1008, &h, 1, mps) == S10B_SESSION_NOT_OPEN);
	CHECK(mtp_txn(0x1002, open, 1, mps) == S10B_OK && !mtp_got.data);

	/* S-10b.003: packets of the endpoint's size, a ZLP after a full
	 * last one, the response a transfer of its own. */
	for (i = 0; i < ARRAY_LEN(mtp_files); i++) {
		h = s10b_handle(i);
		CHECK(mtp_txn(0x1009, &h, 1, mps) == S10B_OK);
		CHECK(mtp_got.len == 12u + mtp_sizes[i]);
		CHECK(mtp_got.zlp == ((12u + mtp_sizes[i]) % mps == 0));
		for (k = 0; k < mtp_sizes[i]; k++)
			if (mtp_data[12 + k] != mtp_byte(i, (uint32_t)k))
				break;
		CHECK(k == mtp_sizes[i]);
	}

	/* S-10b.004: a container out of place gets nothing. */
	container(c, 12, 2, 0x1001, 99);
	CHECK(host_out(MTP_OUT, c, 12));
	s10a_usb_poll();
	s10a_usb_poll();
	CHECK(!hw.armed[1][N(MTP_IN)] && hw.armed[0][N(MTP_OUT)]);
	CHECK(mtp_txn(0x1001, NULL, 0, mps) == S10B_OK && mtp_got.data &&
	      mtp_got.transfers == (mtp_got.len + mps - 1u) / mps +
			       (mtp_got.zlp ? 1u : 0u) + 1u);

	/* S-10b.006: a host data phase ending on a full packet, then its ZLP,
	 * before the response; the ZLP left over is ignored. */
	{
		const uint32_t p[2] = { S10B_STORAGE_ID, 0 };

		CHECK(mtp_send(0x100C, p, 2));
		s10a_usb_poll();
		CHECK(!hw.armed[1][N(MTP_IN)]);
		container(c, mps, 2, 0x100C, mtp_tid);
		memset(c + 12, 0, mps - 12u);
		CHECK(host_out(MTP_OUT, c, mps));
		CHECK(mtp_answer(mps) && !mtp_got.data && mtp_got.code != S10B_OK);
		CHECK(host_out(MTP_OUT, c, 0));
		s10a_usb_poll();
		CHECK(!hw.armed[1][N(MTP_IN)] && hw.armed[0][N(MTP_OUT)]);
	}

	/* S-10b.005: Cancel after the first packet: no more data, no
	 * response; the next command is answered. */
	CHECK(device_status() == S10B_OK);
	get_object_started(mps);
	{
		uint8_t cancel[6] = { 0x01, 0x40 };

		le(cancel + 2, mtp_tid, 4);
		/* S-10b.005: another length, another code or the other
		 * direction: STALL, and the transaction goes on. */
		CHECK(!ctl(0x21, 0x64, 0, mtp_itf, 4, cancel));
		cancel[0] = 0x02;
		CHECK(!ctl(0x21, 0x64, 0, mtp_itf, 6, cancel));
		cancel[0] = 0x01;
		CHECK(!ctl(0xA1, 0x64, 0, mtp_itf, 6, NULL));
		CHECK(!ctl(0xA1, 0x66, 0, mtp_itf, 0, NULL));
		CHECK(device_status() == S10B_DEVICE_BUSY);
		/* whatever the TransactionID holds */
		le(cancel + 2, mtp_tid + 7u, 4);
		CHECK(ctl(0x21, 0x64, 0, mtp_itf, 6, cancel));
	}
	s10a_usb_poll();
	s10a_usb_poll();
	CHECK(!hw.armed[1][N(MTP_IN)]);
	CHECK(device_status() == S10B_OK);
	CHECK(mtp_txn(0x1001, NULL, 0, mps) == S10B_OK && mtp_got.data);

	/* Device Reset: the same; the session stays (S-10b.005). */
	get_object_started(mps);
	CHECK(ctl(0x21, 0x66, 0, mtp_itf, 0, NULL));
	s10a_usb_poll();
	CHECK(!hw.armed[1][N(MTP_IN)] && device_status() == S10B_OK);
	h = s10b_handle(0);
	CHECK(mtp_txn(0x1008, &h, 1, mps) == S10B_OK);

	/* With no transaction in progress, both are accepted and change
	 * nothing (S-10b.005). */
	{
		const uint8_t cancel[6] = { 0x01, 0x40 };

		CHECK(ctl(0x21, 0x64, 0, mtp_itf, 6, cancel));
		CHECK(ctl(0x21, 0x66, 0, mtp_itf, 0, NULL));
		CHECK(device_status() == S10B_OK);
		CHECK(mtp_txn(0x1008, &h, 1, mps) == S10B_OK);
	}

	/* S-10b.009: Get Extended Event Data is 8 zero bytes; Get Device
	 * Status is 4 bytes whatever wLength asks, at most wLength. */
	memset(hw.ep0_in, 0xEE, 8);
	CHECK(ctl(0xA1, 0x65, 0, mtp_itf, 8, NULL) && hw.ep0_in_len == 8);
	{
		static const uint8_t zero[8];

		CHECK(memcmp(hw.ep0_in, zero, 8) == 0);
	}
	CHECK(ctl(0xA1, 0x67, 0, mtp_itf, 64, NULL) && hw.ep0_in_len == 4 &&
	      rd(hw.ep0_in + 2, 2) == S10B_OK);
	CHECK(ctl(0xA1, 0x67, 0, mtp_itf, 2, NULL) && hw.ep0_in_len == 2);
	/* The wrong way, and a request the class does not define: STALL. */
	CHECK(!ctl(0x21, 0x65, 0, mtp_itf, 8, c));
	CHECK(!ctl(0x21, 0x67, 0, mtp_itf, 4, c));
	CHECK(!ctl(0xA1, 0x63, 0, mtp_itf, 4, NULL));

	/* S-10b.007: another alternate setting and a cleared halt of the
	 * interrupt endpoint change nothing of a transaction. */
	get_object_started(mps);
	CHECK(!ctl(0x01, 11, 1, mtp_itf, 0, NULL));
	CHECK(clear_halt(MTP_EVENT));
	CHECK(device_status() == S10B_DEVICE_BUSY);
	CHECK(ctl(0x21, 0x66, 0, mtp_itf, 0, NULL));
	s10a_usb_poll();
	CHECK(!hw.armed[1][N(MTP_IN)] && device_status() == S10B_OK);

	/* S-10b.007: SET_INTERFACE(0) ends the transaction, not the session. */
	get_object_started(mps);
	CHECK(ctl(0x01, 11, 0, mtp_itf, 0, NULL));
	s10a_usb_poll();
	CHECK(!hw.armed[1][N(MTP_IN)] && hw.armed[0][N(MTP_OUT)]);
	CHECK(mtp_txn(0x1008, &h, 1, mps) == S10B_OK);

	CHECK(device_status() == S10B_OK);

	/* So does a cleared halt of either bulk endpoint, halted or not. */
	get_object_started(mps);
	CHECK(clear_halt(MTP_IN));
	s10a_usb_poll();
	CHECK(!hw.armed[1][N(MTP_IN)] && hw.armed[0][N(MTP_OUT)]);
	CHECK(device_status() == S10B_OK);
	CHECK(mtp_txn(0x1008, &h, 1, mps) == S10B_OK);
	get_object_started(mps);
	CHECK(clear_halt(MTP_OUT));
	s10a_usb_poll();
	CHECK(!hw.armed[1][N(MTP_IN)] && hw.armed[0][N(MTP_OUT)]);
	CHECK(device_status() == S10B_OK);
	CHECK(mtp_txn(0x1008, &h, 1, mps) == S10B_OK);

	/* S-10b.007: a bus reset closes the session. */
	enumerate(high);
	CHECK(set_configuration(1));
	CHECK(mtp_txn(0x1008, &h, 1, mps) == S10B_SESSION_NOT_OPEN);
	CHECK(mtp_txn(0x1002, open, 1, mps) == S10B_OK);
	CHECK(mtp_txn(0x1008, &h, 1, mps) == S10B_OK);
	/* And a change of configuration. */
	CHECK(set_configuration(1));
	CHECK(mtp_txn(0x1008, &h, 1, mps) == S10B_SESSION_NOT_OPEN);

	/* S-10b.008: nothing on the interrupt endpoint, ever. */
	CHECK(!hw.armed[1][N(MTP_EVENT)]);
}

/* --- Bus events from inside a request (S-10b.005, S-10b.007) ------------- */

static bool nested_high;

static void ev_cancel(void)
{
	uint8_t c[6] = { 0x01, 0x40 };

	le(c + 2, mtp_tid, 4);
	CHECK(device_status() == S10B_DEVICE_BUSY);	/* the request runs */
	CHECK(ctl(0x21, 0x64, 0, mtp_itf, 6, c));
	CHECK(device_status() == S10B_OK);	/* S-10b.009: once accepted */
}

static void ev_device_reset(void)
{
	CHECK(ctl(0x21, 0x66, 0, mtp_itf, 0, NULL));
	CHECK(device_status() == S10B_OK);
}

static void ev_set_interface(void)
{
	CHECK(ctl(0x01, 11, 0, mtp_itf, 0, NULL));
}

static void ev_bus_reset(void)
{
	enumerate(nested_high);
	CHECK(set_configuration(1));
	CHECK(!hw.armed[0][N(MTP_OUT)]);	/* no command until it returns */
}

/*
 * A GetObject during which the event comes: the stack answers it at once,
 * the responder's state is untouched until the request returns, and then
 * the transaction ends with no data and no response (a bus reset closes the
 * session too); the next command is answered.
 */
static void mtp_nested_case(void (*ev)(void), bool on_data, bool closes)
{
	const uint32_t h = s10b_handle(4), h0 = s10b_handle(0), open[1] = { 1 };
	const uint32_t mps = nested_high ? 512u : 64u;
	const unsigned before = check_failures;

	nested = ev;
	nested_on_data = on_data;
	nested_intact = false;
	CHECK(mtp_send(0x1009, &h, 1));
	CHECK(!hw.armed[0][N(MTP_OUT)]);
	s10a_usb_poll();
	CHECK(nested == NULL && nested_intact);
	s10a_usb_poll();
	CHECK(!hw.armed[1][N(MTP_IN)] && hw.armed[0][N(MTP_OUT)]);
	CHECK(device_status() == S10B_OK);
	if (closes) {
		CHECK(mtp_txn(0x1008, &h0, 1, mps) == S10B_SESSION_NOT_OPEN);
		CHECK(mtp_txn(0x1002, open, 1, mps) == S10B_OK);
	}
	CHECK(mtp_txn(0x1008, &h0, 1, mps) == S10B_OK);
	CHECK(mtp_txn(0x1009, &h, 1, mps) == S10B_OK &&
	      mtp_got.len == 12u + mtp_sizes[4]);
	if (check_failures != before)
		printf("  nested event %s, %s\n", on_data ? "in data" : "in execute",
		       closes ? "closing" : "ending");
}

static void test_mtp_nested(void)
{
	const uint32_t open[1] = { 1 };

	for (int high = 1; high >= 0; high--) {
		nested_high = high != 0;
		MTP_IN = 0x89u;
		MTP_OUT = 0x0Au;
		mtp_itf = 0;
		mtp_engine();
		start(S10A_SET_MTP, "XY0A12BC", nested_high);
		CHECK(set_configuration(1));
		CHECK(mtp_txn(0x1002, open, 1, nested_high ? 512u : 64u) ==
		      S10B_OK);
		/* S-10b.009: a command received and not yet run (the loop's
		 * pass not come round, as through a long operation) is in
		 * progress: Device_Busy until it is answered */
		{
			const uint32_t h0 = s10b_handle(0);

			CHECK(mtp_send(0x1008, &h0, 1));
			CHECK(device_status() == S10B_DEVICE_BUSY);
			CHECK(device_status() == S10B_DEVICE_BUSY);
			CHECK(mtp_answer(nested_high ? 512u : 64u) &&
			      mtp_got.code == S10B_OK);
			CHECK(device_status() == S10B_OK);
		}
		mtp_nested_case(ev_cancel, false, false);
		mtp_nested_case(ev_cancel, true, false);
		mtp_nested_case(ev_device_reset, false, false);
		mtp_nested_case(ev_set_interface, false, false);
		mtp_nested_case(ev_set_interface, true, false);
		mtp_nested_case(ev_bus_reset, false, true);
		mtp_nested_case(ev_bus_reset, true, true);
	}
	nested = NULL;
	s10a_usb_mtp(NULL);
}

static void test_mtp(void)
{
	/* No responder: the endpoints stay idle, the requests STALL. */
	s10a_usb_mtp(NULL);
	start(S10A_SET_MTP, "", true);
	CHECK(set_configuration(1));
	CHECK(!hw.armed[0][N(MTP_OUT)] && !hw.armed[1][N(MTP_IN)]);
	CHECK(!ctl(0xA1, 0x67, 0, mtp_itf, 4, NULL));
	CHECK(!ctl(0x21, 0x66, 0, mtp_itf, 0, NULL));

	mtp_engine();
	mtp_session(S10A_SET_MTP, true);
	mtp_session(S10A_SET_MTP, false);
	mtp_session(S10A_SET_MTP_VENDOR, true);
	mtp_session(S10A_SET_MTP_VENDOR, false);
	mtp_session(S10A_SET_MTP_CDC, true);
	mtp_session(S10A_SET_MTP_VENDOR_CDC, false);
	s10a_usb_mtp(NULL);
}

int main(void)
{
	RUN(test_descriptors);
	RUN(test_configuration_events);
	RUN(test_medium_vectors);
	RUN(test_bot_cases);
	RUN(test_reset_recovery);
	RUN(test_withdraw_in_command);
	RUN(test_withdraw_in_write);
	RUN(test_mtp_nested);
	RUN(test_cdc);
	RUN(test_midi);
	RUN(test_mtp);
	return check_summary("test_s10a_usb");
}
