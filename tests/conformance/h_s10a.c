/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for S-10a's USB device logic: the descriptor sets
 * (src/s10a_desc.c), the control endpoint's answers (s10a_ch9.c), the
 * logical unit (s10a_scsi.c) and the Bulk-Only Transport (s10a_bot.c).
 * s10a_usb.c, the TinyUSB glue, is not built: this program plays its part
 * and the stack's (below), on a sparse disk of 454656 blocks.
 *
 * SET is a set number (enum s10a_set), SERIAL the serial text as hex ("-":
 * none), DATA hex, "-" or pat:SEED (SEED + 7 * i per byte, the length the
 * command needs). Data longer than 4096 bytes is printed as "crc LEN CRC"
 * (h_crc32), three fields.
 *
 * Descriptors (one result line each):
 *   device SET HASSERIAL                -> s10a_desc_device
 *   qualifier SET                       -> s10a_desc_qualifier
 *   config SET HASSERIAL INDEX HIGH OTHER -> s10a_desc_config ("-": 0)
 *   string SET SERIAL INDEX             -> s10a_desc_string ("-": 0)
 *   shape SET                           -> CONFIGS INTERFACES FN:FIRST... P1 P2 P3 P4:
 *                                          bNumConfigurations, bNumInterfaces, each interface's
 *                                          function and first interface, bMaxPower of 1..4
 *   hasep SET ADDR                      -> 1|0 (s10a_desc_has_endpoint)
 *   serial HEX                          -> s10a_serial_from_meta's text, hex ("-": "")
 *   state STATE FLAGS                   -> the set of s10a_set_for_state, or "-"; FLAGS bit 0 the
 *                                          serial flag, bit 1 developer mode
 * The control endpoint, with the stack's part played here:
 *   ch9 SET SERIAL HIGH                 -> ok: s10a_ch9_start; the unit and the BOT idle
 *   setup HEX                           -> stall | accept | reply HEX | reselect |
 *                                          forward WHAT: an 8-byte SETUP through s10a_ch9_setup;
 *                                          for forward, what the stack does with it: the
 *                                          descriptor (cut to wLength) or "stall" for a
 *                                          GET_DESCRIPTOR, the halt bit pair for GET_STATUS of an
 *                                          endpoint, s10a_bot_class_request's answer (HEX, "-" or
 *                                          "stall") for a class request to the mass-storage
 *                                          interface, "other" for any other class request, "-"
 *                                          otherwise
 *   busreset                            -> ok: a bus reset (s10a_ch9_reset, the BOT stopped,
 *                                          s10a_lu_bus_reset, the endpoints closed)
 * The logical unit:
 *   lu SERIAL OFFERED                   -> ok: a new unit on a blank disk (s10a_lu_init, then
 *                                          s10a_lu_offer when OFFERED), the BOT initialized
 *   event NAME                          -> ok: withdraw (for a scan or a host-link transfer),
 *                                          withdraw_own_write (for a write of the bootloader's
 *                                          own), offer, bus_reset, set_configuration_same or
 *                                          set_configuration_0 on the unit alone
 *   lustate                             -> OFFERED STOPPED UA ACQUIRED RELEASES KEY ASC ASCQ INFO
 *   diskfail 0|1                        -> ok: the block device fails every access
 *   blocks LBA N                        -> DATA: the disk's blocks
 *   cmd CDB DATA                        -> STATUS DIR LEN DATA: s10a_scsi_command on CDB
 *                                          (its length is bCBWCBLength), a READ or WRITE moved
 *                                          whole and settled by s10a_scsi_done; DATA is the
 *                                          data-in, or "-" (a WRITE given fewer bytes than LEN
 *                                          writes nothing and is not settled)
 * The Bulk-Only Transport, the host on the two bulk endpoints:
 *   bot                                 -> ok: s10a_ch9_start(disk set) and SET_CONFIGURATION(1)
 *                                          through "setup", so the BOT starts on the unit
 *   host CBW DATA                       -> MOVED DATA HALTS CSW: one command as BOT 1.0 has the
 *                                          host run it (CBW sent, data phase to
 *                                          dCBWDataTransferLength or a halt, halts cleared with
 *                                          CLEAR_FEATURE through "setup", CSW read); HALTS is
 *                                          -, in, out or inout; CSW "-" when none came
 *   send HEX                            -> 1|0: bytes sent on bulk OUT (0: not armed or halted)
 *   take N                              -> DATA: what bulk IN offers, up to N bytes ("halt")
 *   eps                                 -> IN_HALT OUT_HALT IN_ARMED OUT_ARMED OWED BUSY STATE
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hlib.h"
#include "waffle/device.h"
#include "waffle/s10a_ch9.h"
#include "waffle/s10a_desc.h"
#include "waffle/s10a_msc.h"

