/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The master's S-09 file client (src/s09_files.c) end to end: the client
 * on the master's side, s09_slave serving it through a loopback of the S-09
 * bus, and on the slave's side wire_s09 on a yaffs2 volume in RAM and an eMMC
 * model whose user area holds that volume.
 *
 * Covered: the file requests and their failures (S-09.041, S-09.042), long
 * files over several 0x1000 pieces (S-09.035, 0x2B), directory listings,
 * trees, copy and move as S-11.080's staging area needs them, S-15's packs
 * (S-15.041), the factory reset's FAT erase (S-15.052, S-03.073),
 * the format (S-15.072, S-12.057: every group of the yaffs2 region erased, an
 * empty volume after), a refused volume (S-12.052), the OS write (S-15.094,
 * S-03.050-S-03.060) through the eMMC commands (S-09.040), the settings
 * file (S-15.172) and a slave that goes absent mid-transfer (S-09.008,
 * S-09.060). S-11's staging writes on the client: the reset, directories,
 * files, the free space (S-11.031 - S-11.033), the progress record (S-11.062)
 * and checks 16 and 17 end to end through s09_files_zip_ops() over a zip
 * reader fake.
 */
#include <time.h>

#include "waffle/s09_files.h"
#include "waffle/s09_slave.h"
#include "waffle/s11_run.h"
#include "waffle/wire_s09.h"
#include "waffle/wire_yaffs.h"

#include "s06_test.h"
#include "s06_fake.h"
#include "s02_ref_crc.h"

#define BLOCKS 40u			/* one yaffs2 block is one erase group */
#define VOL_LEN ((size_t)BLOCKS * 128u * 4096u)
#define B_SIZE 0x40000u			/* the boot partitions' size B */

/* ---- the slave's store: wire_s09 and an eMMC model ---- */

static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static struct wire_yaffs Y;
static struct wire_s09 S;
static uint8_t *vol;

static struct {
	unsigned part;
	uint8_t bp[2][B_SIZE];
	uint8_t erased[S09_FILES_YAFFS2_GROUPS];
	unsigned erases, fat_erased, other_erased;
	uint32_t last_cmd38;
	int fail_erase_at;		/* erases from this number fail, 0: none */
	int fail_erase_only;		/* this erase alone fails, 0: none */
	bool fail_select;
	bool fail_bp_write;		/* every write to a boot partition fails */
	unsigned flushes;
	unsigned fail_flush_from;	/* flushes from this number fail, 0: none */
} emmc;

static uint8_t *sector_at(uint32_t sector, uint32_t n)
{
	uint64_t off = (uint64_t)sector * S09_SECTOR;

	if (emmc.part == 0)
		return off + n <= VOL_LEN ? vol + off : NULL;
	return off + n <= B_SIZE ? emmc.bp[emmc.part - 1u] + off : NULL;
}

static int32_t st_emmc(void *c, uint8_t cmd, uint32_t a0, uint32_t a1,
		       uint8_t *data, uint32_t n)
{
	uint8_t *p;

	(void)c;
	switch (cmd) {
	case S09_SELECT:
		if (a0 > 2 || emmc.fail_select)
			return 1;
		emmc.part = a0;
		return 0;
	case S09_READ_SECTOR:
	case S09_READ_SECTORS:
		p = sector_at(a0, n);
		if (p == NULL) {
			memset(data, 0, n);	/* the unused rest of the user area */
			return emmc.part == 0 ? 0 : 1;
		}
		memcpy(data, p, n);
		return 0;
	case S09_WRITE_SECTOR:
	case S09_WRITE_SECTORS:
	case S09_WRITE_RELIABLE:
		if (emmc.part != 0 && emmc.fail_bp_write)
			return 1;
		p = sector_at(a0, n);
		if (p == NULL)
			return emmc.part == 0 ? 0 : 1;
		memcpy(p, data, n);
		return 0;
	case S09_ERASE:
		emmc.erases++;
		emmc.last_cmd38 = a1;
		if (emmc.fail_erase_at != 0 && (int)emmc.erases >= emmc.fail_erase_at)
			return 1;
		if (emmc.fail_erase_only != 0 && (int)emmc.erases == emmc.fail_erase_only)
			return 1;
		if (a0 % S03_GROUP_SECTORS != 0)
			return 1;
		if (emmc.part == 0 && a0 / S03_GROUP_SECTORS < S09_FILES_YAFFS2_GROUPS)
			emmc.erased[a0 / S03_GROUP_SECTORS] = 1;
		else if (emmc.part == 0 && a0 / S03_GROUP_SECTORS == S03_FACTORY_RESET_GROUP)
			emmc.fat_erased++;
		else if (emmc.part == 0)
			emmc.other_erased++;
		p = sector_at(a0, S03_GROUP_SECTORS * S09_SECTOR);
		if (p != NULL)
			memset(p, 0, S03_GROUP_SECTORS * S09_SECTOR);	/* erases to 0 */
		return 0;
	case S09_FLUSH:
		emmc.flushes++;
		return emmc.fail_flush_from != 0 && emmc.flushes >= emmc.fail_flush_from ? 1 : 0;
	default:
		return 1;
	}
}

static uint32_t st_erase_group(void *c) { (void)c; return S03_GROUP_SECTORS; }
static uint32_t st_boot_size(void *c) { (void)c; return B_SIZE; }
static uint32_t st_os(void *c) { (void)c; return 1; }
static uint64_t st_cycles(void *c) { (void)c; return 0; }
static void st_adjust(void *c, int64_t d) { (void)c; (void)d; }

/* What the slave's command layer executed: S-09.076's once. */
static struct {
	unsigned file[256];		/* file commands, by command */
	unsigned emmc[256];		/* eMMC commands, by command */
	unsigned pieces;		/* chunked read pieces */
	uint64_t write_bytes;		/* 0x2B's bytes */
} ex;

static struct t_sim sim;
static uint32_t slow_write_us;	/* how long a file write takes the slave */

static int32_t st_file(void *c, const struct s09_req *r, const char *p1,
		       const char *p2, const uint8_t *d, uint32_t n, uint8_t *out,
		       uint32_t *err)
{
	(void)c;
	ex.file[r->cmd]++;
	if (r->cmd == S09_WRITE)
		sim.now += slow_write_us;
	if (r->cmd == S09_WRITE)
		ex.write_bytes += n;
	return wire_s09_file(&S, r, p1, p2, d, n, out, err);
}

static int32_t st_read(void *c, uint32_t fd, uint8_t *buf, uint32_t n, uint32_t *err)
{
	(void)c;
	ex.pieces++;
	return wire_s09_read(&S, fd, buf, n, err);
}

static int32_t st_emmc_counted(void *c, uint8_t cmd, uint32_t a0, uint32_t a1,
			       uint8_t *data, uint32_t n)
{
	ex.emmc[cmd]++;
	return st_emmc(c, cmd, a0, a1, data, n);
}

static const struct s09_store_ops store = {
	0, st_erase_group, st_boot_size, NULL /* dev_cmd */, st_emmc_counted, st_file,
	st_read, st_os, st_cycles, st_adjust,
};

static struct s09_slave sl;

/* ---- the loopback bus (as test_s09_bridge's) ---- */

static struct lb_state {
	bool sel, reply_session, done, dead;
	uint8_t req[70000];
	size_t req_n;
	const uint8_t *rep;
	size_t rep_n, rep_at;
	unsigned sessions, die_after;	/* die_after: sessions, 0 never */
	unsigned writes, reads;		/* 0x2B and 0x2A request sessions */
	unsigned mkdirs, stats;		/* 0x26 and 0x22 */
	/* S-09.075: the rung of the request session being sent (configure) */
	unsigned rung;
	unsigned bad_rungs;	/* bit r: a request sent on rung r arrives with a bit wrong */
	unsigned deaf_rungs;	/* bit r: ... is not heard by the slave at all */
	unsigned flip_in;	/* n: the n-th request session from now arrives wrong, once */
	unsigned break_reply_in;	/* n: the n-th reply session from now breaks after its first word */
	bool breaking;
	unsigned garbled;	/* request sessions the slave could not take */
	unsigned checked;	/* request sessions that ended with a good check */
	/* sends, sessions given more than one (S-13b.083: a request
	 * session's bytes are one array, one send), and the sessions the
	 * master closed (the bus's end) */
	unsigned sends, sends_here, split, ends;
	/* up to two faults (arm(), arm_also()): each on the nth session of
	 * its command from now on, in its request or in the reply whose first
	 * word is that command's; at a word, a bit flipped or the session
	 * broken there, or the reply made a whole "nothing waiting" */
	struct lb_fault {
		uint8_t cmd;
		bool rep, brk, none;
		unsigned word, nth, seen;
		bool fired;
		unsigned rung;		/* where it fired */
	} fs[2];
	unsigned nfs, f_fired;
	bool f_break, f_none;	/* the fault that fired last */
	unsigned f_word;
	bool f_now;		/* this session is the one */
	bool att_stuck;		/* PC5 reads low for good */
	uint8_t drop_cmd;	/* that request's sessions never reach the slave */
	uint32_t session_us;	/* how long each session takes */
	uint32_t word_ns[S09_RUNG_TOP + 1u];	/* a request word's time by rung */
	uint64_t ns_owed;
	unsigned last_rung;	/* the rung of the last request session */
	uint8_t fault_after;	/* the slave falls back once that request is in */
	int f_fill;		/* >= 0: the reply reads all this byte instead */
	bool f_echo;		/* the reply's first word an echo's, its slot kept */
	uint8_t rep_copy[70000];
	/* the send failing as a DMA set-up that does not work would make it
	 * fail (feed_fail): feed_bytes of the session clocked first (all when it
	 * is larger), feed_fails times (~0u: every one), on a rung only unless
	 * feed_plain */
	unsigned feed_fail, feed_bytes, feed_fails;
	bool feed_plain;
	bool feed_kills;	/* ... and the bus is dead from then on */
	/* a failed send not yet followed by the plain set-up (configure(0),
	 * the controller's restart), and sessions begun so */
	bool unreset;
	unsigned unreset_sessions;
	/* the slave's own bounds (S-09.013): a request it was still receiving
	 * when PC11 rose (hold), whose next words it takes from the next session
	 * until T_REPLY has run; a reply it offers from rep_ready for T_REPLY.
	 * Counted: sessions merged into a held request, replies dropped so, a
	 * request session clocked against an offered reply, a reply session
	 * clocked against a slave waiting for a request. */
	bool hold, rep_hold;	/* rep_hold: a reply not taken whole */
	/* a request of slow_cmd keeps the slave at work slow_us after it came
	 * whole (PA6 high), and its reply is offered from then on */
	uint8_t slow_cmd;
	uint32_t slow_us, busy_until;
	uint32_t hold_at, rep_ready, rep_hold_at;
	unsigned merged, dropped, stale, blind;
	uint8_t late_cmd;	/* that request's session: PA6 never rises after
				 * it, though the slave took it whole; 0 none */
	bool late_now;
	/* the reply whose first word is that command's: clocked whole, but
	 * the slave does not see its end, so PA6 never rises after it, and
	 * the slave changes to the count end (S-13b.083, S-09.079 cause 9),
	 * once; 0 none */
	uint8_t end_cmd;
	int hear_quad;		/* -1: quad against single is not heard; 0 or 1:
				 * heard on one data line (quad_heard()) */
	bool spurious_att;	/* PC5 reads low once with nothing waiting */
	unsigned rungs[256];	/* the rung each command's last request went on */
	unsigned cmds[256];	/* request sessions by command */
	uint32_t log;		/* every session's bytes and rung, hashed */
	/* every link test heard by a listener in single I/O (harmless()) */
	bool g2;
	unsigned g2_seen[S09_RUNG_TOP + 1], g2_bad;
	bool in_slave;		/* the slave is taking a request */
	unsigned on_rung;	/* request sessions on a rung above 0 */
} lb;

static bool harmless(const uint8_t *h, size_t n);

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static void lb_log(const uint8_t *d, size_t n, unsigned tag)
{
	uint32_t h = lb.log ^ tag;

	for (size_t i = 0; i < n; i++)
		h = (h ^ d[i]) * 16777619u;
	lb.log = h;
}

/*
 * A quad session as a single-I/O listener hears it: one bit a clock, the
 * bit on data line 0 (MOSI). Each 32-bit word goes out most significant
 * nibble first; which bit of a nibble line 0 carries is the hardware's
 * (line 0 the lowest when map is 0, the highest when 1), so both are
 * tried. The listener puts its bits together into words, most
 * significant first, as S-13b.040 sets it.
 */
static size_t quad_heard(const uint8_t *q, size_t qn, uint8_t *out, int map)
{
	size_t bits = 0;
	uint32_t acc = 0;

	for (size_t i = 0; i + 4 <= qn; i += 4) {
		const uint32_t w = rd32(q + i);

		for (unsigned t = 0; t < 8u; t++) {
			const uint32_t nib = w >> (28u - 4u * t) & 0xFu;

			acc = acc << 1 | (map ? nib >> 3 : nib & 1u);
			if (++bits % 32u == 0u) {
				const size_t o = (bits / 32u - 1u) * 4u;

				out[o] = (uint8_t)acc;
				out[o + 1] = (uint8_t)(acc >> 8);
				out[o + 2] = (uint8_t)(acc >> 16);
				out[o + 3] = (uint8_t)(acc >> 24);
				acc = 0;
			}
		}
	}
	return bits / 32u * 4u;
}

/* The armed fault, if this session's first bytes are its command's. */
static bool fault_here(bool rep, uint8_t cmd)
{
	for (unsigned i = 0; i < lb.nfs; i++) {
		struct lb_fault *f = &lb.fs[i];

		if (f->fired || f->rep != rep || f->cmd != cmd || ++f->seen < f->nth)
			continue;
		f->fired = true;
		f->rung = rep ? lb.last_rung : lb.rung;
		lb.f_fired++;
		lb.f_word = f->word;
		lb.f_break = f->brk;
		lb.f_none = f->none;
		return true;
	}
	return false;
}

static bool lb_ready(void *c)
{
	(void)c;
	if (lb.dead)
		return false;
	if (!lb.sel && (int32_t)(lb.busy_until - sim.now) > 0)
		return false;		/* the slave at work */
	if (lb.sel && lb.late_now)
		return true;		/* PA6 never rises */
	/* a session shorter than the request the slave expects in its mode
	 * (a plain one to a slave on a rung): PA6 does not rise (S-09.013) */
	if (lb.sel && lb.done && !lb.reply_session && lb.req_n >= S09_HDR &&
	    lb.req_n < (size_t)s09_slave_rx_len(&sl, lb.req))
		return true;
	return lb.sel ? !lb.done : true;
}

static bool lb_attention(void *c)
{
	(void)c;
	if (lb.spurious_att || lb.att_stuck) {
		lb.spurious_att = false;
		return !lb.dead;
	}
	return !lb.dead && s09_slave_attention(&sl);
}

/* A request session as the slave takes it: its bytes, d, as heard. */
static void lb_deliver(const uint8_t *d, size_t n)
{
	size_t k;

	if (n < S09_HDR) {
		s09_slave_link_fault(&sl);	/* its header did not come */
		return;
	}
	k = (size_t)s09_slave_rx_len(&sl, d);
	lb.writes += d[0] == S09_WRITE;
	lb.reads += d[0] == S09_READ;
	lb.mkdirs += d[0] == S09_MKDIR;
	lb.stats += d[0] == S09_STAT;
	lb.in_slave = true;
	s09_slave_request(&sl, d, k < n ? k : n);
	lb.in_slave = false;
	if (lb.fault_after != 0u && d[0] == lb.fault_after) {
		lb.fault_after = 0;
		s09_slave_link_fault(&sl);
	}
}

static void lb_request_end(void)
{
	static uint8_t heard[sizeof lb.req];
	const bool quad = lb.rung >= S09_RUNG_QUAD;
	const bool slave_quad = s09_slave_mode(&sl) == S09_MODE_QUAD;

	lb_log(lb.req, lb.req_n, 1u | lb.rung << 8);
	if (lb.req_n == 0)
		return;			/* nothing clocked: the slave listens on */
	lb.on_rung += lb.rung != 0u;
	lb.last_rung = lb.rung;
	if (lb.drop_cmd != 0u && lb.req[0] == lb.drop_cmd)
		return;			/* the slave never hears it */
	if (lb.g2 && lb.req_n >= S09_HDR && lb.req[0] == S09_LINK_TEST) {
		lb.g2_seen[lb.rung]++;
		if (!quad) {
			lb.g2_bad += !harmless(lb.req, lb.req_n);
		} else {
			for (int map = 0; map < 2; map++)
				lb.g2_bad += !harmless(heard, quad_heard(lb.req, lb.req_n,
									 heard, map));
		}
	}
	lb.rungs[lb.req[0]] = lb.rung;
	lb.cmds[lb.req[0]]++;
	/* a session that ended before the request the slave expects: it goes
	 * on receiving until its own bound, the next session's words taken as
	 * more of this one (S-09.013); lb_select() settles it */
	if (quad == slave_quad && !(lb.deaf_rungs >> lb.rung & 1u) &&
	    (lb.req_n < S09_HDR || lb.req_n < (size_t)s09_slave_rx_len(&sl, lb.req))) {
		lb.hold = true;
		lb.hold_at = sim.now;
		return;
	}
	if (lb.req_n >= S09_HDR && quad && !slave_quad && lb.hear_quad >= 0 &&
	    !(lb.deaf_rungs >> lb.rung & 1u)) {
		/* a quad session that a slave listening in single I/O hears */
		lb_deliver(heard, quad_heard(lb.req, lb.req_n, heard, lb.hear_quad));
		return;
	}
	/* quad I/O read as single, or the reverse, or a rung the wire does
	 * not carry: the slave's receive fails (S-09.074) */
	if (lb.req_n >= S09_HDR && (quad != slave_quad || (lb.deaf_rungs >> lb.rung & 1u))) {
		s09_slave_link_fault(&sl);
		lb.garbled++;
		return;
	}
	if (lb.f_now && !lb.f_break && lb.f_word * 4u < lb.req_n)
		lb.req[lb.f_word * 4u + lb.f_word % 4u] ^= (uint8_t)(1u << (lb.f_word % 8u));
	if (lb.req_n >= S09_HDR && ((lb.bad_rungs >> lb.rung & 1u) ||
				    (lb.flip_in != 0u && --lb.flip_in == 0u)))
		lb.req[5] ^= 0x10u;	/* a bit of arg0 */
	if (lb.rung != 0u && lb.req_n >= 20)
		lb.checked += s09_check_word(lb.req, (uint32_t)lb.req_n - 4u) ==
			      rd32(lb.req + lb.req_n - 4u);
	lb_deliver(lb.req, lb.req_n);
}

static void lb_select(void *c, bool on)
{
	(void)c;
	if (on && !lb.sel) {
		lb.sessions++;
		lb.sends_here = 0;
		lb.unreset_sessions += lb.unreset;
		sim.now += lb.session_us;
		if (lb.die_after != 0 && lb.sessions > lb.die_after)
			lb.dead = true;
		/* the slave's bounds, from PC11 high: a request it was receiving
		 * fails, a reply it offered is dropped (S-09.013) */
		if (lb.hold && sim.now - lb.hold_at >= S09_T_REPLY_US) {
			lb.hold = false;
			if (lb.req_n < S09_HDR)
				s09_slave_link_fault(&sl);
			else
				s09_slave_rx_failed(&sl, lb.req, lb.req_n);
			lb.req_n = 0;
		}
		if (!lb.hold && sl.reply_len != 0 && sim.now - lb.rep_ready >= S09_T_REPLY_US) {
			const uint8_t *gone;
			size_t gone_n;

			(void)s09_slave_reply(&sl, &gone, &gone_n);
			lb.dropped++;
		}
		/* ... and a reply it was sending goes on (S-09.013) */
		if (lb.rep_hold && sim.now - lb.rep_hold_at >= S09_T_REPLY_US)
			lb.rep_hold = false;
		lb.merged += lb.hold || lb.rep_hold;
		lb.reply_session = lb.rep_hold || (!lb.hold && sl.reply_len != 0);
		lb.breaking = lb.reply_session && lb.break_reply_in != 0u &&
			      --lb.break_reply_in == 0u;
		lb.done = false;
		lb.late_now = false;
		lb.f_now = false;
		if (!lb.hold)
			lb.req_n = 0;
		if (!lb.rep_hold)
			lb.rep_at = 0;
		lb.hold = false;
		if (lb.rep_hold) {
			lb.rep_hold = false;	/* the reply it was sending, on */
		} else if (lb.reply_session && !lb.dead) {
			(void)s09_slave_reply(&sl, &lb.rep, &lb.rep_n);
			if (lb.rep_n > 0 && fault_here(true, lb.rep[0])) {
				lb.f_now = true;
				memcpy(lb.rep_copy, lb.rep, lb.rep_n);
				if (lb.f_none) {
					/* "nothing waiting", whole, its check right
					 * where the mode has one */
					lb.rep_copy[0] = 0x06;
					lb.rep_copy[1] = 0xFF;
					lb.rep_copy[2] = lb.rep_copy[3] = 0;
					lb.rep_n = 4;
					if (lb.last_rung != 0u) {
						const uint32_t nc = s09_check_word(lb.rep_copy, 4);

						for (unsigned i = 0; i < 4u; i++)
							lb.rep_copy[4 + i] = (uint8_t)(nc >> (8u * i));
						lb.rep_n = 8;
					}
				} else if (lb.f_fill >= 0)	/* a line that carries nothing */
					memset(lb.rep_copy, lb.f_fill, lb.rep_n);
				else if (lb.f_echo)	/* an echo's reply instead */
					lb.rep_copy[0] = S09_ECHO;
				else if (!lb.f_break && lb.f_word * 4u < lb.rep_n)
					lb.rep_copy[lb.f_word * 4u + lb.f_word % 4u] ^=
						(uint8_t)(1u << (lb.f_word % 8u));
				lb.rep = lb.rep_copy;
			}
		}
	} else if (!on && lb.sel && !lb.dead) {
		if (lb.reply_session) {
			lb_log(lb.rep, lb.rep_at, 2u);
			lb.rep_hold = lb.rep_at < lb.rep_n;
			lb.rep_hold_at = sim.now;
		} else {
			const bool slow = lb.slow_cmd != 0u && lb.req_n >= S09_HDR &&
					  lb.req[0] == lb.slow_cmd;

			lb_request_end();
			if (slow) {
				lb.busy_until = sim.now + lb.slow_us;
				lb.slow_cmd = 0;
			}
			lb.rep_ready = (int32_t)(lb.busy_until - sim.now) > 0 ? lb.busy_until :
									 sim.now;
		}
	}
	lb.sel = on;
}

/* The first bytes of a request session: the faults it is armed with. */
static void lb_request_start(const uint8_t *d, size_t n)
{
	if (lb.req_n != 0 || n == 0)
		return;
	if (fault_here(false, d[0]))
		lb.f_now = true;
	if (lb.late_cmd != 0 && d[0] == lb.late_cmd) {
		lb.late_now = true;
		lb.late_cmd = 0;
	}
	/* a session in quad I/O against a slave listening in single, or the
	 * reverse, or on a rung the wire does not carry: the slave takes
	 * other words than the master clocks, and PA6 does not rise at the
	 * session's end (S-09.013) */
	if ((lb.rung >= S09_RUNG_QUAD) != (s09_slave_mode(&sl) == S09_MODE_QUAD) ||
	    (lb.deaf_rungs >> lb.rung & 1u))
		lb.late_now = true;
}

