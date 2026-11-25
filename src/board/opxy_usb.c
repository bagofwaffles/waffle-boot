/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * USB in held mode: the BF70x device port (wb_usb_*) under S-10a (s10a_usb_*),
 * and the master's two host links on it (S-06.011): the CDC port's byte
 * stream and the MIDI carrier (S-10c.020). Frames for the master on either
 * link go to S-10c; the node forwards the rest (S-06.050).
 */
#include <string.h>

#include "waffle/board/opxy_boot.h"
#include "waffle/util/div64.h"
#include "waffle/usb/msc.h"
#include "waffle/host/sysex.h"

#define DISK_IDLE_MS 1000u

/* An S-06 frame's bytes on the wire beyond its payload: the header, the
 * check, COBS's one byte and the delimiter (S-06.001-S-06.005). */
#define WIRE_EXTRA (S06_HEADER + 3u)

/* The stream bytes of the last MIDI carrier message (S-10c.021). */
static uint8_t midi_in[S10C_CARRIER_MAX_RX];
static size_t midi_in_len, midi_in_pos;

static bool up;
/* Kept out of struct opxy_services, whose layout the start-up code shares. */
static enum s10a_set usb_set = S10A_SET_COUNT;

bool opxy_usb_up(void)
{
	return up;
}

/*
 * The device stack's own work: control requests, the disk's answers, the
 * rings' bytes. Never run from inside itself: a wait in something the stack
 * called services nothing.
 */
#define BREATH_US 1000u		/* about a millisecond between two */

/* The board clock's cycles in a microsecond, as the gap meter counts. */
static uint32_t cycles_per_us(void)
{
	return opxy_time.cycles_per_us != 0u ? opxy_time.cycles_per_us : 1u;
}

static bool in_task;
static uint64_t served;		/* the cycle count at the last service */
static uint64_t gap;		/* the longest time between two, while metered */
static bool metered;

static void usb_task(void)
{
	const uint64_t now = wb_core_cycles();

	if (in_task)
		return;
	if (metered && now - served > gap)
		gap = now - served;
	served = now;
	in_task = true;
	wb_usb_task();
	in_task = false;
}

/*
 * The bus kept alive from inside anything else: every wait of the serial
 * protocols (S-07's service hook and the clock's idle), the flash's, a
 * package's check and extraction, a run's steps, a long CRC. The stack's
 * own work alone, at most about once a millisecond: no host link is read,
 * no frame or SysEx message is handed to S-10c and no MTP request runs;
 * only the loop's own pass does that (opxy_usb_poll()). A host therefore
 * never meets a configured device that does not answer (S-10a.049).
 */
void opxy_usb_breathe(void)
{
	if (in_task || wb_core_cycles() - served < (uint64_t)BREATH_US * cycles_per_us())
		return;
	if (up) {
		usb_task();
	} else {
		/* nothing to service: the meter still counts the calls */
		const uint64_t now = wb_core_cycles();

		if (metered && now - served > gap)
			gap = now - served;
		served = now;
	}
}

void opxy_usb_gap_begin(void)
{
	served = wb_core_cycles();
	gap = 0;
	metered = true;
}

