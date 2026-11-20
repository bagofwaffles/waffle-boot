/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A plain start's load through the bridge, end to end on the host: the
 * master's s09_master_start(), 0x05, then s03_load_os() over the board's
 * eMMC ops (opxy_bridge_emmc()), in the plain mode as a boot runs it; on
 * the far side a real s09_slave whose eMMC commands go through the board's
 * opxy_slave_emmc() to an eMMC model in RAM, with yaffs2 on its user area.
 * After the load the bridge must still serve: a mount (0x08), a file
 * written and read back, then the release (0x50) answered 0.
 *
 * The slave runs as the board's serve loop does (opxy_slave.c): it takes a
 * request session whole, raises PA6 and executes it before it looks at
 * anything else, so PA6 stays high and PB2 shows what it showed before the
 * session until the work is done; only then is PB2 set again and the slave
 * ready for the next session. Its work takes time: the first eMMC command
 * turns the features on (S-03.009, seconds at worst), a 128-sector read
 * takes milliseconds, the mount and the release's own load longer; the
 * wire takes 1.28 us a word (S-09.075's plain word). The slave's CPU link
 * may hand the tunnel a frame during the load, which finishes the parked
 * pull (S-09.045) and lowers PB2 while a read is under way.
 *
 * Covered: S-03.040-S-03.046 (BP1 inactive, BP2 active, as on a unit that
 * wrote BP2), S-09.023's 128-sector limit at the request boundaries,
 * S-09.031's polls among a finished pull, S-09.060's bounds, S-09.052;
 * S-09.013 rule 4 with S-03.010's retries: a read or a select whose plain
 * exchange failed costs that request, not the slave (a reply that lost a
 * received word, as the slave's driver ends it now and as it did; a reply
 * broken once and three times; a read taken whole whose session's end was
 * not seen; a select's reply broken), and a write's still costs the slave.
 */
#include <stdlib.h>
#include <string.h>

#include "s06_test.h"
#include "s06_fake.h"
#include "support/fake_core.h"
#include "waffle/board/opxy_bridge.h"
#include "waffle/board/opxy_ops.h"
#include "waffle/s03_emmc.h"
#include "waffle/s06_bytes.h"
#include "waffle/s09_files.h"
#include "waffle/s09_master.h"
#include "waffle/s09_slave.h"
#include "waffle/wire_crc.h"
#include "waffle/wire_s09.h"
#include "waffle/wire_yaffs.h"
#include "yaffsfs.h"

#define BLOCKS 40u			/* the yaffs2 volume on the user area */
#define VOL_LEN ((size_t)BLOCKS * 128u * 4096u)
#define B_SIZE (16u * 1024u * 1024u)	/* EXT_CSD[226] 0x80 */
#define PLAIN_WORD_NS 1280u		/* 32 bits at 25 MHz */

/* ---- the slave's eMMC: two boot partitions and the user area ---- */

static uint8_t *bp[2];
static uint8_t *vol;

static struct {
	unsigned part;
	unsigned reads, max_count;
	bool failed_once;
	bool taint;		/* the next read's data marked: a stale reply */
} mm;

/* What the slave's work costs, in us, for the request under way. */
static uint64_t cost_us;

static struct {
	uint32_t features_us;		/* S-03.009's switches, at the first use */
	uint32_t read_base_us, read_sector_ns;
	uint32_t mount_us, release_us;
	uint32_t usb_us;		/* the master's every wait (clock_idle) */
	uint32_t tunnel_at_us;		/* a CPU-link frame from the slave; 0 none */
	uint32_t tunnel_every_us;	/* ... and again so often; 0 once */
	bool slow_multi;		/* a multi-block read fails once, slowly */
	uint8_t die_cmd;		/* the slave stops after executing this ... */
	unsigned die_nth;		/* ... request for the nth time; 0 never */
	/* the reply sessions of 128-sector reads (counted from 1) from
	 * big_nth on, big_count of them: the slave lost a received word in
	 * it and holds its line low hold_us after the last word (S-13b.045),
	 * or (hold_us 0) the session breaks half-way */
	unsigned big_nth, big_count;
	uint32_t hold_us;
	unsigned late_nth;		/* that 128-sector read's request session:
					 * PA6 never rises, the slave took it whole */
	uint8_t break_cmd;		/* that request's deferred reply breaks
					 * after its first word, break_n times */
	unsigned break_n;
} tm;

static uint8_t *area(uint32_t sector, uint32_t count)
{
	const uint64_t off = (uint64_t)sector * S03_SECTOR;
	const uint64_t n = (uint64_t)count * S03_SECTOR;

	if (mm.part == S03_PART_USER)
		return off + n <= VOL_LEN ? vol + off : NULL;
	return off + n <= B_SIZE ? bp[mm.part - 1u] + off : NULL;
}

static int em_select(void *ctx, unsigned part)
{
	(void)ctx;
	if (part > S03_PART_BP2)
		return -1;
	mm.part = part;
	cost_us += 300u;
	return 0;
}

static int em_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	const uint8_t *p = area(sector, count);

	(void)ctx;
	mm.reads++;
	if (count > mm.max_count)
		mm.max_count = count;
	cost_us += tm.read_base_us + (uint64_t)count * tm.read_sector_ns / 1000u;
	if (tm.slow_multi && count > 1u && !mm.failed_once) {
		/* a multi-block read that runs out of time: the driver's CMD12
		 * path, then S-03.010's retry */
		mm.failed_once = true;
		cost_us += 250000u;
		return -1;
	}
	if (p == NULL)
		return -1;
	memcpy(buf, p, (size_t)count * S03_SECTOR);
	if (mm.taint) {
		mm.taint = false;
		buf[100] ^= 0x5Au;
	}
	return 0;
}

static int em_write(void *ctx, uint32_t sector, uint32_t count, const uint8_t *buf)
{
	uint8_t *p = area(sector, count);

	(void)ctx;
	cost_us += 500u + (uint64_t)count * 40u;
	if (p == NULL)
		return -1;
	memcpy(p, buf, (size_t)count * S03_SECTOR);
	return 0;
}

static int em_erase(void *ctx, uint32_t first)
{
	uint8_t *p = area(first, S03_GROUP_SECTORS);

	(void)ctx;
	cost_us += 2000u;
	if (p == NULL)
		return -1;
	memset(p, 0, (size_t)S03_GROUP_SECTORS * S03_SECTOR);
	return 0;
}

static int em_switch(void *ctx, uint8_t index, uint8_t value)
{
	(void)ctx;
	(void)index;
	(void)value;
	cost_us += 1000u;
	return 0;
}

static int em_erase_arg(void *ctx, uint32_t first, uint32_t arg)
{
	(void)arg;
	return em_erase(ctx, first);
}

static int em_write_reliable(void *ctx, uint32_t sector, uint32_t count,
			     const uint8_t *buf)
{
	return em_write(ctx, sector, count, buf);
}

static const struct s03_emmc_ops em_ops = {
	NULL, em_select, em_read, em_write, em_erase, em_switch,
};
static const struct opxy_emmc_more em_more = { em_erase_arg, em_write_reliable };

/* ---- the slave's store, as opxy_slave.c's ---- */

#define PART_UNKNOWN 0xFFu

static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static struct wire_yaffs Y;
static struct wire_s09 S;
static bool features;
static unsigned sl_part;		/* the slave's own idea (opxy_slave.c) */

static bool features_ready(void)
{
	if (!features) {
		cost_us += tm.features_us;
		features = true;
	}
	return features;
}

static int32_t st_emmc(void *c, uint8_t cmd, uint32_t a0, uint32_t a1,
		       uint8_t *data, uint32_t n)
{
	int32_t rc;

	(void)c;
	if (!features_ready())
		return -1;
	if (sl_part == PART_UNKNOWN && cmd != S09_SELECT && cmd != S09_FLUSH)
		return -1;
	rc = opxy_slave_emmc(&em_ops, &em_more, cmd, a0, a1, data, n);
	if (cmd == S09_SELECT && a0 <= S03_PART_BP2)
		sl_part = rc == 0 ? a0 : PART_UNKNOWN;
	return rc;
}

static uint32_t st_erase_group(void *c) { (void)c; return S03_GROUP_SECTORS; }
static uint32_t st_boot_size(void *c) { (void)c; return B_SIZE; }
static uint64_t st_cycles(void *c) { (void)c; return 0; }
static void st_adjust(void *c, int64_t d) { (void)c; (void)d; }

/* S-09.082: the slave's own staging area, and its last file start's copy */
static uint8_t *sl_staging;
static enum wire_os_file sl_file;

static uint32_t st_os(void *c, uint32_t len, uint32_t crc)
{
	(void)c;
	(void)features_ready();
	cost_us += tm.release_us;
	if (len == 0u)
		return 0;		/* the slave's own OS staged and accepted */
	/* as opxy_slave.c's stage_file(), S-05.024's check apart */
	sl_file = sl_part != S03_PART_USER ? WIRE_OS_FILE_UNREAD :
		  wire_s09_os_file(&S, wire_crc32, NULL, sl_staging, S03_OS_MAX,
				   len, crc);
	return sl_file == WIRE_OS_FILE_OK ? 0u : 1u;
}

static int32_t st_file(void *c, const struct s09_req *r, const char *p1,
		       const char *p2, const uint8_t *d, uint32_t n, uint8_t *out,
		       uint32_t *err)
{
	(void)c;
	if (r->cmd == S09_MOUNT) {
		(void)features_ready();
		cost_us += tm.mount_us;
	} else {
		cost_us += 400u;
	}
	if (sl_part != S03_PART_USER) {
		*err = S09_ENODEV;
		return r->cmd == S09_OPENDIR || r->cmd == S09_READDIR ||
		       r->cmd == S09_FREE_SPACE || r->cmd == S09_TOTAL_SPACE ? 0 : -1;
	}
	return wire_s09_file(&S, r, p1, p2, d, n, out, err);
}

static int32_t st_read(void *c, uint32_t fd, uint8_t *buf, uint32_t n, uint32_t *err)
{
	(void)c;
	cost_us += 800u;
	if (sl_part != S03_PART_USER) {
		*err = S09_ENODEV;
		return -1;
	}
	return wire_s09_read(&S, fd, buf, n, err);
}

static const struct s09_store_ops store = {
	0, st_erase_group, st_boot_size, NULL /* dev_cmd */, st_emmc, st_file,
	st_read, st_os, st_cycles, st_adjust,
};

static struct s09_slave sl;

/* ---- the slave's serve loop in time, and the bus between them ---- */

static struct t_sim sim;

enum { SV_DOWN, SV_LISTEN, SV_RX, SV_EXEC, SV_TX, SV_GONE };

static struct {
	int st;
	uint32_t up_at, busy_until;
	bool pb2_low;		/* PB2 as last set, outside the listen */
	bool sel;
	uint8_t req[S09_SLAVE_BIG + 64u];
	size_t req_n;
	bool rx_done;
	const uint8_t *rep;
	size_t rep_n, rep_at;
	uint64_t ns_owed;
	uint32_t tunnel_next;
	unsigned frames;
	bool released;
	unsigned seen_die_cmd;
	bool dying;
	/* what the master did that the slave could not take */
	unsigned stray_sends, stray_recvs, stray_selects;
	unsigned sessions, polls, pulls, poll_reads;
	unsigned execs[256];	/* requests executed, by command */
	unsigned bigs, big_reqs;	/* 128-sector read replies, requests */
	bool hold_now, half_now, late_now;
	uint32_t last_word_at;
	/* the last sessions, for a failure's account */
	struct {
		uint32_t t;
		uint8_t cmd, kind;	/* kind: 0 request, 1 reply */
		uint32_t len;
	} trace[16];
	unsigned ntrace;
} sv;

/* Every session's bytes, hashed: the same wire, the same value. */
static uint32_t wire_hash;

static void hash(const uint8_t *d, size_t n, unsigned tag)
{
	uint32_t h = wire_hash ^ tag;

	for (size_t i = 0; i < n; i++)
		h = (h ^ d[i]) * 16777619u;
	wire_hash = h;
}

static void trace(uint8_t cmd, uint8_t kind, size_t len)
{
	unsigned i = sv.ntrace++ % 16u;

	sv.trace[i].t = sim.now;
	sv.trace[i].cmd = cmd;
	sv.trace[i].kind = kind;
	sv.trace[i].len = (uint32_t)len;
}

static void trace_dump(void)
{
	unsigned first = sv.ntrace > 16u ? sv.ntrace - 16u : 0u;

	for (unsigned i = first; i < sv.ntrace; i++)
		fprintf(stderr, "  %10u us  %s 0x%02x  %u bytes\n", sv.trace[i % 16u].t,
			sv.trace[i % 16u].kind ? "reply  " : "request", sv.trace[i % 16u].cmd,
			sv.trace[i % 16u].len);
}

/* The wire's time for n bytes. */
static void wire(size_t n)
{
	sv.ns_owed += (uint64_t)(n / 4u) * PLAIN_WORD_NS;
	sim.now += (uint32_t)(sv.ns_owed / 1000u);
	sv.ns_owed %= 1000u;
}

/* The CPU link's frame, written to the tunnel (s06_node_poll() on the
 * slave's loop): it finishes a parked pull. */
static void tunnel_frame(void)
{
	static const uint8_t frame[9] = { 0x7E, 0x21, 0x12, 0x02, 0x00, 0x14, 0x00, 0x5A, 0x7E };

	if (tm.tunnel_at_us == 0u || sv.tunnel_next == 0u ||
	    !s06_time_reached(sim.now, sv.tunnel_next))
		return;
	if (s09_slave_tunnel_port.room(&sl) >= sizeof frame) {
		s09_slave_tunnel_port.write(&sl, frame, sizeof frame);
		sv.frames++;
	}
	sv.tunnel_next = tm.tunnel_every_us != 0u ? sim.now + tm.tunnel_every_us : 0u;
}

/* The top of the serve loop: PB2 set from the server, the CPU link. */
static void loop_top(void)
{
	sv.pb2_low = s09_slave_attention(&sl);
	tunnel_frame();
}

/* The slave's loop brought up to now. */
static void sv_run(void)
{
	for (;;) {
		switch (sv.st) {
		case SV_DOWN:
			if (!s06_time_reached(sim.now, sv.up_at))
				return;
			sv.st = SV_LISTEN;
			continue;
		case SV_EXEC:
			if (!s06_time_reached(sim.now, sv.busy_until))
				return;
			if (sv.dying) {
				sv.st = SV_GONE;	/* it never answers again */
				return;
			}
			loop_top();
			if (sl.reply_len != 0u) {
				(void)s09_slave_reply(&sl, &sv.rep, &sv.rep_n);
				sv.rep_at = 0;
				sv.st = SV_TX;
			} else {
				sv.st = SV_LISTEN;
			}
			return;
		case SV_LISTEN:
			loop_top();
			return;
		case SV_RX:
		case SV_TX:
		case SV_GONE:
		default:
			return;
		}
	}
}

static bool lb_ready(void *c)
{
	(void)c;
	sv_run();
	switch (sv.st) {
	case SV_LISTEN:
		return true;
	case SV_RX:
		return !sv.rx_done || sv.late_now;
	case SV_TX:
		/* a reply that lost a received word: the line held after the
		 * last word (S-13b.045) */
		return sv.rep_at < sv.rep_n ||
		       (sv.hold_now && sim.now - sv.last_word_at < tm.hold_us);
	case SV_DOWN:
	case SV_EXEC:
	case SV_GONE:
	default:
		return false;
	}
}

static bool lb_attention(void *c)
{
	(void)c;
	sv_run();
	if (sv.st == SV_GONE || sv.st == SV_DOWN)
		return false;
	return sv.st == SV_LISTEN ? s09_slave_attention(&sl) : sv.pb2_low;
}

/* A request session taken whole: executed at once, PA6 high meanwhile. */
static void request_end(void)
{
	size_t k;

	if (sv.req_n < S09_HDR) {
		sv.st = SV_LISTEN;	/* no header: nothing in the plain mode */
		return;
	}
	k = (size_t)s09_slave_rx_len(&sl, sv.req);
	trace(sv.req[0], 0, sv.req_n);
	hash(sv.req, sv.req_n, 1u);
	sv.polls += sv.req[0] == S09_POLL;
	sv.pulls += sv.req[0] == S09_PULL;
	sv.execs[sv.req[0]]++;
	cost_us = 0;
	mm.taint = sv.late_now;		/* its reply, if it reached anyone, shows */
	s09_slave_request(&sl, sv.req, k < sv.req_n ? k : sv.req_n);
	sv.busy_until = sim.now + 20u + (uint32_t)cost_us;
	if (tm.die_nth != 0u && sv.req[0] == tm.die_cmd &&
	    ++sv.seen_die_cmd == tm.die_nth)
		sv.dying = true;
	sv.st = SV_EXEC;
}

static void lb_select(void *c, bool on)
{
	(void)c;
	sv_run();
	if (on && !sv.sel) {
		sv.sessions++;
		if (sv.st == SV_LISTEN) {
			sv.pb2_low = s09_slave_attention(&sl);
			sv.st = SV_RX;
			sv.req_n = 0;
			sv.rx_done = false;
			sv.late_now = false;
		} else if (sv.st == SV_TX) {
			const bool big = sv.rep_n > 4096u && sv.rep[0] == S09_READ_SECTORS;
			const unsigned k = big ? ++sv.bigs : 0u;
			const bool hit = big && tm.big_nth != 0u && k >= tm.big_nth &&
					 k < tm.big_nth + tm.big_count;

			sv.hold_now = hit && tm.hold_us != 0u;
			sv.half_now = (hit && tm.hold_us == 0u) ||
				      (tm.break_n != 0u && sv.rep_n > 4u &&
				       sv.rep[0] == tm.break_cmd && sv.rep[0] != S09_PULL);
			if (sv.half_now && !hit)
				tm.break_n--;
		} else {
			sv.stray_selects++;
		}
	} else if (!on && sv.sel) {
		if (sv.st == SV_RX) {
			request_end();
		} else if (sv.st == SV_TX) {
			uint32_t value;
			const uint32_t held = sv.last_word_at + tm.hold_us;

			trace(sv.rep_n >= 4u ? sv.rep[0] : 0u, 1, sv.rep_at);
			hash(sv.rep, sv.rep_at, 2u);
			if (sv.rep_n > 4u && sv.rep[0] != S09_PULL &&
			    (sv.rep[0] == S09_READ_SECTORS || sv.rep[0] == S09_READ_SECTOR))
				sv.poll_reads++;
			if (s09_slave_take_release(&sl, &value)) {
				if (sv.rep_at == sv.rep_n && value == 0u) {
					sv.released = true;
					sv.st = SV_GONE;
					sv.sel = on;
					return;
				}
				sl.released = false;
			}
			loop_top();
			sv.st = SV_LISTEN;
			if (sv.hold_now && !s06_time_reached(sim.now, held)) {
				/* a driver whose reply ends 1 s after the last
				 * word, whatever PB15 says */
				sv.st = SV_EXEC;
				sv.busy_until = held;
			}
			sv.hold_now = false;
		}
	}
	sv.sel = on;
}

static bool lb_send(void *c, const uint8_t *d, size_t n)
{
	(void)c;
	sv_run();
	if (!sv.sel || sv.st != SV_RX || sv.req_n + n > sizeof sv.req) {
		sv.stray_sends++;
		sim.now += S09_T_REPLY_US;	/* the driver's wait for PA6 runs out */
		return false;
	}
	memcpy(sv.req + sv.req_n, d, n);
	sv.req_n += n;
	sv.rx_done = true;
	if (sv.req_n >= S09_HDR && sv.req[0] == S09_READ_SECTORS &&
	    s06_rd_le32(sv.req + 8) == 128u && ++sv.big_reqs == tm.late_nth)
		sv.late_now = true;	/* taken whole; PA6 never rises */
	wire(n);
	return true;
}

static bool lb_recv(void *c, uint8_t *d, size_t n)
{
	(void)c;
	sv_run();
	if (!sv.sel || sv.st != SV_TX || sv.rep_at + n > sv.rep_n) {
		sv.stray_recvs++;
		sim.now += S09_T_REPLY_US;
		return false;
	}
	if (sv.half_now && sv.rep_at >= 4u) {
		/* the reply breaks: half of it moves, then nothing (the
		 * master's word wait runs out) */
		sv.rep_at += n / 2u & ~3u;
		wire(n / 2u);
		sim.now += S09_T_REPLY_US;
		return false;
	}
	memcpy(d, sv.rep + sv.rep_at, n);
	sv.rep_at += n;
	wire(n);
	sv.last_word_at = sim.now;
	return true;
}

static void lb_configure(void *c, int phase)
{
	(void)c;
	(void)phase;
}

static const struct s09_bus_ops bus = {
	.ctx = NULL,
	.ready = lb_ready,
	.attention = lb_attention,
	.select = lb_select,
	.send = lb_send,
	.recv = lb_recv,
	.configure = lb_configure,
};

/* clock_idle (opxy_ops.c): the master's every wait services USB. */
static void master_tick(void *ctx)
{
	(void)ctx;
	sim.now += tm.usb_us;
}

/* ---- the run ---- */

static struct s09_master ms;
static struct s09_files F;
static uint8_t *image, *staging;
static _Alignas(4) uint8_t emmc_scratch[4096];

/* An image of len bytes in BP2, its header active; BP1 written once and
 * inactive. */
static void put_os(uint32_t len, uint32_t seed)
{
	uint8_t hdr[S03_SECTOR];

	for (uint32_t i = 0; i < len; i++) {
		seed = seed * 1103515245u + 12345u;
		image[i] = (uint8_t)(seed >> 16);
	}
	memset(bp[0], 0, B_SIZE);
	memset(bp[1], 0, B_SIZE);
	s03_header_encode(len, wire_crc32(0u, image, len), 0u, hdr);
	memcpy(bp[0], hdr, sizeof hdr);
	s03_header_encode(len, wire_crc32(0u, image, len), 1u, hdr);
	memcpy(bp[1], hdr, sizeof hdr);
	memcpy(bp[1] + S03_SECTOR, image, len);
}

static void fresh(void)
{
	wire_s09_release(&S);
	wire_yaffs_detach(&Y);
	memset(vol, 0, VOL_LEN);
	wb_ramdisk_init(&bd, &ram, vol, (uint32_t)(VOL_LEN / 512u));
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0, BLOCKS - 1));
	wire_s09_init(&S, &Y);
	memset(&mm, 0, sizeof mm);
	memset(&sv, 0, sizeof sv);
	wire_hash = 2166136261u;
	features = false;
	sl_part = S03_PART_USER;	/* S-09.040: until the first select */
	t_sim_init(&sim, S06_MS(2000000));
	sim.tick = master_tick;
	s09_slave_init(&sl, &store);
	sv.st = SV_DOWN;
	sv.up_at = S06_MS(3000);	/* the slave's start-up and bring-up */
	s09_master_init(&ms, &bus, &sim.clock);
}