/* n bytes of a request session clocked; false when it broke on the way. */
static bool lb_take(const uint8_t *d, size_t n)
{
	if (lb.f_now && lb.f_break) {
		const size_t k = lb.f_word * 4u < n ? lb.f_word * 4u : n;

		memcpy(lb.req + lb.req_n, d, k);
		lb.req_n += k;
		return false;
	}
	memcpy(lb.req + lb.req_n, d, n);
	lb.req_n += n;
	lb.done = true;
	/* the wire's time on this rung, when a test sets one */
	lb.ns_owed += (uint64_t)(n / 4u) * lb.word_ns[lb.rung];
	sim.now += (uint32_t)(lb.ns_owed / 1000u);
	lb.ns_owed %= 1000u;
	return true;
}

static bool feed_broke(const uint8_t *d, size_t n, bool first);

static bool lb_send(void *c, const uint8_t *d, size_t n)
{
	(void)c;
	lb.sends++;
	lb.split += ++lb.sends_here > 1u;
	if (lb.dead || lb.req_n + n > sizeof lb.req)
		return false;
	if (lb.feed_fail != 0u && lb.feed_fails != 0u && !lb.reply_session &&
	    (lb.rung != 0u || lb.feed_plain))
		return feed_broke(d, n, true);
	if (lb.reply_session) {
		/* the slave offers a reply: it clocks that out, and hears none
		 * of the master's words; past its end the master's next word
		 * waits for a PA6 that stays high */
		lb.stale++;
		if (lb.rep_at + n > lb.rep_n) {
			lb.rep_at = lb.rep_n;
			lb.done = true;
			sim.now += S09_T_REPLY_US;
			return false;
		}
		lb.rep_at += n;
		lb.done = lb.rep_at == lb.rep_n;
		return true;
	}
	lb_request_start(d, n);
	return lb_take(d, n);
}

static bool lb_recv(void *c, uint8_t *d, size_t n)
{
	(void)c;
	if (lb.dead)
		return false;
	if (!lb.reply_session) {
		/* the slave waits for a request: it takes the master's idle
		 * words as one, and its line carries nothing */
		if (lb.req_n + n > sizeof lb.req)
			return false;
		lb.blind++;
		memset(d, 0, n);
		memset(lb.req + lb.req_n, 0, n);
		lb.req_n += n;
		lb.done = false;
		return true;
	}
	if (lb.rep_at + n > lb.rep_n)
		return false;
	if (lb.breaking && lb.rep_at >= 4u)
		return false;		/* the reply stops after its first word */
	if (lb.f_now && lb.f_break && lb.rep_at + n > lb.f_word * 4u)
		return false;		/* ... or at the armed word */
	memcpy(d, lb.rep + lb.rep_at, n);
	lb.rep_at += n;
	lb.done = lb.rep_at == lb.rep_n;
	if (lb.done && lb.end_cmd != 0u && lb.rep[0] == lb.end_cmd) {
		lb.late_now = true;
		s09_slave_reply_end_changed(&sl, lb.end_cmd, lb.rep_n);
		lb.end_cmd = 0;
	}
	return true;
}

static void lb_configure(void *c, int phase)
{
	(void)c;
	lb.rung = phase > 1 ? (unsigned)phase - 1u : 0u;
	if (phase == 0)
		lb.unreset = false;
}

/* A failed send: feed_bytes of d clocked, then false. A session cut short
 * reaches the slave as one (lb_request_end()); one with every byte is taken
 * whole. */
static bool feed_broke(const uint8_t *d, size_t n, bool first)
{
	const size_t k = lb.feed_bytes < n ? lb.feed_bytes : n;

	if (lb.feed_fails != ~0u)
		lb.feed_fails--;
	lb.unreset = true;
	lb.dead = lb.feed_kills;
	if (first)
		lb_request_start(d, n);
	memcpy(lb.req + lb.req_n, d, k);
	lb.req_n += k;
	lb.done = k == n;
	return false;
}

/* The session's last word moved (S-13b.083: the master's receive closed). */
static void lb_end(void *c)
{
	(void)c;
	lb.ends++;
}

static const struct s09_bus_ops bus = {
	0, lb_ready, lb_attention, lb_select, lb_send, lb_recv, lb_configure, lb_end,
};
static const struct s09_bus_ops *use_bus = &bus;
/* ... and with the link brought to this rung first (S-09.075); -1: as it
 * comes up. */
static int use_rung = -1;

static struct s09_master ms;
static struct s09_files F;
static uint8_t big[0x10000], back[0x10000];
static char path_buf[S09_FILES_PATH_MAX];
static uint8_t image[S03_OS_MAX];

/* A fresh volume (erased to 0) and bridge; the volume mounted over it. */
static void fresh(void)
{
	wire_s09_release(&S);
	wire_yaffs_detach(&Y);
	memset(vol, 0, VOL_LEN);
	memset(&emmc, 0, sizeof emmc);
	memset(&lb, 0, sizeof lb);
	memset(&ex, 0, sizeof ex);
	lb.hear_quad = -1;
	wb_ramdisk_init(&bd, &ram, vol, (uint32_t)(VOL_LEN / 512u));
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0, BLOCKS - 1));
	wire_s09_init(&S, &Y);
	t_sim_init(&sim, S06_MS(2000000));	/* under 2^31 us, as the clock compares */
	s09_slave_init(&sl, &store);
	s09_master_init(&ms, use_bus, &sim.clock);
	CHECK(s09_master_start(&ms));
	if (use_rung >= 0) {
		CHECK(s09_master_link_fast(&ms, (unsigned)use_rung));
		CHECK_EQ(s09_master_rung(&ms), (unsigned)use_rung);
		/* S-13b.083: every request session by the bus's one send */
		CHECK(lb.split == 0u);
		CHECK_EQ(s09_master_fell(&ms), 0u);
	}
	s09_files_init(&F, &ms);
	CHECK_EQ(s09_files_mount(&F), 0);
	CHECK(!s09_files_volume_refused(&F));
}

/* A file with its directories. */
static bool put(const char *path, const uint8_t *d, uint32_t n)
{
	int32_t fd;
	bool ok;

	if (!s09_files_mkdirs(&F, path))
		return false;
	fd = s09_files_open(&F, path, S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);
	ok = fd >= 0 && s09_files_write(&F, fd, d, n) == (int32_t)n;
	return s09_files_close(&F, fd) == 0 && ok;
}

static void pattern(uint8_t *d, size_t n, uint32_t seed)
{
	for (size_t i = 0; i < n; i++) {
		seed = seed * 1103515245u + 12345u;
		d[i] = (uint8_t)(seed >> 16);
	}
}

/* An LDR block header, its checksum byte set. */
static void ldr_header(uint8_t *h, uint32_t flags, uint32_t target, uint32_t count,
		       uint32_t arg)
{
	const uint32_t w[4] = { 0xAD000000u | flags, target, count, arg };
	uint8_t x = 0;

	for (unsigned i = 0; i < 16u; i++)
		h[i] = (uint8_t)(w[i / 4u] >> (8u * (i % 4u)));
	for (unsigned i = 0; i < 16u; i++)
		x = (uint8_t)(x ^ h[i]);
	h[2] = x;
}

/* S-09.041, S-09.042: requests and their failures */
static void test_requests(void)
{
	struct s09_stat st;
	char name[300], longp[S09_FILES_PATH_MAX + 8];
	uint8_t type;
	unsigned before;

	t_case = "requests";
	fresh();
	CHECK_EQ(s09_files_stat(&F, "/yaffs2/none", &st), -1);
	CHECK_EQ(F.err, S09_ENOENT);
	CHECK_EQ(s09_files_open(&F, "none", S09_O_RDONLY), -1);
	CHECK_EQ(F.err, S09_ENOENT);
	CHECK_EQ(s09_files_mkdir(&F, "d"), 0);
	CHECK_EQ(s09_files_mkdir(&F, "d"), -1);
	CHECK_EQ(F.err, S09_EEXIST);
	CHECK(put("d/f", (const uint8_t *)"abc", 3));
	CHECK_EQ(s09_files_rmdir(&F, "d"), -1);
	CHECK_EQ(F.err, S09_ENOTEMPTY);
	CHECK_EQ(s09_files_rename(&F, "d/none", "d/g"), -1);
	CHECK_EQ(F.err, S09_ENOENT);
	CHECK_EQ(s09_files_rename(&F, "d/f", "/yaffs2/d/g"), 0);
	CHECK_EQ(s09_files_stat(&F, "d/g", &st), 0);
	CHECK_EQ(st.size, 3u);
	CHECK_EQ(st.mode & 0xF000u, 0x8000u);
	CHECK(s09_files_dir_exists(&F, "/yaffs2/d"));
	CHECK(!s09_files_dir_exists(&F, "d/g"));
	CHECK(!s09_files_mkdirs(&F, "d/g/x/y"));	/* d/g is a file */
	CHECK(s09_files_mkdirs(&F, "m/n/o"));		/* the last is not made */
	CHECK(s09_files_dir_exists(&F, "m/n") && !s09_files_dir_exists(&F, "m/n/o"));
	CHECK_EQ(s09_files_unlink(&F, "d/none"), -1);
	CHECK_EQ(F.err, S09_ENOENT);
	/* listings: "." and ".." never show; past the end is false, errno 0 */
	CHECK(s09_files_entry(&F, "d", 0, name, sizeof name, &type));
	CHECK(!strcmp(name, "g") && type == 8);
	CHECK(!s09_files_entry(&F, "d", 1, name, sizeof name, &type));
	CHECK_EQ(F.err, 0u);
	CHECK(!s09_files_entry(&F, "d", 0, name, 1, &type));	/* no room */
	CHECK_EQ(F.err, S09_ENAMETOOLONG);
	CHECK(!s09_files_entry(&F, "none", 0, name, sizeof name, &type));
	CHECK_EQ(F.err, S09_ENOENT);
	/* /fat32 has no volume (S-09.041) */
	CHECK_EQ(s09_files_stat(&F, "/fat32/x", &st), -1);
	CHECK_EQ(F.err, S09_ENODEV);
	/* a path too long is refused without a request */
	memset(longp, 'a', sizeof longp - 1);
	longp[sizeof longp - 1] = '\0';
	before = lb.sessions;
	CHECK_EQ(s09_files_stat(&F, longp, &st), -1);
	CHECK_EQ(F.err, S09_ENAMETOOLONG);
	CHECK_EQ(lb.sessions, before);
	/* the boot-partition size (0x05, S-03.008) */
	CHECK_EQ(s09_files_boot_size(&F), B_SIZE);
}

/* S-09.035 and 0x2B: files over several pieces */
static void test_long_files(void)
{
	struct s09_stat st;
	uint32_t len;
	size_t n = 3u * 0x1000u + 123u;

	t_case = "long files";
	fresh();
	pattern(big, n, 7);
	lb.writes = 0;
	CHECK(put("long.bin", big, (uint32_t)n));
	CHECK_EQ(lb.writes, 4u);			/* 3 x 0x1000 and 123 */
	CHECK_EQ(s09_files_stat(&F, "long.bin", &st), 0);
	CHECK_EQ(st.size, (uint32_t)n);
	memset(back, 0, sizeof back);
	lb.reads = 0;
	CHECK_EQ(s09_files_read_file(&F, "long.bin", back, sizeof back), n);
	CHECK_EQ(lb.reads, 1u);			/* one 0x2A, its pieces polled */
	CHECK(!memcmp(back, big, n));
	/* the first cap bytes */
	memset(back, 0, sizeof back);
	CHECK_EQ(s09_files_read_file(&F, "long.bin", back, 0x1801), 0x1801u);
	CHECK(!memcmp(back, big, 0x1801) && back[0x1801] == 0);
	/* whole: refused when longer than the room, and for a directory */
	CHECK(s09_files_read_whole(&F, "long.bin", back, (uint32_t)n, &len));
	CHECK_EQ(len, (uint32_t)n);
	CHECK(!s09_files_read_whole(&F, "long.bin", back, (uint32_t)n - 1u, &len));
	CHECK_EQ(F.err, S09_ERANGE);
	CHECK(!s09_files_read_whole(&F, "/yaffs2", back, sizeof back, &len));
	CHECK(!s09_files_read_whole(&F, "none", back, sizeof back, &len));
	CHECK_EQ(F.err, S09_ENOENT);
	/* an exact multiple of the piece */
	CHECK(put("four.bin", big, 0x4000));
	memset(back, 0, sizeof back);
	CHECK_EQ(s09_files_read_file(&F, "four.bin", back, sizeof back), 0x4000u);
	CHECK(!memcmp(back, big, 0x4000));
}

/* The close that answers once the eMMC has flushed its cache (S-03.068). */
static void test_close_sync(void)
{
	struct s09_stat st;
	int32_t fd;

	t_case = "close sync";
	fresh();
	/* a plain close sends no flush */
	CHECK(put("plain.bin", (const uint8_t *)"abc", 3));
	CHECK_EQ(emmc.flushes, 0u);
	/* the close, then one flush */
	fd = s09_files_open(&F, "os.ldr", S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);
	CHECK(fd >= 0);
	CHECK_EQ(s09_files_write(&F, fd, (const uint8_t *)"waffle", 6), 6);
	CHECK_EQ(s09_files_close_sync(&F, fd), 0);
	CHECK_EQ(emmc.flushes, 1u);
	CHECK_EQ(s09_files_stat(&F, "os.ldr", &st), 0);
	CHECK_EQ(st.size, 6u);
	/* a flush that fails fails the call */
	fd = s09_files_open(&F, "os.ldr", S09_O_WRONLY | S09_O_TRUNC);
	CHECK(fd >= 0);
	emmc.fail_flush_from = emmc.flushes + 1u;
	CHECK_EQ(s09_files_close_sync(&F, fd), -1);
	CHECK_EQ(emmc.flushes, 2u);
	emmc.fail_flush_from = 0;
	/* a close that fails sends no flush */
	CHECK_EQ(s09_files_close_sync(&F, 77), -1);
	CHECK_EQ(emmc.flushes, 2u);
}

/* S-11.080: the staging area's operations */
static void test_staging(void)
{
	struct s11_fs_ops ops;
	char name[S11_PATH_MAX];
	bool dir = false, seen_a = false, seen_d = false;
	uint32_t size = 0;
	size_t n = 2u * 0x1000u + 1u;

	t_case = "staging";
	fresh();
	s09_files_fs_ops(&F, &ops);
	CHECK(ops.ctx == &F);
	pattern(big, n, 11);
	CHECK(s09_files_mkdirs(&F, "upgrade/a/"));
	CHECK(put("upgrade/a/x.bin", big, (uint32_t)n));
	CHECK(put("upgrade/top", (const uint8_t *)"t", 1));
	CHECK(ops.stat(ops.ctx, "upgrade/a/x.bin", &dir, &size));
	CHECK(!dir && size == n);
	CHECK(ops.stat(ops.ctx, "upgrade/a", &dir, &size));
	CHECK(dir);
	CHECK(!ops.stat(ops.ctx, "upgrade/none", &dir, &size));
	memset(back, 0, sizeof back);
	CHECK_EQ(ops.read(ops.ctx, "upgrade/a/x.bin", back, sizeof back), n);
	CHECK(!memcmp(back, big, n));
	CHECK_EQ(ops.read(ops.ctx, "upgrade/none", back, sizeof back), 0u);
	for (size_t i = 0; ops.list(ops.ctx, "upgrade", i, name, sizeof name, &dir); i++) {
		CHECK(i < 2);
		if (!strcmp(name, "a"))
			seen_a = dir;
		else
			seen_d = !strcmp(name, "top") && !dir;
	}
	CHECK(seen_a && seen_d);
	/* copy replaces and creates the directories `to` implies */
	CHECK(put("content/p/x.bin", (const uint8_t *)"old", 3));
	CHECK(ops.copy(ops.ctx, "upgrade/a/x.bin", "content/p/x.bin"));
	CHECK(ops.copy(ops.ctx, "upgrade/a/x.bin", "content/q/r/x.bin"));
	memset(back, 0, sizeof back);
	CHECK_EQ(s09_files_read_file(&F, "content/q/r/x.bin", back, sizeof back), n);
	CHECK(!memcmp(back, big, n));
	CHECK_EQ(s09_files_read_file(&F, "content/p/x.bin", back, sizeof back), n);
	CHECK(!ops.copy(ops.ctx, "upgrade/none", "content/z"));
	CHECK_EQ(F.err, S09_ENOENT);
	/* move replaces a file and creates directories */
	CHECK(put("dest/m/top", (const uint8_t *)"older", 5));
	CHECK(ops.move(ops.ctx, "upgrade/top", "dest/m/top"));
	CHECK(ops.move(ops.ctx, "upgrade/a/x.bin", "new/dir/x.bin"));
	CHECK(!ops.stat(ops.ctx, "upgrade/top", &dir, &size));
	CHECK(ops.stat(ops.ctx, "dest/m/top", &dir, &size));
	CHECK(!dir && size == 1);
	CHECK(ops.stat(ops.ctx, "new/dir/x.bin", &dir, &size));
	CHECK(!dir && size == n);
	/* remove: a tree, a file, a missing path */
	CHECK(put("upgrade/a/b/c/d/e", (const uint8_t *)"e", 1));
	CHECK(put("upgrade/a/b/f", (const uint8_t *)"f", 1));
	CHECK(ops.remove(ops.ctx, "upgrade"));
	CHECK(!ops.stat(ops.ctx, "upgrade", &dir, &size));
	CHECK(ops.remove(ops.ctx, "upgrade"));
	CHECK(ops.remove(ops.ctx, "dest/m/top"));
	CHECK(!ops.stat(ops.ctx, "dest/m/top", &dir, &size));
	CHECK(ops.stat(ops.ctx, "dest/m", &dir, &size) && dir);
}

/* S-11.031 - S-11.033, S-11.062: the staging writes */
static void test_stage_writes(void)
{
	struct s09_stat st;
	char name[S11_PATH_MAX], longp[S09_FILES_PATH_MAX];
	uint64_t free = 0, yfree = 0;
	uint32_t len = 0;
	uint8_t type, rec[2];
	size_t n = 3u * 0x1000u + 7u;

	t_case = "stage writes";
	fresh();
	/* a reset with no upgrade/: its stat leaves ENOENT as the volume's last
	 * error, which the free space does not take for a failure */
	CHECK(s09_files_staging_reset(&F));
	CHECK(s09_files_dir_exists(&F, "upgrade"));
	CHECK(s09_files_free_space(&F, &free));
	CHECK(wire_yaffs_free_space(&Y, &yfree));
	CHECK(free <= yfree && yfree - free < 512u && free > 0u);
	/* a reset over a tree empties upgrade/ only */
	CHECK(put("upgrade/a/b/c", (const uint8_t *)"c", 1));
	CHECK(put("upgrade/progress.log", (const uint8_t *)"\x03\x00", 2));
	CHECK(put("keep/x", (const uint8_t *)"x", 1));
	CHECK(s09_files_staging_reset(&F));
	CHECK(!s09_files_entry(&F, "upgrade", 0, name, sizeof name, &type));
	CHECK_EQ(F.err, 0u);
	CHECK_EQ(s09_files_stat(&F, "keep/x", &st), 0);
	/* directories, with and without a trailing '/', and again */
	CHECK(s09_files_staging_mkdir(&F, "d/e"));
	CHECK(s09_files_staging_mkdir(&F, "f/"));
	CHECK(s09_files_staging_mkdir(&F, "d/e"));
	CHECK(s09_files_dir_exists(&F, "upgrade/d/e"));
	CHECK(s09_files_dir_exists(&F, "upgrade/f"));
	/* a file over several 0x2B pieces, its directories made, one open at
	 * a time */
	pattern(big, n, 5);
	CHECK(s09_files_staging_open(&F, "g/h/i.bin"));
	CHECK(!s09_files_staging_open(&F, "other"));
	CHECK(s09_files_staging_write(&F, big, 100));
	CHECK(s09_files_staging_write(&F, big + 100, n - 100u));
	CHECK(s09_files_staging_close(&F));
	CHECK(!s09_files_staging_close(&F));
	CHECK(!s09_files_staging_write(&F, big, 1));
	memset(back, 0, sizeof back);
	CHECK(s09_files_read_whole(&F, "upgrade/g/h/i.bin", back, sizeof back, &len));
	CHECK(len == n && !memcmp(back, big, n));
	/* a file staged again is replaced */
	CHECK(s09_files_staging_open(&F, "g/h/i.bin"));
	CHECK(s09_files_staging_write(&F, (const uint8_t *)"ab", 2));
	CHECK(s09_files_staging_close(&F));
	CHECK_EQ(s09_files_stat(&F, "upgrade/g/h/i.bin", &st), 0);
	CHECK_EQ(st.size, 2u);
	/* a parent that is a file; a path too long */
	CHECK(!s09_files_staging_open(&F, "g/h/i.bin/j"));
	CHECK(!s09_files_staging_mkdir(&F, "g/h/i.bin/k"));
	CHECK(F.stage_fd < 0);
	memset(longp, 'a', sizeof longp);
	longp[sizeof longp - 8u] = '\0';	/* upgrade/ makes it 512 */
	CHECK(!s09_files_staging_open(&F, longp));
	CHECK_EQ(F.err, S09_ENAMETOOLONG);
	CHECK(!s09_files_staging_mkdir(&F, longp));
	/* the progress record, replaced each time */
	CHECK(s09_files_record_write(&F, 3));
	s11_record_bytes(3, rec);
	CHECK(s09_files_read_whole(&F, S11_RECORD_PATH, back, sizeof back, &len));
	CHECK(len == 2u && !memcmp(back, rec, 2));
	CHECK(s09_files_record_write(&F, 4));
	s11_record_bytes(4, rec);
	CHECK(s09_files_read_whole(&F, S11_RECORD_PATH, back, sizeof back, &len));
	CHECK(len == 2u && !memcmp(back, rec, 2));
	CHECK(back[0] == 4u && back[1] == 0u);	/* S-11.062: the count stays 0 */
}

/* One staged file of a byte; the mkdir and stat requests it took. */
static bool staged_one(const char *path, unsigned *mk, unsigned *st)
{
	bool ok;

	lb.mkdirs = lb.stats = 0;
	ok = s09_files_staging_open(&F, path) &&
	     s09_files_staging_write(&F, (const uint8_t *)"x", 1);
	ok = s09_files_staging_close(&F) && ok;
	*mk = lb.mkdirs;
	*st = lb.stats;
	return ok;
}