uint32_t opxy_usb_gap_end(void)
{
	const uint64_t us = wb_udiv64(gap, cycles_per_us());

	metered = false;
	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

/*
 * S-10a.049: what the host sends on CDC and MIDI is taken by the loop's own
 * pass alone (opxy_usb_poll()). The two host links are held (s06_link_hold())
 * at all other times, so no wait that polls the node (S-07's, S-08's, a
 * request's) reads them, forwards a frame from them to the PSoC, the Nordic
 * or the slave, or queues one. The pass lets them go only while it reads
 * them, and holds them again before it hands S-10c what it read.
 *
 * The host hold keeps the pass itself from running, for the whole of a long
 * operation (a scan, an install, a host transfer's submission and wipe, held
 * mode's file operations), between whose steps the loop could otherwise
 * come round. The bytes wait in S-10a's receive rings meanwhile, and a full
 * ring holds the host off with NAKs. What goes to the host (the log,
 * replies, progress) is not affected. Pairs nest.
 */
static unsigned hold;
static bool in_pass;

void opxy_host_hold_begin(struct opxy_master *m)
{
	(void)m;
	hold++;
}

void opxy_host_hold_end(struct opxy_master *m)
{
	(void)m;
	if (hold != 0u)
		hold--;
}

bool opxy_host_held(void)
{
	return hold != 0u;
}

/* A host link read, by the loop's pass alone, with nothing else run
 * meanwhile: frames for the master are queued, the others forwarded
 * (S-06.050). */
static void read_host_link(struct opxy_master *m, unsigned link)
{
	s06_link_hold(&m->link[link], false);
	s06_node_poll_link(&m->node, link);
	s06_link_hold(&m->link[link], true);
}

enum s10a_set opxy_usb_set(void)
{
	return usb_set;
}

/* --- The CDC link (S-10c.030-S-10c.033) --- */

static size_t cdc_read(void *ctx, uint8_t *buf, size_t max)
{
	(void)ctx;
	return s10a_usb_cdc_read(buf, max);
}

/* No room while the port is closed: the frames the master originates and
 * those it forwards are dropped then (S-10c.031, Q-C7). A frame goes into
 * S-10a's ring whole or not at all (S-06.046). */
static size_t cdc_room(void *ctx)
{
	(void)ctx;
	return s10a_usb_cdc_open(NULL) ? s10a_usb_cdc_room() : 0u;
}

static void cdc_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	(void)s10a_usb_cdc_write(data, n);
}

static const struct s06_port_ops cdc_port = { cdc_read, cdc_room, cdc_write };

/*
 * S-10c.033: wait for the host to take what is queued, at least
 * len / 10 + 110 ms, servicing the bus meanwhile. This runs inside S-10c
 * (a reply), so it must not call s10a_usb_poll(), which hands S-10c what
 * arrived (usb/device.h): the device stack and the ring's kick move the
 * bytes.
 */
static void cdc_wait(struct opxy_master *m, size_t need)
{
	const uint64_t end = wb_time_ms(&m->hw.time) +
			     s10c_write_timeout_ms(need);

	while (up && s10a_usb_cdc_open(NULL) && s10a_usb_cdc_room() < need &&
	       wb_time_ms(&m->hw.time) < end)
		usb_task();
}

/* --- The MIDI link (S-10c.020-S-10c.022) --- */

static size_t midi_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t n = midi_in_len - midi_in_pos;

	(void)ctx;
	if (n > max)
		n = max;
	memcpy(buf, midi_in + midi_in_pos, n);
	midi_in_pos += n;
	return n;
}

/* What n wire bytes take of S-10a's queue as S-10c.022's carrier
 * messages, at most 245 stream bytes each. */
static size_t carrier_size(size_t n)
{
	size_t total = 0;

	while (n > 0u) {
		const size_t k = n < S10C_CARRIER_MAX_STREAM ?
				 n : S10C_CARRIER_MAX_STREAM;

		total += s10a_usb_sysex_size(7u + s10c_packed_len(k));
		n -= k;
	}
	return total;
}

/*
 * s10a_usb_send_sysex() drops a message that does not fit, and a frame over
 * 245 wire bytes takes two (S-10c.022): the room is the longest frame whose
 * messages all fit, so that none is cut (S-06.046).
 */
static size_t midi_room(void *ctx)
{
	const size_t room = s10a_usb_midi_room();
	size_t n = S06_MAX_WIRE;

	(void)ctx;
	while (n > 0u && carrier_size(n) > room)
		n--;
	return n;
}

static void midi_write(void *ctx, const uint8_t *data, size_t n)
{
	const struct opxy_master *m = ctx;

	s10c_carrier_send_frame(opxy_dev(m), data, n, s10a_usb_send_sysex, NULL);
}

static const struct s06_port_ops midi_port = { midi_read, midi_room,
					       midi_write };

/*
 * S-10c.033 on MIDI, as cdc_wait(): `need` bytes of S-10a's queue, waited
 * for while the bus is serviced. This too runs inside S-10c (a reply, an
 * install's progress), so it only moves bytes: nothing that arrives is
 * handed to S-10c meanwhile.
 */