#define BLOCKS		454656u		/* S-12.001 */
#define BIG		(257u * S10A_BLOCK)
#define SHOW_MAX	4096u

/* --- A sparse disk: every block reads 0 until written --------------------- */

#define SLOTS 1024u
static struct {
	uint32_t lba[SLOTS];
	uint8_t data[SLOTS][S10A_BLOCK];
	unsigned used;
	bool fail;
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
	if (disk.fail || lba > BLOCKS || count > BLOCKS - lba)
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
	if (disk.fail || lba > BLOCKS || count > BLOCKS - lba)
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

/* --- The device: the parts s10a_usb.c holds ------------------------------ */

static struct s10a_lu lu;
static struct s10a_bot bot;
static struct s10a_ch9 ch9;
static char serial[S10A_SERIAL_LEN + 1];
static bool high;

/* The stack's endpoints: a halt per address, the two bulk transfers armed. */
static struct {
	bool halted[2][16];
	const uint8_t *in;
	uint32_t in_n;
	uint8_t *out;
	uint32_t out_n;
	bool in_armed, out_armed;
} ep;

static bool *halt_of(uint8_t addr)
{
	return &ep.halted[addr >> 7][addr & 15u];
}

static bool xfer_in(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	if (*halt_of(S10A_EP_BULK_IN) || ep.in_armed)
		return false;
	ep.in = buf;
	ep.in_n = n;
	ep.in_armed = true;
	return true;
}

static bool xfer_out(void *ctx, uint8_t *buf, uint32_t n)
{
	(void)ctx;
	if (*halt_of(S10A_EP_BULK_OUT) || ep.out_armed)
		return false;
	ep.out = buf;
	ep.out_n = n;
	ep.out_armed = true;
	return true;
}

static void stall_in(void *ctx)
{
	(void)ctx;
	*halt_of(S10A_EP_BULK_IN) = true;
	ep.in_armed = false;
}

static void stall_out(void *ctx)
{
	(void)ctx;
	*halt_of(S10A_EP_BULK_OUT) = true;
	ep.out_armed = false;
}

static const struct s10a_bot_port port = {
	.xfer_in = xfer_in, .xfer_out = xfer_out,
	.stall_in = stall_in, .stall_out = stall_out,
};

/* As s10a_usb.c: the control endpoint learns the BOT's reset wait. */
static void sync_reset_owed(void)
{
	ch9.reset_owed = s10a_bot_reset_owed(&bot);
}

static void close_endpoints(void)
{
	memset(&ep, 0, sizeof(ep));
}

static bool disk_set(void)
{
	return ch9.set == S10A_SET_DISK || ch9.set == S10A_SET_DISK_CDC;
}

static bool msc_endpoint(uint8_t addr)
{
	return disk_set() &&
	       (addr == S10A_EP_BULK_IN || addr == S10A_EP_BULK_OUT);
}

/* SET_CONFIGURATION as the stack carries it to s10a_usb.c. */
static void configure(unsigned value)
{
	if (value == 0) {
		s10a_bot_stop(&bot);
		sync_reset_owed();
		s10a_lu_deconfigure(&lu);
		close_endpoints();
		return;
	}
	close_endpoints();
	s10a_lu_select_configuration(&lu);
	if (disk_set()) {
		s10a_bot_start(&bot);
		sync_reset_owed();
	}
}

/*
 * CLEAR_FEATURE(ENDPOINT_HALT) forwarded to the stack. As usbd_edpt_clear_stall()
 * and the BF70x dcd_edpt_clear_stall() do, the endpoint's transfer is dropped,
 * halted or not; then s10a_usb.c's halt_cleared() runs.
 */
static void clear_halt(uint8_t addr)
{
	*halt_of(addr) = false;
	if (addr == S10A_EP_BULK_IN)
		ep.in_armed = false;
	else if (addr == S10A_EP_BULK_OUT)
		ep.out_armed = false;
	if (msc_endpoint(addr)) {
		s10a_bot_clear_halt(&bot, addr == S10A_EP_BULK_IN);
		sync_reset_owed();
	}
}

static void set_halt(uint8_t addr)
{
	if (addr == S10A_EP_BULK_IN && msc_endpoint(addr))
		stall_in(NULL);
	else if (addr == S10A_EP_BULK_OUT && msc_endpoint(addr))
		stall_out(NULL);
	else
		*halt_of(addr) = true;
}

/* What the stack does with a request s10a_ch9_setup() forwards. */
static void forward(const uint8_t s[8])
{
	const unsigned type = (unsigned)(s[0] >> 5) & 3u;
	const unsigned rcpt = s[0] & 0x1Fu;
	const unsigned value = (unsigned)s[2] | (unsigned)s[3] << 8;
	const unsigned length = (unsigned)s[6] | (unsigned)s[7] << 8;
	const uint8_t addr = s[4];
	uint8_t d[S10A_CONFIG_MAX];
	size_t n = 0;

	h_put("forward");
	if (type == 1) {
		enum s10a_function fn;
		unsigned first;
		uint8_t reply = 0;

		if (!s10a_desc_interface(ch9.set, s[4], &fn, &first) ||
		    fn != S10A_FN_MSC) {
			h_put("other");
		} else if (!s10a_bot_class_request(&bot, s[0], s[1],
						   (uint16_t)value,
						   (uint16_t)length, &reply)) {
			h_put("stall");
		} else if (s[1] == 0xFEu) {
			h_puthex(&reply, 1);
		} else {
			h_put("-");
		}
		sync_reset_owed();
		return;
	}
	switch (s[1]) {
	case 0:		/* GET_STATUS of an endpoint */
		d[0] = (uint8_t)(*halt_of(addr) ? 1u : 0u);
		d[1] = 0;
		h_puthex(d, 2);
		return;
	case 1:		/* CLEAR_FEATURE(ENDPOINT_HALT) */
		clear_halt(addr);
		break;
	case 3:		/* SET_FEATURE(ENDPOINT_HALT) */
		set_halt(addr);
		break;
	case 6:		/* GET_DESCRIPTOR: the s10a_usb.c callbacks */
		switch (value >> 8) {
		case 1:
			s10a_desc_device(&wb_device_opxy, ch9.set, serial[0] != '\0', d);
			n = 18;
			break;
		case 2:
		case 7:
			n = s10a_desc_config(ch9.set, serial[0] != '\0',
					     value & 0xFFu, high,
					     (value >> 8) == 7, d);
			break;
		case 3:
			n = s10a_desc_string(&wb_device_opxy, ch9.set, serial, value & 0xFFu, d);
			break;
		case 6:
			s10a_desc_qualifier(ch9.set, d);
			n = 10;
			break;
		default:
			break;
		}
		if (n == 0)
			h_put("stall");
		else
			h_puthex(d, n < length ? n : length);
		return;
	case 9:		/* SET_CONFIGURATION */
		if (rcpt == 0)
			configure(value);
		break;
	default:	/* SET_ADDRESS, SET_INTERFACE */
		break;
	}
	h_put("-");
}

static void setup(const uint8_t s[8])
{
	uint8_t reply[2];
	uint16_t len;

	switch (s10a_ch9_setup(&ch9, s, reply, &len)) {
	case S10A_CH9_STALL:
		h_put("stall");
		break;
	case S10A_CH9_ACCEPT:
		h_put("accept");
		break;
	case S10A_CH9_REPLY:
		h_put("reply");
		h_puthex(reply, len);
		break;
	case S10A_CH9_RESELECT:
		configure(0);
		configure((unsigned)s[2] | (unsigned)s[3] << 8);
		h_put("reselect");
		break;
	case S10A_CH9_FORWARD:
		forward(s);
		break;
	default:
		h_put("?");
		break;
	}
}

/* A SETUP the host model sends without printing its answer. */
static void quiet_setup(uint8_t bm, uint8_t req, uint16_t value,
			uint16_t index)
{
	const uint8_t s[8] = { bm, req, (uint8_t)value, (uint8_t)(value >> 8),
			       (uint8_t)index, (uint8_t)(index >> 8), 0, 0 };
	uint8_t reply[2];
	uint16_t len;

	switch (s10a_ch9_setup(&ch9, s, reply, &len)) {
	case S10A_CH9_FORWARD:
		if (req == 1)
			clear_halt((uint8_t)index);
		else if (req == 9)
			configure(value);
		break;
	case S10A_CH9_RESELECT:
		configure(0);
		configure(value);
		break;
	case S10A_CH9_STALL:
	case S10A_CH9_ACCEPT:
	case S10A_CH9_REPLY:
	default:
		break;
	}
}

/* --- Output and input of data -------------------------------------------- */

static void put_data(const uint8_t *p, size_t n)
{
	if (n <= SHOW_MAX) {
		h_puthex(p, n);
		return;
	}
	h_put("crc");
	h_putu((unsigned long)n);
	h_putx(h_crc32(0, p, (uint32_t)n));
}

static uint8_t big[BIG];
static uint8_t data_out[BIG];

/* DATA of field i into data_out: its length (for pat:SEED, `want`). */
static size_t get_data(const struct h_cmd *c, int i, size_t want)
{
	const char *s = h_str(c, i);

	if (s && strncmp(s, "pat:", 4) == 0) {
		const unsigned seed = (unsigned)strtoul(s + 4, NULL, 0);
		size_t k;

		if (want > sizeof(data_out))
			want = sizeof(data_out);
		for (k = 0; k < want; k++)
			data_out[k] = (uint8_t)(seed + 7u * k);
		return want;
	}
	return h_hex(c, i, data_out, sizeof(data_out));
}

static void get_serial(const struct h_cmd *c, int i,
		       char out[S10A_SERIAL_LEN + 1])
{
	uint8_t t[S10A_SERIAL_LEN];
	const size_t n = h_hex(c, i, t, sizeof(t));

	memset(out, 0, S10A_SERIAL_LEN + 1);
	memcpy(out, t, n);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

/* --- The host on the bulk endpoints -------------------------------------- */

static bool saw_in_halt, saw_out_halt;

/* The host sends n bytes on bulk OUT; false when it cannot. */
static bool host_send(const uint8_t *p, uint32_t n)
{
	uint32_t k;

	if (*halt_of(S10A_EP_BULK_OUT)) {
		saw_out_halt = true;
		return false;
	}
	if (!ep.out_armed)
		return false;
	k = n < ep.out_n ? n : ep.out_n;
	memcpy(ep.out, p, k);
	ep.out_armed = false;
	s10a_bot_xfer_done(&bot, false, k);
	sync_reset_owed();
	return true;
}

/* The host takes what bulk IN offers, up to `room` bytes. */
static uint32_t host_take(uint8_t *dst, uint32_t room)
{
	uint32_t n;

	if (*halt_of(S10A_EP_BULK_IN)) {
		saw_in_halt = true;
		return 0;
	}
	if (!ep.in_armed)
		return 0;
	n = ep.in_n < room ? ep.in_n : room;
	memcpy(dst, ep.in, n);
	ep.in_armed = false;
	s10a_bot_xfer_done(&bot, true, n);
	sync_reset_owed();
	return n;
}

static void host_command(const struct h_cmd *c, const uint8_t *cbw,
			 size_t cbw_n, size_t out_n)
{
	const uint32_t host_len = cbw_n >= 12 ? le32(cbw + 8) : 0;
	const bool host_in = cbw_n >= 13 && (cbw[12] & 0x80u) != 0;
	uint32_t moved = 0;
	uint8_t csw[S10A_BOT_CSW_LEN];
	unsigned guard;

	saw_in_halt = saw_out_halt = false;
	if (!host_send(cbw, (uint32_t)cbw_n)) {
		h_error(c, "CBW not taken");
		return;
	}
	for (guard = 0; host_len && moved < host_len && guard < 1024; guard++) {
		if (host_in) {
			const uint32_t room = host_len - moved < BIG - moved ?
					      host_len - moved : BIG - moved;
			const uint32_t n = room ? host_take(big + moved, room) : 0;

			if (!n)
				break;
			moved += n;
		} else {
			uint32_t k = host_len - moved;
			const uint32_t k0 = ep.out_armed && k > ep.out_n ?
					    ep.out_n : k;
			uint8_t zero[S10A_BOT_BUF];
			const uint8_t *p = zero;

			memset(zero, 0, sizeof(zero));
			k = k0 < sizeof(zero) ? k0 : (uint32_t)sizeof(zero);
			if (moved + k <= out_n)
				p = data_out + moved;
			if (!host_send(p, k))
				break;
			moved += k;
		}
	}
	/* BOT 5.3.3: a halt seen in the data phase is cleared first. */
	if (*halt_of(S10A_EP_BULK_OUT) && !host_in && host_len) {
		saw_out_halt = true;
		quiet_setup(0x02, 1, 0, S10A_EP_BULK_OUT);
	}
	if (*halt_of(S10A_EP_BULK_IN)) {
		saw_in_halt = true;
		quiet_setup(0x02, 1, 0, S10A_EP_BULK_IN);
	}
	h_putu(moved);
	put_data(big, host_in ? moved : 0);
	h_put(saw_in_halt ? (saw_out_halt ? "inout" : "in") :
	      saw_out_halt ? "out" : "-");
	if (host_take(csw, sizeof(csw)) == sizeof(csw))
		h_puthex(csw, sizeof(csw));
	else
		h_put("-");
}

/* --- The logical unit on its own ----------------------------------------- */

static void scsi_cmd(const struct h_cmd *c)
{
	uint8_t cdb[16] = { 0 };
	const size_t cdb_n = h_hex(c, 1, cdb, sizeof(cdb));
	struct s10a_scsi_cmd cmd;
	size_t out_n;
	const uint8_t *data = NULL;
	uint32_t n = 0;
	uint8_t status;

	s10a_scsi_command(&lu, cdb, (unsigned)cdb_n, &cmd);
	out_n = get_data(c, 2, cmd.dir == S10A_DIR_OUT ? cmd.len : 0);
	status = cmd.status;
	if (cmd.blocks && cmd.len <= sizeof(big)) {
		const uint32_t count = cmd.len / S10A_BLOCK;

		if (cmd.dir == S10A_DIR_IN) {
			const bool good = disk_read(NULL, cmd.lba, big,
						    count) == 0;

			status = s10a_scsi_done(&lu, &cmd, good, false);
			if (good) {
				data = big;
				n = cmd.len;
			}
		} else if (out_n >= cmd.len) {
			const bool good = disk_write(NULL, cmd.lba, data_out,
						     count) == 0;

			status = s10a_scsi_done(&lu, &cmd, good, true);
		}
	} else if (cmd.blocks) {
		h_error(c, "transfer too long");
		return;
	} else if (cmd.dir == S10A_DIR_IN) {
		data = cmd.data;
		n = cmd.len;
	}
	h_putu(status);
	h_putu((unsigned)cmd.dir);
	h_putu(cmd.len);
	put_data(data, n);
}

static void lu_event(const struct h_cmd *c, const char *name)
{
	if (strcmp(name, "withdraw") == 0)
		s10a_lu_withdraw(&lu, S10A_WITHDRAW_SCAN);
	else if (strcmp(name, "withdraw_own_write") == 0)
		s10a_lu_withdraw(&lu, S10A_WITHDRAW_OWN_WRITE);
	else if (strcmp(name, "offer") == 0)
		s10a_lu_offer(&lu);
	else if (strcmp(name, "bus_reset") == 0)
		s10a_lu_bus_reset(&lu);
	else if (strcmp(name, "set_configuration_same") == 0)
		s10a_lu_select_configuration(&lu);
	else if (strcmp(name, "set_configuration_0") == 0)
		s10a_lu_deconfigure(&lu);
	else
		h_error(c, "unknown event");
}

/* --- Descriptors --------------------------------------------------------- */

static bool get_set(const struct h_cmd *c, int i, enum s10a_set *set)
{
	const unsigned long v = h_num(c, i);

	if (v >= S10A_SET_COUNT) {
		h_error(c, "no such set");
		return false;
	}
	*set = (enum s10a_set)v;
	return true;
}

static void shape(enum s10a_set set)
{
	enum s10a_function fn;
	unsigned i, first;

	h_putu(s10a_desc_num_configs(set));
	h_putu(s10a_desc_num_interfaces(set));
	for (i = 0; s10a_desc_interface(set, i, &fn, &first); i++) {
		char t[32];
		const int k = snprintf(t, sizeof(t), "%u:%u", (unsigned)fn,
				       first);

		if (k < 0 || (size_t)k >= sizeof(t))
			t[0] = '\0';
		h_put(t);
	}
	for (i = 1; i <= S10A_MAX_CONFIGS + 1u; i++)
		h_putu(s10a_desc_max_power(set, i));
}

int main(void)
{
	struct h_cmd c;

	s10a_lu_init(&lu, &dev, "", &wb_device_opxy);
	s10a_bot_init(&bot, &lu, &port);
	s10a_ch9_start(&ch9, S10A_SET_DISK);
	while (h_next(&c)) {
		const char *k = c.tok[0];
		enum s10a_set set;

		if (strcmp(k, "device") == 0 && c.n == 3) {
			uint8_t d[18];

			if (!get_set(&c, 1, &set))
				continue;
			s10a_desc_device(&wb_device_opxy, set, h_num(&c, 2) != 0, d);
			h_puthex(d, sizeof(d));
		} else if (strcmp(k, "qualifier") == 0 && c.n == 2) {
			uint8_t d[10];

			if (!get_set(&c, 1, &set))
				continue;
			s10a_desc_qualifier(set, d);
			h_puthex(d, sizeof(d));
		} else if (strcmp(k, "config") == 0 && c.n == 6) {
			uint8_t d[S10A_CONFIG_MAX];
			size_t n;

			if (!get_set(&c, 1, &set))
				continue;
			n = s10a_desc_config(set, h_num(&c, 2) != 0,
					     (unsigned)h_num(&c, 3),
					     h_num(&c, 4) != 0, h_num(&c, 5) != 0,
					     d);
			h_puthex(d, n);
		} else if (strcmp(k, "string") == 0 && c.n == 4) {
			uint8_t d[S10A_STRING_MAX];
			char text[S10A_SERIAL_LEN + 1];
			size_t n;

			if (!get_set(&c, 1, &set))
				continue;
			get_serial(&c, 2, text);
			n = s10a_desc_string(&wb_device_opxy, set, text, (unsigned)h_num(&c, 3), d);
			h_puthex(d, n);
		} else if (strcmp(k, "shape") == 0 && c.n == 2) {
			if (!get_set(&c, 1, &set))
				continue;
			shape(set);
		} else if (strcmp(k, "hasep") == 0 && c.n == 3) {
			if (!get_set(&c, 1, &set))
				continue;
			h_putu(s10a_desc_has_endpoint(set,
						      (uint8_t)h_num(&c, 2)));
		} else if (strcmp(k, "serial") == 0 && c.n == 2) {
			uint8_t meta[64];
			char out[S10A_SERIAL_LEN + 1];
			const size_t n = h_hex(&c, 1, meta, sizeof(meta));

			s10a_serial_from_meta(meta, n, out);
			h_puthex((const uint8_t *)out, strlen(out));
		} else if (strcmp(k, "state") == 0 && c.n == 3) {
			if (s10a_set_for_state((enum s10a_state)h_num(&c, 1),
					       (h_num(&c, 2) & 1u) != 0,
					       (h_num(&c, 2) & 2u) != 0, &set))
				h_putu((unsigned)set);
			else
				h_put("-");
		} else if (strcmp(k, "ch9") == 0 && c.n == 4) {
			if (!get_set(&c, 1, &set))
				continue;
			get_serial(&c, 2, serial);
			high = h_num(&c, 3) != 0;
			s10a_bot_stop(&bot);
			close_endpoints();
			s10a_ch9_start(&ch9, set);
			sync_reset_owed();
			h_put("ok");
		} else if (strcmp(k, "setup") == 0 && c.n == 2) {
			uint8_t s[8];

			if (h_hex(&c, 1, s, sizeof(s)) != sizeof(s)) {
				h_error(&c, "a SETUP is 8 bytes");
				continue;
			}
			setup(s);
		} else if (strcmp(k, "busreset") == 0 && c.n == 1) {
			s10a_ch9_reset(&ch9);
			s10a_bot_stop(&bot);
			sync_reset_owed();
			s10a_lu_bus_reset(&lu);
			close_endpoints();
			h_put("ok");
		} else if (strcmp(k, "lu") == 0 && c.n == 3) {
			get_serial(&c, 1, serial);
			memset(&disk, 0, sizeof(disk));
			s10a_lu_init(&lu, &dev, serial, &wb_device_opxy);
			if (h_num(&c, 2))
				s10a_lu_offer(&lu);
			s10a_bot_init(&bot, &lu, &port);
			close_endpoints();
			sync_reset_owed();
			h_put("ok");
		} else if (strcmp(k, "event") == 0 && c.n == 2) {
			lu_event(&c, c.tok[1]);
			h_put("ok");
		} else if (strcmp(k, "lustate") == 0 && c.n == 1) {
			h_putu(lu.offered);
			h_putu(lu.stopped);
			h_putu(lu.ua);
			h_putu(lu.acquired);
			h_putu(lu.releases);
			h_putu(lu.key);
			h_putu(lu.asc);
			h_putu(lu.ascq);
			h_putu(lu.info);
		} else if (strcmp(k, "diskfail") == 0 && c.n == 2) {
			disk.fail = h_num(&c, 1) != 0;
			h_put("ok");
		} else if (strcmp(k, "blocks") == 0 && c.n == 3) {
			const unsigned long lba = h_num(&c, 1);
			const unsigned long n = h_num(&c, 2);
			const bool was = disk.fail;

			if (n > BIG / S10A_BLOCK) {
				h_error(&c, "too many blocks");
				continue;
			}
			disk.fail = false;
			if (disk_read(NULL, (uint32_t)lba, big, (uint32_t)n) != 0)
				h_put("-");
			else
				put_data(big, n * S10A_BLOCK);
			disk.fail = was;
		} else if (strcmp(k, "cmd") == 0 && c.n == 3) {
			scsi_cmd(&c);
		} else if (strcmp(k, "bot") == 0 && c.n == 1) {
			s10a_ch9_start(&ch9, S10A_SET_DISK);
			close_endpoints();
			quiet_setup(0x00, 9, 1, 0);
			h_put("ok");
		} else if (strcmp(k, "host") == 0 && c.n == 3) {
			uint8_t cbw[64];
			const size_t n = h_hex(&c, 1, cbw, sizeof(cbw));
			const size_t len = n >= 12 ? le32(cbw + 8) : 0;

			host_command(&c, cbw, n, get_data(&c, 2, len));
		} else if (strcmp(k, "send") == 0 && c.n == 2) {
			uint8_t b[S10A_BLOCK + 16];
			const size_t n = h_hex(&c, 1, b, sizeof(b));

			saw_in_halt = saw_out_halt = false;
			h_putu(host_send(b, (uint32_t)n));
		} else if (strcmp(k, "take") == 0 && c.n == 2) {
			const unsigned long room = h_num(&c, 1);

			if (*halt_of(S10A_EP_BULK_IN))
				h_put("halt");
			else
				put_data(big, host_take(big, room < BIG ?
							(uint32_t)room : BIG));
		} else if (strcmp(k, "eps") == 0 && c.n == 1) {
			h_putu(*halt_of(S10A_EP_BULK_IN));
			h_putu(*halt_of(S10A_EP_BULK_OUT));
			h_putu(ep.in_armed);
			h_putu(ep.out_armed);
			h_putu(s10a_bot_reset_owed(&bot));
			h_putu(s10a_bot_busy(&bot));
			h_putu((unsigned)bot.state);
		} else {
			h_error(&c, "unknown command");
			continue;
		}
		h_end();
	}
	return h_status();
}