/* The staging area sends no mkdir for a directory its last path made. */
static void test_stage_dirs(void)
{
	struct s09_stat st;
	unsigned mk, sn;

	t_case = "stage dirs";
	fresh();
	CHECK(s09_files_staging_reset(&F));
	/* upgrade is there (a mkdir that fails, then a stat); a, b, c are new */
	CHECK(staged_one("a/b/c/1", &mk, &sn) && mk == 4u && sn == 1u);
	CHECK(staged_one("a/b/c/2", &mk, &sn) && mk == 0u && sn == 0u);
	CHECK(staged_one("a/b/d/3", &mk, &sn) && mk == 1u && sn == 0u);
	CHECK(staged_one("a/4", &mk, &sn) && mk == 0u && sn == 0u);
	/* only the last path's directories are remembered: b and c again */
	CHECK(staged_one("a/b/c/5", &mk, &sn) && mk == 2u && sn == 2u);
	/* a name that only begins like a remembered one is another directory */
	CHECK(staged_one("a/b/cc/6", &mk, &sn) && mk == 1u && sn == 0u);
	CHECK(staged_one("a/bb/7", &mk, &sn) && mk == 1u && sn == 0u);
	CHECK(s09_files_staging_mkdir(&F, "a/bb/e") && s09_files_dir_exists(&F, "upgrade/a/bb/e"));
	lb.mkdirs = 0;
	CHECK(s09_files_staging_mkdir(&F, "a/bb/e/f") && lb.mkdirs == 1u);
	/* a removal forgets them */
	CHECK(s09_files_remove_tree(&F, "upgrade/a/bb"));
	CHECK(staged_one("a/bb/e/8", &mk, &sn) && mk == 4u && sn == 2u);
	CHECK_EQ(s09_files_stat(&F, "upgrade/a/bb/e/8", &st), 0);
	/* a remembered directory that is gone all the same is made again */
	CHECK(staged_one("a/bb/e/9", &mk, &sn) && mk == 0u);
	CHECK_EQ(s09_files_unlink(&F, "upgrade/a/bb/e/8"), 0);
	CHECK_EQ(s09_files_unlink(&F, "upgrade/a/bb/e/9"), 0);
	CHECK_EQ(yaffs_rmdir("/yaffs2/upgrade/a/bb/e"), 0);
	CHECK(staged_one("a/bb/e/10", &mk, &sn) && mk == 4u);
	CHECK_EQ(s09_files_stat(&F, "upgrade/a/bb/e/10", &st), 0);
	/* the record and every file after a reset */
	CHECK(s09_files_record_write(&F, 2));
	CHECK(s09_files_staging_reset(&F));
	CHECK(staged_one("a/b/c/1", &mk, &sn) && mk == 4u && sn == 1u);
}

/* A staging write is left running: the next request collects it first, and
 * its failure shows at the next write or at the close. */
static void test_stage_running(void)
{
	const bool wide = use_rung > 0;		/* writes of up to 0x8000 */
	uint32_t len = 0;

	t_case = "stage running";
	fresh();
	CHECK(s09_files_staging_reset(&F));
	pattern(big, sizeof big, 9);
	CHECK(s09_files_staging_open(&F, "r/one"));
	lb.writes = 0;
	CHECK(s09_files_staging_write(&F, big, 0x1000));
	CHECK(lb.writes == 1u);
	/* any other request: the write reaches the slave first */
	CHECK(s09_files_dir_exists(&F, "upgrade/r"));
	CHECK(lb.writes == 1u);
	CHECK(s09_files_staging_write(&F, big + 0x1000, 0x2000 + 5u));
	CHECK(s09_files_staging_close(&F));
	CHECK_EQ(lb.writes, wide ? 2u : 4u);
	memset(back, 0, sizeof back);
	CHECK(s09_files_read_whole(&F, "upgrade/r/one", back, sizeof back, &len));
	CHECK(len == 0x3005u && !memcmp(back, big, len));

	/* a write the slave refuses: known when the next one is asked for */
	CHECK(s09_files_staging_open(&F, "r/two"));
	CHECK_EQ(s09_files_close(&F, F.stage_fd), 0);	/* behind its back */
	CHECK(s09_files_staging_write(&F, big, 0x1000));
	CHECK(!s09_files_staging_write(&F, big, 0x1000));
	CHECK_EQ(F.err, S09_EBADF);
	CHECK(!s09_files_staging_close(&F));
	/* ... or at the close, when it was the last */
	CHECK(s09_files_staging_open(&F, "r/three"));
	CHECK_EQ(s09_files_close(&F, F.stage_fd), 0);
	CHECK(s09_files_staging_write(&F, big, 7));
	CHECK(!s09_files_staging_close(&F));
	CHECK(F.stage_fd < 0 && F.stage_sent == 0u);
	/* the staging area still works after it */
	CHECK(s09_files_staging_open(&F, "r/four"));
	CHECK(s09_files_staging_write(&F, big, 0x1001));
	CHECK(s09_files_staging_close(&F));
	CHECK(s09_files_read_whole(&F, "upgrade/r/four", back, sizeof back, &len));
	CHECK(len == 0x1001u && !memcmp(back, big, len));
	CHECK(lb.split == 0u);
}

/* A fresh bridge as it comes up. */
static void fresh_plain(void)
{
	const int rung = use_rung;

	use_rung = -1;
	fresh();
	use_rung = rung;
}

/* One staged file of n bytes of the pattern; read back whole. */
static bool staged_file(const char *path, uint32_t n, uint32_t seed)
{
	uint32_t len = 0;
	bool ok;

	pattern(big, n, seed);
	ok = s09_files_staging_open(&F, path) && s09_files_staging_write(&F, big, n);
	ok = s09_files_staging_close(&F) && ok;
	memset(back, 0, sizeof back);
	path_buf[0] = '\0';
	strcat(strcat(path_buf, "upgrade/"), path);
	return ok && s09_files_read_whole(&F, path_buf, back, sizeof back, &len) &&
	       len == n && !memcmp(back, big, n);
}

/* S-09.070-S-09.075: the link's rungs, their tests and the way back. */
static void test_link_rungs(void)
{
	t_case = "link rungs";

	/* every rung good: the top one, every request session of the raise
	 * by the bus's one send (S-13b.083), nothing lost */
	fresh_plain();
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_PLAIN);
	CHECK_EQ(s09_master_write_max(&ms), S09_TUNNEL_MAX);
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_TOP);
	CHECK(s09_master_fell(&ms) == 0u && lb.split == 0u);
	CHECK(lb.ends != 0u);
	CHECK(s09_slave_mode(&sl) == S09_MODE_QUAD && !s09_slave_on_trial(&sl));
	CHECK_EQ(s09_master_write_max(&ms), S09_WRITE_MAX);
	CHECK(lb.checked > 12u && lb.garbled == 0u);
	CHECK(s09_files_staging_reset(&F));
	lb.writes = 0;
	CHECK(staged_file("t/top", 0x4321u, 1));
	CHECK_EQ(lb.writes, 1u);		/* one request, not five */
	CHECK(lb.split == 0u);
	/* asked again: nothing to do, nothing sent */
	lb.sessions = 0;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP) && lb.sessions == 0u);
	/* a lower ceiling than the rung it is on leaves it there */
	CHECK(s09_master_link_fast(&ms, S09_RUNG_CHECKED));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_TOP);

	/* the faster quad clock corrupts: the slower quad rung is kept */
	fresh_plain();
	lb.bad_rungs = 1u << S09_RUNG_QUAD_FAST;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_QUAD);
	CHECK_EQ(s09_master_fell(&ms), 1u);
	CHECK(s09_slave_mode(&sl) == S09_MODE_QUAD && !s09_slave_on_trial(&sl));
	CHECK(s09_files_staging_reset(&F) && staged_file("t/quad", 0x2000u, 2));
	/* ... and it is not tried again */
	lb.sessions = 0;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP) && lb.sessions == 0u);

	/* quad I/O is not heard at all: single I/O at the faster clock */
	fresh_plain();
	lb.deaf_rungs = 1u << S09_RUNG_QUAD_FAST | 1u << S09_RUNG_QUAD;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_CHECKED_FAST);
	CHECK_EQ(s09_master_fell(&ms), 2u);
	CHECK(s09_slave_mode(&sl) == S09_MODE_CHECKED && !s09_slave_on_trial(&sl));
	CHECK(s09_files_staging_reset(&F) && staged_file("t/fast", 0x9000u, 3));

	/* nothing above the plain rung works: the link is as it came up, and
	 * installs go on */
	fresh_plain();
	lb.bad_rungs = 0x1Eu;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_PLAIN);
	/* rungs 4, 3 and 2, then rung 1 (S-09.075) */
	CHECK(s09_master_fell(&ms) == 4u && !ms.absent);
	CHECK(s09_master_settled(&ms) && !sl.recovering);
	CHECK(lb.split == 0u);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_PLAIN);
	CHECK_EQ(s09_master_write_max(&ms), S09_TUNNEL_MAX);
	CHECK(s09_files_staging_reset(&F));
	lb.writes = 0;
	CHECK(staged_file("t/plain", 0x2001u, 4));
	CHECK_EQ(lb.writes, 3u);
	CHECK(lb.split == 0u);

	/* a ceiling: no rung above it is asked for */
	fresh_plain();
	CHECK(s09_master_link_fast(&ms, S09_RUNG_CHECKED_FAST));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_CHECKED_FAST);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_CHECKED);
	CHECK(s09_master_link_fast(&ms, S09_RUNG_QUAD));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_QUAD);
	/* a ceiling of 1: rung 1, checked, with no gate */
	fresh_plain();
	CHECK(s09_master_link_fast(&ms, S09_RUNG_CHECKED));
	CHECK(s09_master_rung(&ms) == S09_RUNG_CHECKED);
	CHECK(s09_slave_mode(&sl) == S09_MODE_CHECKED && !s09_slave_on_trial(&sl));
}

/* S-09.074, S-09.076: a request that does not get through on a rung:
 * both ends back in the plain mode, then on the next rung down that passes
 * its tests, and the request sent again with its number, done once. */
static void test_link_fallback(void)
{
	struct s09_stat st;
	uint32_t len = 0;

	t_case = "link fallback";
	/* a request arrives with a bit wrong: not executed; sent again on
	 * the rung below */
	for (unsigned when = 1; when <= 6u; when++) {
		fresh_plain();
		CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
		CHECK(s09_files_staging_reset(&F));
		lb.flip_in = when;	/* the open, a write, its poll, ... */
		ex.file[S09_WRITE] = 0;
		CHECK(staged_file("f/once", 0x7000u, 10u + when));
		CHECK_EQ(s09_master_rung(&ms), S09_RUNG_QUAD);
		CHECK_EQ(s09_slave_mode(&sl), S09_MODE_QUAD);
		CHECK(s09_master_fell(&ms) == 1u && !ms.absent);
		CHECK_EQ(s09_files_stat(&F, "upgrade/f/once", &st), 0);
		CHECK_EQ(st.size, 0x7000u);		/* once, not twice */
		CHECK_EQ(ex.file[S09_WRITE], 1u);
		/* the rung that failed is not tried again */
		lb.sessions = 0;
		CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP) && lb.sessions == 0u);
		CHECK(staged_file("f/after", 0x3000u, 20u + when));
	}

	/* the slave falls back by itself (its trial or its receive ran
	 * out): the master's next request finds out, and is done once */
	fresh_plain();
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(s09_files_staging_reset(&F));
	s09_slave_link_fault(&sl);
	CHECK(s09_slave_attention(&sl));
	ex.file[S09_WRITE] = 0;
	CHECK(staged_file("f/alone", 0x1234u, 30));
	CHECK(s09_master_rung(&ms) == S09_RUNG_QUAD && s09_master_fell(&ms) == 1u);
	CHECK(!s09_slave_attention(&sl) && ex.file[S09_WRITE] == 1u);

	/* ... and held mode's service finds out with nothing to send */
	fresh_plain();
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	s09_slave_link_fault(&sl);
	s09_master_service(&ms);
	CHECK(s09_master_rung(&ms) == S09_RUNG_QUAD && !ms.absent);
	CHECK(!s09_slave_attention(&sl));
	CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);

	/* a chunked read whose later poll is lost goes on where it was */
	fresh_plain();
	pattern(big, 0x3000, 40);
	CHECK(put("/yaffs2/r", big, 0x3000));
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	lb.flip_in = 4u;		/* the stat, the open, the read, a poll */
	memset(back, 0, sizeof back);
	ex.pieces = 0;
	CHECK(s09_files_read_whole(&F, "/yaffs2/r", back, sizeof back, &len));
	CHECK(len == 0x3000u && !memcmp(back, big, len));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_QUAD);
	CHECK_EQ(ex.pieces, 3u);

	/* a reply the slave had begun breaks after its first word: outside
	 * the plain mode the poll is sent again with its number and the
	 * slave gives the same reply; the write was done once */
	fresh_plain();
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(s09_files_staging_reset(&F));
	CHECK(s09_files_staging_open(&F, "f/broken"));
	lb.writes = 0;
	ex.file[S09_WRITE] = 0;
	lb.break_reply_in = 1u;		/* the write's own reply, in its poll */
	CHECK(s09_files_staging_write(&F, big, 0x1800));
	CHECK(s09_files_staging_close(&F));
	CHECK(!ms.absent && s09_master_rung(&ms) == S09_RUNG_QUAD);
	CHECK(lb.writes == 1u && ex.file[S09_WRITE] == 1u);
	CHECK_EQ(s09_files_stat(&F, "upgrade/f/broken", &st), 0);
	CHECK_EQ(st.size, 0x1800u);		/* once */

	/* ... in the plain mode it is the slave absent, as ever (S-09.013) */
	fresh_plain();
	CHECK(s09_files_staging_reset(&F));
	CHECK(s09_files_staging_open(&F, "f/broken"));
	lb.writes = 0;
	lb.break_reply_in = 1u;
	CHECK(s09_files_staging_write(&F, big, 0x800));
	CHECK(!s09_files_staging_close(&F));
	CHECK(ms.absent && F.err == S09_ENODEV);
	CHECK_EQ(lb.writes, 1u);
	{
		struct yaffs_stat ys;

		CHECK_EQ(yaffs_stat("/yaffs2/upgrade/f/broken", &ys), 0);
		CHECK_EQ((uint32_t)ys.st_size, 0x800u);	/* once */
	}
	for (int h = 0; h < 200; h++)
		(void)yaffs_close(h);

	/* the bus dies during the way back: the slave is absent, as before */
	fresh_plain();
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(s09_files_staging_reset(&F));
	CHECK(s09_files_staging_open(&F, "f/dead"));
	lb.flip_in = 1u;
	lb.die_after = lb.sessions + 2u;
	CHECK(s09_files_staging_write(&F, big, 0x1000));
	CHECK(!s09_files_staging_close(&F));
	CHECK(ms.absent);
	for (int h = 0; h < 200; h++)
		(void)yaffs_close(h);

	/* a mode the slave does not know, a keep with no trial, a test too
	 * long: answered, nothing changed */
	fresh_plain();
	{
		struct s09_req r = { S09_LINK_MODE, 0, 0, 7, S09_LINK_TRY };
		uint32_t err;

		CHECK_EQ(s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err), 0);
		r.arg0 = S09_MODE_QUAD;
		r.arg1 = S09_LINK_KEEP;
		CHECK_EQ(s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err), 0);
		CHECK(err == 0u && s09_slave_mode(&sl) == S09_MODE_PLAIN);
	}
}

/* A fresh volume and bridge on bus b, brought to `rung` (0: as it comes
 * up). */
static void fresh_on(const struct s09_bus_ops *b, unsigned rung)
{
	const int r0 = use_rung;
	const struct s09_bus_ops *b0 = use_bus;

	use_bus = b;
	use_rung = rung == 0u ? -1 : (int)rung;
	fresh();
	use_rung = r0;
	use_bus = b0;
}

/* A fault armed for the first session of cmd from now: in its request or
 * its reply, at word w, a bit flipped or the session broken there. */
static void arm(uint8_t cmd, bool rep, bool brk, unsigned w)
{
	memset(lb.fs, 0, sizeof lb.fs);
	lb.fs[0].cmd = cmd;
	lb.fs[0].rep = rep;
	lb.fs[0].brk = brk;
	lb.fs[0].word = w;
	lb.fs[0].nth = 1;
	lb.nfs = 1;
	lb.f_fired = 0;
	lb.f_fill = -1;
	lb.f_echo = false;
}

/* G: a reply session that reads all zeros or all ones (a slave not
 * replying) is never taken for a reply: outside the plain mode it is sent
 * again, in it the slave is absent. */
static void test_link_blank_reply(void)
{
	struct s09_req r = { S09_BOOT_SIZE, 0, 0, 0, 0 };
	struct s09_stat st;
	uint32_t err;

	t_case = "link blank reply";
	for (int fill = 0; fill <= 0xFF; fill += 0xFF) {
		fresh_on(&bus, S09_RUNG_TOP);
		arm(S09_BOOT_SIZE, true, false, 0);
		lb.f_fill = fill;
		CHECK_EQ(s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err), (int32_t)B_SIZE);
		CHECK(lb.f_fired && !ms.absent && s09_master_fell(&ms) == 1u);
		ex.file[S09_MKDIR] = 0;
		arm(S09_MKDIR, true, false, 0);
		lb.f_fill = fill;
		CHECK_EQ(s09_files_mkdir(&F, "blank"), 0);
		CHECK(lb.f_fired && ex.file[S09_MKDIR] == 1u && !ms.absent);
		CHECK_EQ(s09_files_stat(&F, "blank", &st), 0);
		CHECK(s09_files_rmdir(&F, "blank") == 0);

		/* plain: 0x05 is S-09.013 rule 4's, so the request fails (EIO)
		 * and the slave is kept; a mkdir's costs the slave */
		fresh_on(&bus, 0);
		arm(S09_BOOT_SIZE, true, false, 0);
		lb.f_fill = fill;
		CHECK_EQ(s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err), -1);
		CHECK(err == S09_EIO && !ms.absent);
		CHECK_EQ(s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err), (int32_t)B_SIZE);
		arm(S09_MKDIR, true, false, 0);
		lb.f_fill = fill;
		CHECK(s09_files_mkdir(&F, "blank") < 0 && ms.absent);
	}
	/* an echo's reply where an immediate reply or a deferred one is
	 * waited for is never taken for it: a failed exchange */
	{
		fresh_on(&bus, S09_RUNG_TOP);
		arm(S09_BOOT_SIZE, true, false, 0);
		lb.f_echo = true;
		CHECK_EQ(s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err), (int32_t)B_SIZE);
		CHECK(lb.f_fired && s09_master_fell(&ms) == 1u && !ms.absent);
		ex.file[S09_MKDIR] = 0;
		arm(S09_MKDIR, true, false, 0);
		lb.f_echo = true;
		CHECK_EQ(s09_files_mkdir(&F, "echoed"), 0);
		CHECK(lb.f_fired && ex.file[S09_MKDIR] == 1u && s09_master_fell(&ms) == 2u);
		CHECK_EQ(s09_files_stat(&F, "echoed", &st), 0);
		fresh_on(&bus, 0);
		arm(S09_BOOT_SIZE, true, false, 0);
		lb.f_echo = true;
		CHECK(s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err) == -1 && err == S09_EIO &&
		      !ms.absent);
		arm(S09_MKDIR, true, false, 0);
		lb.f_echo = true;
		CHECK(s09_files_mkdir(&F, "echoed") < 0 && ms.absent);
	}
}

/* The bus every pass runs on: one send, by DMA (S-13b.083). */
#define BUSES 1u
static const struct s09_bus_ops *const buses[BUSES] = { &bus };

/* S-09.076: a reply whose first word comes wrong after the slave executed
 * the request: sent again with its number, the same reply comes, the
 * request done once. A write's, a chunked read's and an immediate's, and
 * an immediate reply that fails after its first word. */
static void test_link_repeat(void)
{
	struct s09_stat st;
	uint32_t len = 0;

	t_case = "link repeat";
	for (unsigned b = 0; b < BUSES; b++) {
		/* the rung below; from rung 1, rung 1 again (S-09.075) */
		for (unsigned rung = S09_RUNG_CHECKED; rung <= S09_RUNG_TOP; rung++) {
			const unsigned below = rung > S09_RUNG_CHECKED ? rung - 1u :
					       S09_RUNG_CHECKED;
			struct s09_req r = { S09_BOOT_SIZE, 0, 0, 0, 0 };
			uint32_t err;

			fresh_on(buses[b], rung);
			CHECK(s09_files_staging_reset(&F));
			ex.file[S09_WRITE] = 0;
			arm(S09_WRITE, true, false, 0);
			CHECK(staged_file("c/w", 0x1800u, 50u + rung));
			CHECK(lb.f_fired && !ms.absent);
			CHECK_EQ(ex.file[S09_WRITE], 1u);
			CHECK_EQ(s09_files_stat(&F, "upgrade/c/w", &st), 0);
			CHECK_EQ(st.size, 0x1800u);
			CHECK_EQ(s09_master_rung(&ms), below);

			fresh_on(buses[b], rung);
			pattern(big, 0x3000, 60u + rung);
			CHECK(put("/yaffs2/r", big, 0x3000));
			ex.pieces = 0;
			arm(S09_READ, true, false, 0);	/* its first piece */
			memset(back, 0, sizeof back);
			CHECK(s09_files_read_whole(&F, "/yaffs2/r", back, sizeof back, &len));
			CHECK(lb.f_fired && len == 0x3000u && !memcmp(back, big, len));
			CHECK_EQ(ex.pieces, 3u);
			CHECK_EQ(s09_master_rung(&ms), below);

			fresh_on(buses[b], rung);
			arm(S09_BOOT_SIZE, true, true, 1);	/* after its first word */
			CHECK_EQ(s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err), (int32_t)B_SIZE);
			CHECK(lb.f_fired && err == 0u && !ms.absent);
			fresh_on(buses[b], rung);
			arm(S09_BOOT_SIZE, true, false, 0);
			CHECK_EQ(s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err), (int32_t)B_SIZE);
			CHECK(lb.f_fired && err == 0u && !ms.absent);
			CHECK_EQ(s09_master_rung(&ms), below);
		}
	}

	/* S-09.076: a poll's complete "nothing waiting" does not make a later
	 * failure the slave's absence */
	fresh_on(&bus, S09_RUNG_TOP);
	lb.spurious_att = true;
	s09_master_service(&ms);
	CHECK(!lb.spurious_att && !ms.absent);
	CHECK(s09_files_staging_reset(&F));
	lb.flip_in = 2u;
	CHECK(staged_file("c/after", 0x900u, 70));
	CHECK(!ms.absent && s09_master_rung(&ms) == S09_RUNG_QUAD);
}

/* S-09.076: a request the slave took whole whose session failed at its
 * end: sent again with the number, not executed twice, no second reply left
 * waiting. */
static void test_link_not_begun(void)
{
	t_case = "link not begun";
	pattern(big, 0x6000, 80);
	for (unsigned b = 0; b < BUSES; b++) {
		uint32_t len = 0;

		fresh_on(buses[b], S09_RUNG_CHECKED_FAST);
		CHECK(s09_files_staging_reset(&F));
		ex.file[S09_WRITE] = 0;
		CHECK(s09_files_staging_open(&F, "n/two"));
		lb.late_cmd = S09_WRITE;
		CHECK(s09_files_staging_write(&F, big, 0x5000));
		CHECK(s09_files_staging_close(&F));
		CHECK(lb.late_cmd == 0u && !ms.absent);
		CHECK_EQ(ex.file[S09_WRITE], 1u);
		CHECK_EQ(sl.nqueue, 0u);
		CHECK(s09_files_read_whole(&F, "upgrade/n/two", back, sizeof back, &len));
		CHECK(len == 0x5000u && !memcmp(back, big, len));
		/* sent again on rung 1, which stays (S-09.075) */
		CHECK_EQ(s09_master_rung(&ms), S09_RUNG_CHECKED);
	}
}

/* Payload bytes for a quad write that a single-I/O listener hears, from the
 * header's last word on, as the request h (S-09.074's case): data line 0
 * carries each wanted bit, for either bit of the nibble it is. */