static void midi_wait(struct opxy_master *m, size_t need)
{
	const uint64_t end = wb_time_ms(&m->hw.time) +
			     s10c_write_timeout_ms(need);

	while (up && !in_task && s10a_usb_midi_open() &&
	       s10a_usb_midi_room() < need && wb_time_ms(&m->hw.time) < end)
		usb_task();
}

/* s10c_ops.send_sysex: a message that does not fit yet is waited for. */
static void send_sysex(void *ctx, const uint8_t *msg, size_t n)
{
	midi_wait(ctx, s10a_usb_sysex_size(n));
	s10a_usb_send_sysex(NULL, msg, n);
}

/* --- S-10c's transports --- */

static void send_frame(void *ctx, enum s10c_link link, uint8_t route,
		       uint8_t id, const uint8_t *payload, size_t len)
{
	struct opxy_master *m = ctx;
	unsigned to;

	switch (link) {
	case S10C_LINK_CDC:
		cdc_wait(m, len + WIRE_EXTRA);
		to = S06_LINK_CDC;
		break;
	case S10C_LINK_PSOC:
		to = S06_LINK_PSOC;
		break;
	case S10C_LINK_NORDIC:
		to = S06_LINK_NORDIC;
		break;
	case S10C_LINK_MIDI:
	default:
		midi_wait(m, carrier_size(len + WIRE_EXTRA));
		to = S06_LINK_MIDI;
		break;
	}
	(void)s06_send(&m->node, to, route, id, payload, len);
}

/* S-10c.032: s10a_usb_poll() calls this once the bytes before the
 * request have been read, so only an incomplete frame is lost. */
static void cdc_rx_restart(void *ctx)
{
	struct opxy_master *m = ctx;

	s06_rx_reset(&m->link[S06_LINK_CDC].rx);
}

/* S-10c Q-C10: each carrier message starts the receiver afresh. The frames the
 * last message completed are taken first. S-10c calls this from
 * s10a_usb_poll(), which the loop's pass alone runs. */
static void midi_rx_restart(void *ctx)
{
	struct opxy_master *m = ctx;

	read_host_link(m, S06_LINK_MIDI);
	s06_rx_reset(&m->link[S06_LINK_MIDI].rx);
	midi_in_len = midi_in_pos = 0;
}

static void midi_rx_feed(void *ctx, const uint8_t *stream, size_t n)
{
	(void)ctx;
	if (n > sizeof(midi_in))
		n = sizeof(midi_in);
	memcpy(midi_in, stream, n);
	midi_in_len = n;
	midi_in_pos = 0;
}

/* The next SysEx message once the link has read all of the last carrier's
 * stream (the restart would lose the rest), and never under the hold. */
static bool midi_ready(void *ctx)
{
	(void)ctx;
	return hold == 0u && midi_in_pos == midi_in_len;
}

/* --- S-11.050's transfer storage, and the disk offered again --- */

/* S-15.104: a host transfer's begin makes the upgrade screen current. */
static void upgrade_screen(void *ctx, uint32_t total)
{
	struct opxy_master *m = ctx;

	s15_up_transfer_begin(&m->sv.menu, total);
}

/* A begin refused for its product word (S-10c.071, S-10c.088),
 * on the screen its upgrade_screen() opened. */
static void begin_refused(void *ctx, enum s11_reason reason)
{
	struct opxy_master *m = ctx;

	s15_up_refused(&m->sv.menu, s11_reason_name(reason));
}

/*
 * S-11.050: the transfer's storage (opxy_transfer.c). A begin that starts
 * its transfer gets the Transfer row; one that cannot prepare the storage
 * shows that the transfer could not start, and the screen goes idle again
 * (S-15.103).
 */
static bool transfer_start(void *ctx)
{
	struct opxy_master *m = ctx;

	if (opxy_transfer_start(m)) {
		s15_up_transfer_started(&m->sv.menu);
		return true;
	}
	s15_up_no_transfer(&m->sv.menu);
	return false;
}

static bool transfer_append(void *ctx, const uint8_t *p, size_t n)
{
	return opxy_transfer_append(ctx, p, n);
}

static void transfer_discard(void *ctx)
{
	opxy_transfer_discard(ctx);
}