static void times_unit(void)
{
	memset(&tm, 0, sizeof tm);
	tm.features_us = S06_MS(3500);
	tm.read_base_us = 300u;
	tm.read_sector_ns = 20000u;	/* 128 sectors: 2.9 ms */
	tm.mount_us = S06_MS(400);
	tm.release_us = S06_MS(1500);
}

/* What a plain start runs, then the bridge used after it. ok: everything
 * as it should be. */
static bool plain_start(uint32_t len, const char *what)
{
	struct s03_emmc_ops bops;
	struct s03_env env;
	struct s03_load ld;
	uint32_t boot_size, got = 0;
	const int before = t_failures;
	static uint8_t file[20000], back[20000];
	int32_t fd;
	bool wrote;

	t_case = what;
	put_os(len, len * 7u + 1u);
	fresh();
	/* S-09.018, S-09.060: PA6 low within T_INIT */
	{
		const uint32_t end = sim.now + S09_T_INIT_US;

		while (!bus.ready(bus.ctx) && !s06_time_reached(sim.now, end))
			s06_idle(&sim.clock, sim.now + 100u);
	}
	CHECK(s09_master_start(&ms));
	boot_size = opxy_bridge_boot_size(&ms);
	CHECK_EQ(boot_size, B_SIZE);
	/* S-05.003: the PMIC, the logo and the PSoC drained before the load */
	{
		const uint32_t end = sim.now + S06_MS(60);

		while (!s06_time_reached(sim.now, end))
			s06_idle(&sim.clock, end);
	}
	/* tunnel_at_us 1: the frame is there as the load starts */
	sv.tunnel_next = tm.tunnel_at_us != 0u ? sim.now + tm.tunnel_at_us - 1u : 0u;
	sv_run();

	bops = opxy_bridge_emmc(&ms);
	memset(&env, 0, sizeof env);
	env.ops = &bops;
	env.crc32 = opxy_crc32_long;	/* the board's, as opxy_master.c sets it */
	env.boot_size = boot_size;
	env.scratch = emmc_scratch;
	env.scratch_len = sizeof emmc_scratch;
	env.max_xfer = 128u;
	memset(staging, 0xA5, S03_OS_MAX);
	s03_load_os(&env, staging, S03_OS_MAX, &ld);
	CHECK_EQ(ld.result, S03_LOAD_OK);
	CHECK_EQ(ld.part, S03_PART_BP2);
	CHECK_EQ(ld.length, len);
	CHECK(memcmp(staging, image, len) == 0);
	CHECK(!ld.select0_failed);
	CHECK(!ms.absent);
	CHECK_EQ(mm.max_count, 128u < s03_sectors(len) ? 128u : s03_sectors(len));
	CHECK_EQ(sl_part, S03_PART_USER);
#if defined(S09_LOST_IN_POLL)
	{
		struct s09_lost lost;

		CHECK(!s09_master_lost(&ms, &lost));
	}
#endif

	/* the bridge after the load: a mount, a file written and read back */
	s09_files_init(&F, &ms);
	CHECK_EQ(s09_files_mount(&F), 0);
	for (size_t i = 0; i < sizeof file; i++)
		file[i] = (uint8_t)(i * 31u + len);
	fd = s09_files_open(&F, "/yaffs2/after.bin", S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);
	wrote = fd >= 0 && s09_files_write(&F, fd, file, sizeof file) == (int32_t)sizeof file;
	CHECK(s09_files_close(&F, fd) == 0 && wrote);
	CHECK(s09_files_read_whole(&F, "/yaffs2/after.bin", back, sizeof back, &got));
	CHECK_EQ(got, sizeof file);
	CHECK(memcmp(back, file, sizeof file) == 0);

	/* S-09.050-S-09.052 */
	CHECK_EQ(s09_master_release(&ms), 0);
	CHECK(sv.released);
	CHECK(!ms.absent);
	CHECK_EQ(sv.stray_sends + sv.stray_recvs + sv.stray_selects, 0u);
	if (getenv("WB_PLAIN_LOAD_VERBOSE") != NULL)
		printf("  [%s] %u sessions, %u polls, %u pulls, %u frames, %u read replies, "
		       "%u us, wire %08x\n", what, sv.sessions, sv.polls, sv.pulls,
		       sv.frames, sv.poll_reads, sim.now, wire_hash);
	if (t_failures != before) {
		fprintf(stderr, "  [%s] %u sessions, %u polls, %u pulls, %u frames, "
			"absent %d, at %u us; the last sessions:\n", what, sv.sessions,
			sv.polls, sv.pulls, sv.frames, ms.absent, sim.now);
#if defined(S09_LOST_IN_POLL)
		{
			struct s09_lost lost;

			if (s09_master_lost(&ms, &lost))
				fprintf(stderr, "  lost: cmd 0x%02x at 0x%02x arg0 %u words "
					"%08x %08x %08x %08x\n", lost.cmd, lost.at, lost.arg0,
					lost.fault[0], lost.fault[1], lost.fault[2], lost.fault[3]);
		}
#endif
		trace_dump();
	}
	return t_failures == before;
}