static void quad_payload_for(const uint8_t *h, size_t hn, uint8_t *pl, size_t words)
{
	for (size_t p = 0; p < words; p++) {
		const size_t j = (p + 3u) / 4u, q = (p + 3u) % 4u;
		const uint8_t byte = 4u * j + 3u - q < hn ? h[4u * j + 3u - q] : 0u;
		uint32_t w = 0;

		for (unsigned t = 0; t < 8u; t++)
			if (byte >> (7u - t) & 1u)
				w |= 0x9u << (28u - 4u * t);
		pl[4 * p] = (uint8_t)w;
		pl[4 * p + 1] = (uint8_t)(w >> 8);
		pl[4 * p + 2] = (uint8_t)(w >> 16);
		pl[4 * p + 3] = (uint8_t)(w >> 24);
	}
}

/* S-09.074: after a fall-back the slave executes nothing until the recovery
 * echo has come whole. A quad write it hears in single I/O as a sector
 * write, an erase or another echo does nothing; the master's way back then
 * writes the file once. */
static void test_link_heard_plain(void)
{
	static const uint32_t n = 0x2200u;	/* no bit on data line 0's places:
						 * slot 0 as heard */
	static uint8_t h[S09_HDR + S09_SECTOR], pl[0x2200];
	struct s09_stat st;

	t_case = "link heard plain";
	for (unsigned kind = 0; kind < 3u; kind++) {
		/* a sector write, an erase, and an echo that is not the
		 * recovery echo */
		static const struct s09_req as[3] = {
			{ S09_WRITE_SECTOR, 0, 0x200, 100u, 0 },
			{ S09_ERASE, 0, 0, 3u * S03_GROUP_SECTORS, 0 },
			{ S09_ECHO, 0, 0, 0x1234u, 0 },
		};
		const struct s09_req r = as[kind];

		s09_req_pack(&r, h);
		memset(h + S09_HDR, 0xEE, S09_SECTOR);
		quad_payload_for(h, sizeof h, pl, n / 4u);
		for (int map = 0; map < 2; map++) {
			struct s09_slave alone;

			/* the control: a slave in the plain mode that has not fallen
			 * back executes what it hears */
			fresh_on(&bus, S09_RUNG_TOP);
			{
				static uint8_t q[S09_HDR + 0x2200 + 8];
				uint8_t heard[0x900];
				struct s09_req w = { S09_WRITE, 1, (uint16_t)n, 0, n };
				const struct s09_req sel = { S09_SELECT, 0, 0, 1, 0 };
				uint8_t b[S09_HDR];
				size_t hn;

				s09_req_pack(&w, q);
				memcpy(q + S09_HDR, pl, n);
				memset(q + S09_HDR + n, 0, 8);
				hn = quad_heard(q, sizeof q, heard, map);
				CHECK(hn >= sizeof h && !memcmp(heard + 4, h + 4, sizeof h - 4));
				CHECK(heard[0] == h[0] && heard[1] < S09_SLOTS);
				s09_slave_init(&alone, &store);
				alone.linked = true;
				s09_req_pack(&sel, b);
				s09_slave_request(&alone, b, S09_HDR);
				memset(&ex, 0, sizeof ex);
				s09_slave_request(&alone, heard,
						  (size_t)s09_slave_rx_len(&alone, heard));
				if (r.cmd == S09_ECHO) {
					const uint8_t *e;
					size_t en;
					const struct s09_req ask = { S09_LINK_MODE, 0, 0,
								     S09_MODE_CHECKED, 0 };

					CHECK(s09_slave_reply(&alone, &e, &en) && en == 8u);
					/* the same, heard by a slave that has
					 * fallen back: dropped, nothing ended */
					s09_req_pack(&ask, b);
					s09_slave_request(&alone, b, S09_HDR);
					(void)s09_slave_reply(&alone, &e, &en);
					s09_slave_link_fault(&alone);
					CHECK_EQ(s09_slave_rx_len(&alone, heard), (uint64_t)S09_HDR);
					s09_slave_request(&alone, heard, S09_HDR);
					CHECK(!s09_slave_reply(&alone, &e, &en));
					CHECK(s09_slave_attention(&alone) && alone.fell_notice);
				} else {
					CHECK_EQ(ex.emmc[r.cmd], 1u);
				}
				emmc.part = 0;
			}

			CHECK(s09_files_staging_reset(&F));
			CHECK(s09_files_staging_open(&F, "h/one"));
			s09_slave_link_fault(&sl);
			lb.hear_quad = map;
			memset(&ex, 0, sizeof ex);
			CHECK(s09_files_staging_write(&F, pl, n));
			CHECK(s09_files_staging_close(&F));
			CHECK(!ms.absent && s09_master_fell(&ms) == 1u);
			CHECK(ex.emmc[S09_WRITE_SECTOR] == 0u && ex.emmc[S09_ERASE] == 0u);
			CHECK_EQ(ex.file[S09_WRITE], 1u);
			CHECK_EQ(s09_files_stat(&F, "upgrade/h/one", &st), 0);
			CHECK_EQ(st.size, n);
		}
	}
}

/* A request session as a master on a non-plain rung sends it: header,
 * payload, trailer with the number seq (S-09.072, S-09.076). */
static size_t numbered(uint8_t *d, const struct s09_req *r, const void *p,
		       uint32_t plen, uint32_t seq)
{
	const uint32_t body = S09_HDR + s09_pad4(plen);

	s09_req_pack(r, d);
	memset(d + S09_HDR, 0, s09_pad4(plen));
	if (plen)
		memcpy(d + S09_HDR, p, plen);
	(void)s09_trailer(d, body, seq, NULL, NULL);
	return body + S09_CHECK_LEN;
}

/* The slave's side of S-09.074 and S-09.076, driven directly. */
static void test_link_slave_rules(void)
{
	struct s09_req r = { S09_INIT, 0, 0, 0, 0 };
	static uint8_t d[256];
	const uint8_t *sr;
	size_t rn, n;

	t_case = "link slave rules";
	fresh();
	s09_slave_init(&sl, &store);
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(s09_slave_reply(&sl, &sr, &rn));
	r.cmd = S09_ECHO;
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(s09_slave_reply(&sl, &sr, &rn) && sl.linked);
	/* the checked mode, kept */
	r = (struct s09_req){ S09_LINK_MODE, 0, 0, S09_MODE_CHECKED, S09_LINK_TRY };
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(s09_slave_reply(&sl, &sr, &rn));
	r.arg1 = S09_LINK_KEEP;
	n = numbered(d, &r, NULL, 0, S09_SEQ_NONE);
	s09_slave_request(&sl, d, n);
	CHECK(s09_slave_reply(&sl, &sr, &rn));
	CHECK(s09_slave_mode(&sl) == S09_MODE_CHECKED && !s09_slave_on_trial(&sl));

	/* a numbered mkdir: executed once, a repeat not at all */
	r = (struct s09_req){ S09_MKDIR, 1, 10, 0, 0 };
	memset(&ex, 0, sizeof ex);
	n = numbered(d, &r, "/yaffs2/a\0", 10, 1000u);
	s09_slave_request(&sl, d, n);
	s09_slave_request(&sl, d, n);
	CHECK_EQ(ex.file[S09_MKDIR], 1u);
	CHECK(s09_slave_mode(&sl) == S09_MODE_CHECKED && sl.nqueue == 1u);
	/* a request with no number, other than 0x0A, 0x19, 0x1A: refused */
	n = numbered(d, &r, "/yaffs2/z\0", 10, S09_SEQ_NONE);
	s09_slave_request(&sl, d, n);
	CHECK_EQ(ex.file[S09_MKDIR], 1u);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_PLAIN);
	CHECK(s09_slave_attention(&sl));
	/* an echo that is not the recovery echo, as a garbled session may read:
	 * dropped, no reply, the fall-back's state kept */
	r = (struct s09_req){ S09_ECHO, 0, 0, 5, 0 };
	s09_req_pack(&r, d);
	CHECK_EQ(s09_slave_rx_len(&sl, d), (uint64_t)S09_HDR);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(!s09_slave_reply(&sl, &sr, &rn) && s09_slave_attention(&sl));
	r = (struct s09_req){ S09_ECHO, 0, 4, S09_ECHO_RECOVER, 0 };	/* a payload */
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR + 4u);
	CHECK(!s09_slave_reply(&sl, &sr, &rn) && s09_slave_attention(&sl));
	r = (struct s09_req){ S09_ECHO, 0, 0, S09_ECHO_RECOVER, 1 };	/* arg1 */
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(!s09_slave_reply(&sl, &sr, &rn) && s09_slave_attention(&sl));
	/* (back in the checked mode, as the master's recovery echo and trial
	 * bring it) */
	r = (struct s09_req){ S09_ECHO, 0, 0, S09_ECHO_RECOVER, 0 };
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(s09_slave_reply(&sl, &sr, &rn) && rd32(sr + 4) == ~S09_ECHO_RECOVER);
	r = (struct s09_req){ S09_LINK_MODE, 0, 0, S09_MODE_CHECKED, S09_LINK_TRY };
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(s09_slave_reply(&sl, &sr, &rn));
	r.arg1 = S09_LINK_KEEP;
	n = numbered(d, &r, NULL, 0, S09_SEQ_NONE);
	s09_slave_request(&sl, d, n);
	CHECK(s09_slave_reply(&sl, &sr, &rn) && s09_slave_mode(&sl) == S09_MODE_CHECKED);
	/* the same number with another check is a fault, not a repeat */
	r = (struct s09_req){ S09_MKDIR, 2, 10, 0, 0 };
	n = numbered(d, &r, "/yaffs2/b\0", 10, 1000u);
	s09_slave_request(&sl, d, n);
	CHECK_EQ(ex.file[S09_MKDIR], 1u);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_PLAIN);

	/* after the fall-back: nothing but a plain echo is executed */
	CHECK(s09_slave_attention(&sl));
	r = (struct s09_req){ S09_MKDIR, 2, 10, 0, 0 };
	s09_req_pack(&r, d);
	memcpy(d + S09_HDR, "/yaffs2/c\0\0", 12);
	CHECK_EQ(s09_slave_rx_len(&sl, d), (uint64_t)S09_HDR);
	s09_slave_request(&sl, d, S09_HDR + 12u);
	r = (struct s09_req){ S09_ERASE, 2, 0, 0, 0 };
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	r = (struct s09_req){ S09_POLL, 3, 0, 0, 0 };
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	r = (struct s09_req){ S09_LINK_MODE, 3, 0, S09_MODE_QUAD, S09_LINK_TRY };
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(!s09_slave_reply(&sl, &sr, &rn));
	CHECK(ex.file[S09_MKDIR] == 1u && ex.emmc[S09_ERASE] == 0u);
	CHECK(s09_slave_attention(&sl) && sl.nqueue == 1u);
	CHECK_EQ(s09_slave_mode(&sl), S09_MODE_PLAIN);
	r = (struct s09_req){ S09_ECHO, 3, 0, 9, 0 };
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(!s09_slave_reply(&sl, &sr, &rn) && s09_slave_attention(&sl));
	r.arg0 = S09_ECHO_RECOVER;
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(s09_slave_reply(&sl, &sr, &rn) && rn == 8u &&
	      rd32(sr + 4) == ~S09_ECHO_RECOVER);
	/* after the recovery echo, until the master asks for a mode, nothing
	 * else either: a 0x19 a bit error made another command is dropped */
	r = (struct s09_req){ S09_MKDIR, 2, 10, 0, 0 };
	s09_req_pack(&r, d);
	memcpy(d + S09_HDR, "/yaffs2/c\0\0", 12);
	s09_slave_request(&sl, d, S09_HDR + 12u);
	r = (struct s09_req){ S09_SELECT, 3, 0, 2, 0 };	/* 0x19 with bit 0 flipped */
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	r = (struct s09_req){ S09_LINK_MODE, 3, 0, S09_MODE_PLAIN, 1 };	/* not arg1 0 */
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(!s09_slave_reply(&sl, &sr, &rn));
	CHECK(ex.file[S09_MKDIR] == 1u && ex.emmc[S09_SELECT] == 0u);
	r = (struct s09_req){ S09_LINK_MODE, 3, 0, S09_MODE_PLAIN, 0 };
	s09_req_pack(&r, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(s09_slave_reply(&sl, &sr, &rn) && rd32(sr + 4) == S09_MODE_PLAIN);
	r = (struct s09_req){ S09_MKDIR, 2, 10, 0, 0 };
	s09_req_pack(&r, d);
	memcpy(d + S09_HDR, "/yaffs2/c\0\0", 12);
	s09_slave_request(&sl, d, S09_HDR + 12u);
	CHECK_EQ(ex.file[S09_MKDIR], 2u);

	/* a slave that has just started is not in that state */
	s09_slave_init(&sl, &store);
	CHECK(!s09_slave_attention(&sl));
}

/* S-09.078: the link lowered with the handshake; the release always plain. */
static void test_link_lowered(void)
{
	struct s09_stat st;

	t_case = "link lowered";
	for (unsigned b = 0; b < BUSES; b++) {
		fresh_on(buses[b], S09_RUNG_TOP);
		CHECK(s09_master_link_plain(&ms));
		CHECK(s09_master_rung(&ms) == S09_RUNG_PLAIN);
		CHECK(s09_slave_mode(&sl) == S09_MODE_PLAIN && !s09_slave_attention(&sl));
		CHECK_EQ(s09_master_fell(&ms), 0u);
		CHECK_EQ(lb.rungs[S09_LINK_MODE], S09_RUNG_TOP);
		CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
		CHECK_EQ(lb.rungs[S09_STAT], S09_RUNG_PLAIN);
		/* raised again: the rung did not fail */
		CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
		CHECK_EQ(s09_master_rung(&ms), S09_RUNG_TOP);
		CHECK_EQ(s09_master_release(&ms), 1);		/* st_os: no OS */
		CHECK_EQ(lb.rungs[S09_RELEASE], S09_RUNG_PLAIN);
		CHECK(s09_master_rung(&ms) == S09_RUNG_PLAIN && !ms.absent);
		/* 0x19's reply lost: the echoes bring both ends down all the same */
		CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
		arm(S09_LINK_MODE, true, true, 0);
		CHECK(s09_master_link_plain(&ms));
		CHECK(lb.f_fired && s09_master_rung(&ms) == S09_RUNG_PLAIN);
		CHECK(s09_slave_mode(&sl) == S09_MODE_PLAIN && !ms.absent);
		CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
	}
}

/* An engine for the check that gives a wrong one while `engine_bad`. */
static bool engine_bad;
static unsigned engine_calls;

static uint32_t engine(void *ctx, const uint8_t *d, size_t n)
{
	(void)ctx;
	engine_calls++;
	return s09_check_word(d, (uint32_t)n) ^ (engine_bad ? 0x10u : 0u);
}

/* S-09.072: the slave refuses a check that came from the engine, and the
 * table gives another: the engine is not used again, and the rung is not
 * blamed. */
static void test_link_engine(void)
{
	struct s09_stat st;

	t_case = "link engine";
	/* wrong from the first test on */
	fresh_on(&bus, 0);
	s09_master_check_engine(&ms, engine, NULL);
	engine_bad = true;
	engine_calls = 0;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(s09_master_rung(&ms) == S09_RUNG_TOP && ms.soft_check);
	CHECK(engine_calls == 1u && s09_master_fell(&ms) == 1u);
	CHECK(s09_files_staging_reset(&F) && staged_file("e/one", 0x2000u, 3));
	CHECK_EQ(engine_calls, 1u);

	/* right through the tests, wrong at a write: the write once, on the
	 * same rung */
	fresh_on(&bus, 0);
	s09_master_check_engine(&ms, engine, NULL);
	engine_bad = false;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(s09_master_rung(&ms) == S09_RUNG_TOP && !ms.soft_check);
	CHECK(s09_files_staging_reset(&F));
	CHECK(s09_files_staging_open(&F, "e/two"));
	engine_bad = true;
	ex.file[S09_WRITE] = 0;
	CHECK(s09_files_staging_write(&F, big, 0x3000));
	CHECK(s09_files_staging_close(&F));
	CHECK(ms.soft_check && s09_master_rung(&ms) == S09_RUNG_TOP);
	CHECK(s09_master_fell(&ms) == 1u && ex.file[S09_WRITE] == 1u);
	CHECK_EQ(s09_files_stat(&F, "upgrade/e/two", &st), 0);
	CHECK_EQ(st.size, 0x3000u);
	engine_bad = false;
}

/* Memory that keeps nothing: the tables read back as zeros after each
 * build, on both ends or on the slave's alone. */
static int known_bad;		/* 1 both ends, 2 the slave's */

static void tables_lost(void)
{
	if (known_bad == 1 || (known_bad == 2 && lb.in_slave))
		memset(s09_check_t, 0, sizeof s09_check_t);
}

/* S-09.072: an end whose check fails its known answers stays plain. */
static void test_link_known_answer(void)
{
	uint32_t want;

	t_case = "link known answer";
	s09_check_built = tables_lost;
	/* the master's: no 0x19 goes out, and the run is the plain one */
	fresh_on(&bus, 0);
	CHECK(s09_files_staging_reset(&F));
	lb.log = 0;
	CHECK(staged_file("k/same", 0x2345u, 91));
	want = lb.log;
	fresh_on(&bus, 0);
	known_bad = 1;
	lb.sessions = 0;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(s09_master_check_bad(&ms) && lb.sessions == 0u);
	CHECK(s09_master_rung(&ms) == S09_RUNG_PLAIN && s09_master_fell(&ms) == 0u);
	CHECK(s09_files_staging_reset(&F));
	lb.log = 0;
	CHECK(staged_file("k/same", 0x2345u, 91));
	CHECK_EQ(lb.log, want);
	CHECK(lb.rungs[S09_LINK_MODE] == 0u);
	/* built afresh at the next raise: memory that works again serves */
	known_bad = 0;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(!s09_master_check_bad(&ms) && s09_master_rung(&ms) == S09_RUNG_TOP);

	/* the slave's: it answers plain, the master ends there, and nothing
	 * goes on a faster mode */
	fresh_on(&bus, 0);
	known_bad = 2;
	lb.on_rung = 0;
	memset(&ex, 0, sizeof ex);
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(s09_master_rung(&ms) == S09_RUNG_PLAIN && !ms.absent);
	CHECK(s09_slave_mode(&sl) == S09_MODE_PLAIN && !s09_master_check_bad(&ms));
	CHECK_EQ(lb.on_rung, 0u);
	CHECK(s09_files_staging_reset(&F) && staged_file("k/slave", 0x1800u, 92));
	CHECK_EQ(lb.on_rung, 0u);
	known_bad = 0;
	s09_check_built = NULL;

	/* tables spoiled between two raises: built again, the link works */
	fresh_on(&bus, S09_RUNG_TOP);
	CHECK(s09_master_link_plain(&ms));
	memset(s09_check_t, 0xA5, sizeof s09_check_t);
	lb.checked = 0;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_TOP);
	CHECK(s09_files_staging_reset(&F) && staged_file("k/again", 0x5000u, 93));
	CHECK(lb.checked > 13u && s09_master_fell(&ms) == 0u);
}

/* The tunnel's bytes the slave has, as its CPU link would read them. */
static size_t slave_rx(uint8_t *d, size_t max)
{
	return s09_slave_tunnel_port.read(&sl, d, max);
}

/* S-09.074, S-09.076: a push on a rung that the slave refused, it having
 * fallen back alone, goes again with its number; one it had taken is not
 * taken twice. */
static void test_link_push(void)
{
	static const uint8_t msg[] = "push me once";
	uint8_t got[64];

	t_case = "link push";
	for (unsigned b = 0; b < BUSES; b++) {
		/* refused: the slave fell back before it came */
		fresh_on(buses[b], S09_RUNG_TOP);
		s09_slave_link_fault(&sl);
		s09_tunnel_port.write(&ms, msg, sizeof msg);
		CHECK(!ms.absent && s09_master_fell(&ms) == 1u && ms.tx_n == 0u);
		CHECK(slave_rx(got, sizeof got) == sizeof msg && !memcmp(got, msg, sizeof msg));
		/* taken, then the slave fell back: sent again, taken once */
		fresh_on(buses[b], S09_RUNG_TOP);
		arm(S09_PUSH0, false, false, 0);
		lb.fs[0].nth = 1000;		/* counts pushes, fires none */
		lb.fault_after = S09_PUSH0;
		s09_tunnel_port.write(&ms, msg, sizeof msg);
		CHECK(!ms.absent && s09_master_fell(&ms) == 1u);
		CHECK_EQ(lb.fs[0].seen, 2u);	/* sent twice ... */
		CHECK(slave_rx(got, sizeof got) == sizeof msg && !memcmp(got, msg, sizeof msg));
		CHECK_EQ(slave_rx(got, sizeof got), 0u);	/* ... taken once */
	}
}

/* S-09.015, S-09.074: a master that starts again finds the slave, from
 * each state an earlier master can leave it in. */
static void restart(const struct s09_bus_ops *b, const char *from)
{
	struct s09_stat st;

	t_case = from;
	/* a master starts long after the last one's sessions: a reply the
	 * slave offered, a request it was receiving, are given up by then */
	sim.now += S06_MS(1000);
	s09_master_init(&ms, b, &sim.clock);
	CHECK(s09_master_start(&ms));
	CHECK(!ms.absent && s09_slave_mode(&sl) == S09_MODE_PLAIN);
	CHECK(!s09_slave_attention(&sl) && sl.nqueue == 0u);
	s09_files_init(&F, &ms);
	CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP) && s09_master_rung(&ms) == S09_RUNG_TOP);
	CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
}

static void test_link_restart(void)
{
	const struct s09_req mk = { S09_MKDIR, 1, 2, 0, 0 };
	uint8_t d[16];

	t_case = "link restart";
	for (unsigned b = 0; b < BUSES; b++) {
		/* plain, with a result queued and the pull parked */
		fresh_on(buses[b], 0);
		s09_req_pack(&mk, d);
		memcpy(d + S09_HDR, "q\0\0\0", 4);
		s09_slave_request(&sl, d, S09_HDR + 4u);
		CHECK(s09_slave_attention(&sl));
		restart(buses[b], "link restart: a result queued");
		/* on a rung */
		fresh_on(buses[b], S09_RUNG_TOP);
		restart(buses[b], "link restart: rung 4");
		fresh_on(buses[b], S09_RUNG_CHECKED_FAST);
		restart(buses[b], "link restart: rung 2");
		/* fallen back */
		fresh_on(buses[b], S09_RUNG_TOP);
		s09_slave_link_fault(&sl);
		restart(buses[b], "link restart: fallen back");
		/* mid-recovery: the recovery echo taken, a trial asked for */
		fresh_on(buses[b], S09_RUNG_TOP);
		s09_slave_link_fault(&sl);
		{
			const struct s09_req e = { S09_ECHO, 0, 0, S09_ECHO_RECOVER, 0 };
			const struct s09_req t = { S09_LINK_MODE, 0, 0, S09_MODE_QUAD, 0 };
			const uint8_t *rd;
			size_t rn;

			s09_req_pack(&e, d);
			s09_slave_request(&sl, d, S09_HDR);
			(void)s09_slave_reply(&sl, &rd, &rn);
			s09_req_pack(&t, d);
			s09_slave_request(&sl, d, S09_HDR);
			CHECK(s09_slave_on_trial(&sl));
			restart(buses[b], "link restart: mid-recovery");
		}
		/* a reply waiting to go out */
		fresh_on(buses[b], 0);
		{
			const struct s09_req e = { S09_ECHO, 0, 0, 5, 0 };

			s09_req_pack(&e, d);
			s09_slave_request(&sl, d, S09_HDR);
			restart(buses[b], "link restart: a reply waiting");
		}
		t_case = "link restart";
		/* garbage that is not the master's init exactly leaves the
		 * fall-back's state as it was */
		fresh_on(buses[b], S09_RUNG_TOP);
		s09_slave_link_fault(&sl);
		{
			const struct s09_req i1 = { S09_INIT, 1, 0, 0, 0 };
			const struct s09_req i2 = { S09_INIT, 0, 0, 1, 0 };
			const uint8_t *rd;
			size_t rn;

			s09_req_pack(&i1, d);
			s09_slave_request(&sl, d, S09_HDR);
			s09_req_pack(&i2, d);
			s09_slave_request(&sl, d, S09_HDR);
			CHECK(!s09_slave_reply(&sl, &rd, &rn) && sl.fell_notice);
		}
	}
}