/* S-10c.075: the firmware command forces the package on either link. */
static void transfer_submit(void *ctx, enum s10c_source source,
			    struct s11_outcome *out)
{
	opxy_transfer_submit(ctx, source != S10C_SOURCE_TE, out);
}

/*
 * S-11.050: after a host transfer the disk is offered again, with its
 * earlier contents, where the set presented offers it and S-15.115 has not
 * ended its offers (a zip read from it in this boot). No scan follows
 * (S-15.103): S-15 ignores the release a host-link package left.
 */
/* cppcheck-suppress constParameterCallback ; s10c_ops fixes the type */
static void disk_offer_again(void *ctx)
{
	const struct opxy_master *m = ctx;

	if (m->sv.usb_state == S10A_STATE_UPGRADE && m->sv.disk_formatted &&
	    s15_up_disk_offerable(&m->sv.menu))
		s10a_usb_disk_offer();
}

void opxy_usb_host_ops(struct opxy_master *m, struct s10c_ops *ops)
{
	(void)m;
	memset(ops, 0, sizeof(*ops));
	ops->send_frame = send_frame;
	ops->send_sysex = send_sysex;
	ops->cdc_open = s10a_usb_cdc_open;
	ops->cdc_rx_restart = cdc_rx_restart;
	ops->midi_rx_restart = midi_rx_restart;
	ops->midi_rx_feed = midi_rx_feed;
	ops->upgrade_screen = upgrade_screen;
	ops->begin_refused = begin_refused;
	ops->transfer_start = transfer_start;
	ops->transfer_append = transfer_append;
	ops->transfer_discard = transfer_discard;
	ops->transfer_submit = transfer_submit;
	ops->ramdisk_offer = disk_offer_again;
}

/* --- S-10a under the board --- */

/*
 * S-07.057: S-10a gives 2 x bMaxPower when configured, 100 mA after
 * SET_CONFIGURATION(0), a bus reset or the loss of VBUS. Some sets grant
 * 100 mA or less, so the device's state, not the current, says which:
 * TinyUSB has the configuration selected by the time it reports it.
 */
static void input_current(void *ctx, unsigned ma)
{
	struct opxy_master *m = ctx;

	if (wb_usb_state() == WB_USB_CONFIGURED)
		s07_usb_configured(&m->psoc, ma);
	else
		s07_usb_reset(&m->psoc);
}

/* S-07.054's USB activity: a bus reset, a suspend, a
 * resume or the loss of VBUS, configured or not. */
static void usb_activity(void *ctx)
{
	struct opxy_master *m = ctx;

	s07_usb_activity(&m->psoc);
}

/*
 * S-15.105: S-11.045's scan is due after a release. S-10a reports no
 * acquisition, so opxy_usb_poll() looks for it; a release clears what it
 * saw, so an acquisition after the release reaches S-15, which
 * cancels the scan, even when both come between two polls.
 */
static void disk_released(void *ctx)
{
	struct opxy_master *m = ctx;

	m->sv.disk_acquired = false;
	s15_up_released(&m->sv.menu, (uint32_t)wb_time_ms(&m->hw.time));
}