/* The image sizes: the unit's, and around S-09.023's 128-sector requests. */
static const uint32_t sizes[] = {
	742764u,			/* 1451 sectors and the header: 1452 */
	S03_SECTOR - 37u, 2u * S03_SECTOR, 127u * S03_SECTOR - 1u, 128u * S03_SECTOR,
	128u * S03_SECTOR + 1u, 255u * S03_SECTOR, 256u * S03_SECTOR - 100u,
	257u * S03_SECTOR, S03_OS_MAX,
};
#define NSIZES (sizeof sizes / sizeof sizes[0])

static void run_sizes(const char *what)
{
	char name[96];

	for (unsigned i = 0; i < NSIZES; i++) {
		snprintf(name, sizeof name, "%s, %u bytes", what, sizes[i]);
		(void)plain_start(sizes[i], name);
	}
}

/* Realistic times, no tunnel traffic. */
static void test_plain(void)
{
	times_unit();
	run_sizes("plain");
}

/*
 * A frame from the slave's CPU link during the load: the parked pull is
 * finished and PB2 low while reads run. Not covered: a frame the slave's
 * loop takes just before the load's first select, whose features keep PA6
 * high for seconds with PB2 low, so that the poll's T_REPLY for PA6 runs
 * out (S-09.060) and the slave is absent; a plain start's master sends
 * nothing on the CPU link, so its slave has no frame to answer then.
 */