/* S-09.060: a request's wait is its own: PC5 stuck low and polls that
 * find nothing end it, with ENODEV and the slave absent. */
static void test_link_bounded(void)
{
	uint32_t err;

	t_case = "link bounded";
	for (unsigned rung = 0; rung <= S09_RUNG_TOP; rung += S09_RUNG_TOP) {
		int32_t r;

		fresh_on(&bus, rung);
		lb.att_stuck = true;
		lb.drop_cmd = S09_MKDIR;
		lb.session_us = 100;
		r = s09_files_mkdir(&F, "never");
		CHECK(r == -1 && F.err == S09_ENODEV && ms.absent);
		/* the slot to take: every one but the last held */
		fresh_on(&bus, rung);
		lb.att_stuck = true;
		lb.session_us = 100;
		ms.slot[1].state = S09_SLOT_WAIT;
		ms.slot[2].state = S09_SLOT_WAIT;
		{
			struct s09_req q = { S09_MKDIR, 0, 2, 0, 0 };

			r = s09_master_call(&ms, &q, (const uint8_t *)"x", 2, 0, 0, 0, &err);
			CHECK(r == -1 && err == S09_ENODEV && ms.absent);
		}
	}
}

/* S-09.076: a repeat of the last numbered request but a poll is known
 * after any number of polls: its finished reply, once handed out, is
 * queued again; and a reply that reads as "nothing waiting", whole and
 * checked, costs no second execution. */
static void test_link_two_memories(void)
{
	struct s09_stat st;
	uint32_t len = 0;

	t_case = "link two memories";
	for (unsigned b = 0; b < BUSES; b++) {
		fresh_on(buses[b], S09_RUNG_TOP);
		ex.file[S09_MKDIR] = 0;
		arm(S09_MKDIR, true, false, 0);
		lb.fs[0].none = true;	/* its result handed out, seen as nothing */
		CHECK_EQ(s09_files_mkdir(&F, "twice"), 0);
		CHECK(lb.f_fired && ex.file[S09_MKDIR] == 1u && !ms.absent);
		CHECK_EQ(s09_master_fell(&ms), 0u);
		CHECK_EQ(s09_files_stat(&F, "twice", &st), 0);
		/* a chunked read's first piece so */
		pattern(big, 0x2800, 77);
		CHECK(put("/yaffs2/two", big, 0x2800));
		ex.pieces = 0;
		arm(S09_READ, true, false, 0);
		lb.fs[0].none = true;
		memset(back, 0, sizeof back);
		CHECK(s09_files_read_whole(&F, "/yaffs2/two", back, sizeof back, &len));
		CHECK(lb.f_fired && len == 0x2800u && !memcmp(back, big, len));
		CHECK_EQ(ex.pieces, 3u);
		/* a file write so */
		CHECK(s09_files_staging_reset(&F));
		ex.file[S09_WRITE] = 0;
		arm(S09_WRITE, true, false, 0);
		lb.fs[0].none = true;
		CHECK(staged_file("m/once", 0x3000u, 78));
		CHECK(lb.f_fired && ex.file[S09_WRITE] == 1u);
	}
}

/* S-09.075's gate: a rung is kept only when one largest request on it beats
 * the plain wire's time for the same words by a sixteenth; one that does
 * not is left with 0x19 mode 0, no fall-back, and the next one down is
 * tried. In the middle of a run a fault drops one rung at a time, to the
 * next that passes its tests and its gate, and each request is done once. */
static void test_link_gate(void)
{
	const struct s09_record *rec;
	const uint32_t plain_us = ((S09_HDR + S09_TEST_MAX) / 4u * S09_PLAIN_WORD_NS + 999u) /
				  1000u;

	t_case = "link gate";
	/* rung 4 a fortieth faster than the plain wire: too little */
	fresh_plain();
	lb.word_ns[S09_RUNG_QUAD_FAST] = 1250;
	lb.word_ns[S09_RUNG_QUAD] = 1100;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_QUAD);
	CHECK(s09_slave_mode(&sl) == S09_MODE_QUAD && !s09_slave_on_trial(&sl));
	CHECK(s09_master_fell(&ms) == 0u && !sl.fell_notice);
	rec = s09_master_record(&ms);
	CHECK_EQ(rec->nrungs, 2u);
	CHECK(rec->rung[0].rung == S09_RUNG_QUAD_FAST && rec->rung[0].result == S09_RR_GATE);
	CHECK_EQ(rec->rung[0].plain_us, plain_us);
	/* each of the three sessions alone: 16,389 words at 1,250 ns */
	for (unsigned k = 0; k < S09_GATE_TIMES; k++)
		CHECK(rec->rung[0].big_us[k] >= 20480u && rec->rung[0].big_us[k] <= 20490u);
	CHECK(rec->rung[1].rung == S09_RUNG_QUAD && rec->rung[1].result == S09_RR_KEPT);
	CHECK(rec->rung[1].big_us[2] * 16u <= plain_us * 15u);
	CHECK(rec->kept == S09_RUNG_QUAD && rec->falls == 0u && rec->negotiations == 1u);
	lb.sessions = 0;		/* not tried again */
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP) && lb.sessions == 0u);
	/* lowered and raised again: the verdict kept, not timed again */
	CHECK(s09_master_link_plain(&ms) && s09_master_settled(&ms));
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	rec = s09_master_record(&ms);
	CHECK(s09_master_rung(&ms) == S09_RUNG_QUAD && rec->nrungs == 1u &&
	      rec->rung[0].plain_us == 0u && rec->rung[0].result == S09_RR_KEPT);

	/* none beats the plain wire: rung 1, checked, from start to end */
	for (unsigned b = 0; b < BUSES; b++) {
		fresh_on(buses[b], 0);
		for (unsigned r = S09_RUNG_CHECKED; r <= S09_RUNG_TOP; r++)
			lb.word_ns[r] = 1300;
		CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
		CHECK(s09_master_rung(&ms) == S09_RUNG_CHECKED && s09_slave_mode(&sl) == S09_MODE_CHECKED);
		CHECK(s09_master_fell(&ms) == 0u && !sl.fell_notice && !sl.recovering);
		CHECK(lb.split == 0u);
		rec = s09_master_record(&ms);
		CHECK(rec->nrungs == 4u && rec->kept == S09_RUNG_CHECKED);
		for (unsigned i = 0; i < 3u; i++)
			CHECK(rec->rung[i].rung == S09_RUNG_TOP - i && rec->rung[i].result == S09_RR_GATE);
		CHECK(rec->rung[3].rung == S09_RUNG_CHECKED && rec->rung[3].result == S09_RR_KEPT &&
		      rec->rung[3].plain_us == 0u);
		CHECK(s09_files_staging_reset(&F));
		lb.cmds[S09_WRITE] = 0;
		CHECK(staged_file("g/checked", 0x5000u, 89));
		CHECK(lb.rungs[S09_WRITE] == S09_RUNG_CHECKED && lb.cmds[S09_WRITE] != 0u);
		/* a fault on rung 1 mid-run: sent again on rung 1, once */
		ex.file[S09_WRITE] = 0;
		arm(S09_WRITE, false, false, 3);
		CHECK(staged_file("g/again", 0x3000u, 88));
		CHECK(lb.f_fired && ex.file[S09_WRITE] == 1u && !ms.absent);
		CHECK(s09_master_rung(&ms) == S09_RUNG_CHECKED && s09_master_record(&ms)->falls == 1u);
		CHECK(s09_master_record(&ms)->fall[0].from == S09_RUNG_CHECKED &&
		      s09_master_record(&ms)->fall[0].to == S09_RUNG_CHECKED);
		CHECK(s09_master_link_plain(&ms) && s09_master_settled(&ms));
	}

	/* rung 1's own tests failing too: plain, with rung 1 in the record */
	fresh_plain();
	lb.bad_rungs = 0x1Eu;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(s09_master_rung(&ms) == S09_RUNG_PLAIN && s09_master_settled(&ms) && !sl.recovering);
	rec = s09_master_record(&ms);
	CHECK(rec->nrungs == 4u && rec->rung[3].rung == S09_RUNG_CHECKED &&
	      rec->rung[3].result == S09_RR_TEST && rec->kept == S09_RUNG_PLAIN);
	lb.bad_rungs = 0;
	lb.sessions = 0;		/* plain for the boot */
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP) && lb.sessions == 0u);

	for (unsigned b = 0; b < BUSES; b++) {
		/* in a run: rung 4 faults, rung 3 fails its gate, rung 2 */
		fresh_on(buses[b], S09_RUNG_TOP);
		lb.word_ns[S09_RUNG_QUAD] = 1300;
		CHECK(s09_files_staging_reset(&F));
		ex.file[S09_WRITE] = 0;
		arm(S09_WRITE, false, false, 3);
		CHECK(staged_file("g/one", 0x6000u, 90));
		CHECK(lb.f_fired && !ms.absent);
		CHECK_EQ(ex.file[S09_WRITE], 1u);
		CHECK_EQ(s09_master_rung(&ms), S09_RUNG_CHECKED_FAST);
		rec = s09_master_record(&ms);
		CHECK(rec->falls == 1u && rec->fall[0].from == S09_RUNG_TOP &&
		      rec->fall[0].to == S09_RUNG_CHECKED_FAST);
		/* the slave said why: the write's check */
		CHECK(rec->fall[0].st.told && rec->fall[0].st.why == S09_WHY_CHECK &&
		      rec->fall[0].st.cmd == S09_WRITE && rec->fall[0].st.fell == 1u &&
		      rec->fall[0].st.mode == S09_MODE_QUAD);
		CHECK(rec->nrungs == 2u && rec->rung[0].rung == S09_RUNG_QUAD &&
		      rec->rung[0].result == S09_RR_GATE &&
		      rec->rung[1].rung == S09_RUNG_CHECKED_FAST &&
		      rec->rung[1].result == S09_RR_KEPT);
		/* the run goes on there */
		CHECK(staged_file("g/two", 0x7000u, 91));
		CHECK(s09_master_rung(&ms) == S09_RUNG_CHECKED_FAST && ex.file[S09_WRITE] == 2u);

		/* from rung 2: rung 1, and the run stays checked there */
		fresh_on(buses[b], S09_RUNG_CHECKED_FAST);
		CHECK(s09_files_staging_reset(&F));
		ex.file[S09_WRITE] = 0;
		arm(S09_WRITE, false, false, 3);
		CHECK(staged_file("g/three", 0x3000u, 92));
		CHECK(ex.file[S09_WRITE] == 1u && s09_master_rung(&ms) == S09_RUNG_CHECKED);
		CHECK(s09_slave_mode(&sl) == S09_MODE_CHECKED && !sl.recovering);
		rec = s09_master_record(&ms);
		CHECK(rec->falls == 1u && rec->fall[0].to == S09_RUNG_CHECKED);
		CHECK(rec->nrungs == 1u && rec->rung[0].rung == S09_RUNG_CHECKED &&
		      rec->rung[0].result == S09_RR_KEPT);
		lb.sessions = 0;	/* nothing above it to try */
		CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP) && lb.sessions == 0u);
		CHECK(staged_file("g/five", 0x800u, 96));
		CHECK_EQ(lb.rungs[S09_WRITE], S09_RUNG_CHECKED);	/* the next write */
		CHECK(s09_master_link_plain(&ms) && s09_master_settled(&ms));
	}
}

static uint32_t words_asked;

static void test_words(void *ctx, uint32_t out[4])
{
	(void)ctx;
	words_asked++;
	for (unsigned i = 0; i < 4u; i++)
		out[i] = 0x100u * (i + 1u) + words_asked;
}

/* S-09.079: 0x1B, the link's status, plain and unnumbered: taken after the
 * recovery echo, exactly, and outside the fall-back's states; refused in its
 * first state. The slave's causes, each recorded. */