void opxy_usb_start(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;
	struct opxy_disk_part parts[OPXY_DISK_PARTS];
	const struct wb_usb_config cfg = {
		.time = &m->hw.time,
		.irq_prio = OPXY_USB_IRQ_PRIO,
	};

	midi_in_len = midi_in_pos = 0;
	s06_link_init(&m->link[S06_LINK_CDC], &cdc_port, m, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m->node.link[S06_LINK_CDC] = &m->link[S06_LINK_CDC];
	s06_link_init(&m->link[S06_LINK_MIDI], &midi_port, m, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m->node.link[S06_LINK_MIDI] = &m->link[S06_LINK_MIDI];
	/* USB holds the host while a link is not read: no frame for the
	 * master is lost to a full inbox. Read by the loop's pass alone. */
	m->link[S06_LINK_CDC].paced = true;
	m->link[S06_LINK_MIDI].paced = true;
	s06_link_hold(&m->link[S06_LINK_CDC], true);
	s06_link_hold(&m->link[S06_LINK_MIDI], true);

	opxy_disk_ddr_dev(opxy_dev(m), parts);	/* S-18.030 */
	opxy_disk_init(&sv->disk, parts);
	sv->disk_formatted = false;
	sv->disk_acquired = false;
	sv->usb_state = S10A_STATE_MAIN;

	sv->usb_board.ctx = m;
	sv->usb_board.input_current = input_current;
	sv->usb_board.suspend = NULL;
	sv->usb_board.activity = usb_activity;
	sv->usb_board.disk_released = disk_released;
	sv->usb_board.midi_ready = midi_ready;
	s10a_usb_init(&sv->usb_board, &sv->host, opxy_dev(m));
	opxy_mtp_start(m);
	wb_usb_set_setup_filter(s10a_usb_setup_filter, NULL);
	/* The battery wait may have brought the controller up already. */
	if (up)
		return;
	up = wb_usb_init(&cfg) == WB_USB_OK;
	if (!up)
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG, "usb init failed");
}

/*
 * S-07.054: the battery wait starts USB mass storage, which is S-10a.010's
 * battery row (the disk set with no medium), and stops it when the wait
 * ends. This runs before held mode: held mode's opxy_usb_start() then sets
 * S-10a up afresh on the controller already running.
 */
void opxy_usb_battery(struct opxy_master *m, bool on)
{
	if (on) {
		opxy_usb_start(m);
		opxy_usb_select(m, S10A_STATE_BATTERY);
	} else if (up && wb_usb_attached()) {
		wb_usb_detach();
	}
}

/* The frames for the master the two links hold, to S-10c; false if none. */
static bool take_frames(struct opxy_master *m)
{
	struct s06_frame f;
	bool any = false;

	while (s06_link_take(&m->link[S06_LINK_CDC], &f)) {
		any = true;
		OPXY_TESTOS(if (!opxy_testos_monitor(m, &f)))
		if (!OPXY_TESTOS_OR(false, opxy_dev_host(m, &f) ||
				    opxy_hostlink_frame(m, S06_LINK_CDC, &f)))
			s10c_host_frame(&m->sv.host, S10C_LINK_CDC, f.route, f.id,
					f.payload, f.len);
	}
	while (s06_link_take(&m->link[S06_LINK_MIDI], &f)) {
		any = true;
		if (!OPXY_TESTOS_OR(false, opxy_hostlink_frame(m, S06_LINK_MIDI, &f)))
			s10c_host_frame(&m->sv.host, S10C_LINK_MIDI, f.route, f.id,
					f.payload, f.len);
	}
	return any;
}

/* Rounds of one pass: the paced links give at most an inbox each. */
#define POLL_ROUNDS 4u

/*
 * S-10a.049: the loop's own pass, the one place the host's input is taken:
 * S-10a's bytes, the SysEx messages and MTP requests it hands on, and the
 * host links' frames, in order. Called from the loop alone; from inside
 * anything it runs (a request a frame starts, the waits under it), and
 * under the host hold, it does the stack's own work only.
 */
void opxy_usb_poll(struct opxy_master *m)
{
	unsigned round;

	if (!up)
		return;
	usb_task();
	if (hold != 0u || in_pass)
		return;
	in_pass = true;
	for (round = 0; round < POLL_ROUNDS; round++) {
		s10a_usb_poll();
		if (s10a_usb_disk()->acquired && !m->sv.disk_acquired) {
			m->sv.disk_acquired = true;
			s15_up_acquired(&m->sv.menu);
		}
		read_host_link(m, S06_LINK_CDC);
		read_host_link(m, S06_LINK_MIDI);
		if (!take_frames(m))
			break;
	}
	in_pass = false;
}

/* S-15 starts the upgrade screen after reason 40 with its main set
 * (S-15.102), which S-10a.010 gives the same row. */
enum s10a_state opxy_usb_state(enum s15_usb_set set)
{
	switch (set) {
	case S15_USB_DISK:
		return S10A_STATE_UPGRADE;
	case S15_USB_MTP_VENDOR:
		return S10A_STATE_FACTORY_MTP_VENDOR;
	case S15_USB_MTP:
		return S10A_STATE_FACTORY_MTP;
	case S15_USB_SERIAL:
		return S10A_STATE_FACTORY_MTP_OFF;
	case S15_USB_MAIN:
	default:
		return S10A_STATE_MAIN;
	}
}

/*
 * S-12.002: the volume is formatted once in the session, before the disk is
 * first offered, and kept as it is after that. The format writes the disk
 * while it is offered to no host (S-12.018), and FatFs lets go of it
 * before the host may write it.
 */
bool opxy_disk_ready(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;

	if (sv->disk_formatted)
		return true;
	opxy_menu_show_now(m);	/* S-15.002: the format takes a while */
	if (!wire_fat_format(&sv->fat, &sv->disk.dev, opxy_dev(m))) {
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG,
			    "ramdisk format failed");
		return false;
	}
	/* The test OS (waffle/testos.h) puts flash.bin and testos.txt on
	 * the fresh volume, before any host sees it. */
	OPXY_TESTOS(OPXY_TESTOS_CHECK_OR(opxy_testos_check_disk(m), opxy_testos_disk_files(m));)
	wire_fat_unmount(&sv->fat);
	sv->disk_formatted = true;
	return true;
}