static void test_tunnel_frame(void)
{
	for (uint32_t at = S06_MS(97); at < S06_MS(6000); at += S06_MS(97)) {
		char name[64];

		times_unit();
		tm.tunnel_at_us = at;
		snprintf(name, sizeof name, "a tunnel frame at +%u us", at);
		(void)plain_start(742764u, name);
		tm.tunnel_every_us = S06_MS(7);
		snprintf(name, sizeof name, "tunnel frames from +%u us", at);
		(void)plain_start(742764u, name);
	}
}

/* The master's waits servicing USB with a host attached. */
static void test_usb_service(void)
{
	static const uint32_t us[] = { 50u, 1000u, 5000u };

	for (unsigned i = 0; i < sizeof us / sizeof us[0]; i++) {
		char name[64];

		times_unit();
		tm.usb_us = us[i];
		tm.tunnel_at_us = S06_MS(3650);
		tm.tunnel_every_us = S06_MS(11);
		snprintf(name, sizeof name, "USB service %u us", us[i]);
		(void)plain_start(742764u, name);
	}
}

/* A multi-block read that fails slowly once (the driver's CMD12 path), and
 * S-03.010's retry of it. */
static void test_slow_multi(void)
{
	times_unit();
	tm.slow_multi = true;
	(void)plain_start(742764u, "a multi-block read failing once");
	CHECK(mm.failed_once);
}