static void test_link_status(void)
{
	static const struct s09_req st0 = { S09_LINK_STATUS, 1, 0, 0, 0 };
	const struct s09_req echo = { S09_ECHO, 0, 0, S09_ECHO_RECOVER, 0 };
	const uint8_t *sr;
	size_t rn;
	uint8_t d[S09_HDR + S09_CHECK_LEN];

	t_case = "link status";
	fresh_on(&bus, S09_RUNG_TOP);
	words_asked = 0;
	s09_slave_fault_source(&sl, test_words, NULL);
	s09_slave_link_fault(&sl);		/* a receive that failed */
	CHECK_EQ(words_asked, 1u);
	s09_req_pack(&st0, d);
	s09_slave_request(&sl, d, S09_HDR);	/* the first state: dropped */
	CHECK(!s09_slave_reply(&sl, &sr, &rn) && sl.fell_notice);
	s09_req_pack(&echo, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(s09_slave_reply(&sl, &sr, &rn) && sl.recovering);
	s09_req_pack(&st0, d);
	s09_slave_request(&sl, d, S09_HDR);	/* the second: answered */
	CHECK(s09_slave_reply(&sl, &sr, &rn) && rn == S09_STATUS_LEN);
	if (rn == S09_STATUS_LEN) {
		CHECK_EQ(rd32(sr), s09_hdr_word(S09_LINK_STATUS, 1));
		CHECK_EQ(rd32(sr + 4), 1u);
		CHECK_EQ(rd32(sr + 8), S09_WHY_RECEIVE | (uint32_t)S09_MODE_QUAD << 16);
		CHECK(rd32(sr + 20) == 0x101u && rd32(sr + 32) == 0x401u);
	}
	CHECK(sl.recovering);			/* still: only a 0x19 ends it */
	{
		const struct s09_req odd = { S09_LINK_STATUS, 1, 0, 1, 0 };

		s09_req_pack(&odd, d);
		s09_slave_request(&sl, d, S09_HDR);
		CHECK(!s09_slave_reply(&sl, &sr, &rn) && sl.recovering);
	}
	/* the causes */
	{
		const struct s09_req ask = { S09_LINK_MODE, 0, 0, S09_MODE_QUAD, 0 };
		const struct s09_req mk = { S09_MKDIR, 1, 2, 0, 0 };
		uint8_t q[S09_HDR + 4u + S09_CHECK_LEN];

		s09_req_pack(&ask, d);
		s09_slave_request(&sl, d, S09_HDR);	/* on trial, quad */
		(void)s09_slave_reply(&sl, &sr, &rn);
		s09_req_pack(&mk, q);
		memcpy(q + S09_HDR, "m\0\0\0", 4);
		(void)s09_trailer(q, S09_HDR + 4u, 9, NULL, NULL);
		s09_slave_request(&sl, q, sizeof q);	/* not a test: on trial */
		CHECK(sl.why == S09_WHY_ON_TRIAL && sl.why_cmd == S09_MKDIR && sl.fell == 2u);
		CHECK(sl.why_len == 2u && sl.why_got == sizeof q);
		s09_req_pack(&echo, d);
		s09_slave_request(&sl, d, S09_HDR);
		(void)s09_slave_reply(&sl, &sr, &rn);
		s09_req_pack(&ask, d);
		s09_slave_request(&sl, d, S09_HDR);
		(void)s09_slave_reply(&sl, &sr, &rn);
		s09_slave_trial_lapsed(&sl);
		CHECK(sl.why == S09_WHY_TRIAL && sl.fell == 3u);
		s09_slave_trial_lapsed(&sl);		/* not on trial: nothing */
		CHECK(sl.fell == 3u);
	}
	/* the master asks after every recovery echo, and keeps the answer */
	fresh_on(&bus, S09_RUNG_TOP);
	s09_slave_fault_source(&sl, test_words, NULL);
	lb.cmds[S09_LINK_STATUS] = 0;
	s09_slave_link_fault(&sl);
	CHECK(s09_files_staging_reset(&F) && staged_file("s/one", 0x1000u, 94));
	CHECK_EQ(lb.cmds[S09_LINK_STATUS], 1u);
	CHECK(s09_master_record(&ms)->fall[0].st.told &&
	      s09_master_record(&ms)->fall[0].st.why == S09_WHY_RECEIVE);
	/* a status that does not come is no error */
	fresh_on(&bus, S09_RUNG_TOP);
	s09_slave_link_fault(&sl);
	lb.drop_cmd = S09_LINK_STATUS;
	CHECK(s09_files_staging_reset(&F) && staged_file("s/two", 0x1000u, 95));
	CHECK(!ms.absent && !s09_master_record(&ms)->fall[0].st.told);
}

/*
 * S-09.079 cause 9, S-13b.083: the slave records a reply whose end it did
 * not see as its last cause, its mode and n unchanged; a fall-back before
 * a 0x1B has reported it is counted but leaves it the cause; one after
 * replaces it; the init keeps n and the cause. End to end, on a rung and
 * in the plain mode: the reply's words all come, PA6 never rises, the
 * master falls back (or recovers by S-09.013 rule 4), asks the status,
 * reads 9 and tells it once.
 */
static void test_link_reply_end(void)
{
	static const struct s09_req st0 = { S09_LINK_STATUS, 1, 0, 0, 0 };
	const struct s09_req echo = { S09_ECHO, 0, 0, S09_ECHO_RECOVER, 0 };
	const struct s09_req size = { S09_BOOT_SIZE, 0, 0, 0, 0 };
	const struct s09_req init = { S09_INIT, 0, 0, 0, 0 };
	const uint8_t *sr;
	size_t rn;
	uint8_t d[S09_HDR + S09_CHECK_LEN];
	uint32_t err = 0;

	t_case = "link reply end";
	/* the slave alone, on a rung */
	fresh_on(&bus, S09_RUNG_TOP);
	CHECK(s09_slave_mode(&sl) != S09_MODE_PLAIN && sl.fell == 0u);
	s09_slave_reply_end_changed(&sl, S09_READ, 4108u);
	CHECK(sl.why == S09_WHY_REPLY_END && sl.why_cmd == S09_READ &&
	      sl.why_mode == s09_slave_mode(&sl) && sl.why_len == 0u && sl.why_got == 4108u);
	CHECK(sl.fell == 0u && s09_slave_mode(&sl) != S09_MODE_PLAIN && !sl.fell_notice);
	s09_slave_link_fault(&sl);		/* the fall-back such a reply brings */
	CHECK(sl.fell == 1u && s09_slave_mode(&sl) == S09_MODE_PLAIN);
	CHECK(sl.why == S09_WHY_REPLY_END && sl.why_cmd == S09_READ);
	s09_req_pack(&echo, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(s09_slave_reply(&sl, &sr, &rn));
	s09_req_pack(&st0, d);
	s09_slave_request(&sl, d, S09_HDR);
	CHECK(s09_slave_reply(&sl, &sr, &rn) && rn == S09_STATUS_LEN);
	if (rn == S09_STATUS_LEN) {
		CHECK_EQ(rd32(sr + 4), 1u);
		CHECK_EQ(rd32(sr + 8), S09_WHY_REPLY_END | (uint32_t)S09_READ << 8 |
				       (uint32_t)S09_MODE_QUAD << 16);
		CHECK_EQ(rd32(sr + 16), 4108u);
	}
	/* the init keeps n and the cause */
	s09_req_pack(&init, d);
	s09_slave_request(&sl, d, S09_HDR);
	(void)s09_slave_reply(&sl, &sr, &rn);
	CHECK(sl.fell == 1u && sl.why == S09_WHY_REPLY_END);
	/* reported: a later fall-back is the cause again */
	{
		const struct s09_req ask = { S09_LINK_MODE, 0, 0, S09_MODE_QUAD, 0 };

		s09_req_pack(&ask, d);
		s09_slave_request(&sl, d, S09_HDR);
		(void)s09_slave_reply(&sl, &sr, &rn);
		s09_slave_trial_lapsed(&sl);
		CHECK(sl.why == S09_WHY_TRIAL && sl.fell == 2u);
	}
	/* in the plain mode: nothing changes but the cause */
	fresh();
	s09_slave_reply_end_changed(&sl, S09_ECHO, 8u);
	CHECK(sl.why == S09_WHY_REPLY_END && sl.why_mode == S09_MODE_PLAIN &&
	      sl.fell == 0u && s09_slave_mode(&sl) == S09_MODE_PLAIN);

	/* end to end on a rung: the request sent again, the status read */
	fresh_on(&bus, S09_RUNG_TOP);
	lb.cmds[S09_LINK_STATUS] = 0;
	lb.end_cmd = S09_BOOT_SIZE;
	CHECK_EQ(s09_master_call(&ms, &size, 0, 0, 0, 0, 0, &err), (int32_t)B_SIZE);
	CHECK(!ms.absent && lb.end_cmd == 0u && err == 0u);
	CHECK_EQ(lb.cmds[S09_LINK_STATUS], 1u);
	CHECK(sl.fell == 1u);
	CHECK(s09_master_record(&ms)->fall[0].st.told &&
	      s09_master_record(&ms)->fall[0].st.why == S09_WHY_REPLY_END &&
	      s09_master_record(&ms)->fall[0].st.cmd == S09_BOOT_SIZE);
	CHECK(s09_master_end_changed(&ms));
	CHECK(!s09_master_end_changed(&ms));	/* told once */
	/* a later fall-back: another cause, nothing more told */
	s09_slave_link_fault(&sl);
	CHECK_EQ(s09_master_call(&ms, &size, 0, 0, 0, 0, 0, &err), (int32_t)B_SIZE);
	CHECK(!s09_master_end_changed(&ms));

	/* in the plain mode: S-09.013 rule 4, then the status */
	fresh();
	lb.cmds[S09_LINK_STATUS] = 0;
	lb.end_cmd = S09_BOOT_SIZE;
	CHECK(s09_master_call(&ms, &size, 0, 0, 0, 0, 0, &err) == -1 && err == S09_EIO);
	CHECK(!ms.absent && lb.end_cmd == 0u && sl.fell == 0u);
	CHECK_EQ(lb.cmds[S09_LINK_STATUS], 1u);
	CHECK(s09_master_end_changed(&ms));
	CHECK(!s09_master_end_changed(&ms));	/* told once */
	CHECK_EQ(s09_master_call(&ms, &size, 0, 0, 0, 0, 0, &err), (int32_t)B_SIZE);

	/* no change, nothing told */
	fresh_on(&bus, S09_RUNG_TOP);
	s09_slave_link_fault(&sl);
	CHECK_EQ(s09_master_call(&ms, &size, 0, 0, 0, 0, 0, &err), (int32_t)B_SIZE);
	CHECK(!s09_master_end_changed(&ms));
}

/* The record of fall-backs: the last S09_REC_FALLS, the oldest dropped and
 * counted, each with where it broke. */
static void test_link_record(void)
{
	const struct s09_record *rec;
	struct s09_stat st;

	t_case = "link record";
	fresh_on(&bus, S09_RUNG_TOP);
	for (unsigned k = 0; k < S09_REC_FALLS + 3u; k++) {
		ms.ceiling = S09_RUNG_TOP;	/* raised again each time */
		CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
		s09_slave_link_fault(&sl);
		CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
	}
	rec = s09_master_record(&ms);
	CHECK_EQ(rec->falls, S09_REC_FALLS + 3u);
	/* the newest at (falls - 1) % S09_REC_FALLS */
	CHECK(rec->fall[(rec->falls - 1u) % S09_REC_FALLS].cmd == S09_STAT);
	CHECK(rec->fall[(rec->falls - 1u) % S09_REC_FALLS].from == S09_RUNG_TOP);
	/* where it broke: the reply's check (the poll that carries it) */
	fresh_on(&bus, S09_RUNG_TOP);
	arm(S09_STAT, true, false, 2);
	CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
	rec = s09_master_record(&ms);
	CHECK(rec->falls == 1u && rec->fall[0].fault.at == S09_AT_CHECK &&
	      rec->fall[0].cmd == S09_POLL && rec->fall[0].tries == 0u);
	fresh_on(&bus, S09_RUNG_TOP);
	arm(S09_STAT, true, true, 1);
	CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
	CHECK_EQ(s09_master_record(&ms)->fall[0].fault.at, S09_AT_REPLY_BROKE);
	fresh_on(&bus, S09_RUNG_TOP);
	arm(S09_STAT, true, false, 0);
	CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
	CHECK_EQ(s09_master_record(&ms)->fall[0].fault.at, S09_AT_FIRST_WORD);
	fresh_on(&bus, S09_RUNG_TOP);
	words_asked = 0;
	s09_master_fault_source(&ms, test_words, NULL);
	arm(S09_STAT, false, true, 2);
	CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
	rec = s09_master_record(&ms);
	CHECK(rec->fall[0].fault.at == S09_AT_SEND && rec->fall[0].fault.words[0] == 0x101u);
	/* the largest test overruns on rung 4: the rung refused, recorded
	 * with both ends' words, the next rung kept */
	fresh_plain();
	words_asked = 0;
	s09_master_fault_source(&ms, test_words, NULL);
	s09_slave_fault_source(&sl, test_words, NULL);
	arm(S09_LINK_TEST, false, true, 2000);
	lb.fs[0].nth = 14;		/* rung 4's test of 0x10000 bytes */
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_QUAD);
	rec = s09_master_record(&ms);
	CHECK(rec->nrungs == 2u && rec->rung[0].rung == S09_RUNG_TOP &&
	      rec->rung[0].result == S09_RR_TEST && rec->rung[0].len == S09_TEST_MAX &&
	      rec->rung[0].fault.at == S09_AT_SEND && rec->rung[0].fault.words[0] != 0u);
	CHECK(rec->rung[0].st.told && rec->rung[0].st.why == S09_WHY_RECEIVE &&
	      rec->rung[0].st.cmd == S09_LINK_TEST && rec->rung[0].st.got == 8000u &&
	      rec->rung[0].st.words[3] != 0u);
	CHECK(rec->rung[1].result == S09_RR_KEPT && rec->falls == 0u);
	/* a plain request's failure: recorded, the slave absent */
	fresh_on(&bus, 0);
	arm(S09_STAT, true, false, 0);
	CHECK(s09_files_stat(&F, "/yaffs2", &st) != 0 && ms.absent);
	rec = s09_master_record(&ms);
	CHECK(rec->falls == 1u && rec->fall[0].to == S09_REC_ABSENT &&
	      rec->fall[0].from == S09_RUNG_PLAIN);
}

static unsigned bench_between;

static void count_between(void *ctx)
{
	(void)ctx;
	bench_between++;
}

/* The link bench, one pass (S-09.075): every rung and length, then plain,
 * every session by the bus's one send, each request timed; a fault counted
 * where it broke, kept in the record, and the run going on; nothing but the
 * link's own requests sent; the boot's state as it was after it. */
static void test_link_bench(void)
{
	static struct s09_bench b;
	static const uint32_t ns[S09_RUNG_TOP + 1u] = { 1280, 1280, 640, 320, 160 };
	unsigned others = 0;

	t_case = "link bench";
	fresh_plain();
	lb.bad_rungs = 1u << S09_RUNG_TOP;	/* struck in the boot's negotiation */
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(s09_master_rung(&ms) == S09_RUNG_QUAD && ms.ceiling == S09_RUNG_QUAD);
	CHECK(s09_master_link_plain(&ms));
	lb.bad_rungs = 0;
	memcpy(lb.word_ns, ns, sizeof ns);
	memset(&b, 0, sizeof b);
	memset(lb.cmds, 0, sizeof lb.cmds);
	b.n = 3;
	b.between = count_between;
	bench_between = 0;
	/* rung 1's sixth test of the run (its third of 64 bytes, after the 14
	 * of its entry): a bit wrong */
	arm(S09_LINK_TEST, false, false, 9);
	lb.fs[0].nth = 20;
	{
		const uint32_t fell = s09_master_fell(&ms), falls = s09_master_record(&ms)->falls;
		const uint32_t negs = s09_master_record(&ms)->negotiations;

		CHECK(s09_master_link_bench(&ms, &b));
		CHECK(s09_master_rung(&ms) == S09_RUNG_PLAIN && !s09_slave_on_trial(&sl));
		CHECK(s09_slave_mode(&sl) == S09_MODE_PLAIN && !sl.recovering && !sl.fell_notice);
		CHECK(b.settled && s09_master_settled(&ms) && !b.cut);
		/* its fall-back counted in the boot's count too, the record's
		 * fall-backs of requests untouched */
		CHECK(b.fell == 1u && s09_master_fell(&ms) == fell + 1u);
		CHECK(s09_master_record(&ms)->falls == falls);
		CHECK_EQ(s09_master_record(&ms)->negotiations, negs);
		CHECK_EQ(ms.ceiling, S09_RUNG_QUAD);	/* as it was */
	}
	/* (a reply session met by a slave waiting for a request gives it the
	 * master's idle words, a "command" 0) */
	for (unsigned c = 1; c < 256u; c++)
		others += c != S09_ECHO && c != S09_LINK_MODE && c != S09_LINK_TEST &&
			  c != S09_LINK_STATUS ? lb.cmds[c] : 0u;
	CHECK_EQ(others, 0u);
	for (unsigned r = 0; r <= S09_RUNG_TOP; r++) {
		for (unsigned l = 0; l < S09_BENCH_LENS; l++) {
			const struct s09_bench_cell *c = &b.cell[r][l];
			const uint32_t words = (S09_HDR + s09_pad4(s09_bench_len[l]) +
						(r ? S09_CHECK_LEN : 0u)) / 4u;

			CHECK_EQ(c->sent, 3u);
			CHECK(c->passed == 3u || c->passed == 2u);
			if (c->passed == 3u)
				CHECK(c->us + 3u >= 3u * words * ns[r] / 1000u &&
				      c->us <= 3u * words * ns[r] / 1000u + 3u);
		}
		if (r)
			CHECK(b.entry[r] == S09_RR_KEPT);
	}
	CHECK_EQ(bench_between, 3u * S09_BENCH_LENS * 5u);
	CHECK(lb.split == 0u);
	/* the fault: counted where it broke, the rung entered again */
	{
		const struct s09_bench_cell *c = &b.cell[S09_RUNG_CHECKED][1];
		unsigned at = 0;

		for (unsigned k = 0; k < S09_AT_COUNT; k++)
			at += c->at[k];
		CHECK(lb.fs[0].fired && c->passed == 2u && at == 1u);
		CHECK(b.entries[S09_RUNG_CHECKED] == 2u && b.entries[S09_RUNG_TOP] == 1u);
		CHECK(!b.stopped);
	}
	/* ... and in the record, with the slave's account */
	{
		const struct s09_record *rec = s09_master_record(&ms);

		CHECK(rec->bench_faults == 1u && rec->bench[0].kind == S09_BF_REQUEST);
		CHECK(rec->bench[0].rung == S09_RUNG_CHECKED &&
		      rec->bench[0].len == 64u && rec->bench[0].req == 2u);
		CHECK(rec->bench[0].to == S09_RUNG_PLAIN);
		CHECK(rec->bench[0].st.told && rec->bench[0].st.why == S09_WHY_CHECK &&
		      rec->bench[0].st.cmd == S09_LINK_TEST);
	}
	/* a plain request after it is taken */
	{
		struct s09_stat st;

		CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
	}
	/* an install after it negotiates afresh, below its struck rung */
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK_EQ(s09_master_rung(&ms), S09_RUNG_QUAD);

	/* a session fault on the last rung's last request (64 KiB): the slave
	 * is told the link is plain, the plain cells pass, and a plain request
	 * after it is taken */
	{
		struct s09_stat st;

		fresh_on(&bus, 0);
		memcpy(lb.word_ns, ns, sizeof ns);
		memset(&b, 0, sizeof b);
		b.n = 3;
		/* each rung: 14 tests to enter it, then its 21 */
		arm(S09_LINK_TEST, false, true, 100);
		lb.fs[0].nth = 4u * 35u;
		CHECK(s09_master_link_bench(&ms, &b));
		CHECK(lb.fs[0].fired && b.settled && s09_master_settled(&ms) && !sl.recovering);
		CHECK(b.cell[S09_RUNG_TOP][S09_BENCH_LENS - 1u].passed == 2u);
		for (unsigned l = 0; l < S09_BENCH_LENS; l++)
			CHECK(b.cell[S09_RUNG_PLAIN][l].passed == 3u);
		CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
		CHECK(!ms.absent);
	}

	/* two faults in a length: its other requests not sent */
	fresh_on(&bus, 0);
	memset(&b, 0, sizeof b);
	b.n = 4;
	arm(S09_LINK_TEST, false, false, 4);
	lb.fs[0].nth = 15;		/* rung 1's first two tests of 12 bytes */
	lb.fs[1] = lb.fs[0];
	/* ... the second, after the rung's entry again (the session the first
	 * fault fired in counts for the first alone) */
	lb.fs[1].nth = 15 + 14;
	lb.nfs = 2;
	CHECK(s09_master_link_bench(&ms, &b));
	CHECK_EQ(b.cell[S09_RUNG_CHECKED][0].sent, 2u);
	CHECK_EQ(b.cell[S09_RUNG_CHECKED][0].passed, 0u);
	CHECK_EQ(b.cell[S09_RUNG_CHECKED][1].sent, 4u);
	CHECK(b.settled);

	/* the time bound: the run cut short, plain and settled after it */
	fresh_on(&bus, 0);
	memcpy(lb.word_ns, ns, sizeof ns);
	memset(&b, 0, sizeof b);
	b.n = 3;
	b.limit_us = 50000u;
	CHECK(s09_master_link_bench(&ms, &b));
	CHECK(b.cut && b.settled && s09_master_rung(&ms) == S09_RUNG_PLAIN);
	CHECK(b.cell[S09_RUNG_TOP][S09_BENCH_LENS - 1u].sent == 0u);
	{
		struct s09_stat st;

		CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
	}

	/* with the slave absent: nothing */
	fresh_plain();
	s09_master_set_absent(&ms);
	CHECK(!s09_master_link_bench(&ms, &b));
}

/*
 * S-09.013 rule 3, S-09.074: a session the master let go whose request the
 * slave had taken whole and goes on executing, 60 ms, 1 s or nearly T_EXEC,
 * before it offers the reply: past the quiet wait. The first session after
 * the let-go is the echo's request, which clocks that reply out and fails;
 * the next echo is answered. The stale reply is never taken for another's:
 * in the plain mode (0x05 is S-09.013 rule 4's) the request fails with EIO
 * and the next 0x05 and a stat are answered right; on a rung the request is
 * sent again with its number and answered right.
 */
static void test_link_late_offer(void)
{
	static const uint32_t slow[] = { S06_MS(60), S06_MS(1000), S09_T_EXEC_US - S06_MS(100) };
	const struct s09_req r = { S09_BOOT_SIZE, 0, 0, 0, 0 };

	t_case = "link late offer";
	for (unsigned k = 0; k < sizeof slow / sizeof slow[0]; k++) {
		for (unsigned on_rung = 0; on_rung < 2u; on_rung++) {
			struct s09_stat st;
			uint32_t err = 0;
			int32_t v;

			fresh_on(&bus, on_rung ? S09_RUNG_TOP : 0u);
			lb.late_cmd = S09_BOOT_SIZE;
			lb.slow_cmd = S09_BOOT_SIZE;
			lb.slow_us = slow[k];
			v = s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err);
			CHECK(!ms.absent && lb.late_cmd == 0u && lb.slow_cmd == 0u);
			CHECK_EQ(lb.stale, 1u);		/* clocked by an echo, consumed */
			CHECK_EQ(lb.merged, 0u);
			if (on_rung) {
				CHECK_EQ(v, (int32_t)B_SIZE);
				CHECK(s09_master_rung(&ms) != S09_RUNG_PLAIN);
			} else {
				CHECK(v == -1 && err == S09_EIO);
			}
			CHECK_EQ(s09_master_call(&ms, &r, 0, 0, 0, 0, 0, &err), (int32_t)B_SIZE);
			CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
			CHECK(!ms.absent && lb.stale == 1u && lb.merged == 0u);
		}
	}
}

/*
 * A DMA set-up that does not work on a unit (S-13b.083's first test): the
 * send failing on every try on a rung, with no byte of its session clocked,
 * a word, the header, the header and its payload, or all of it (the slave
 * then took the test whole and offers its reply). The slave as the board's
 * is: it goes on receiving a request cut short, and offers a reply, until
 * its own T_REPLY from PC11 high (lb.hold, lb.rep_ready).
 *
 * Without S-09.013's quiet wait the first fault loses the slave: the first
 * echo is taken as more of the cut request, or clocked against the offered
 * reply, and each echo's reply session then gives the slave idle words that
 * the next echo completes, so no echo is ever heard whole.
 *
 * With it, the link test tries every rung, each failure followed by the
 * bus's plain set-up (its controller's restart) before the next session and
 * by the way back; the slave is never lost, no session meets the slave in
 * another, the record keeps the faults with the slave's account, the plain
 * cells pass, the run ends settled and a plain request after it is taken;
 * an install after it runs plain while the send still fails, and on the top
 * rung once it works. With the send failing in the plain mode too there is
 * no bridge at all: the slave is absent (S-09.008).
 */
static void test_link_feed_fails(void)
{
	static struct s09_bench b;
	static const unsigned bytes[] = { 0u, 4u, 12u, 24u, ~0u };

	t_case = "link feed fails";
	/* without the quiet wait */
	for (unsigned k = 0; k < sizeof bytes / sizeof bytes[0]; k++) {
		fresh_on(&bus, 0);
		memset(&b, 0, sizeof b);
		b.n = 16;
		ms.t_quiet_us = 0;
		lb.feed_fail = 1;
		lb.feed_bytes = bytes[k];
		lb.feed_fails = ~0u;
		CHECK(!s09_master_link_bench(&ms, &b));
		CHECK(ms.absent && b.fell == 1u && b.stopped && lb.merged != 0u);
		CHECK(b.end_rung == S09_RUNG_CHECKED && b.cell[S09_RUNG_CHECKED][0].sent == 0u);
		CHECK(s09_master_record(&ms)->bench[0].to == S09_REC_ABSENT);
	}
	for (unsigned k = 0; k < sizeof bytes / sizeof bytes[0]; k++) {
		const struct s09_record *rec;
		struct s09_stat st;

		fresh_on(&bus, 0);
		memset(&b, 0, sizeof b);
		b.n = 16;
		b.limit_us = 90000000u;
		lb.feed_fail = 1;
		lb.feed_bytes = bytes[k];
		lb.feed_fails = ~0u;
		CHECK(s09_master_link_bench(&ms, &b));
		CHECK(!ms.absent && b.settled && !b.cut && !b.stopped);
		CHECK(s09_master_rung(&ms) == S09_RUNG_PLAIN);
		CHECK(s09_slave_mode(&sl) == S09_MODE_PLAIN && !sl.recovering && !sl.fell_notice);
		CHECK(lb.unreset_sessions == 0u && lb.merged == 0u && lb.stale == 0u);
		CHECK(lb.split == 0u);
		/* the offered replies given up by the slave's bound */
		CHECK_EQ(lb.dropped, bytes[k] >= 24u ? 4u : 0u);	/* rung 1's first test whole */
		/* every rung tried, its first test failed in the send; the
		 * plain cells whole */
		for (unsigned r = S09_RUNG_CHECKED; r <= S09_RUNG_TOP; r++) {
			CHECK(b.entry[r] == S09_RR_TEST && b.entries[r] == 0u);
			for (unsigned l = 0; l < S09_BENCH_LENS; l++)
				CHECK(b.cell[r][l].sent == 0u);
		}
		for (unsigned l = 0; l < S09_BENCH_LENS; l++)
			CHECK(b.cell[S09_RUNG_PLAIN][l].sent == 16u &&
			      b.cell[S09_RUNG_PLAIN][l].passed == 16u);
		CHECK(b.fell == 4u);
		/* the record: each rung's entry, in order, with the slave's
		 * account */
		rec = s09_master_record(&ms);
		CHECK_EQ(rec->bench_faults, 4u);
		for (unsigned i = 0; i < 4u; i++)
			CHECK(rec->bench[i].no == i && rec->bench[i].kind == S09_BF_ENTRY &&
			      rec->bench[i].rung == S09_RUNG_CHECKED + i &&
			      rec->bench[i].result == S09_RR_TEST &&
			      rec->bench[i].fault.at == S09_AT_SEND &&
			      rec->bench[i].to == S09_RUNG_PLAIN &&
			      rec->bench[i].echo.at == S09_AT_NONE && rec->bench[i].st.told);
		CHECK_EQ(s09_files_stat(&F, "/yaffs2", &st), 0);
		/* an install after it, once the send works: the run struck no
		 * rung, so the top one */
		lb.feed_fail = 0;
		CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
		CHECK(s09_master_rung(&ms) == S09_RUNG_TOP);
		CHECK(s09_files_staging_reset(&F) && staged_file("f/after", 0x6000u, 81));
		CHECK(lb.split == 0u);
		/* ... and an install with the send still failing on every rung:
		 * plain, its bytes right */
		fresh_on(&bus, 0);
		lb.feed_fail = 1;
		lb.feed_bytes = bytes[k];
		lb.feed_fails = ~0u;
		CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
		CHECK(s09_master_rung(&ms) == S09_RUNG_PLAIN && !ms.absent && s09_master_settled(&ms));
		CHECK(s09_files_staging_reset(&F) && staged_file("f/plain", 0x3000u, 80));
	}

	/* the send failing in the plain mode too: no bridge at all, the
	 * slave absent from the start (S-09.008) */
	for (unsigned k = 0; k < sizeof bytes / sizeof bytes[0]; k++) {
		struct s09_stat st;

		fresh_on(&bus, 0);
		s09_slave_init(&sl, &store);
		lb.feed_fail = 1;
		lb.feed_plain = true;
		lb.feed_bytes = bytes[k];
		lb.feed_fails = ~0u;
		s09_master_init(&ms, &bus, &sim.clock);
		CHECK(!s09_master_start(&ms) && ms.absent);
		s09_files_init(&F, &ms);
		CHECK(s09_files_stat(&F, "/yaffs2", &st) < 0);
		CHECK(!s09_master_link_bench(&ms, &b));
	}

	/* the bus dies with the send: the run ends at rung 1's entry, the
	 * slave lost, the rungs not reached not tried, and the record says
	 * how the echoes failed */
	fresh_on(&bus, 0);
	memset(&b, 0, sizeof b);
	b.n = 4;
	lb.feed_fail = 1;
	lb.feed_fails = ~0u;
	lb.feed_kills = true;
	CHECK(!s09_master_link_bench(&ms, &b));
	CHECK(ms.absent && !b.settled);
	CHECK(b.stopped && b.end_rung == S09_RUNG_CHECKED && b.end_len == 12u && b.end_req == 0u);
	for (unsigned r = 0; r <= S09_RUNG_TOP; r++)
		for (unsigned l = 0; l < S09_BENCH_LENS; l++)
			CHECK(b.cell[r][l].sent == 0u);
	CHECK(b.entry[S09_RUNG_CHECKED] == S09_RR_TEST);
	for (unsigned r = S09_RUNG_CHECKED_FAST; r <= S09_RUNG_TOP; r++)
		CHECK(b.entry[r] == S09_RR_NONE && b.entries[r] == 0u);
	{
		const struct s09_record *rec = s09_master_record(&ms);

		CHECK(rec->bench_faults == 1u && rec->bench[0].to == S09_REC_ABSENT);
		CHECK(rec->bench[0].fault.at == S09_AT_SEND && rec->bench[0].echo.at == S09_AT_READY);
		CHECK(rec->bench[0].fall_us >= 8u * S09_T_REPLY_US && !rec->bench[0].st.told);
	}

	/* a send that fails after the slave took the test whole, its reply on
	 * offer; the next session waits it out */
	fresh_on(&bus, 0);
	memset(&b, 0, sizeof b);
	b.n = 2;
	arm(S09_LINK_TEST, false, true, 100000);
	lb.fs[0].nth = 14u + 3u * 2u + 1u;	/* rung 1's first test of 4 KiB */
	CHECK(s09_master_link_bench(&ms, &b));
	CHECK(lb.fs[0].fired && b.settled && b.fell == 1u && lb.dropped == 1u);
	CHECK(lb.merged == 0u && lb.stale == 0u);
	CHECK(b.cell[S09_RUNG_CHECKED][3].passed == 1u &&
	      b.cell[S09_RUNG_CHECKED][3].at[S09_AT_SEND] == 1u);

	/* a rung that cannot be entered is left, the next one measured */
	fresh_on(&bus, 0);
	memset(&b, 0, sizeof b);
	b.n = 2;
	lb.deaf_rungs = 1u << S09_RUNG_QUAD;
	CHECK(s09_master_link_bench(&ms, &b));
	CHECK(b.settled && !b.stopped);
	CHECK(b.entry[S09_RUNG_QUAD] != S09_RR_KEPT && b.entry[S09_RUNG_QUAD] != S09_RR_NONE);
	CHECK(b.entries[S09_RUNG_QUAD] == 0u && b.cell[S09_RUNG_QUAD][0].sent == 0u);
	CHECK(b.entry[S09_RUNG_TOP] == S09_RR_KEPT && b.cell[S09_RUNG_TOP][6].passed == 2u);
	CHECK(s09_master_record(&ms)->bench_faults == 1u &&
	      s09_master_record(&ms)->bench[0].kind == S09_BF_ENTRY &&
	      s09_master_record(&ms)->bench[0].rung == S09_RUNG_QUAD);
}

/* With every faster rung failing its tests, the requests after are the
 * plain link's, byte for byte, every one by the bus's one send. */
static void test_link_all_fail(void)
{
	uint32_t want;

	t_case = "link all fail";
	fresh_on(&bus, 0);
	CHECK(s09_files_staging_reset(&F));
	lb.log = 0;
	CHECK(staged_file("p/same", 0x2345u, 90));
	want = lb.log;
	fresh_on(&bus, 0);
	lb.bad_rungs = 0x1Eu;
	CHECK(s09_master_link_fast(&ms, S09_RUNG_TOP));
	CHECK(s09_master_rung(&ms) == S09_RUNG_PLAIN && lb.split == 0u);
	CHECK(s09_files_staging_reset(&F));
	lb.log = 0;
	CHECK(staged_file("p/same", 0x2345u, 90));
	CHECK_EQ(lb.log, want);
}