/*
 * S-10a.049: the disk withdrawn, and the command the host may have started
 * given DISK_IDLE_MS to end; the medium is gone by then, so a host sends no
 * new one. From the withdrawal on, S-10a stores nothing more of a WRITE,
 * however long its data phase lasts and whether or not the disk is offered
 * again, so no command can write the volume behind the caller. False when
 * the command does not end: a READ may still be reading, so the caller
 * does not use the volume.
 */
bool opxy_disk_take(struct opxy_master *m, enum s10a_withdrawal why)
{
	uint64_t end;

	s10a_usb_disk_withdraw(why);
	end = wb_time_ms(&m->hw.time) + DISK_IDLE_MS;
	while (s10a_usb_disk_busy() && wb_time_ms(&m->hw.time) < end)
		usb_task();
	return !s10a_usb_disk_busy();
}

void opxy_usb_select(struct opxy_master *m, enum s10a_state state)
{
	enum s10a_set set;
	char serial[S10A_SERIAL_LEN + 1];
	const struct wb_blockdev *disk = NULL;
	bool offer = false;

	/* S-10a.011: developer mode keeps the serial port on the MTP sets. */
	if (!up || !s10a_set_for_state(state, opxy_serial_flag(m),
				       m->sv.menu.dev_mode, &set))
		return;
	/* S-10a.010: the upgrade screen offers the disk, until a zip has been
	 * read from it (S-15.115: the set then keeps no medium); the battery
	 * wait's disk set has no medium (S-10a.051), so no disk at all. */
	if (state == S10A_STATE_UPGRADE) {
		disk = &m->sv.disk.dev;
		offer = s15_up_disk_offerable(&m->sv.menu) &&
			opxy_disk_ready(m);
	}
	m->sv.usb_state = state;
	usb_set = set;
	m->sv.disk_acquired = false;
	/* S-10a.004 */
	s10a_serial_from_meta((const uint8_t *)m->meta.serial,
			      S10A_SERIAL_LEN, serial);
	if (wb_usb_attached())
		wb_usb_detach();
	midi_in_len = midi_in_pos = 0;
	s06_rx_reset(&m->link[S06_LINK_CDC].rx);
	s06_rx_reset(&m->link[S06_LINK_MIDI].rx);
	s10a_usb_select(set, serial, disk);
	(void)wb_usb_attach();
	/* The set starts with no medium; the offer is S-10a.049's medium
	 * change. */
	if (offer)
		s10a_usb_disk_offer();
}

/* S-05.016, S-10a.030: the pull-up off, the device stack stopped, the PHY
 * off, whether or not USB came up. opxy_usb_start() starts it afresh. */
void opxy_usb_stop(void)
{
	wb_usb_stop();
	up = false;
}

/*
 * S-10a's serial flag (S-04.007): reason 10, developer mode
 * as it is now (S-15.172), an unprovisioned unit (S-16.095, a failed GP1
 * read included) or the debug key held at start (S-07.032).
 */
bool opxy_serial_flag(const struct opxy_master *m)
{
	return OPXY_TESTOS(OPXY_TESTOS_MON_ON ||) m->boot_reason == 10u || m->sv.menu.dev_mode ||
	       !m->sv.provisioned || m->debug_key;
}