#if defined(S09_LOST_IN_POLL)
/*
 * A slave that stops answering mid-load: the load fails, every later
 * request fails at once with no traffic (S-09.008), and the bridge says
 * which request it was serving, its arg0 and where its exchange broke
 * (s09_master_lost()).
 */
static void lost_case(uint8_t die_cmd, unsigned nth, uint8_t cmd, uint8_t at,
		      uint32_t arg0, const char *what)
{
	struct s03_emmc_ops bops;
	struct s03_env env;
	struct s03_load ld;
	struct s09_lost lost;
	unsigned sessions;

	t_case = what;
	times_unit();
	tm.die_cmd = die_cmd;
	tm.die_nth = nth;
	put_os(742764u, 5u);
	fresh();
	while (!bus.ready(bus.ctx))
		s06_idle(&sim.clock, sim.now + 100u);
	CHECK(s09_master_start(&ms));
	CHECK(!s09_master_lost(&ms, &lost));
	CHECK_EQ(lost.cmd, 0);
	bops = opxy_bridge_emmc(&ms);
	memset(&env, 0, sizeof env);
	env.ops = &bops;
	env.crc32 = opxy_crc32_long;
	env.boot_size = opxy_bridge_boot_size(&ms);
	env.scratch = emmc_scratch;
	env.scratch_len = sizeof emmc_scratch;
	env.max_xfer = 128u;
	s03_load_os(&env, staging, S03_OS_MAX, &ld);
	CHECK_EQ(ld.result, S03_LOAD_IO);
	CHECK(ms.absent);
	CHECK(s09_master_lost(&ms, &lost));
	CHECK_EQ(lost.cmd, cmd);
	CHECK_EQ(lost.at, at);
	CHECK_EQ(lost.arg0, arg0);
	CHECK_EQ(lost.fault[0] | lost.fault[1] | lost.fault[2] | lost.fault[3], 0u);
	sessions = sv.sessions;
	s09_files_init(&F, &ms);
	CHECK(s09_files_mount(&F) < 0);
	CHECK_EQ(sv.sessions, sessions);
	tm.die_nth = 0;
}