/* S-09.071: every link test as sent on each rung, heard by a listener in
 * single I/O (the slave fell back, or its trial ran out): the header it
 * takes is ignored, no command, or the link test itself. */
static bool harmless(const uint8_t *h, size_t n)
{
	if (n < S09_HDR || h[1] >= S09_SLOTS)
		return true;		/* no header, or one ignored */
	return h[0] == S09_LINK_TEST || s09_kind(h[0]) == S09_K_UNDEFINED;
}

static void test_link_tests_heard(void)
{
	t_case = "link tests heard";
	for (unsigned b = 0; b < BUSES; b++) {
		for (unsigned top = S09_RUNG_CHECKED_FAST; top <= S09_RUNG_TOP; top++) {
			fresh_on(buses[b], 0);
			lb.g2 = true;
			CHECK(s09_master_link_fast(&ms, top));
			CHECK_EQ(s09_master_rung(&ms), top);
			CHECK(lb.g2_seen[top] >= 13u);
			CHECK_EQ(lb.g2_bad, 0u);
		}
	}
	/* the control: a random first word that is not guarded is heard as
	 * a command of the master's choosing */
	{
		static uint8_t q[64], heard[16];
		const struct s09_req r = { S09_LINK_TEST, 0, 40, 0, 40 };

		s09_req_pack(&r, q);
		quad_payload_for((const uint8_t *)"\x14\x00\x00\x00", 4, q + S09_HDR, 1);
		CHECK(quad_heard(q, 64, heard, 0) == 16u && heard[0] == S09_ERASE &&
		      !harmless(heard, 16));
	}
}

/*
 * The exhaustive one (S-09.076): faults at every word of the sessions a
 * file write, a sector write and a chunked read make, on every rung (every
 * session by the bus's one send, S-13b.083); during the way back; two in one
 * run; with a slave that takes longer than T_REPLY. A run:
 * the operation, its bus and rung, its faults.
 */
enum { OP_WRITE, OP_SECTORS, OP_READ };

struct sweep_run {
	const struct s09_bus_ops *b;
	unsigned rung, op;
	uint32_t n;
	struct lb_fault f[2];
	unsigned nf;
	uint32_t slow;
	unsigned slow_rung;
};

static struct {
	unsigned runs, fired, on_rung, fell, plain;
	unsigned long words;		/* request and reply words the faults met */
} sweep;

static const char *const op_name[] = { "file write", "sector write", "read" };

#define SWEEP_CHECK(cond, what)                                               \
	do {                                                                   \
		if (!(cond)) {                                                 \
			t_failures++;                                          \
			fprintf(stderr, "%s:%d: [%s] %s: %s of %u, rung %u;" \
				" %02x %s %s word %u #%u",                     \
				__FILE__, __LINE__, t_case, what, op_name[r->op], \
				(unsigned)r->n, r->rung,                       \
				r->f[0].cmd, r->f[0].rep ? "reply" : "request", \
				r->f[0].brk ? "break" : r->f[0].none ? "none" : "flip", \
				r->f[0].word, r->f[0].nth);                    \
			if (r->nf > 1u)                                        \
				fprintf(stderr, "; %02x %s %s word %u #%u",    \
					r->f[1].cmd, r->f[1].rep ? "reply" : "request", \
					r->f[1].brk ? "break" : "flip",        \
					r->f[1].word, r->f[1].nth);            \
			fputc('\n', stderr);                                   \
		}                                                              \
	} while (0)

static void sweep_one(const struct sweep_run *r, const uint8_t *want)
{
	char path[40] = "";
	uint32_t len = 0, err = 0;
	bool ok, all;

	/* both ends as the link came up on the run's rung: brought up once
	 * per bus and rung, then copied back for each run */
	static struct {
		const struct s09_bus_ops *b;
		unsigned rung;
		struct s09_slave sl;
		struct s09_master ms;
		struct lb_state lb;
	} up;

	sim.now = 0;		/* each run its own time */
	if (up.b != r->b || up.rung != r->rung) {
		memset(&lb, 0, sizeof lb);
		lb.hear_quad = -1;
		s09_slave_init(&sl, &store);
		s09_master_init(&ms, r->b, &sim.clock);
		CHECK(s09_master_start(&ms));
		if (r->rung != 0u)
			CHECK(s09_master_link_fast(&ms, r->rung) &&
			      s09_master_rung(&ms) == r->rung);
		up.b = r->b;
		up.rung = r->rung;
		up.sl = sl;
		up.ms = ms;
		up.lb = lb;
	}
	sl = up.sl;
	ms = up.ms;
	lb = up.lb;
	if (r->slow_rung != 0u)		/* a rung that fails its gate */
		lb.word_ns[r->slow_rung] = 1300;
	s09_files_init(&F, &ms);
	if (r->op == OP_READ)
		CHECK(put("/yaffs2/rd", want, r->n));
	memset(&ex, 0, sizeof ex);
	memcpy(lb.fs, r->f, sizeof lb.fs);
	lb.nfs = r->nf;
	lb.f_fill = -1;
	slow_write_us = r->slow;
	if (r->op == OP_WRITE) {
		static unsigned name;

		snprintf(path, sizeof path, "s/%u", name++);
		ok = s09_files_staging_open(&F, path) &&
		     s09_files_staging_write(&F, want, r->n);
		ok = s09_files_staging_close(&F) && ok;
	} else if (r->op == OP_SECTORS) {
		const struct s09_req sel = { S09_SELECT, 0, 0, 1, 0 };
		struct s09_req wr = { S09_WRITE_SECTORS, 0, 0, 8, r->n / S09_SECTOR };

		memset(emmc.bp[0], 0, 16u * S09_SECTOR);
		ok = s09_master_call(&ms, &sel, 0, 0, 0, 0, 0, &err) == 0 &&
		     s09_master_call(&ms, &wr, want, r->n, 0, 0, 0, &err) == 0;
	} else {
		memset(back, 0, r->n);
		ok = s09_files_read_whole(&F, "/yaffs2/rd", back, sizeof back, &len) &&
		     len == r->n;
	}
	slow_write_us = 0;
	all = lb.f_fired == r->nf;
	sweep.runs++;
	sweep.fired += all;
	if (lb.f_fired == 0u) {
		SWEEP_CHECK(ok, "no fault fired");
		return;
	}
	if (r->rung == 0u) {
		/* the plain mode: the failure reported, nothing sent again */
		sweep.plain++;
		SWEEP_CHECK(!ok && ms.absent, "plain: not reported");
		SWEEP_CHECK(ex.file[S09_WRITE] + ex.emmc[S09_WRITE_SECTORS] <= 1u,
			    "plain: executed twice");
		for (int h = 0; h < 200; h++)
			(void)yaffs_close(h);
		return;
	}
	sweep.on_rung++;
	SWEEP_CHECK(lb.merged == 0u && lb.stale == 0u, "a session met the slave in another");
	sweep.fell += s09_master_fell(&ms) != 0u;
	SWEEP_CHECK(s09_master_fell(&ms) != 0u, "no fall-back");
	/* S-09.075: the link never goes plain while rung 1 can be entered */
	SWEEP_CHECK(ok && !ms.absent, "not through");
	SWEEP_CHECK(s09_master_rung(&ms) != S09_RUNG_PLAIN, "left unchecked");
	if (r->op == OP_WRITE) {
		SWEEP_CHECK(ex.file[S09_WRITE] == 1u, "executed other than once");
		path_buf[0] = '\0';
		strcat(strcat(path_buf, "upgrade/"), path);
		memset(back, 0, r->n);
		SWEEP_CHECK(s09_files_read_whole(&F, path_buf, back, sizeof back, &len) &&
			    len == r->n && !memcmp(back, want, r->n), "the file differs");
		(void)s09_files_unlink(&F, path_buf);
	} else if (r->op == OP_SECTORS) {
		SWEEP_CHECK(ex.emmc[S09_WRITE_SECTORS] == 1u, "executed other than once");
		SWEEP_CHECK(!memcmp(emmc.bp[0] + 8u * S09_SECTOR, want, r->n),
			    "the sectors differ");
		emmc.part = 0;
	} else {
		SWEEP_CHECK(!memcmp(back, want, r->n), "the data differ");
		SWEEP_CHECK(ex.pieces == (r->n + S09_READ_CHUNK - 1u) / S09_READ_CHUNK,
			    "pieces read other than once");
	}
	/* the run's end: plain, and the slave told so (S-09.074, S-09.078) */
	SWEEP_CHECK(s09_master_link_plain(&ms) && s09_master_settled(&ms) && !sl.recovering &&
		    !sl.fell_notice, "not settled after");
}

static struct lb_fault fault_at(uint8_t cmd, bool rep, bool brk, unsigned w,
				unsigned nth)
{
	struct lb_fault f;

	memset(&f, 0, sizeof f);
	f.cmd = cmd;
	f.rep = rep;
	f.brk = brk;
	f.word = w;
	f.nth = nth;
	return f;
}

/* One fault at every word of a session of cmd (its nth from now on): a
 * flip and a break, in the request (words req) or the reply (words rep);
 * in the plain mode a flip after a reply's first word, or in a request, is
 * not caught, so only the breaks and that word there. stride: every so
 * many words, the first 4 and the last 4 always. */
static void sweep_session(struct sweep_run *r, const uint8_t *want, uint8_t cmd,
			  unsigned nth, unsigned req, unsigned rep, unsigned stride)
{
	for (unsigned side = 0; side < 2u; side++) {
		const unsigned words = side ? rep : req;

		for (unsigned w = 0; w < words; w++) {
			if (stride > 1u && w >= 4u && w + 4u < words && w % stride != 0u)
				continue;
			for (unsigned brk = 0; brk < 2u; brk++) {
				if (!brk && r->rung == 0u && !(side && w == 0u))
					continue;
				r->f[0] = fault_at(cmd, side != 0u, brk != 0u, w, nth);
				r->nf = 1;
				sweep_one(r, want);
				sweep.words++;
			}
		}
	}
}

/* The words of a request session of n payload bytes on the rung, and of a
 * reply of n bytes (its check outside the plain mode). */
static unsigned req_words(unsigned rung, uint32_t n)
{
	return (S09_HDR + s09_pad4(n) + (rung ? S09_CHECK_LEN : 0u)) / 4u;
}

static unsigned rep_words(unsigned rung, uint32_t n)
{
	return (n + (rung ? 4u : 0u)) / 4u;
}

static void test_link_sweep(void)
{
	static uint8_t want[0x8000];
	uint32_t seed = 2024;
	unsigned pairs = 0;

	t_case = "link sweep";
	pattern(want, sizeof want, 99);
	memset(&sweep, 0, sizeof sweep);
	{
		for (unsigned rung = 0; rung <= S09_RUNG_TOP; rung++) {
			struct sweep_run r;

			memset(&r, 0, sizeof r);
			r.b = &bus;
			r.rung = rung;
			fresh_on(&bus, 0);
			CHECK(s09_files_staging_reset(&F));
			/* a file write of 0x100 bytes: its request, its poll's
			 * request and the poll's reply, every word */
			r.op = OP_WRITE;
			r.n = 0x100;
			sweep_session(&r, want, S09_WRITE, 1, req_words(rung, 0x100), 0, 1);
			sweep_session(&r, want, S09_WRITE, 1, 0, rep_words(rung, 12), 1);
			sweep_session(&r, want, S09_POLL, 1, req_words(rung, 0), 0, 1);
			/* ... with a slave that takes longer than T_REPLY */
			r.slow = 2u * S09_T_REPLY_US;
			sweep_session(&r, want, S09_WRITE, 1, 0, rep_words(rung, 12), 1);
			r.slow = 0;
			/* a write of 32 KiB on a rung: every word on the top
			 * rung, every 61st elsewhere */
			if (rung != 0u) {
				r.n = 0x8000;
				sweep_session(&r, want, S09_WRITE, 1,
					      req_words(rung, 0x8000), rep_words(rung, 12),
					      rung == S09_RUNG_TOP ? 1u : 61u);
			}
			/* a sector write */
			r.op = OP_SECTORS;
			r.n = 0x200;
			sweep_session(&r, want, S09_WRITE_SECTORS, 1, req_words(rung, 0x200),
				      rep_words(rung, 12), 1);
			/* a chunked read of 0x1100 bytes: its request and every
			 * word of its first piece, data included */
			r.op = OP_READ;
			r.n = 0x1100;
			sweep_session(&r, want, S09_READ, 1, req_words(rung, 0),
				      rep_words(rung, 8u + S09_READ_CHUNK), 1);
			if (rung == 0u)
				continue;
			/* the way back after a write's request came wrong: a
			 * fault in each of its sessions, every word */
			r.op = OP_WRITE;
			r.n = 0x100;
			{
				/* the rung the way back enters: the next down, rung 1
				 * from rung 1 or 2; above rung 1 its gate, three timed
				 * tests of 64 KiB, follows its keep */
				const bool gated = rung >= S09_RUNG_QUAD;
				const unsigned largest = req_words(rung, S09_TEST_MAX);
				const unsigned gate_nth = 15u;
				const struct {
					uint8_t cmd;
					unsigned nth, req, rep, stride, slow;
				} back_to[] = {
					{ S09_ECHO, 1, 3, 2, 1, 0 },		/* plain */
					{ S09_LINK_STATUS, 1, 3, 9, 1, 0 },	/* 0x1B, plain */
					{ S09_LINK_MODE, 1, 3, 2, 1, 0 },	/* the try, plain */
					{ S09_LINK_TEST, 1, 6, 3, 1, 0 },	/* the first test */
					{ S09_LINK_MODE, 2, 5, 3, 1, 0 },	/* the keep */
					/* the repeat: the write the first fault fired in
					 * counts for the first alone, so the next is #1 */
					{ S09_WRITE, 1, req_words(rung, 0x100), 0, 1, 0 },
					{ S09_POLL, 1, 5, 0, 1, 0 },
					/* the gate's first timed test */
					{ S09_LINK_TEST, gated ? gate_nth : 0u, largest, 3, 4093, 0 },
					/* the gate failed: its 0x19 mode 0 on the rung */
					{ S09_LINK_MODE, gated ? 3u : 0u, 5, 3, 1, rung - 1u },
				};

				for (unsigned k = 0; k < sizeof back_to / sizeof back_to[0]; k++) {
					unsigned met = 0;

					if (back_to[k].nth == 0u)
						continue;	/* no such session from this rung */
					r.slow_rung = back_to[k].slow;
					for (unsigned side = 0; side < 2u; side++) {
						const unsigned words = side ? back_to[k].rep : back_to[k].req;

						for (unsigned w = 0; w < words; w++) {
							if (back_to[k].stride > 1u && w >= 4u && w + 4u < words &&
							    w % back_to[k].stride != 0u)
								continue;
							for (unsigned brk = 0; brk < 2u; brk++) {
								r.f[0] = fault_at(S09_WRITE, false, false, 3, 1);
								r.f[1] = fault_at(back_to[k].cmd, side != 0u,
										  brk != 0u, w, back_to[k].nth);
								r.nf = 2;
								sweep_one(&r, want);
								sweep.words++;
								met += lb.fs[1].fired;
							}
						}
					}
					/* each session of the way back was met */
					if (met == 0u)
						fprintf(stderr, "link sweep: way back %u never met, rung %u\n",
							k, rung);
					CHECK(met != 0u);
				}
				r.slow_rung = 0;
			}
			fprintf(stderr, "link sweep: rung %u, %u runs so far, %.0f s\n",
				rung, sweep.runs,
				(double)clock() / CLOCKS_PER_SEC);
			/* two faults anywhere in the run, a seeded sample; each
			 * at a word every session of its kind has */
			for (unsigned k = 0; k < 600u; k++, pairs++) {
				static const struct {
					uint8_t cmd, req, rep;
				} kinds[] = {
					{ S09_WRITE, 0, 4 },	/* req: the write's own */
					{ S09_POLL, 5, 2 },
					{ S09_ECHO, 3, 2 },	/* plain */
					{ S09_LINK_MODE, 3, 2 },
					{ S09_LINK_TEST, 6, 3 },
					{ S09_LINK_STATUS, 3, 9 },
				};

				r.nf = 2;
				for (unsigned i = 0; i < 2u; i++) {
					unsigned c, rep, words;

					seed = seed * 1103515245u + 12345u;
					c = (seed >> 16) % (sizeof kinds / sizeof kinds[0]);
					seed = seed * 1103515245u + 12345u;
					rep = seed >> 20 & 1u;
					words = rep ? kinds[c].rep : kinds[c].cmd == S09_WRITE ?
						req_words(rung, 0x100) : kinds[c].req;
					r.f[i] = fault_at(kinds[c].cmd, rep != 0u,
							  (seed >> 21 & 1u) != 0u,
							  (seed >> 8) % words,
							  1u + (seed >> 24) % 2u);
				}
				sweep_one(&r, want);
			}
		}
	}
	fprintf(stderr, "link sweep: %u runs, %lu single-fault positions, %u pairs, "
		"%u in the plain mode\n", sweep.runs, sweep.words, pairs, sweep.plain);
	/* every fault was met where it was aimed (two-fault samples aside),
	 * and on a rung each one cost a fall-back */
	CHECK(sweep.runs > 20000u);
	CHECK_EQ(sweep.fell, sweep.on_rung);
}

/* A zip reader fake: the members' names, data and stated sizes. */
struct fz_member {
	const char *name;
	const uint8_t *data;
	uint32_t len, stated;		/* stated 0: len */
};

