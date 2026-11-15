/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for the S-09 bridge (src/s09_bridge.c, s09_master.c,
 * s09_slave.c): the master's bus wired to the slave's session interface,
 * every session logged, the slave's store programmable.
 *
 * Commands (one result line each):
 *   rig                            -> ok: a fresh master and slave
 *   store CMD RESULT ERR DATA      -> ok: what the store answers for CMD (0x03 and 0x05: RESULT is
 *                                     the value; 0x07: the OS status; eMMC and file commands:
 *                                     RESULT, ERR and DATA, the sectors read or the record)
 *   sample N                       -> ok: the cycle counter (S-09.036)
 *   file DATA POS FAIL             -> ok: the file read by 0x2A, its position, the piece that fails
 *                                     with ENODEV (-1: none)
 *   tunnel DATA                    -> ok: bytes the slave has to send on channel 0
 *   start                          -> 1|0 (s09_master_start)
 *   call CMD LEN ARG0 ARG1 PAYLOAD MAX -> RESULT ERR DATA (s09_master_call); PAYLOAD is hex, or
 *                                     r7:N for N bytes (i*7)&0xff (requests.json's data_rule)
 *   read FD N                      -> RESULT ERR DATA (s09_master_read)
 *   service                        -> ok (s09_master_service)
 *   release                        -> s09_master_release
 *   inject REQUEST SLOT            -> RX OPS PATH REPLY: REQUEST handed to the slave directly as a
 *                                     session (RX: the bytes s09_slave_rx_len() took; of a malformed
 *                                     write's, REQUEST need carry only the header, the slave keeping
 *                                     s09_slave_rx_keep() bytes and discarding the rest), then a poll in
 *                                     SLOT and its reply ("-": none); OPS counts the store's eMMC and
 *                                     file calls it made, PATH is the last file call's path ("-": none)
 *   sessions                       -> MOSI:MISO:RX:SESS per session since the last "sessions"; RX
 *                                     is what s09_slave_rx_len() took of a request session (0: none),
 *                                     SESS the bytes the slave moved in it before its ready line rose:
 *                                     RX and the trailing bytes s09_slave_rx_trail() takes (S-09.021)
 *   stat|statfs|dirent FIELDS...   -> the record (s09_*_pack); dirent's name is hex
 *   boot PATH CRC MODE SEED STUCK  -> RESULT END HIGH RELEASE ELAPSED SESSION...: s09_boot_slave on
 *                                     the slot file against a boot-link model (B-STREAM). MODE: 0
 *                                     answers FF to 03 and drives PA6 high 0-50 us (seeded) after each
 *                                     byte; 1 PA6 never low (no_ready); 2 answers 00 (no_open); 3 as 2,
 *                                     PA6 never rising (no_open_stuck); 4 as 0, PA6 held high for
 *                                     900 ms after STUCK bytes (slow_block). STUCK, other modes: PA6
 *                                     stays high after that many bytes (0: never). HIGH: bytes started
 *                                     with PA6 high;
 *   bootx PATH CRC MODE SEED STUCK FC LATE [SETTLE] -> RESULT END HIGH RELEASE ELAPSED EARLY
 *                                     SESSION...:
 *                                     the same, with FC 1 through the ops' send() (BOOT_LINK_FC),
 *                                     the stream's sessions moved by a model of the SPI controller
 *                                     under flow control: it looks at PA6 as each byte ends and
 *                                     starts the next at once while PA6 reads low. LATE: the slave
 *                                     raises PA6 only that many us after a byte's end. EARLY: bytes
 *                                     started in that gap, after a byte and before the slave had
 *                                     raised PA6 for it. SETTLE: the ops' xfer() returns that many
 *                                     us after its byte, as the board's does (its settle time; 0
 *                                     when absent), which send() does not;
 *                                     RELEASE: us from the last byte to the chip select's release;
 *                                     ELAPSED: us of the whole call; each SESSION hex
 *   sreq HEX                       -> RX REPLY: HEX handed to the slave directly as one request
 *                                     session (RX: the bytes it takes; HEX zero padded to the
 *                                     s09_slave_rx_keep() of them it keeps), then the reply session it
 *                                     has ("-": none)
 *   satt                           -> 1|0: PB2 low (s09_slave_attention)
 *   stake                          -> 1 VALUE | 0: the release hook (s09_slave_take_release)
 *   cpu AUTO                       -> ok: S-06 nodes on both ends of channel 0 (the master's own
 *                                     address 1, the slave's 5); AUTO 1: the slave handles its CPU
 *                                     link after every request session it is handed
 *   msend ID ROUTE HEX             -> 1|0 (s06_send on the master's node)
 *   mwrite HEX                     -> ok (HEX written to s09_tunnel_port, as its room allows)
 *   mtake                          -> ID:ROUTE:HEX... the frames the master's node delivers
 *   mclock SECS                    -> ok (s09_clock_message)
 *   poweroff                       -> 1|0 ELAPSED (s09_power_off; ELAPSED in us)
 *   spoll                          -> ok (s09_slave_cpu_poll)
 *   scpu                           -> POWEROFFS CLOCKS FRAMES: S-04.025 calls, the clock values
 *                                     given (comma separated, "-": none), other frames (ID:HEX,...)
 *   srx                            -> HEX: the slave's received channel-0 bytes ("-": none)
 *   model K ORDER                  -> ok: the slave model of M-DEFER/M-SLOTS (below); ORDER fifo or
 *                                     lifo; K 0: off
 *   modelstat                      -> POLLS NONE HELD: polls it served, those it answered 06 FF 00 00,
 *                                     replies it still holds
 *   nest CMD LEN ARG0 ARG1 HEX MAX -> ok: a call made from the next PC5 read of a waiting call
 *   nested                         -> RESULT:ERR:DATA per nested call made
 *
 * The link modes (S-09.070-S-09.075). The bus records each session's rung
 * (configure()'s phase) and carries a request session on rung r:
 *   - not at all, when quad I/O meets a slave listening in single I/O or the
 *     reverse, or r is deaf: the slave's receive fails (s09_slave_link_fault);
 *   - with a bit of arg0 wrong after the check was made, when r is bad or the
 *     flip count runs out (request sessions on a rung above 0 only);
 *   - cut where the slave's session ends: a master that clocks more words than
 *     the slave expects is held and its wait runs out, one that clocks fewer
 *     finds PA6 low at the end; either session is logged late.
 *   link CEIL                      -> OK RUNG FELL (s09_master_link_fast)
 *   mrung                          -> RUNG FELL WMAX ABSENT
 *   wire BAD DEAF VBAD             -> ok: rung masks (bit r): request sessions with a bit wrong,
 *                                     not heard, 0x1A replies with a bit of the value wrong
 *   flip N                         -> ok: the Nth request session on a rung from now has a bit wrong
 *   dead N                         -> ok: after N more sessions PA6 stays high and PC5 high
 *   smode                          -> MODE TRIAL ATT: the slave's link mode, on trial, PB2 low
 *   sfault                         -> ok: the slave's receive failed (s09_slave_link_fault)
 *   sraw HEX                       -> RX REPLY: HEX handed to the slave as a session of exactly
 *                                     its bytes (a short one included)
 *   sops CMD                       -> N: the store's calls for CMD since the last sops CMD
 *   sput HEX                       -> ok: HEX appended to a request kept for sgo ("-": emptied)
 *   sgo                            -> RX REPLY: that request handed over as sraw hands HEX
 * "sessions" gives MOSI:MISO:RX:SESS:RUNG:LATE per session.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hlib.h"
#include "waffle/s06_bytes.h"
#include "waffle/s06_clock.h"
#include "waffle/s06_link.h"
#include "waffle/s09_bridge.h"
#include "waffle/s09_master.h"
#include "waffle/s09_slave.h"

static uint8_t buf[H_MAXLINE / 2];
static uint8_t pay[128u * S09_SECTOR];

/* --- the store --- */

struct answer {
	bool set;
	int32_t result;
	uint32_t err;
	uint8_t data[2 * S09_SECTOR];
	uint32_t n;
};

static struct answer answer[256];
static uint64_t sample;
static uint8_t file_data[0x4000];
static uint32_t file_len, file_pos;
static int fail_piece, piece;
static unsigned store_ops, cmd_ops[256];
static char store_path[S09_TUNNEL_MAX + 16u];

static uint32_t st_erase_group(void *ctx)
{
	(void)ctx;
	return (uint32_t)answer[S09_ERASE_GROUP].result;
}

static uint32_t st_boot_size(void *ctx)
{
	(void)ctx;
	return (uint32_t)answer[S09_BOOT_SIZE].result;
}

static int32_t st_emmc(void *ctx, uint8_t cmd, uint32_t arg0, uint32_t arg1, uint8_t *data, uint32_t n)
{
	const struct answer *a = &answer[cmd];

	(void)ctx;
	(void)arg0;
	(void)arg1;
	store_ops++;
	cmd_ops[cmd]++;
	if (cmd == S09_READ_SECTOR || cmd == S09_READ_SECTORS) {
		memset(data, 0, n);
		memcpy(data, a->data, a->n < n ? a->n : n);
	}
	return a->result;
}

static int32_t st_file(void *ctx, const struct s09_req *r, const char *path, const char *path2,
		       const uint8_t *data, uint32_t n, uint8_t *out, uint32_t *err)
{
	const struct answer *a = &answer[r->cmd];

	(void)ctx;
	(void)path2;
	(void)data;
	(void)n;
	store_ops++;
	cmd_ops[r->cmd]++;
	snprintf(store_path, sizeof store_path, "%s", path ? path : "");
	if (out && a->n)
		memcpy(out, a->data, a->n);
	*err = a->err;
	return a->result;
}

static int32_t st_read(void *ctx, uint32_t fd, uint8_t *b, uint32_t n, uint32_t *err)
{
	uint32_t r;

	(void)ctx;
	(void)fd;
	if (piece++ == fail_piece) {
		*err = S09_ENODEV;
		return -1;
	}
	r = file_pos < file_len ? file_len - file_pos : 0;
	if (r > n)
		r = n;
	memcpy(b, file_data + file_pos, r);
	file_pos += r;
	*err = 0;
	return (int32_t)r;
}

static uint32_t st_os_status(void *ctx)
{
	(void)ctx;
	return (uint32_t)answer[S09_RELEASE].result;
}

static uint64_t st_cycles(void *ctx)
{
	(void)ctx;
	return sample;
}

static void st_adjust(void *ctx, int64_t delta)
{
	(void)ctx;
	(void)delta;
}

static const struct s09_store_ops store = {
	NULL, st_erase_group, st_boot_size, NULL /* dev_cmd */, st_emmc, st_file, st_read, st_os_status, st_cycles, st_adjust
};

/* --- the bus: one session at a time, full duplex, logged --- */

static struct s09_slave slave;
static struct s09_master master;

struct session {
	uint8_t *mosi, *miso;
	size_t n, rx, sess;
	unsigned rung;
	bool late;
};

#define MAX_SESSIONS 4096
#define SESSION_MAX (S09_HDR + 128u * S09_SECTOR + 64u)

static struct session sess[MAX_SESSIONS];
static unsigned nsess;
static bool in_session, moved, reply_taken, cpu_auto;

/* The link modes' bus (above). */
static struct {
	unsigned rung;			/* configure()'s, for the next session */
	unsigned bad, deaf, vbad;	/* rung masks */
	unsigned flip;			/* 0 none; else counts request sessions on a rung */
	unsigned dead_in;		/* 0 none; else sessions until PA6 stays high */
	bool dead;
	bool under;			/* this request session is short of the slave's */
	uint8_t last_cmd;		/* of the last request session */
	unsigned last_rung;
} lk;

/* --- the CPU link over channel 0 (S-09.045, S-09.046) --- */

/*
 * The S-06 frame check (tests/oracle/conformance/serial/crc8.py): poly 0x4D
 * LSB first, whitening 0xA5 per byte.  Test glue, as in h_s06.
 */
static uint8_t crc_table[256];

static void crc_init(void)
{
	for (unsigned i = 0; i < 256; i++) {
		uint8_t c = (uint8_t)(i ^ 0xA5u);

		for (unsigned k = 0; k < 8; k++)
			c = (uint8_t)((c & 1u) ? (c >> 1) ^ 0xB2u : c >> 1);
		crc_table[i] = c;
	}
}

static uint8_t crc8(const uint8_t *data, size_t len)
{
	uint8_t c = 0;

	for (size_t i = 0; i < len; i++)
		c = crc_table[c ^ data[i]];
	return c;
}

static struct s06_node mnode, snode;
static struct s06_link mlink, slink;
static unsigned poweroffs;
static char clocks[512], frames[4096];

static void cpu_power_off(void *ctx)
{
	(void)ctx;
	poweroffs++;
}

static void cpu_clock(void *ctx, uint32_t secs)
{
	size_t n = strlen(clocks);

	(void)ctx;
	snprintf(clocks + n, sizeof clocks - n, "%s%lu", n ? "," : "", (unsigned long)secs);
}

static void cpu_common(void *ctx, const struct s06_frame *f)
{
	size_t n = strlen(frames);

	(void)ctx;
	n += (size_t)snprintf(frames + n, sizeof frames - n, "%s%02x:", n ? "," : "", f->id);
	for (unsigned i = 0; i < f->len && n + 3 < sizeof frames; i++)
		n += (size_t)snprintf(frames + n, sizeof frames - n, "%02x", f->payload[i]);
}

static const struct s09_cpu_ops cpu_ops = { NULL, cpu_power_off, cpu_clock, cpu_common };
static const uint8_t *reply;
static size_t reply_len, reply_pos;
static uint32_t clock_us;

static struct session *cur(void)
{
	return &sess[nsess - 1];
}

static bool b_ready(void *ctx)
{
	(void)ctx;
	if (lk.dead)
		return false;			/* PA6 stays high */
	if (in_session && moved && lk.under)
		return true;			/* the slave waits for more words */
	return !(in_session && moved);	/* PA6 high once the session's bytes moved */
}

/*
 * The slave model of M-DEFER and M-SLOTS: the real slave's finished replies
 * are taken from it as they finish and held; a held reply is shown (PC5 low)
 * once K request sessions have followed the one that finished it, and a poll
 * collects the oldest shown one (FIFO) or the newest (LIFO).
 */
#define HELD 8

static struct {
	bool on, lifo;
	unsigned k, polls, none;
	unsigned nheld;
	struct {
		uint8_t d[S09_SLAVE_SMALL > S09_SLAVE_BIG ? S09_SLAVE_SMALL : S09_SLAVE_BIG];
		size_t n;
		unsigned after;
	} held[HELD];
	uint8_t out[S09_SLAVE_BIG];
	size_t out_n;
	bool out_set;
} model;

static void model_drain(void)
{
	static const uint8_t poll[S09_HDR] = { S09_POLL };

	while (s09_slave_attention(&slave) && model.nheld < HELD) {
		const uint8_t *d;
		size_t n;

		s09_slave_request(&slave, poll, S09_HDR);
		if (!s09_slave_reply(&slave, &d, &n))
			break;
		memcpy(model.held[model.nheld].d, d, n);
		model.held[model.nheld].n = n;
		model.held[model.nheld].after = 0;
		model.nheld++;
	}
}

/* The index of the reply a poll collects now, or -1. */
static int model_pick(void)
{
	int pick = -1;

	for (unsigned i = 0; i < model.nheld; i++)
		if (model.held[i].after >= model.k && (pick < 0 || model.lifo))
			pick = (int)i;
	return pick;
}

/* A request session reached the model: count it, and serve it if a poll. */
static bool model_session(const uint8_t *d, size_t n)
{
	int i;

	for (unsigned j = 0; j < model.nheld; j++)
		model.held[j].after++;
	if (n < S09_HDR || d[0] != S09_POLL)
		return false;
	model.polls++;
	i = model_pick();
	if (i < 0) {
		static const uint8_t none[4] = { 0x06, 0xFF, 0x00, 0x00 };

		model.none++;
		memcpy(model.out, none, sizeof none);
		model.out_n = sizeof none;
	} else {
		memcpy(model.out, model.held[i].d, model.held[i].n);
		model.out_n = model.held[i].n;
		for (unsigned j = (unsigned)i + 1u; j < model.nheld; j++)
			model.held[j - 1u] = model.held[j];
		model.nheld--;
	}
	model.out_set = true;
	return true;
}

/*
 * The hook of two caller requests in flight: an armed call is made from the
 * PC5 read of the call that is waiting, as a second caller would.
 */
#define ARMED 16

static struct {
	unsigned n, next;
	bool running;
	struct s09_req r[ARMED];
	uint8_t pay[ARMED][S09_SECTOR];
	uint32_t plen[ARMED], max[ARMED];
	char out[ARMED][24 + 2u * S09_DIRENT_LEN];
} armed;

static void run_armed(void)
{
	static uint8_t data[ARMED][S09_DIRENT_LEN];
	unsigned i = armed.next++;
	uint32_t got = 0, err = 0;
	int32_t res = s09_master_call(&master, &armed.r[i], armed.pay[i], armed.plen[i], data[i],
				      armed.max[i] < sizeof data[i] ? armed.max[i] : (uint32_t)sizeof data[i],
				      &got, &err);
	size_t o = (size_t)snprintf(armed.out[i], sizeof armed.out[i], "%ld:%lu:", (long)res,
				    (unsigned long)err);

	for (uint32_t j = 0; j < got && o + 3 < sizeof armed.out[i]; j++)
		o += (size_t)snprintf(armed.out[i] + o, sizeof armed.out[i] - o, "%02x", data[i][j]);
	if (got == 0)
		snprintf(armed.out[i] + o, sizeof armed.out[i] - o, "-");
}

static bool b_attention(void *ctx)
{
	(void)ctx;
	if (armed.next < armed.n && !armed.running) {
		armed.running = true;		/* one level: the hook's call makes none */
		run_armed();
		armed.running = false;
	}
	if (lk.dead)
		return false;
	if (!model.on)
		return s09_slave_attention(&slave);
	model_drain();
	return model_pick() >= 0;
}

static void b_select(void *ctx, bool on)
{
	(void)ctx;
	if (on) {
		if (nsess == MAX_SESSIONS) {
			fprintf(stderr, "h_s09: too many sessions\n");
			exit(1);
		}
		nsess++;
		cur()->mosi = malloc(SESSION_MAX);
		cur()->miso = malloc(SESSION_MAX);
		cur()->n = 0;
		cur()->rx = 0;
		cur()->sess = 0;
		cur()->rung = lk.rung;
		cur()->late = false;
		lk.under = false;
		if (lk.dead_in != 0 && --lk.dead_in == 0)
			lk.dead_in = ~0u;	/* dead once this session has ended */
		in_session = true;
		moved = false;
		reply_taken = false;
		return;
	}
	if (!in_session)
		return;
	in_session = false;
	if (lk.under)
		cur()->late = true;
	if (lk.dead_in == ~0u) {
		lk.dead_in = 0;
		lk.dead = true;
	}
	if (!reply_taken && cur()->n >= (slave.clock_second ? 8u : S09_HDR)) {
		const uint8_t *hdr = slave.clock_second ? NULL : cur()->mosi;
		uint64_t want = s09_slave_rx_len(&slave, hdr);
		size_t keep = s09_slave_rx_keep(&slave, hdr);
		static uint8_t heard[SESSION_MAX];
		const bool quad = cur()->rung >= S09_RUNG_QUAD;

		lk.last_cmd = cur()->mosi[0];
		lk.last_rung = cur()->rung;
		cur()->rx = want < cur()->n ? (size_t)want : cur()->n;
		want += s09_slave_rx_trail(&slave, hdr);
		cur()->sess = want < cur()->n ? (size_t)want : cur()->n;
		if (model.on && model_session(cur()->mosi, cur()->rx))
			return;
		/* S-09.074: quad I/O against single, or a rung the wire does not carry */
		if (!slave.clock_second && (quad != (s09_slave_mode(&slave) == S09_MODE_QUAD) ||
					    (lk.deaf >> cur()->rung & 1u))) {
			s09_slave_link_fault(&slave);
			cur()->rx = 0;
			return;
		}
		memcpy(heard, cur()->mosi, cur()->n);
		if (cur()->rung != 0u && ((lk.bad >> cur()->rung & 1u) ||
					  (lk.flip != 0u && --lk.flip == 0u)))
			heard[5] ^= 0x10u;	/* a bit of arg0, after the check was made */
		s09_slave_request(&slave, heard, keep < cur()->rx ? keep : cur()->rx);
		if (cpu_auto)
			s09_slave_cpu_poll(&snode, S06_LINK_CPU, &cpu_ops);
		if (model.on)
			model_drain();
	}
}

static bool b_send(void *ctx, const uint8_t *d, size_t n)
{
	struct session *s = cur();

	(void)ctx;
	if (s->n + n > SESSION_MAX)
		return false;
	if (s->n == 0 && n >= S09_HDR && !slave.clock_second && !model.on && !lk.dead &&
	    (s->rung >= S09_RUNG_QUAD) == (s09_slave_mode(&slave) == S09_MODE_QUAD) &&
	    !(lk.deaf >> s->rung & 1u)) {
		/* the slave's session: its request and S-09.021's further bytes */
		const uint64_t want = s09_slave_rx_len(&slave, d) + s09_slave_rx_trail(&slave, d);

		if (n > want) {
			/* the slave's line rose after its last word: the master
			 * is held, its wait runs out (S-09.013) */
			memcpy(s->mosi, d, (size_t)want);
			memset(s->miso, 0, (size_t)want);
			s->n = (size_t)want;
			s->late = true;
			moved = true;
			return false;
		}
		lk.under = n < want;
	}
	memcpy(s->mosi + s->n, d, n);
	memset(s->miso + s->n, 0, n);
	s->n += n;
	moved = true;
	return true;
}

static bool b_recv(void *ctx, uint8_t *d, size_t n)
{
	struct session *s = cur();

	(void)ctx;
	if (s->n + n > SESSION_MAX)
		return false;
	if (!reply_taken) {
		reply_taken = true;
		reply_pos = 0;
		if (model.out_set) {
			model.out_set = false;
			reply = model.out;
			reply_len = model.out_n;
		} else if (!s09_slave_reply(&slave, &reply, &reply_len)) {
			reply_len = 0;
		}
		if (reply_len >= 8u && reply[0] == S09_LINK_TEST && lk.last_cmd == S09_LINK_TEST &&
		    (lk.vbad >> lk.last_rung & 1u)) {
			static uint8_t bent[12];

			memcpy(bent, reply, reply_len < 12u ? reply_len : 12u);
			bent[4] ^= 0x01u;	/* a bit of the value */
			/* the value wrong as the slave gave it: its check
			 * word, outside the plain mode, right (S-09.072) */
			if (reply_len >= 12u)
				s06_wr_le32(bent + 8, s09_check_word(bent, 8));
			reply = bent;
		}
	}
	for (size_t i = 0; i < n; i++)
		d[i] = reply_pos < reply_len ? reply[reply_pos++] : 0;
	memset(s->mosi + s->n, 0, n);
	memcpy(s->miso + s->n, d, n);
	s->n += n;
	moved = true;
	return true;
}

static void b_configure(void *ctx, int phase)
{
	(void)ctx;
	lk.rung = phase > 1 ? (unsigned)phase - 1u : 0u;	/* S-09.075 */
}

static const struct s09_bus_ops bus = {
	NULL, b_ready, b_attention, b_select, b_send, b_recv, b_configure, NULL,
};

static uint32_t c_now(void *ctx)
{
	(void)ctx;
	return clock_us++;
}

static void c_idle(void *ctx, uint32_t until)
{
	(void)ctx;
	if ((int32_t)(until - clock_us) > 0)
		clock_us = until;
}

static const struct s06_clock clk = { c_now, c_idle, NULL };

/* --- the boot link (B-STREAM) --- */

#define BOOT_LOG 0x4000u
#define BOOT_SESSIONS 256u

static struct {
	int mode;
	uint32_t rng, rise_at, ready_at, last_t, release_t, late, settle;
	unsigned long bytes, stuck, high, early;
	bool on;
	uint8_t log[BOOT_LOG];
	size_t n, start[BOOT_SESSIONS + 1u];
	unsigned nsess;
} bl;

static uint32_t bl_rand(void)
{
	bl.rng ^= bl.rng << 13;
	bl.rng ^= bl.rng >> 17;
	bl.rng ^= bl.rng << 5;
	return bl.rng;
}

static bool bl_ready(void *ctx)
{
	(void)ctx;
	if (bl.mode == 1 || (bl.mode != 4 && bl.stuck && bl.bytes >= bl.stuck))
		return false;
	if (bl.mode == 3 && bl.bytes > 1)
		return true;		/* PA6 never rises after the 0F bytes */
	if ((int32_t)(clock_us - bl.rise_at) < 0)
		return true;		/* the slave has not raised it yet */
	return (int32_t)(clock_us - bl.ready_at) >= 0;
}

static void bl_select(void *ctx, bool on)
{
	(void)ctx;
	if (on && !bl.on && bl.nsess < BOOT_SESSIONS)
		bl.start[bl.nsess++] = bl.n;
	if (!on && bl.on)
		bl.release_t = clock_us;
	bl.on = on;
}

static uint8_t bl_byte(void *ctx, uint8_t tx)
{
	if (!bl_ready(ctx))
		bl.high++;
	else if (bl.bytes != 0 && (int32_t)(clock_us - bl.rise_at) < 0)
		bl.early++;
	if (bl.n < BOOT_LOG)
		bl.log[bl.n++] = tx;
	bl.bytes++;
	bl.last_t = clock_us;
	bl.rise_at = clock_us + bl.late;
	bl.ready_at = bl.rise_at + 1u + bl_rand() % 51u;
	if (bl.mode == 4 && bl.bytes == bl.stuck)
		bl.ready_at = bl.rise_at + S06_MS(900);
	if (bl.bytes == 1)
		return bl.mode == 2 || bl.mode == 3 ? 0x00 : 0xFF;
	return 0;
}

/* The ops' byte: back once the byte is in and the settle time has passed. */
static uint8_t bl_xfer(void *ctx, uint8_t tx)
{
	const uint8_t rx = bl_byte(ctx, tx);

	clock_us += bl.settle;
	return rx;
}

/* BOOT_LINK_FC: the controller under flow control. It looks at PA6 as each
 * byte ends and starts the next at once while it reads low; a byte held for
 * T_STREAM fails the session, as the controller's word timeout does. */
static bool bl_send(void *ctx, const uint8_t *d, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		const uint32_t from = clock_us;

		while (!bl_ready(ctx)) {
			if (clock_us - from >= S09_T_STREAM_US)
				return false;
			clock_us++;
		}
		(void)bl_byte(ctx, d[i]);
	}
	return true;
}

static const struct s09_boot_ops boot_ops = { NULL, bl_ready, bl_select, bl_xfer, NULL, NULL };
static const struct s09_boot_ops boot_ops_fc = { NULL, bl_ready, bl_select, bl_xfer, bl_send, NULL };

static void do_boot(const struct h_cmd *c)
{
	static uint8_t slotb[0x10000];
	FILE *f = fopen(h_str(c, 1), "rb");
	size_t len;
	uint32_t t0;
	int end, rc;
	char s[64];

	if (!f) {
		h_error(c, "cannot open");
		return;
	}
	len = fread(slotb, 1, sizeof slotb, f);
	fclose(f);
	memset(&bl, 0, sizeof bl);
	bl.mode = (int)h_num(c, 3);
	bl.rng = (uint32_t)h_num(c, 4) | 1u;
	bl.stuck = h_num(c, 5);
	bl.late = c->n >= 8 ? (uint32_t)h_num(c, 7) : 0u;
	bl.settle = c->n == 9 ? (uint32_t)h_num(c, 8) : 0u;
	bl.rise_at = bl.ready_at = clock_us;
	t0 = clock_us;
	rc = s09_boot_slave(c->n >= 8 && h_num(c, 6) != 0 ? &boot_ops_fc : &boot_ops, &clk, slotb,
			    len, h_num(c, 2) != 0, &end);
	snprintf(s, sizeof s, "%d %d %lu %ld %lu", rc, end, bl.high,
		 bl.nsess ? (long)(bl.release_t - bl.last_t) : -1L, (unsigned long)(clock_us - t0));
	h_put(s);
	if (c->n >= 8)
		h_putu(bl.early);
	bl.start[bl.nsess] = bl.n;
	for (unsigned i = 0; i < bl.nsess; i++)
		h_puthex(bl.log + bl.start[i], bl.start[i + 1u] - bl.start[i]);
	h_end();
}

static void put_hex_field(const uint8_t *p, size_t n);

static void do_sreq(const struct h_cmd *c)
{
	static uint8_t req[S09_HDR + 128u * S09_SECTOR + 64u];
	size_t n = h_hex(c, 1, req, sizeof req), keep = 0;
	uint64_t want = 0;
	const uint8_t *d;
	size_t dn;

	if (slave.clock_second || n >= S09_HDR) {
		const uint8_t *hdr = slave.clock_second ? NULL : req;

		want = s09_slave_rx_len(&slave, hdr);
		keep = s09_slave_rx_keep(&slave, hdr);
	}
	if (keep > sizeof req) {
		h_error(c, "request too long");
		return;
	}
	if (keep > n)
		memset(req + n, 0, keep - n);
	s09_slave_request(&slave, req, keep);
	h_putu((unsigned long)want);
	if (s09_slave_reply(&slave, &d, &dn))
		h_puthex(d, dn);
	else
		h_put("-");
	h_end();
}

/* The CPU-link commands; false: not one of them. */
static bool do_cpu(const struct h_cmd *c, const char *k)
{
	size_t n;

	if (strcmp(k, "cpu") == 0 && c->n == 2) {
		crc_init();
		s06_node_init(&mnode, crc8, &clk);
		s06_node_init(&snode, crc8, &clk);
		s06_link_init(&mlink, &s09_tunnel_port, &master, S06_ADDR_MASTER, S06_ADDR_SLAVE);
		s06_link_init(&slink, &s09_slave_tunnel_port, &slave, S06_ADDR_SLAVE, S06_ADDR_MASTER);
		mnode.link[S06_LINK_CPU] = &mlink;
		snode.link[S06_LINK_CPU] = &slink;
		cpu_auto = h_num(c, 1) != 0;
		poweroffs = 0;
		clocks[0] = frames[0] = '\0';
		h_put("ok");
	} else if (strcmp(k, "msend") == 0 && c->n == 4) {
		n = h_hex(c, 3, buf, sizeof buf);
		h_putu(s06_send(&mnode, S06_LINK_CPU, (uint8_t)h_num(c, 2), (uint8_t)h_num(c, 1), buf, n));
	} else if (strcmp(k, "mwrite") == 0 && c->n == 2) {
		size_t at = 0;

		n = h_hex(c, 1, buf, sizeof buf);
		while (at < n) {
			size_t room = s09_tunnel_port.room(&master), k2 = n - at < room ? n - at : room;

			if (k2 == 0)
				break;
			s09_tunnel_port.write(&master, buf + at, k2);
			at += k2;
		}
		h_putu(at);
	} else if (strcmp(k, "mtake") == 0) {
		struct s06_frame f;

		s06_node_poll_link(&mnode, S06_LINK_CPU);
		while (s06_link_take(&mlink, &f)) {
			char s[16];

			snprintf(s, sizeof s, "%02x:%02x:", f.id, f.route);
			h_put(s);
			put_hex_field(f.payload, f.len);
		}
		h_put(";");
	} else if (strcmp(k, "mclock") == 0 && c->n == 2) {
		static struct s09_clock_msg cm;

		s09_clock_message(&cm, &mnode, S06_LINK_CPU, (uint32_t)h_num(c, 1));
		h_put("ok");
	} else if (strcmp(k, "poweroff") == 0) {
		uint32_t t0 = clock_us;
		bool ok = s09_power_off(&master, &mnode, S06_LINK_CPU);

		h_putu(ok);
		h_putu(clock_us - t0);
	} else if (strcmp(k, "spoll") == 0) {
		s09_slave_cpu_poll(&snode, S06_LINK_CPU, &cpu_ops);
		h_put("ok");
	} else if (strcmp(k, "scpu") == 0) {
		h_putu(poweroffs);
		h_put(clocks[0] ? clocks : "-");
		h_put(frames[0] ? frames : "-");
	} else if (strcmp(k, "srx") == 0) {
		n = s09_slave_tunnel_port.read(&slave, buf, sizeof buf);
		if (n)
			h_puthex(buf, n);
		else
			h_put("-");
	} else {
		return false;
	}
	h_end();
	return true;
}

static void put_hex_field(const uint8_t *p, size_t n)
{
	if (n == 0)
		putchar('-');
	for (size_t i = 0; i < n; i++)
		printf("%02x", p[i]);
}

static void do_sessions(void)
{
	for (unsigned i = 0; i < nsess; i++) {
		h_put("");
		put_hex_field(sess[i].mosi, sess[i].n);
		putchar(':');
		put_hex_field(sess[i].miso, sess[i].n);
		printf(":%zu:%zu:%u:%d", sess[i].rx, sess[i].sess, sess[i].rung, sess[i].late ? 1 : 0);
		free(sess[i].mosi);
		free(sess[i].miso);
	}
	nsess = 0;
	h_end();
}

static void do_records(const struct h_cmd *c, const char *k)
{
	uint8_t out[S09_DIRENT_LEN];

	if (strcmp(k, "stat") == 0 && c->n == 14) {
		struct s09_stat s;
		uint32_t *f[13] = { &s.dev, &s.ino, &s.mode, &s.nlink, &s.uid, &s.gid, &s.rdev,
				    &s.size, &s.blksize, &s.blocks, &s.atime, &s.mtime, &s.ctime };

		for (int i = 0; i < 13; i++)
			*f[i] = (uint32_t)h_num(c, i + 1);
		s09_stat_pack(&s, out);
		h_puthex(out, S09_STAT_LEN);
	} else if (strcmp(k, "statfs") == 0 && c->n == 4) {
		struct s09_statfs s = { (uint32_t)h_num(c, 1), (uint32_t)h_num(c, 2), (uint32_t)h_num(c, 3) };

		s09_statfs_pack(&s, out);
		h_puthex(out, S09_STATFS_LEN);
	} else if (strcmp(k, "dirent") == 0 && c->n == 5) {
		struct s09_dirent d;
		size_t n;

		memset(&d, 0, sizeof d);
		d.ino = (uint32_t)h_num(c, 1);
		d.off = (uint32_t)h_num(c, 2);
		d.type = (uint8_t)h_num(c, 3);
		n = h_hex(c, 4, (uint8_t *)d.name, sizeof d.name - 1);
		d.name[n] = '\0';
		s09_dirent_pack(&d, out);
		h_puthex(out, S09_DIRENT_LEN);
	} else {
		h_error(c, "unknown command");
		return;
	}
	h_end();
}

/* The link modes' commands (S-09.070-S-09.075); false: not one of them. */
static bool do_link(const struct h_cmd *c, const char *k)
{
	static uint8_t req[S09_HDR + 128u * S09_SECTOR + 64u];
	static size_t kept;
	const uint8_t *d;
	size_t n, dn;

	if (strcmp(k, "sput") == 0 && c->n == 2) {
		if (strcmp(c->tok[1], "-") == 0)
			kept = 0;
		else
			kept += h_hex(c, 1, req + kept, sizeof req - kept);
		h_put("ok");
		h_end();
		return true;
	}
	if (strcmp(k, "sgo") == 0) {
		h_putu(kept >= S09_HDR ? (unsigned long)s09_slave_rx_len(&slave, req) : 0ul);
		s09_slave_request(&slave, req, kept);
		if (s09_slave_reply(&slave, &d, &dn))
			h_puthex(d, dn);
		else
			h_put("-");
		kept = 0;
		h_end();
		return true;
	}

	if (strcmp(k, "link") == 0 && c->n == 2) {
		h_putu(s09_master_link_fast(&master, (unsigned)h_num(c, 1)));
		h_putu(s09_master_rung(&master));
		h_putu(s09_master_fell(&master));
	} else if (strcmp(k, "mrung") == 0) {
		h_putu(s09_master_rung(&master));
		h_putu(s09_master_fell(&master));
		h_putu(s09_master_write_max(&master));
		h_putu(master.absent);
	} else if (strcmp(k, "wire") == 0 && c->n == 4) {
		lk.bad = (unsigned)h_num(c, 1);
		lk.deaf = (unsigned)h_num(c, 2);
		lk.vbad = (unsigned)h_num(c, 3);
		h_put("ok");
	} else if (strcmp(k, "flip") == 0 && c->n == 2) {
		lk.flip = (unsigned)h_num(c, 1);
		h_put("ok");
	} else if (strcmp(k, "dead") == 0 && c->n == 2) {
		lk.dead_in = (unsigned)h_num(c, 1);
		lk.dead = lk.dead_in == 0u;
		h_put("ok");
	} else if (strcmp(k, "smode") == 0) {
		h_putu(s09_slave_mode(&slave));
		h_putu(s09_slave_on_trial(&slave));
		h_putu(s09_slave_attention(&slave));
	} else if (strcmp(k, "sfault") == 0) {
		s09_slave_link_fault(&slave);
		h_put("ok");
	} else if (strcmp(k, "sraw") == 0 && c->n == 2) {
		n = h_hex(c, 1, req, sizeof req);
		h_putu(n >= S09_HDR ? (unsigned long)s09_slave_rx_len(&slave, req) : 0ul);
		s09_slave_request(&slave, req, n);
		if (s09_slave_reply(&slave, &d, &dn))
			h_puthex(d, dn);
		else
			h_put("-");
	} else if (strcmp(k, "sops") == 0 && c->n == 2) {
		h_putu(cmd_ops[h_num(c, 1) & 0xFFu]);
		cmd_ops[h_num(c, 1) & 0xFFu] = 0;
	} else {
		return false;
	}
	h_end();
	return true;
}

int main(void)
{
	struct h_cmd c;
	static uint8_t data[S09_HDR + 128u * S09_SECTOR];

	while (h_next(&c)) {
		const char *k = c.tok[0];

		if (strcmp(k, "rig") == 0) {
			memset(answer, 0, sizeof answer);
			s09_slave_init(&slave, &store);
			s09_master_init(&master, &bus, &clk);
			fail_piece = -1;
			cpu_auto = false;
			memset(&model, 0, sizeof model);
			memset(&armed, 0, sizeof armed);
			memset(&lk, 0, sizeof lk);
			memset(cmd_ops, 0, sizeof cmd_ops);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "store") == 0 && c.n == 5) {
			struct answer *a = &answer[h_num(&c, 1) & 0xFFu];

			a->set = true;
			a->result = (int32_t)(long)h_num(&c, 2);
			a->err = (uint32_t)h_num(&c, 3);
			a->n = (uint32_t)h_hex(&c, 4, a->data, sizeof a->data);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "sample") == 0 && c.n == 2) {
			sample = (uint64_t)h_num(&c, 1);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "file") == 0 && c.n == 4) {
			file_len = (uint32_t)h_hex(&c, 1, file_data, sizeof file_data);
			file_pos = (uint32_t)h_num(&c, 2);
			fail_piece = (int)(long)h_num(&c, 3);
			piece = 0;
			h_put("ok");
			h_end();
		} else if (strcmp(k, "tunnel") == 0 && c.n == 2) {
			size_t n = h_hex(&c, 1, buf, sizeof buf);

			s09_slave_tunnel_port.write(&slave, buf, n);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "start") == 0) {
			h_putu(s09_master_start(&master));
			h_end();
		} else if (strcmp(k, "call") == 0 && c.n == 7) {
			struct s09_req r;
			uint32_t got = 0, err = 0;
			size_t plen;
			uint32_t max = (uint32_t)h_num(&c, 6);
			char s[24];

			if (strncmp(c.tok[5], "r7:", 3) == 0) {
				plen = strtoul(c.tok[5] + 3, NULL, 0);
				if (plen > sizeof pay) {
					h_error(&c, "payload too long");
					continue;
				}
				for (size_t i = 0; i < plen; i++)
					pay[i] = (uint8_t)(i * 7u);
			} else {
				plen = h_hex(&c, 5, pay, sizeof pay);
			}
			r.cmd = (uint8_t)h_num(&c, 1);
			r.slot = 0;
			r.len = (uint16_t)h_num(&c, 2);
			r.arg0 = (uint32_t)h_num(&c, 3);
			r.arg1 = (uint32_t)h_num(&c, 4);
			snprintf(s, sizeof s, "%ld", (long)s09_master_call(&master, &r, pay, (uint32_t)plen, data,
									    max < sizeof data ? max : sizeof data,
									    &got, &err));
			h_put(s);
			h_putu(err);
			h_puthex(data, got);
			h_end();
		} else if (strcmp(k, "read") == 0 && c.n == 3) {
			uint32_t err = 0, n = (uint32_t)h_num(&c, 2);
			int32_t got;
			char s[24];

			if (n > sizeof data)
				n = sizeof data;
			got = s09_master_read(&master, (uint32_t)h_num(&c, 1), data, n, &err);
			snprintf(s, sizeof s, "%ld", (long)got);
			h_put(s);
			h_putu(err);
			h_puthex(data, got > 0 ? (size_t)got : 0);
			h_end();
		} else if (strcmp(k, "service") == 0) {
			s09_master_service(&master);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "release") == 0) {
			char s[16];

			snprintf(s, sizeof s, "%d", s09_master_release(&master));
			h_put(s);
			h_end();
		} else if (strcmp(k, "inject") == 0 && c.n == 3) {
			static uint8_t req[S09_HDR + 128u * S09_SECTOR + 64u];
			uint8_t poll[S09_HDR] = { S09_POLL };
			size_t n = h_hex(&c, 1, req, sizeof req), keep;
			uint64_t want;
			const uint8_t *d;
			size_t dn;

			store_ops = 0;
			store_path[0] = '\0';
			want = n >= S09_HDR ? s09_slave_rx_len(&slave, req) : 0;
			keep = n >= S09_HDR ? s09_slave_rx_keep(&slave, req) : 0;
			if (keep > sizeof req) {
				h_error(&c, "request too long");
				continue;
			}
			if (keep > n)
				memset(req + n, 0, keep - n);
			s09_slave_request(&slave, req, keep);
			h_putu((unsigned long)want);
			h_putu(store_ops);
			if (store_path[0])
				h_puthex((const uint8_t *)store_path, strlen(store_path));
			else
				h_put("-");
			poll[1] = (uint8_t)h_num(&c, 2);
			if (s09_slave_attention(&slave)) {
				s09_slave_request(&slave, poll, s09_slave_rx_len(&slave, poll));
				if (s09_slave_reply(&slave, &d, &dn))
					h_puthex(d, dn);
				else
					h_put("-");
			} else {
				h_put("-");
			}
			h_end();
		} else if (do_link(&c, k)) {
			continue;
		} else if (strcmp(k, "sessions") == 0) {
			do_sessions();
		} else if ((strcmp(k, "boot") == 0 && c.n == 6) ||
			   (strcmp(k, "bootx") == 0 && (c.n == 8 || c.n == 9))) {
			do_boot(&c);
		} else if (strcmp(k, "sreq") == 0 && c.n == 2) {
			do_sreq(&c);
		} else if (strcmp(k, "satt") == 0) {
			h_putu(s09_slave_attention(&slave));
			h_end();
		} else if (strcmp(k, "stake") == 0) {
			uint32_t v;
			bool took = s09_slave_take_release(&slave, &v);

			h_putu(took);
			if (took)
				h_putu(v);
			h_end();
		} else if (do_cpu(&c, k)) {
			continue;
		} else if (strcmp(k, "model") == 0 && c.n == 3) {
			model.k = (unsigned)h_num(&c, 1);
			model.on = model.k != 0;
			model.lifo = strcmp(h_str(&c, 2), "lifo") == 0;
			h_put("ok");
			h_end();
		} else if (strcmp(k, "modelstat") == 0) {
			h_putu(model.polls);
			h_putu(model.none);
			h_putu(model.nheld);
			h_end();
		} else if (strcmp(k, "nest") == 0 && c.n == 7 && armed.n < ARMED) {
			unsigned i = armed.n++;

			armed.r[i].cmd = (uint8_t)h_num(&c, 1);
			armed.r[i].slot = 0;
			armed.r[i].len = (uint16_t)h_num(&c, 2);
			armed.r[i].arg0 = (uint32_t)h_num(&c, 3);
			armed.r[i].arg1 = (uint32_t)h_num(&c, 4);
			armed.plen[i] = (uint32_t)h_hex(&c, 5, armed.pay[i], sizeof armed.pay[i]);
			armed.max[i] = (uint32_t)h_num(&c, 6);
			h_put("ok");
			h_end();
		} else if (strcmp(k, "nested") == 0) {
			for (unsigned i = 0; i < armed.next; i++)
				h_put(armed.out[i]);
			h_put(";");
			h_end();
		} else {
			do_records(&c, k);
		}
	}
	return h_status();
}