static void test_lost(void)
{
	/* the third 128-sector read of BP2 (sectors 257-384) executed, its
	 * reply never offered: T_EXEC runs out */
	lost_case(S09_READ_SECTORS, 3u, S09_READ_SECTORS, S09_AT_EXEC, 257u,
		  "lost waiting for a read's reply");
	/* a poll executed and its reply never offered: the poll for the
	 * fourth read's reply, which never started */
	lost_case(S09_POLL, 6u, S09_READ_SECTORS, S09_AT_REPLY_START | S09_LOST_IN_POLL,
		  1u + 128u, "lost in a poll's reply");
	/* the first select, its features under way */
	lost_case(S09_SELECT, 1u, S09_SELECT, S09_AT_EXEC, S03_PART_BP1,
		  "lost in the first select");
}
#else
static void test_lost(void)
{
}
#endif

/*
 * S-09.013 rule 4: a plain start's load whose read, select or reply fails
 * with the slave still there. The load's own result, then the bridge after
 * it: no reply left queued for a later request, a mount, a file written
 * and read back. Returns the load.
 */
static struct s03_load faulty_load(const char *what)
{
	struct s03_emmc_ops bops;
	struct s03_env env;
	struct s03_load ld;
	static uint8_t file[5000], back[5000];
	uint32_t got = 0;
	int32_t fd;
	bool wrote;

	t_case = what;
	put_os(742764u, 9u);
	fresh();
	while (!bus.ready(bus.ctx))
		s06_idle(&sim.clock, sim.now + 100u);
	CHECK(s09_master_start(&ms));
	bops = opxy_bridge_emmc(&ms);
	memset(&env, 0, sizeof env);
	env.ops = &bops;
	env.crc32 = opxy_crc32_long;
	env.boot_size = opxy_bridge_boot_size(&ms);
	env.scratch = emmc_scratch;
	env.scratch_len = sizeof emmc_scratch;
	env.max_xfer = 128u;
	memset(staging, 0xA5, S03_OS_MAX);
	s03_load_os(&env, staging, S03_OS_MAX, &ld);
	/* the slave kept, nothing of the failed exchanges left for later */
	CHECK(!ms.absent);
	sv_run();
	CHECK(sv.st == SV_LISTEN && !s09_slave_attention(&sl));
	CHECK_EQ(sl_part, S03_PART_USER);
	s09_files_init(&F, &ms);
	CHECK_EQ(s09_files_mount(&F), 0);
	for (size_t i = 0; i < sizeof file; i++)
		file[i] = (uint8_t)(i * 13u + 5u);
	fd = s09_files_open(&F, "/yaffs2/kept.bin", S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);
	wrote = fd >= 0 && s09_files_write(&F, fd, file, sizeof file) == (int32_t)sizeof file;
	CHECK(s09_files_close(&F, fd) == 0 && wrote);
	CHECK(s09_files_read_whole(&F, "/yaffs2/kept.bin", back, sizeof back, &got));
	CHECK(got == sizeof file && memcmp(back, file, sizeof file) == 0);
	CHECK(!ms.absent && sv.stray_sends + sv.stray_recvs + sv.stray_selects == 0u);
	return ld;
}