static struct {
	const struct fz_member *m;
	size_t n;
	unsigned calls;
} fz;

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool fz_member_fn(void *c, size_t i, struct s11_member *m)
{
	CHECK(c == &fz);
	fz.calls++;
	if (i >= fz.n)
		return false;
	memset(m, 0, sizeof *m);
	snprintf(m->name, sizeof m->name, "%s", fz.m[i].name);
	m->size = fz.m[i].stated != 0 ? fz.m[i].stated : fz.m[i].len;
	m->crc32 = ref_crc32(0, fz.m[i].data, fz.m[i].len);
	return true;
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool fz_zip64(void *c)
{
	CHECK(c == &fz);
	return false;
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool fz_extract(void *c, size_t i, s11_sink_fn sink, void *arg)
{
	const struct fz_member *e = &fz.m[i];

	CHECK(c == &fz);
	for (uint32_t o = 0; o < e->len; o += 3000u) {
		uint32_t k = e->len - o < 3000u ? e->len - o : 3000u;

		if (!sink(arg, e->data + o, k))
			return false;
	}
	return true;
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static uint32_t fz_crc(void *c, uint32_t crc, const uint8_t *p, size_t n)
{
	CHECK(c == &fz);
	return ref_crc32(crc, p, (uint32_t)n);
}

static const struct s11_zip_ops fz_reader = {
	fz_member_fn, fz_zip64, fz_extract, fz_crc, NULL, NULL, NULL, NULL, NULL,
	NULL, &fz,
};

/* Checks 16 and 17 (S-11.030 - S-11.035) staged over the bridge. */
static void test_check_zip(void)
{
	static uint8_t os[0x2345];
	static const char ver[] = "1.2.0";
	const struct fz_member good[] = {
		{ "firmware/", NULL, 0, 0 },
		{ "firmware/blackfin/bin/os.ldr", os, sizeof os, 0 },
		{ "firmware/blackfin/bin/os.version", (const uint8_t *)ver, 5, 0 },
		{ "firmware/blackfin/content/a.json", (const uint8_t *)"{}", 2, 0 },
	};
	const struct fz_member huge[] = {
		{ "firmware/blackfin/content/b.bin", os, sizeof os, (uint32_t)VOL_LEN },
	};
	struct s09_files_zip sz;
	struct s11_zip_ops zip;
	struct s11_fs_ops fs;
	struct s11_staged_sizes sizes;
	struct s11_staged st;
	struct s09_stat sst;
	char name[S11_PATH_MAX];
	uint8_t type;
	uint64_t free = 0;
	uint32_t len = 0;
	unsigned s0, total;

	t_case = "check zip";
	fresh();
	pattern(os, sizeof os, 21);
	/* an OS image the hand-over takes (check 18): no destination, FINAL */
	ldr_header(os, 0x5000u, 0x11A00000u, 0, 16u);
	ldr_header(os + 16, 0x8000u, 0x11A00000u, 0, 0);
	fz.m = good;
	fz.n = sizeof good / sizeof good[0];
	CHECK(put("upgrade/progress.log", (const uint8_t *)"\x03\x00", 2));
	CHECK(put("upgrade/old.bin", (const uint8_t *)"o", 1));
	s09_files_zip_ops(&sz, &F, &fz_reader, &zip);
	CHECK(zip.ctx == &sz && sz.f == &F);
	s0 = lb.sessions;
	CHECK_EQ(s11_check_zip(&zip, &sizes), S11_REASON_NONE);
	total = lb.sessions - s0;
	CHECK(fz.calls != 0u);
	CHECK_EQ(sizes.size[S11_C_OS], (uint32_t)sizeof os);
	memset(back, 0, sizeof back);
	CHECK(s09_files_read_whole(&F, "upgrade/firmware/blackfin/bin/os.ldr", back,
				   sizeof back, &len));
	CHECK(len == sizeof os && !memcmp(back, os, sizeof os));
	CHECK_EQ(sizes.crc32[S11_C_OS], ref_crc32(0, os, sizeof os)); /* S-11.030 */
	CHECK_EQ(s09_files_stat(&F, "upgrade/old.bin", &sst), -1);
	CHECK_EQ(s09_files_stat(&F, S11_RECORD_PATH, &sst), -1);
	/* the run's view of what was staged (S-11.034, S-11.036) */
	s09_files_fs_ops(&F, &fs);
	s11_staged_scan(&fs, &st);
	CHECK(st.exists && st.content_dir);
	CHECK_EQ(st.size[S11_C_OS], (uint32_t)sizeof os);
	CHECK(st.version[S11_C_OS].major == 1 && st.version[S11_C_OS].minor == 2);

	/* S-11.033: a stated total the volume cannot hold; the area emptied */
	fz.m = huge;
	fz.n = 1;
	CHECK_EQ(s11_check_zip(&zip, &sizes), S11_REASON_SPACE);
	CHECK(s09_files_dir_exists(&F, "upgrade"));
	CHECK(!s09_files_entry(&F, "upgrade", 0, name, sizeof name, &type));

	/* the slave gone while a member is staged: refused MEMBER (S-11.031),
	 * nothing left open; then no free space can be read (SPACEQUERY) */
	fz.m = good;
	fz.n = sizeof good / sizeof good[0];
	lb.die_after = lb.sessions + total * 3u / 4u;
	CHECK_EQ(s11_check_zip(&zip, &sizes), S11_REASON_MEMBER);
	CHECK(F.stage_fd < 0);
	CHECK(!s09_files_free_space(&F, &free));
	CHECK_EQ(s11_space_rule(1, false, free), S11_REASON_SPACEQUERY);
}

/* S-15.041, S-15.052 */
static void test_menus(void)
{
	char name[S15_NAME_MAX + 1u], a[S15_NAME_MAX + 1u];
	uint8_t buf[64];
	static const char man[] = "{\"name\":\"pack a\"}";
	char longname[S15_NAME_MAX + 1u];

	t_case = "menus";
	fresh();
	CHECK(!s09_files_pack_dir(&F, 0, name));	/* no content/dlc */
	CHECK(s09_files_mkdirs(&F, "/yaffs2/content/dlc/a/"));
	CHECK(s09_files_mkdirs(&F, "/yaffs2/content/dlc/c/"));
	CHECK(put("/yaffs2/content/dlc/b.txt", (const uint8_t *)"b", 1));
	CHECK(put("/yaffs2/content/dlc/a/manifest.json", (const uint8_t *)man,
		  sizeof man - 1));
	CHECK(s09_files_pack_dir(&F, 0, a));
	CHECK(s09_files_pack_dir(&F, 1, name));
	CHECK((!strcmp(a, "a") && !strcmp(name, "c")) ||
	      (!strcmp(a, "c") && !strcmp(name, "a")));
	CHECK(!s09_files_pack_dir(&F, 2, name));
	memset(buf, 0, sizeof buf);
	CHECK_EQ(s09_files_read_manifest(&F, "a", buf, sizeof buf), sizeof man - 1);
	CHECK(!memcmp(buf, man, sizeof man - 1));
	CHECK_EQ(s09_files_read_manifest(&F, "a", buf, 4), 4u);
	CHECK_EQ(s09_files_read_manifest(&F, "c", buf, sizeof buf), 0u);
	/* a 255-byte pack name fits name[S15_NAME_MAX + 1] and its manifest path */
	memset(longname, 'n', S15_NAME_MAX);
	longname[S15_NAME_MAX] = '\0';
	CHECK(s09_files_read_manifest(&F, longname, buf, sizeof buf) == 0);
	/* the factory reset's removals and erase */
	CHECK(put("/yaffs2/user/a/b", (const uint8_t *)"b", 1));
	CHECK(put("/yaffs2/logs/l1", (const uint8_t *)"l", 1));
	CHECK(s09_files_remove_tree(&F, "/yaffs2/user"));
	CHECK(s09_files_remove_tree(&F, "/yaffs2/logs"));
	CHECK(s09_files_remove_tree(&F, "/yaffs2/content/dlc"));
	CHECK(!s09_files_dir_exists(&F, "/yaffs2/user"));
	CHECK(!s09_files_dir_exists(&F, "/yaffs2/content/dlc"));
	CHECK(s09_files_dir_exists(&F, "/yaffs2/content"));
	CHECK(s09_files_remove_tree(&F, "/yaffs2/user"));	/* missing */
	CHECK(s09_files_erase_os_fat(&F));
	CHECK_EQ(emmc.erases, 1u);
	CHECK_EQ(emmc.fat_erased, 1u);
	CHECK_EQ(emmc.last_cmd38, 0u);
	CHECK_EQ(emmc.part, 0u);
}

static size_t root_entries(char first[S09_DIRENT_NAME])
{
	uint8_t type;
	size_t n = 0;

	while (s09_files_entry(&F, "/yaffs2", n, first, S09_DIRENT_NAME, &type)) {
		n++;
		if (n == 1 && strcmp(first, "lost+found"))
			return 99;
	}
	return n;
}

/* S-15.072, S-12.057, S-12.052, S-15.073 */
static void test_format(void)
{
	char first[S09_DIRENT_NAME];
	size_t erased = 0, len = 8u * 128u * 4096u;
	FILE *f;
	char path[4096];
	const char *dir = getenv("WB_VECTORS");

	t_case = "format";
	fresh();
	CHECK(put("/yaffs2/user/x", big, 0x2000));
	CHECK(s09_files_mkdirs(&F, "/yaffs2/content/dlc/p/"));
	CHECK(s09_files_format(&F));
	for (size_t g = 0; g < S09_FILES_YAFFS2_GROUPS; g++)
		erased += emmc.erased[g];
	CHECK_EQ(erased, (size_t)S09_FILES_YAFFS2_GROUPS);	/* every group */
	CHECK_EQ(emmc.erases, (unsigned)S09_FILES_YAFFS2_GROUPS);
	CHECK_EQ(emmc.fat_erased + emmc.other_erased, 0u);
	CHECK_EQ(root_entries(first), 1u);		/* the root and lost+found */
	CHECK(!s09_files_volume_refused(&F));
	CHECK(put("/yaffs2/after", (const uint8_t *)"a", 1));	/* usable */

	/* S-12.057: a failed erase does not stop the format; it is a failed
	 * format, and the volume is mounted again */
	memset(emmc.erased, 0, sizeof emmc.erased);
	emmc.erases = 0;
	emmc.fail_erase_only = 10;
	CHECK(!s09_files_format(&F));
	CHECK_EQ(emmc.erases, (unsigned)S09_FILES_YAFFS2_GROUPS);
	erased = 0;
	for (size_t g = 0; g < S09_FILES_YAFFS2_GROUPS; g++)
		erased += emmc.erased[g];
	CHECK_EQ(erased, (size_t)S09_FILES_YAFFS2_GROUPS - 1u);
	CHECK(!emmc.erased[9]);
	CHECK(s09_files_dir_exists(&F, "/yaffs2/lost+found"));
	emmc.fail_erase_only = 0;

	/* a failed select erases nothing; the volume is mounted again */
	emmc.erases = 0;
	emmc.fail_select = true;
	CHECK(!s09_files_format(&F));
	CHECK_EQ(emmc.erases, 0u);
	emmc.fail_select = false;
	CHECK(s09_files_dir_exists(&F, "/yaffs2/lost+found"));

	/* a refused (big-endian) volume: its mount fails, the format erases it */
	t_case = "format refused";
	fresh();
	snprintf(path, sizeof path, "%s/s12/yaffs_big_endian.img",
		 dir != NULL ? dir : "build/vectors");
	f = fopen(path, "rb");
	CHECK(f != NULL);
	if (f == NULL)
		return;
	CHECK(s09_files_unmount(&F));
	memset(vol, 0, VOL_LEN);
	CHECK_EQ(fread(vol, 1, len, f), len);
	fclose(f);
	CHECK(s09_files_mount(&F) < 0);
	CHECK(s09_files_volume_refused(&F));
	CHECK(!s09_files_dir_exists(&F, "/yaffs2"));
	CHECK_EQ(F.err, S09_ENODEV);
	CHECK(!s09_files_remove_tree(&F, "/yaffs2/user"));	/* ENODEV is not missing */
	memset(emmc.erased, 0, sizeof emmc.erased);
	emmc.erases = 0;
	CHECK(s09_files_format(&F));
	CHECK_EQ(emmc.erases, (unsigned)S09_FILES_YAFFS2_GROUPS);
	CHECK(!s09_files_volume_refused(&F));
	CHECK_EQ(root_entries(first), 1u);
}

/* S-15.094's report, recorded: "s<step>" at a step's start, "d<a>,<b>" at
 * its success, with the progress counted apart. */
static struct {
	char s[256];
	unsigned progress;
	uint32_t done, total;
	bool backwards;		/* a step's progress went down */
} rp;

static void rp_step(void *c, enum s15_osw_step s)
{
	size_t n = strlen(rp.s);

	(void)c;
	snprintf(rp.s + n, sizeof rp.s - n, "s%u ", (unsigned)s);
	rp.done = rp.total = 0;
}

static void rp_progress(void *c, uint32_t done, uint32_t total)
{
	(void)c;
	rp.progress++;
	if (done < rp.done || done > total)
		rp.backwards = true;
	rp.done = done;
	rp.total = total;
}

static void rp_done(void *c, uint32_t a, uint32_t b)
{
	size_t n = strlen(rp.s);

	(void)c;
	snprintf(rp.s + n, sizeof rp.s - n, "d%u,%u ", (unsigned)a, (unsigned)b);
}

static const struct s15_osw_report rp_ops = { NULL, rp_step, rp_progress, rp_done };

static enum s15_os_write os_write(const struct s03_env *env, uint32_t cap,
				  struct s15_osw_end *end)
{
	memset(&rp, 0, sizeof rp);
	memset(end, 0xA5, sizeof *end);		/* every field is the write's */
	return s09_files_os_write(&F, env, image, cap, &rp_ops, end);
}

/* S-15.094 through S-03's OS write and the eMMC commands */
static void test_os_write(void)
{
	struct s03_emmc_ops cops;
	struct s03_env env;
	struct s03_header h;
	struct s15_osw_end e;
	uint8_t scratch[S09_MAX_SECTORS * S09_SECTOR];
	size_t n = 100000u;
	uint8_t *os = malloc(300000u);

	t_case = "os write";
	CHECK(os != NULL);
	if (os == NULL)
		return;
	fresh();
	s09_files_emmc_ops(&F, &cops);
	CHECK(cops.ctx == &F);
	CHECK(cops.switch_byte(cops.ctx, 0, 0) != 0);
	CHECK(cops.switch_byte(cops.ctx, 32, 0) != 0);		/* only the flush */
	CHECK(cops.switch_byte(cops.ctx, 33, 1) != 0);
	CHECK_EQ(emmc.flushes, 0u);
	env.ops = &cops;
	env.crc32 = ref_crc32;
	env.boot_size = s09_files_boot_size(&F);
	env.scratch = scratch;
	env.scratch_len = sizeof scratch;
	env.max_xfer = 0;		/* the client splits into 128 sectors */
	CHECK_EQ(env.boot_size, B_SIZE);

	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_UNREADABLE);
	CHECK(e.result == S15_OSW_UNREADABLE && e.why == S15_OSW_WHY_MISSING &&
	      e.emmc == S15_OSW_EMMC_UNTOUCHED && e.value == S09_ENOENT && !e.select0_failed);
	CHECK(strcmp(rp.s, "s0 ") == 0);
	CHECK_EQ(emmc.erases, 0u);
	CHECK(s09_files_mkdir(&F, "/yaffs2/os.ldr") == 0);	/* a directory */
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_UNREADABLE);
	CHECK(e.why == S15_OSW_WHY_DIRECTORY && e.emmc == S15_OSW_EMMC_UNTOUCHED);
	CHECK_EQ(s09_files_rmdir(&F, "/yaffs2/os.ldr"), 0);

	/* S-12.052: a refused volume is not read */
	pattern(os, n, 3);
	CHECK(put("/yaffs2/os.ldr", os, (uint32_t)n));
	F.mount_sent = F.mount_failed = true;
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_UNREADABLE);
	CHECK(e.why == S15_OSW_WHY_REFUSED && strcmp(rp.s, "s0 ") == 0);
	F.mount_failed = false;

	/* Every step in order, each done with the values its row names: the
	 * length; N + 1 and B in sectors; the target. */
	emmc.flushes = 0;
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_OK);
	CHECK_EQ(emmc.flushes, 3u);			/* S-03.068 */
	CHECK(e.result == S15_OSW_OK && e.why == S15_OSW_WHY_NONE && e.target == 1u &&
	      !e.select0_failed);
	CHECK(strcmp(rp.s, "s0 d100000,0 s1 d197,512 s2 d1,0 s3 d0,0 s4 d0,0 s5 d0,0 s6 d1,0 ") == 0);
	CHECK(rp.progress >= 4u && !rp.backwards);
	s03_header_decode(emmc.bp[0], &h);
	CHECK_EQ(h.length, (uint32_t)n);
	CHECK_EQ(h.crc, ref_crc32(0, os, (uint32_t)n));
	CHECK(s03_header_active(&h));
	CHECK(!memcmp(emmc.bp[0] + S03_SECTOR, os, n));
	CHECK_EQ(emmc.part, 0u);			/* select 0 at the end */
	/* the next write goes to the other partition */
	pattern(os, n, 4);
	CHECK(put("/yaffs2/os.ldr", os, (uint32_t)n));
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_OK);
	CHECK(e.target == 2u && strstr(rp.s, "s2 d2,0 ") != NULL && strstr(rp.s, "s6 d2,0 ") != NULL);
	s03_header_decode(emmc.bp[1], &h);
	CHECK(s03_header_active(&h) && h.length == n);
	CHECK(!memcmp(emmc.bp[1] + S03_SECTOR, os, n));
	s03_header_decode(emmc.bp[0], &h);
	CHECK(!s03_header_active(&h));

	/* S-03.051 refuses a short image: no size check of the write's own */
	emmc.erases = 0;
	CHECK(put("/yaffs2/os.ldr", os, 100));
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_FAILED);
	CHECK(e.why == S15_OSW_WHY_TOO_SMALL && e.value == 100u && e.limit == S03_MIN_IMAGE &&
	      e.emmc == S15_OSW_EMMC_UNTOUCHED && strcmp(rp.s, "s0 d100,0 s1 ") == 0);
	CHECK_EQ(emmc.erases, 0u);
	/* one that does not fit B */
	pattern(os, 300000u, 6);
	CHECK(put("/yaffs2/os.ldr", os, 300000u));
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_FAILED);
	CHECK(e.why == S15_OSW_WHY_NO_ROOM && e.value == 587u && e.limit == 512u);
	CHECK_EQ(emmc.erases, 0u);
	/* longer than the buffer: a file S-03.051 refuses */
	CHECK(put("/yaffs2/os.ldr", os, 0x1000));
	CHECK_EQ(os_write(&env, 0x800, &e), S15_OSW_FAILED);
	CHECK(e.why == S15_OSW_WHY_TOO_LARGE && e.value == 0x1000u && e.limit == 0x800u &&
	      strcmp(rp.s, "s0 ") == 0);
	/* an eMMC failure: the first write of the target's image, which
	 * erases nothing in group 0 (S-03.054, S-03.056) */
	CHECK(put("/yaffs2/os.ldr", os, (uint32_t)n));
	emmc.erases = 0;
	emmc.fail_bp_write = true;
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_FAILED);
	CHECK(e.why == S15_OSW_WHY_EMMC_WRITE && e.part == 1u && e.target == 1u && e.value == 1u &&
	      e.emmc == S15_OSW_EMMC_UNCHANGED);
	CHECK(strlen(rp.s) >= 3u && strcmp(rp.s + strlen(rp.s) - 3u, "s4 ") == 0);
	CHECK_EQ(emmc.erases, 0u);
	emmc.fail_bp_write = false;
	/* S-03.068: the flushes in turn fail, on every try */
	emmc.fail_flush_from = emmc.flushes + 1u;
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_FAILED);
	CHECK(e.why == S15_OSW_WHY_FLUSH && e.emmc == S15_OSW_EMMC_UNCHANGED);
	s03_header_decode(emmc.bp[e.target - 1u], &h);
	CHECK(!s03_header_active(&h));			/* the target stays inactive */
	emmc.fail_flush_from = emmc.flushes + 2u;
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_FAILED);
	CHECK(e.why == S15_OSW_WHY_FLUSH && e.emmc == S15_OSW_EMMC_BOTH_ACTIVE);
	emmc.fail_flush_from = emmc.flushes + 3u;
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_FAILED);
	CHECK(e.why == S15_OSW_WHY_FLUSH && e.emmc == S15_OSW_EMMC_UNCONFIRMED);
	s03_header_decode(emmc.bp[e.target - 1u], &h);
	CHECK(s03_header_active(&h));			/* written, not confirmed */
	emmc.fail_flush_from = 0;
	/* S-03.061: BP1 cannot be selected; nothing is written, and the
	 * final select 0 fails too (S-03.063) */
	emmc.fail_select = true;
	CHECK_EQ(os_write(&env, sizeof image, &e), S15_OSW_FAILED);
	CHECK(e.why == S15_OSW_WHY_SELECT && e.part == 1u && e.emmc == S15_OSW_EMMC_UNTOUCHED &&
	      e.select0_failed && e.target == 0u);
	emmc.fail_select = false;
	free(os);
}

/* S-15.172: the settings file */
static void test_settings_file(void)
{
	static const char on_off[] =
		"waffle-boot settings 1\ndeveloper_mode=on\ncolor=off\n"
		"mtp_target=macos\n";
	struct s15_settings s;
	struct s09_stat st;
	uint8_t buf[80];

	t_case = "settings file";
	fresh();
	s.dev_mode = true;
	s.color = false;
	s09_files_settings_read(&F, &s);
	CHECK(!s.dev_mode && s.color);			/* missing: defaults */
	s.dev_mode = true;
	s.color = false;
	emmc.flushes = 0;
	CHECK(s09_files_settings_save(&F, &s));
	CHECK_EQ(emmc.flushes, 1u);			/* the eMMC's cache, after the rename */
	CHECK_EQ(s09_files_read_file(&F, S15_SETTINGS_PATH, buf, sizeof buf),
		 sizeof on_off - 1u);
	CHECK(!memcmp(buf, on_off, sizeof on_off - 1u));
	CHECK(s09_files_stat(&F, S15_SETTINGS_NEW_PATH, &st) != 0);	/* renamed */
	s.dev_mode = false;
	s.color = true;
	s09_files_settings_read(&F, &s);
	CHECK(s.dev_mode && !s.color);
	/* written again over the old file */
	s.color = true;
	CHECK(s09_files_settings_save(&F, &s));
	s09_files_settings_read(&F, &s);
	CHECK(s.dev_mode && s.color);
	/* a stale .new from a power cut is overwritten */
	CHECK(put(S15_SETTINGS_NEW_PATH, big, 0x300));
	s.dev_mode = false;
	CHECK(s09_files_settings_save(&F, &s));
	CHECK_EQ(s09_files_stat(&F, S15_SETTINGS_PATH, &st), 0);
	CHECK_EQ(st.size, 68u);
	/* a failed flush: renamed, and reported as not saved */
	emmc.fail_flush_from = emmc.flushes + 1u;
	s.dev_mode = true;
	CHECK(!s09_files_settings_save(&F, &s));
	emmc.fail_flush_from = 0;
	s.dev_mode = false;
	s09_files_settings_read(&F, &s);
	CHECK(s.dev_mode);
	/* corrupt: too long, then not text: the defaults */
	CHECK(put(S15_SETTINGS_PATH, big, 513));
	s.dev_mode = true;
	s.color = false;
	s09_files_settings_read(&F, &s);
	CHECK(!s.dev_mode && s.color);
	memcpy(buf, on_off, sizeof on_off - 1u);
	buf[30] = 0x00;
	CHECK(put(S15_SETTINGS_PATH, buf, sizeof on_off - 1u));
	s.dev_mode = true;
	s.color = false;
	s09_files_settings_read(&F, &s);
	CHECK(!s.dev_mode && s.color);
	/* a refused volume: defaults, nothing written */
	CHECK(put(S15_SETTINGS_PATH, (const uint8_t *)on_off,
		  sizeof on_off - 1u));
	CHECK(s09_files_unmount(&F));
	s.dev_mode = true;
	s.color = false;
	s09_files_settings_read(&F, &s);
	CHECK(!s.dev_mode && s.color);
	CHECK(!s09_files_settings_save(&F, &s));
}

/* S-09.008, S-09.060: the slave stops answering mid-transfer */
static void test_absent(void)
{
	struct s09_stat st;
	size_t n = 4u * 0x1000u + 5u;
	unsigned at;

	t_case = "absent read";
	fresh();
	pattern(big, n, 9);
	CHECK(put("long.bin", big, (uint32_t)n));
	lb.die_after = lb.sessions + 8u;		/* inside the chunked read */
	lb.reads = 0;
	memset(back, 0, sizeof back);
	CHECK_EQ(s09_files_read_file(&F, "long.bin", back, sizeof back), 0u);
	CHECK_EQ(lb.reads, 1u);
	CHECK(!memcmp(back, big, 0x1000));		/* a piece came first */
	CHECK_EQ(F.err, S09_ENODEV);
	CHECK(ms.absent);
	at = lb.sessions;
	CHECK_EQ(s09_files_stat(&F, "long.bin", &st), -1);	/* no traffic now */
	CHECK_EQ(F.err, S09_ENODEV);
	CHECK_EQ(lb.sessions, at);
	CHECK(!s09_files_remove_tree(&F, "user"));
	CHECK(!s09_files_erase_os_fat(&F));
	CHECK(!s09_files_format(&F));
	CHECK(!s09_files_volume_refused(&F));		/* absent is not refused */

	t_case = "absent write";
	fresh();
	CHECK(s09_files_mkdirs(&F, "w/"));
	lb.die_after = lb.sessions + 6u;		/* inside the 0x2B pieces */
	CHECK(!put("w.bin", big, (uint32_t)n));
	CHECK(lb.writes >= 1u && lb.writes < 5u);
	CHECK_EQ(F.err, S09_ENODEV);
	CHECK(ms.absent);

	t_case = "absent copy";
	fresh();
	CHECK(put("src.bin", big, (uint32_t)n));
	lb.die_after = lb.sessions + 14u;
	lb.reads = lb.writes = 0;
	CHECK(!s09_files_copy(&F, "src.bin", "dst/dst.bin"));
	CHECK(lb.reads >= 1u && lb.writes >= 1u);
	CHECK_EQ(F.err, S09_ENODEV);

	t_case = "absent tree";
	fresh();
	CHECK(put("t/a/b/c", big, 10));
	CHECK(put("t/a/d", big, 10));
	lb.die_after = lb.sessions + 9u;
	CHECK(!s09_files_remove_tree(&F, "t"));
	CHECK_EQ(F.err, S09_ENODEV);

	t_case = "absent os write";
	fresh();
	{
		struct s03_emmc_ops cops;
		struct s03_env env = { &cops, ref_crc32, B_SIZE, back, sizeof back, 0 };
		struct s15_osw_end e;

		s09_files_emmc_ops(&F, &cops);
		pattern(big, 0x5000, 5);
		CHECK(put("/yaffs2/os.ldr", big, 0x5000));
		lb.die_after = lb.sessions + 40u;	/* reading it, or writing */
		CHECK(s09_files_os_write(&F, &env, image, sizeof image, NULL, &e) != S15_OSW_OK);
		CHECK(e.result != S15_OSW_OK && e.why != S15_OSW_WHY_NONE);
	}
}

int main(void)
{
	vol = calloc(1, VOL_LEN);
	if (vol == NULL)
		return 2;
	for (unsigned pass = 0; pass < 5u; pass++) {
		/* as it comes up, then on rungs 2 and 4 */
		use_rung = pass == 0u ? -1 : pass == 1u ? 0 :
			   pass == 3u ? (int)S09_RUNG_CHECKED_FAST : (int)S09_RUNG_TOP;
		/* files the last pass's dead-bus tests left open on the volume */
		for (int h = 0; h < 200; h++)
			(void)yaffs_close(h);
		test_requests();
		test_long_files();
		test_close_sync();
		test_staging();
		test_stage_writes();
		test_stage_dirs();
		test_stage_running();
		if (pass == 0u) {
			test_link_rungs();
			test_link_fallback();
			test_link_repeat();
			test_link_not_begun();
			test_link_heard_plain();
			test_link_slave_rules();
			test_link_lowered();
			test_link_engine();
			test_link_known_answer();
			test_link_push();
			test_link_restart();
			test_link_bounded();
			test_link_two_memories();
			test_link_gate();
			test_link_status();
			test_link_reply_end();
			test_link_record();
			test_link_bench();
			test_link_feed_fails();
			test_link_late_offer();
			test_link_blank_reply();
			test_link_all_fail();
			test_link_tests_heard();
			test_link_sweep();
		}
		test_check_zip();
		test_menus();
		test_format();
		test_os_write();
		test_settings_file();
		test_absent();
	}
	wire_s09_release(&S);
	wire_yaffs_detach(&Y);
	free(vol);
	return t_finish("test_s09_files");
}