/* The image loaded whole, every 128-sector read executed once, and as many
 * again as the faults cost. */
static void loaded(const struct s03_load *ld, unsigned again)
{
	const unsigned n = (s03_sectors(742764u) + 127u) / 128u;

	CHECK_EQ(ld->result, S03_LOAD_OK);
	CHECK(ld->length == 742764u && memcmp(staging, image, ld->length) == 0);
	CHECK_EQ(sv.execs[S09_READ_SECTORS], n + again);
}

/*
 * A slave that loses one received word in a 64 KiB reply. As the driver now
 * ends that reply (S-13b.045: from the transmit side 0.5 ms after its last
 * word), the master's wait for PA6 high holds, and the load goes on with no
 * retry. As it was (the line held 1 s), the master's wait runs out: the
 * read fails, the slave is kept (S-09.013 rule 4), S-03.010 retries it, and
 * the load completes.
 */
static void test_lost_word(void)
{
	struct s03_load ld;

	times_unit();
	tm.big_nth = 2u;
	tm.big_count = 1u;
	tm.hold_us = 500u;
	ld = faulty_load("a received word lost, the reply ended at once");
	loaded(&ld, 0u);
	times_unit();
	tm.big_nth = 2u;
	tm.big_count = 1u;
	tm.hold_us = S06_MS(1000);
	ld = faulty_load("a received word lost, the line held 1 s");
	loaded(&ld, 1u);
}

/* A 128-sector read's reply that breaks half-way: once, S-03.010's retry
 * loads the image; three times in a row, the load fails as I/O with the
 * image read's sector, and the slave is kept. */
static void test_broken_reply(void)
{
	struct s03_load ld;

	times_unit();
	tm.big_nth = 3u;
	tm.big_count = 1u;
	ld = faulty_load("a read's reply broken once");
	loaded(&ld, 1u);
	times_unit();
	tm.big_nth = 3u;
	tm.big_count = S03_RETRIES;
	ld = faulty_load("a read's reply broken three times");
	CHECK(ld.result == S03_LOAD_IO && ld.step == S03_STEP_IMAGE && ld.length == 0u);
	CHECK(ld.step_part == S03_PART_BP2 && ld.sector == 1u + 2u * 128u && ld.count == 128u);
	CHECK(!ld.select0_failed);
	CHECK_EQ(sv.execs[S09_READ_SECTORS], 2u + S03_RETRIES);
	{
		struct s09_lost lost;

		CHECK(!s09_master_lost(&ms, &lost));	/* "could not be read" */
	}
}

/* A read the slave took whole and executed, whose request session's end the
 * master waited out: its reply, queued, is drained and discarded before
 * the read is sent again, never handed to a later request. Its data are
 * marked, so that the image would show it if it were (without the drain
 * the load ends on a CRC mismatch). */
static void test_late_read(void)
{
	struct s03_load ld;

	times_unit();
	tm.late_nth = 2u;
	ld = faulty_load("a read taken whole, its session's end not seen");
	loaded(&ld, 1u);
}

/* The first select's reply broken after its first word: selected again. */
static void test_select(void)
{
	struct s03_load ld;

	times_unit();
	tm.break_cmd = S09_SELECT;
	tm.break_n = 1u;
	ld = faulty_load("a select's reply broken");
	loaded(&ld, 0u);
	CHECK_EQ(sv.execs[S09_SELECT], 4u);	/* BP1 twice, BP2, the user area */
}

/* The control: a file write's reply broken in the plain mode. Whether it
 * was executed is unknown, and the plain mode cannot send it again with a
 * number: the slave is absent, as before (S-09.013). */
static void test_write_still_absent(void)
{
	static const uint8_t data[64] = { 1, 2, 3 };
	int32_t fd;

	t_case = "a write's reply broken";
	times_unit();
	fresh();
	while (!bus.ready(bus.ctx))
		s06_idle(&sim.clock, sim.now + 100u);
	CHECK(s09_master_start(&ms));
	s09_files_init(&F, &ms);
	CHECK_EQ(s09_files_mount(&F), 0);
	fd = s09_files_open(&F, "/yaffs2/w.bin", S09_O_WRONLY | S09_O_CREAT);
	CHECK(fd >= 0);
	tm.break_cmd = S09_WRITE;
	tm.break_n = 1u;
	CHECK(s09_files_write(&F, fd, data, sizeof data) < 0);
	CHECK(ms.absent && F.err == S09_ENODEV);
	CHECK_EQ(sv.execs[S09_WRITE], 1u);
}

/* ---- S-05.035, S-09.082: a file start ---- */

/* /yaffs2/os.ldr as an earlier boot left it: len bytes of image, or a
 * directory; then unmounted, so the start's 0x08 mounts the volume. */
static void put_os_file(uint32_t len, bool dir)
{
	CHECK_EQ(wire_yaffs_mount(&Y), WIRE_MOUNT_OK);
	if (dir) {
		CHECK(yaffs_mkdir("/yaffs2/os.ldr", 0755) == 0);
	} else {
		const int fd = yaffs_open("/yaffs2/os.ldr", O_CREAT | O_WRONLY | O_TRUNC, 0666);

		CHECK(fd >= 0);
		CHECK(yaffs_write(fd, image, len) == (int)len);
		CHECK(yaffs_close(fd) == 0);
	}
	/* forced: an earlier case's run may have left a handle open */
	CHECK(yaffs_unmount2("/yaffs2", 1) == 0);
	Y.mounted = false;
}

/* The start-up a plain start runs, up to the stage. */
static void start_up(void)
{
	const uint32_t end = sim.now + S09_T_INIT_US;

	while (!bus.ready(bus.ctx) && !s06_time_reached(sim.now, end))
		s06_idle(&sim.clock, sim.now + 100u);
	CHECK(s09_master_start(&ms));
	sv_run();
}

/*
 * The master reads the file over the bridge as the board's stage does
 * (s09_files_os_read() after the 0x08), its copy's CRC-32 goes in the
 * release, and the slave reads its own copy from the volume: both the
 * file's bytes, no boot partition read. A copy that is not the master's
 * (its CRC or length) is refused with reply 1, the bridge kept.
 */
static void test_file_start(void)
{
	static const uint32_t lens[] = { 742764u, S03_MIN_IMAGE, S03_OS_MAX };
	struct s15_os_file out;
	uint32_t len, crc;

	times_unit();
	for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) {
		t_case = "file start";
		put_os(lens[i], lens[i] * 3u + 5u);	/* the image, also in BP2 */
		memset(bp[1], 0, B_SIZE);		/* no partition active */
		fresh();
		put_os_file(lens[i], false);
		start_up();
		memset(staging, 0xA5, S03_OS_MAX);
		memset(sl_staging, 0x5A, S03_OS_MAX);
		mm.reads = 0;
		s09_files_init(&F, &ms);
		CHECK(s09_files_os_read(&F, staging, S03_OS_MAX, &out, &len));
		CHECK(out.why == S15_OSW_WHY_NONE && len == lens[i]);
		CHECK(F.mount_sent && F.mounted);
		CHECK(memcmp(staging, image, len) == 0);
		CHECK_EQ(mm.reads, 0u);			/* no sector read */
		crc = opxy_crc32_long(0u, staging, len);
		CHECK_EQ(s09_master_release_file(&ms, len, crc), 0);
		CHECK(sv.released && sl_file == WIRE_OS_FILE_OK);
		CHECK(memcmp(sl_staging, image, len) == 0);
		CHECK_EQ(sl_part, S03_PART_USER);
	}

	/* the slave's copy is not the master's: reply 1, the bridge kept */
	t_case = "file start, another crc";
	put_os(742764u, 11u);
	fresh();
	put_os_file(742764u, false);
	start_up();
	s09_files_init(&F, &ms);
	CHECK(s09_files_os_read(&F, staging, S03_OS_MAX, &out, &len));
	staging[1000] ^= 0x40u;			/* damaged on the bridge */
	crc = opxy_crc32_long(0u, staging, len);
	CHECK_EQ(s09_master_release_file(&ms, len, crc), 1);
	CHECK(!sv.released && sl_file == WIRE_OS_FILE_CRC && !ms.absent);
	CHECK_EQ(s09_master_release_file(&ms, len + 1u, crc), 1);
	CHECK(sl_file == WIRE_OS_FILE_LENGTH && !ms.absent);
}

/* S-05.035's refusals before any release: each with its reason. */
static void file_refused(const char *what, uint32_t len, bool dir, bool none,
			 enum s15_osw_why why, uint32_t value, uint32_t limit)
{
	struct s15_os_file out;
	uint32_t got = 7u;

	t_case = what;
	put_os(len != 0u ? len : 1000u, 3u);
	fresh();
	if (!none)
		put_os_file(len, dir);
	start_up();
	s09_files_init(&F, &ms);
	CHECK(!s09_files_os_read(&F, staging, S03_OS_MAX, &out, &got));
	CHECK_EQ(out.why, why);
	CHECK(out.value == value && out.limit == limit && got == 0u);
	CHECK(!ms.absent);
}

static void test_file_refused(void)
{
	times_unit();
	file_refused("no os.ldr", 0u, false, true, S15_OSW_WHY_MISSING, S09_ENOENT, 0u);
	file_refused("os.ldr a directory", 0u, true, false, S15_OSW_WHY_DIRECTORY, 0u, 0u);
	file_refused("os.ldr too small", S03_MIN_IMAGE - 1u, false, false,
		     S15_OSW_WHY_TOO_SMALL, S03_MIN_IMAGE - 1u, S03_MIN_IMAGE);
	file_refused("os.ldr empty", 0u, false, false, S15_OSW_WHY_TOO_SMALL, 0u,
		     S03_MIN_IMAGE);
	file_refused("os.ldr too large", S03_OS_MAX + 1u, false, false,
		     S15_OSW_WHY_TOO_LARGE, S03_OS_MAX + 1u, S03_OS_MAX);
}

int main(void)
{
	vol = calloc(1, VOL_LEN);
	bp[0] = calloc(1, B_SIZE);
	bp[1] = calloc(1, B_SIZE);
	image = calloc(1, S03_OS_MAX + 1u);
	staging = calloc(1, S03_OS_MAX);
	sl_staging = calloc(1, S03_OS_MAX);
	if (vol == NULL || bp[0] == NULL || bp[1] == NULL || image == NULL ||
	    staging == NULL || sl_staging == NULL)
		return 2;
	test_plain();
	test_tunnel_frame();
	test_usb_service();
	test_slow_multi();
	test_lost();
	test_lost_word();
	test_broken_reply();
	test_late_read();
	test_select();
	test_write_still_absent();
	test_file_start();
	test_file_refused();
	wire_s09_release(&S);
	wire_yaffs_detach(&Y);
	free(vol);
	free(bp[0]);
	free(bp[1]);
	free(image);
	free(staging);
	free(sl_staging);
	return t_finish("test_board_plain_load");
}
