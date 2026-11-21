/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A boot's milestones and their report (see waffle/boot_time.h). */
#include "waffle/boot_time.h"

#include <stdarg.h>

#include "waffle/bytes.h"

#define CPLB_VALID_BIT 1u	/* PRM: bit 0 of a CPLB data word */

/* The milestones' names in their order, packed into one string. */
#define NAMES(X)                                                                          \
	X("init entry") X("init clocks") X("init ddr") X("front entry") X("front hand-off")   \
	X("main") X("bring-up") X("ddr check") X("metadata") X("psoc open")                  \
	X("battery wait start") X("battery wait end") X("keys") X("panel light sent")        \
	X("slot probed") X("slot read") X("slot checked") X("slot found") X("stream end")     \
	X("slave ready")                                                                     \
	X("bridge linked") X("slave done") X("pre-load") X("os copy start")                  \
	X("os copy last read") X("os staged") X("os checked") X("release reply")             \
	X("held mode")                                                                       \
	X("test os entry") X("stream ready wait") X("stream ready") X("stream opened")       \
	X("stream first header") X("stream last byte") X("hand-over begun")                  \
	X("pmic step done")                                                                  \
	X("boot report queued") X("os crc done") X("release sent")                           \
	X("slave server stopped")                                                            \
	X("slave os started") X("teardown begun") X("kernel call") X("copy link up")         \
	X("slave teardown begun") X("slave kernel call") X("slave kernel returned")          \
	X("slave jump")                                                                      \
	X("loading line") X("mount sent") X("mount reply") X("settings read")                \
	X("usb started")                                                                     \
	X("nordic greeting done") X("psoc info done") X("nordic info done")                  \
	X("first menu frame")                                                                \
	X("first menu frame ended")
#define NAME_TEXT(s) s "\0"
#define NAME_ONE(s) + 1u

static const char names[] = NAMES(NAME_TEXT);
_Static_assert(0u NAMES(NAME_ONE) == BOOT_TIME_COUNT, "a name for each milestone");

/* The name after p in a packed list. */
static const char *next_name(const char *p)
{
	while (*p++ != '\0')
		;
	return p;
}

/* Name id of a packed list: its names one after another, each ended by
 * 0x00. */
static const char *name_in(const char *p, unsigned id)
{
	for (; id != 0u; id--)
		p = next_name(p);
	return p;
}

static bool reached(const struct boot_time *t, unsigned id)
{
	return id < BOOT_TIME_COUNT && (t->set >> id & 1u) != 0u;
}

/* v / div with n decimals, div being 10^n (n at most 3). The remainder is
 * taken from the quotient: a 64-bit remainder would link libgcc's own. */
static void text_fixed(struct s14_text *out, uint64_t v, uint32_t div, unsigned n)
{
	const uint64_t whole = v / div;
	uint32_t frac = (uint32_t)(v - whole * div);
	char d[4];
	unsigned i;

	s14_text_dec(out, (uint32_t)whole);
	d[0] = '.';
	for (i = n; i > 0u; i--) {
		d[i] = (char)('0' + frac % 10u);
		frac /= 10u;
	}
	s14_text_bytes(out, (const uint8_t *)d, n + 1u);
}

/* Microseconds as milliseconds with three decimals. */
static void text_ms(struct s14_text *out, uint64_t us)
{
	text_fixed(out, us, 1000u, 3u);
	s14_text_str(out, " ms");
}

/* Hundredths of a microsecond as microseconds with two decimals, no unit. */
static void text_hundredths(struct s14_text *out, uint64_t hundredths)
{
	text_fixed(out, hundredths, 100u, 2u);
}

/* Cycles as hundredths of a microsecond. */
static __attribute__((noinline)) uint64_t c2h(uint64_t cycles, uint32_t cpu)
{
	return cycles * 100u / cpu;
}

/*
 * Text from a pattern, one call for a line's pieces: "%d" a uint32_t in
 * decimal, "%m" a uint64_t of microseconds as milliseconds (text_ms()), "%h"
 * a uint64_t of hundredths of a microsecond (text_hundredths()), "%s" a
 * string, "%x" and a digit a uint32_t in hex of at least that many digits,
 * "%%" a percent sign; the rest as it stands.
 */
static void put(struct s14_text *out, const char *f, ...)
{
	va_list ap;

	va_start(ap, f);
	while (*f != '\0') {
		size_t n = 0;

		while (f[n] != '\0' && f[n] != '%')
			n++;
		s14_text_bytes(out, (const uint8_t *)f, n);
		f += n;
		if (*f == '\0')
			break;
		f += 2;
		switch (f[-1]) {
		case 'd':
			s14_text_dec(out, va_arg(ap, uint32_t));
			break;
		case 'm':
			text_ms(out, va_arg(ap, uint64_t));
			break;
		case 'h':
			text_hundredths(out, va_arg(ap, uint64_t));
			break;
		case 's':
			s14_text_str(out, va_arg(ap, const char *));
			break;
		case 'x':
			s14_text_hex(out, va_arg(ap, uint32_t), (unsigned)(*f++ - '0'));
			break;
		default:	/* "%%" */
			s14_text_bytes(out, (const uint8_t *)f - 1, 1u);
			break;
		}
	}
	va_end(ap);
}

/*
 * The copy's quad data replies (S-09.081), from copy_qstate: its state
 * (0 not tried, 1 not taken up, 2 on, 3 dropped, 4 ended with the link),
 * why (struct s09_qdata_rec's codes) and the test's first failure.
 */
static void copy_quad(struct s14_text *out, const struct boot_time *t)
{
	const uint32_t state = t->copy_qstate & 0xFFu, why = t->copy_qstate >> 8 & 0xFFu;

	if (state == 0u) {
		s14_text_str(out, ", replies single I/O");
	} else if (state == 1u) {
		put(out, ", replies single I/O, quad data not taken up (why %d", why);
		if (why == 2u)
			put(out, ", test 0x%x2", t->copy_qstate >> 16 & 0xFFu);
		s14_text_str(out, ")");
	} else {
		put(out, ", sector data in quad I/O: %d sessions, %d bytes", t->copy_quad,
		    t->copy_qbytes);
		if (state == 3u)
			put(out, ", then single I/O (why %d)", why);
	}
	if (t->flags & BOOT_TIME_COPY_REREAD)
		s14_text_str(out, ", its CRC failed: read again in single I/O");
}

void boot_time_watch_byte(struct boot_time_watch *w, uint32_t sent)
{
	w->sent = sent;
	w->active = true;
	w->first = true;
	w->rose = false;
	w->held = false;
	w->rose_at = 0;
}

static uint32_t most(uint32_t max, uint64_t v)
{
	if (v > UINT32_MAX)
		v = UINT32_MAX;
	return v > max ? (uint32_t)v : max;
}

void boot_time_watch_sample(struct boot_time *t, struct boot_time_watch *w, bool high,
			    uint64_t since_byte, bool in_window)
{
	if (!w->active)
		return;
	if (high && !w->rose && in_window) {
		w->rose = true;
		w->held = true;
		w->rose_at = since_byte;
		if (w->first) {
			if (t->line_high_first < BOOT_TIME_FIRST)
				t->line_high_at[t->line_high_first] = w->sent;
			t->line_high_first++;
		} else {
			if (t->line_late < BOOT_TIME_FIRST)
				t->line_late_at[t->line_late] = w->sent;
			t->line_late++;
			t->line_late_max = most(t->line_late_max, since_byte);
		}
		if (t->line_window != 0u) {
			uint64_t step = since_byte * BOOT_TIME_RAISE_STEPS / t->line_window;

			if (step >= BOOT_TIME_RAISE_STEPS)
				step = BOOT_TIME_RAISE_STEPS - 1u;
			t->line_raise[step]++;
		}
	} else if (!high && w->held) {
		w->held = false;
		t->line_hold_max = most(t->line_hold_max, since_byte - w->rose_at);
	}
	w->first = false;
}

void boot_time_walk_head(struct boot_time *t, struct boot_time_walk *w, uint8_t b)
{
	uint32_t code, count;

	if (w->have == 0u)
		t->split.blocks++;
	w->head[w->have++] = b;
	if (w->have < sizeof w->head)
		return;
	w->have = 0;
	code = wb_get_le32(w->head);
	count = wb_get_le32(w->head + 8);
	/* S-09.005: a fill, or a count of 0, has no payload */
	if ((code & 0x0100u) || count == 0u) {
		t->split.fills++;
		w->left = 0;
	} else {
		w->left = count;
	}
}

void boot_time_walk(struct boot_time *t, struct boot_time_walk *w, const uint8_t *d,
		    uint32_t n)
{
	while (n != 0u) {
		if (w->left != 0u) {
			const uint32_t k = w->left < n ? w->left : n;

			t->split.bytes += k;
			w->left -= k;
			d += k;
			n -= k;
		} else {
			boot_time_walk_byte(t, w, *d++);
			n--;
		}
	}
}

void boot_time_gap(struct boot_time *t, uint64_t cycles, uint32_t at)
{
	struct boot_time_gap *g = t->split.gap;
	const uint32_t c = cycles > UINT32_MAX ? UINT32_MAX : (uint32_t)cycles;
	unsigned i = BOOT_TIME_GAPS;

	if (c == 0u || c <= g[BOOT_TIME_GAPS - 1u].cycles)
		return;
	while (i > 0u && g[i - 1u].cycles < c) {
		if (i < BOOT_TIME_GAPS)
			g[i] = g[i - 1u];
		i--;
	}
	g[i].cycles = c;
	g[i].at = at;
	g[i].block = t->split.blocks != 0u ? t->split.blocks - 1u : BOOT_TIME_NO_BLOCK;
}

/* From one count to a later one; 0 when it is not later. */
static __attribute__((noinline)) uint64_t span_us(const struct boot_time *t, unsigned from, unsigned to,
			uint32_t cycles_per_us)
{
	return t->at[to] > t->at[from] ? (t->at[to] - t->at[from]) / cycles_per_us : 0u;
}

/* Whether milestone a's line comes before b's: by stamp, then by number. */
static bool before(const struct boot_time *t, unsigned a, unsigned b)
{
	if (t->at[a] != t->at[b])
		return t->at[a] < t->at[b];
	return a < b;
}

/* The milestone of the report's line k, BOOT_TIME_COUNT when there are
 * fewer: the one with k reached milestones before it. */
static unsigned nth(const struct boot_time *t, unsigned k)
{
	unsigned id, other;

	for (id = 0; id < BOOT_TIME_COUNT; id++) {
		unsigned rank = 0;

		if (!reached(t, id))
			continue;
		for (other = 0; other < BOOT_TIME_COUNT; other++)
			if (other != id && reached(t, other) && before(t, other, id))
				rank++;
		if (rank == k)
			return id;
	}
	return BOOT_TIME_COUNT;
}

/* The milestone at which the switch went on after a charge wait, or
 * BOOT_TIME_COUNT without one. */
static unsigned switch_on(const struct boot_time *t)
{
	if ((t->flags & BOOT_TIME_CHARGE_WAITED) && reached(t, BOOT_TIME_LOGO))
		return BOOT_TIME_LOGO;
	if ((t->flags & BOOT_TIME_BATTERY_RAN) && reached(t, BOOT_TIME_BATTERY_END))
		return BOOT_TIME_BATTERY_END;
	return BOOT_TIME_COUNT;
}

static bool milestone(const struct boot_time *t, unsigned k, uint32_t cpu,
		      struct s14_text *out)
{
	const unsigned id = nth(t, k), on = switch_on(t);
	const unsigned prev = k != 0u ? nth(t, k - 1u) : BOOT_TIME_COUNT;

	if (id >= BOOT_TIME_COUNT)
		return false;
	put(out, "%d%d %s", (uint32_t)(id / 10u), (uint32_t)(id % 10u), name_in(names, id));
	if (id < BOOT_TIME_INIT_DDR) {
		/* The core still on its reset clock: the count as it stands. */
		put(out, ": counter %x8%x8", (uint32_t)(t->at[id] >> 32), (uint32_t)t->at[id]);
		return true;
	}
	put(out, ": %m", t->at[id] / cpu);
	/* nothing is counted from INIT's entry, taken on the reset clock */
	if (prev < BOOT_TIME_COUNT && prev != BOOT_TIME_INIT_ENTRY)
		put(out, ", +%m", span_us(t, prev, id, cpu));
	if (on < BOOT_TIME_COUNT && !before(t, id, on))
		put(out, ", since on %m", span_us(t, on, id, cpu));
	return true;
}

/* Cycles as microseconds, for "%m". */
static __attribute__((noinline)) uint64_t c2us(uint64_t cycles, uint32_t cpu)
{
	return cycles / cpu;
}

/* The stream's split, its blocks, its bytes, its longest waits (two lines)
 * and the measuring's own cost: lines 0 to 6. */
static __attribute__((noinline)) bool stream_line(const struct boot_time *t, unsigned k,
						  uint32_t cpu, struct s14_text *out)
{
	const struct boot_time_split *sp = &t->split;
	const bool fc = (t->flags & BOOT_TIME_LINK_FC) != 0u;

	if (!reached(t, BOOT_TIME_STREAM_END) || !reached(t, BOOT_TIME_STREAM_WAIT) ||
	    !reached(t, BOOT_TIME_STREAM_READY))
		return false;
	switch (k) {
	case 0: {
		const uint64_t whole = t->at[BOOT_TIME_STREAM_END] - t->at[BOOT_TIME_STREAM_WAIT];
		const uint64_t open = t->at[BOOT_TIME_STREAM_READY] - t->at[BOOT_TIME_STREAM_WAIT];

		put(out, "stream split: %m in all, ready wait %m, transfers %m, %s %m, samples %m",
		    c2us(whole, cpu), c2us(open, cpu), c2us(sp->xfer, cpu), fc ? "sends" : "windows",
		    c2us(fc ? sp->send : sp->window, cpu), c2us(sp->sample, cpu));
		return true;
	}
	case 1: {
		const uint64_t whole = t->at[BOOT_TIME_STREAM_END] - t->at[BOOT_TIME_STREAM_WAIT];
		/* paced by the controller, the waits are inside the sends */
		const uint64_t parts = t->at[BOOT_TIME_STREAM_READY] - t->at[BOOT_TIME_STREAM_WAIT] +
				       sp->xfer + sp->window + sp->sample + sp->select + sp->send +
				       (fc ? 0u : t->stream_wait);

		put(out, "stream split: %s %m, chip select %m, the rest %s%m",
		    fc ? "beyond the wire in the sends" : "waits on a high line",
		    c2us(t->stream_wait, cpu), c2us(sp->select, cpu), parts <= whole ? "" : "-",
		    c2us(parts <= whole ? whole - parts : parts - whole, cpu));
		return true;
	}
	case 2:
		put(out, "stream blocks: %d blocks, %d fill or header-only, %d sessions, %d bytes",
		    sp->blocks, sp->fills, sp->sessions, sp->bytes);
		return true;
	case 3: {
		const uint64_t wire = (uint64_t)sp->wire_byte * sp->xfers;
		uint64_t avg;

		if (fc) {
			put(out, "stream sends: %d sessions in %m, the longest %m; on the wire alone %m",
			    sp->sessions, c2us(sp->send, cpu), c2us(sp->send_max, cpu),
			    c2us((uint64_t)sp->wire_byte * sp->bytes, cpu));
			return true;
		}
		if (sp->xfers == 0u)
			return false;
		avg = sp->xfer / sp->xfers;
		put(out, "stream bytes: %d transfers less a counter read, avg %h us, min %h us, "
			 "max %h us; on the wire alone %m, in this build %m",
		    sp->xfers, c2h(avg > sp->read ? avg - sp->read : 0u, cpu),
		    c2h(sp->xfer_min > sp->read ? sp->xfer_min - sp->read : 0u, cpu),
		    c2h(sp->xfer_max > sp->read ? sp->xfer_max - sp->read : 0u, cpu),
		    c2us(wire, cpu), c2us(sp->xfer, cpu));
		return true;
	}
	case 4:
	case 5: {
		const unsigned from = (k - 4u) * (BOOT_TIME_GAPS / 2u);
		unsigned i;

		if (sp->gap[from].cycles == 0u)
			return false;
		s14_text_str(out, k == 4u ? "stream longest waits:" : "stream longest waits, more:");
		for (i = from; i < from + BOOT_TIME_GAPS / 2u && sp->gap[i].cycles != 0u; i++) {
			put(out, "%s%m at byte %d", i == from ? " " : "; ", c2us(sp->gap[i].cycles, cpu),
			    sp->gap[i].at);
			if (sp->gap[i].block == BOOT_TIME_NO_BLOCK)
				s14_text_str(out, ", no block yet");
			else
				put(out, ", block %d", sp->gap[i].block);
		}
		return true;
	}
	case 6:
		put(out, "stream measuring: %d counter reads added, %d cycles a read, at most %m",
		    sp->reads, sp->read, c2us((uint64_t)sp->reads * sp->read, cpu));
		return true;
	default:
		return false;
	}
}

/* Fed by DMA, in place of the lines about the line after a byte: the parts
 * and their guards, then the fall-back. The causes are S09_PART_*'s. */
static bool dma_line(const struct boot_time *t, bool fell, struct s14_text *out)
{
	static const char cause[] =
		"none\0the slave's hold\0a DMA error\0the controller stopped with the line low\0"
		"no end of the transmit with the line low\0the driver refused the part";

	if (!fell) {
		put(out, "stream by DMA: %d parts, guard %d us, %m in the guards, %d parts began with "
			 "the ready line high; FCS set in %d of %d samples, %d parts",
		    t->dma_parts, t->dma_guard, (uint64_t)t->dma_guard_us, t->dma_high,
		    t->dma_fcs_held, t->dma_fcs_samples, t->dma_fcs_parts);
		if (t->dma_fcs_first != 0u)
			put(out, ", the first at byte %d", t->dma_fcs_first - 1u);
		return true;
	}
	if (t->dma_fell == 0u) {
		s14_text_str(out, t->dma_held ? "stream fall-back: none; the slave held the line past T_STREAM" :
						"stream fall-back: none");
		return true;
	}
	put(out, "stream fall-back at byte %d of session %d: %s, then %d bytes one at a time",
	    t->dma_fell_at, t->dma_fell_session,
	    t->dma_fell < 6u ? name_in(cause, t->dma_fell) : "?", t->dma_slow_bytes);
	if (t->dma_held)
		s14_text_str(out, "; the slave held the line past T_STREAM");
	return true;
}

/* S-09.075's rungs, slowest first. */
static void text_rung(struct s14_text *out, uint32_t r)
{
	static const char wire[] =
		"plain\0checked\0checked at 50 MHz\0quad\0quad at 50 MHz";

	s14_text_dec(out, r);
	if (r < 5u)
		put(out, " (%s)", name_in(wire, r));
}

/* The power at the start: the PSoC's state and its battery report. */
static bool power_line(const struct boot_time *t, struct s14_text *out)
{
	const uint32_t st = t->power_state, status = t->battery & 0xFFu;

	if (!(t->flags & (BOOT_TIME_POWER_KNOWN | BOOT_TIME_BATTERY_KNOWN)))
		return false;
	s14_text_str(out, "power at start: ");
	if (t->flags & BOOT_TIME_POWER_KNOWN)
		put(out, "state 0x%x2%s", st, st == 0x10u ? " (on)" : st == 0x14u ? " (off)" :
					     st == 0x16u ? " (charging, switch off)" : "");
	else
		s14_text_str(out, "no state reported");
	if (!(t->flags & BOOT_TIME_BATTERY_KNOWN)) {
		s14_text_str(out, ", no battery report");
		return true;
	}
	put(out, ", battery status 0x%x2", status);
	if (status == 0u) {
		s14_text_str(out, " (none)");
		return true;
	}
	/* the report's current is a signed 16-bit value, negative leaving the
	 * battery */
	put(out, "%s%d %%, %d mV, %s%d mA", status == 2u ? " (charging), " : ", ",
	    t->battery >> 8 & 0xFFu, t->battery >> 16, (t->battery_ma & 0x8000u) ? "-" : "",
	    (t->battery_ma & 0x8000u) ? 0x10000u - (t->battery_ma & 0xFFFFu) :
					t->battery_ma & 0xFFFFu);
	/* switched on, not charging, full: a battery start and a USB start
	 * whose charger has stopped report the same */
	if ((t->flags & BOOT_TIME_POWER_KNOWN) && st == 0x10u && status == 1u &&
	    (t->battery >> 8 & 0xFFu) == 100u)
		s14_text_str(out, "; battery or USB at a full charge, not told apart");
	return true;
}

/* A span of the start into the OS, when both its ends were reached; the
 * first of a list in parentheses (*first false and *open true) opens them. */
static void span_in(const struct boot_time *t, struct s14_text *out, const char *name,
		    unsigned from, unsigned to, uint32_t cpu, bool *first, bool *open)
{
	if (!reached(t, from) || !reached(t, to))
		return;
	put(out, "%s%s %m", *open ? " (" : *first ? " " : ", ", name, span_us(t, from, to, cpu));
	*open = false;
	*first = false;
}

static void start_span(const struct boot_time *t, struct s14_text *out, const char *name,
		       unsigned from, unsigned to, uint32_t cpu, bool *first)
{
	bool open = false;

	span_in(t, out, name, from, to, cpu, first, &open);
}

/* What a span's end adds to the load line, when it was reached: the early
 * logo kept, the quad reply test, the CRC's way. */
static void load_note(const struct boot_time *t, unsigned to, struct s14_text *out)
{
	if (!reached(t, to))
		return;
	if (to == BOOT_TIME_BOOT_REPORT && (t->flags & BOOT_TIME_LOGO_KEPT))
		s14_text_str(out, " (the early logo kept)");
	if (to == BOOT_TIME_LINK_UP && t->copy_qtest_us != 0u)
		put(out, " (quad reply test %m)", (uint64_t)t->copy_qtest_us);
	if (to == BOOT_TIME_COPY_CRC && (t->flags & BOOT_TIME_COPY_CRC_HW))
		s14_text_str(out, (t->flags & BOOT_TIME_COPY_CRC_AGAIN) ?
			     " (peripheral, then software)" : " (peripheral)");
}

struct span {
	uint8_t from, to;
};

/* A span list's names, packed (name_in()), and its ends. */
#define SPAN_NAME(name, from, to) name "\0"
#define SPAN_ENDS(name, from, to) { from, to },

/* A list of spans, each with its note, their names from `name` on; with
 * `open`, the first one shown opens parentheses. Returns whether they are
 * still to open (none shown). */
static bool spans(const struct boot_time *t, struct s14_text *out, const struct span *s,
		  const char *name, unsigned n, uint32_t cpu, bool *first, bool open)
{
	unsigned i;

	for (i = 0; i < n; i++, name = next_name(name)) {
		span_in(t, out, name, s[i].from, s[i].to, cpu, first, &open);
		load_note(t, s[i].to, out);
	}
	return open;
}

/* The start into the OS in spans, from the hand-over's start to the kernel
 * call, in two lines. First the pre-load in its steps, the link's raise
 * with the quad reply test in it, the copy's reads,
 * its CRC, the return to the user area and the image check; then the log's
 * tidy, the release (the slave's own staging, CRC and check, with its
 * request and reply), the slave's server stop, the slave's OS start (with
 * its teardown, kernel load, entry and OS start when its beacons came), the
 * master's teardown and the kernel's load to the OS's entry. */
#define LOAD_SPANS(X)                                                \
	X("pmic", BOOT_TIME_HANDOFF, BOOT_TIME_PMIC)                     \
	X("logo and report", BOOT_TIME_PMIC, BOOT_TIME_BOOT_REPORT)      \
	X("drain", BOOT_TIME_BOOT_REPORT, BOOT_TIME_PRE_LOAD)            \
	X("link up", BOOT_TIME_PRE_LOAD, BOOT_TIME_LINK_UP)              \
	X("reads", BOOT_TIME_COPY_START, BOOT_TIME_COPY_LAST)            \
	X("crc", BOOT_TIME_COPY_LAST, BOOT_TIME_COPY_CRC)                \
	X("user area", BOOT_TIME_COPY_CRC, BOOT_TIME_STAGED)             \
	X("check", BOOT_TIME_STAGED, BOOT_TIME_CHECKED)

/* The slave's OS start's parts (in parentheses) from the fifth, then the
 * master's from the ninth. */
#define START_SPANS(X)                                               \
	X("log", BOOT_TIME_CHECKED, BOOT_TIME_RELEASE_SENT)              \
	X("release", BOOT_TIME_RELEASE_SENT, BOOT_TIME_RELEASED)         \
	X("server stop", BOOT_TIME_RELEASED, BOOT_TIME_SERVER_STOP)      \
	X("slave os", BOOT_TIME_SERVER_STOP, BOOT_TIME_SLAVE_OS)         \
	X("its teardown", BOOT_TIME_SERVER_STOP, BOOT_TIME_SLAVE_KERNEL) \
	X("kernel load", BOOT_TIME_SLAVE_KERNEL, BOOT_TIME_SLAVE_RETURNED) \
	X("entry", BOOT_TIME_SLAVE_RETURNED, BOOT_TIME_SLAVE_JUMP)       \
	X("os to PA4 high", BOOT_TIME_SLAVE_JUMP, BOOT_TIME_SLAVE_OS)    \
	X("teardown", BOOT_TIME_TEARDOWN, BOOT_TIME_KERNEL_CALL)         \
	X("kernel to os", BOOT_TIME_KERNEL_CALL, BOOT_TIME_OS_ENTRY)

static const char load_names[] = LOAD_SPANS(SPAN_NAME);
static const struct span load_spans[] = { LOAD_SPANS(SPAN_ENDS) };
static const char start_names[] = START_SPANS(SPAN_NAME);
static const struct span start_spans[] = { START_SPANS(SPAN_ENDS) };

static bool load_line(const struct boot_time *t, uint32_t cpu, struct s14_text *out)
{
	bool first = true;

	if (!reached(t, BOOT_TIME_HANDOFF))
		return false;
	s14_text_str(out, "os load split:");
	(void)spans(t, out, load_spans, load_names, sizeof load_spans / sizeof load_spans[0], cpu,
		    &first, false);
	return true;
}

static bool start_line(const struct boot_time *t, uint32_t cpu, struct s14_text *out)
{
	bool first = true, in = false;

	if (!reached(t, BOOT_TIME_HANDOFF))
		return false;
	s14_text_str(out, "os start split:");
	(void)spans(t, out, start_spans, start_names, 4u, cpu, &first, false);
	if (reached(t, BOOT_TIME_SLAVE_OS) &&
	    !spans(t, out, start_spans + 4, name_in(start_names, 4u), 4u, cpu, &in, true))
		s14_text_str(out, ")");
	(void)spans(t, out, start_spans + 8, name_in(start_names, 8u), 2u, cpu, &first, false);
	start_span(t, out, "in all", BOOT_TIME_HANDOFF,
		   reached(t, BOOT_TIME_OS_ENTRY) ? BOOT_TIME_OS_ENTRY :
		   reached(t, BOOT_TIME_KERNEL_CALL) ? BOOT_TIME_KERNEL_CALL : BOOT_TIME_SLAVE_OS,
		   cpu, &first);
	if (t->flags & BOOT_TIME_PA6_POLLED)
		s14_text_str(out, "; PA6 polled in the line window");
	return true;
}

/*
 * Held mode's start to its first frame in spans, each from the step reached
 * before it, held mode's entry first, in two lines: the line under the
 * logo, the set-up to the mount request, the mount (its result when it
 * failed), the settings and USB; then the Nordic's greeting with its tries,
 * the PSoC's and the Nordic's info, the first menu frame's wait and its
 * transfer, and the whole from held mode's entry.
 */
#define MENU_STEPS(X)                                   \
	X("line", BOOT_TIME_MENU_LINE)                      \
	X("to mount", BOOT_TIME_MOUNT_SENT)                 \
	X("mount", BOOT_TIME_MOUNT_REPLY)                   \
	X("settings", BOOT_TIME_SETTINGS)                   \
	X("usb", BOOT_TIME_USB)                             \
	X("nordic greeting", BOOT_TIME_NORDIC_GREETING)     \
	X("psoc info", BOOT_TIME_PSOC_INFO)                 \
	X("nordic info", BOOT_TIME_NORDIC_INFO)             \
	X("to the frame", BOOT_TIME_MENU_FRAME)             \
	X("frame", BOOT_TIME_MENU_SHOWN)
#define STEP_NAME(name, id) name "\0"
#define STEP_ID(name, id) id,
static const char menu_names[] = MENU_STEPS(STEP_NAME);
static const uint8_t menu_steps[] = { MENU_STEPS(STEP_ID) };
#define MENU_FIRST_LINE 5u	/* the steps on the first line */

static bool menu_line(const struct boot_time *t, bool second, uint32_t cpu,
		      struct s14_text *out)
{
	const unsigned n = sizeof menu_steps;
	const char *name = menu_names;
	unsigned i, from = BOOT_TIME_HELD, last = BOOT_TIME_HELD;
	bool first = true;

	if (!reached(t, BOOT_TIME_HELD))
		return false;
	s14_text_str(out, second ? "boot menu split, links and frame:" : "boot menu split:");
	for (i = 0; i < n; i++, name = next_name(name)) {
		const unsigned id = menu_steps[i];

		if (!reached(t, id))
			continue;
		if (second == (i >= MENU_FIRST_LINE)) {
			start_span(t, out, name, from, id, cpu, &first);
			if (id == BOOT_TIME_MENU_LINE)
				s14_text_str(out, (t->flags & BOOT_TIME_LINE_DRAWN) ?
					     " (drawn)" : " (not drawn)");
			if (id == BOOT_TIME_MOUNT_REPLY && (int32_t)t->mount_rc < 0)
				put(out, " (failed, error %d)", t->mount_err);
			if (id == BOOT_TIME_NORDIC_GREETING)
				put(out, " (%d tries, %s)", t->greet_tries,
				    (t->flags & BOOT_TIME_NORDIC_ANSWERED) ? "answered" :
									    "not answered");
		}
		from = id;
		last = id;
	}
	if (!second && !reached(t, BOOT_TIME_MOUNT_SENT) && reached(t, BOOT_TIME_SETTINGS))
		s14_text_str(out, first ? " no mount" : ", no mount");
	if (second && last != BOOT_TIME_HELD)
		start_span(t, out, "in all", BOOT_TIME_HELD, last, cpu, &first);
	return true;
}

/* min/mean/max in us: the shortest and longest in cycles at cpu, the mean
 * from a total in us over n. */
static void text_spread(struct s14_text *out, uint32_t min, uint64_t total_us, uint32_t n,
			uint32_t max, uint32_t cpu)
{
	put(out, "%h/%h/%h us", c2h(min, cpu), n != 0u ? total_us * 100u / n : 0u, c2h(max, cpu));
}

/* The slave's mount (struct boot_time_mount), in two lines: its time, its
 * chunk reads and their time; then a read's phases and the rest. */
static bool mount_line(const struct boot_time *t, bool phases, struct s14_text *out)
{
	static const char names_ph[] = "cache \0set-up \0command \0data \0stop ";
	const struct boot_time_mount *mt = &t->mount;
	const uint32_t cpu = mt->cpu != 0u ? mt->cpu : 1u;
	uint64_t sum = 0;
	unsigned i;

	if (!(t->flags & BOOT_TIME_MOUNT_READ)) {
		if (phases || !reached(t, BOOT_TIME_MOUNT_REPLY))
			return false;
		s14_text_str(out, "slave mount: no figures from the slave");
		return true;
	}
	if (!phases) {
		put(out, "slave mount: %m in all, %d chunk reads in %m, a read min/mean/max ",
		    (uint64_t)mt->mount_us, mt->reads, (uint64_t)mt->read_us);
		text_spread(out, mt->read_min, mt->read_us, mt->reads, mt->read_max, cpu);
		put(out, ", %d failed; %d tags only in %m, %d whole, %d fell back", mt->failed,
		    mt->tags_reads, (uint64_t)mt->tags_us, mt->whole_reads, mt->fell_back);
		return true;
	}
	if (mt->reads == 0u)
		return false;
	s14_text_str(out, "slave mount, a read's phases min/mean/max:");
	for (i = 0; i < BOOT_TIME_PHASES; i++) {
		put(out, "%s%s", i == 0u ? " " : ", ", name_in(names_ph, i));
		text_spread(out, mt->phase_min[i], mt->phase_us[i], mt->reads, mt->phase_max[i],
			    cpu);
		sum += mt->phase_us[i];
	}
	put(out, ", the rest mean %h us",
	    mt->read_us > sum ? (mt->read_us - sum) * 100u / mt->reads : 0u);
	return true;
}

/* A CPLB data word's caching (BOOT_TIME_CPLB_DFLT: the default's). */
static void text_cplb(struct s14_text *out, uint32_t w, bool code)
{
	static const char data[] =
		"not cached\0write-back\0props 2\0props 3\0I/O\0write-through\0props 6\0props 7";

	const char *how;

	if (code)
		how = (w >> 12 & 3u) != 0u ? "cached" : "not cached";
	else if (!(w & CPLB_VALID_BIT))
		how = "invalid";
	else
		how = name_in(data, w >> 12 & 7u);
	put(out, "%x8 (%s%s)", w & ~BOOT_TIME_CPLB_DFLT, how,
	    (w & BOOT_TIME_CPLB_DFLT) ? ", default" : "");
}

/* The slave's mount, how it went (S-12.061): its way, the restore, the
 * stale check and a scan after a doubt. */
static bool mount_how(const struct boot_time *t, struct s14_text *out)
{
	static const char ways[] =
		"scanned, no checkpoint read\0restored\0scanned, no checkpoint\0"
		"scanned, the checkpoint not valid\0scanned, the checkpoint stale\0"
		"scanned, the checkpoint unreadable\0scanned for a write";
	const struct boot_time_mount *mt = &t->mount;

	if (!(t->flags & BOOT_TIME_MOUNT_READ))
		return false;
	put(out, "slave mount, how: %s, %d objects; restore %m (%d reads), stale check %m (%d reads), "
		 "scan %m (%d reads)",
	    mt->way < 7u ? name_in(ways, mt->way) : "way ?", mt->objects,
	    (uint64_t)mt->restore_us, mt->restore_reads, (uint64_t)mt->check_us, mt->check_reads,
	    (uint64_t)mt->scan_us, mt->scan_reads);
	return true;
}

/* The slave's mount outside its reads, and its caches at the mount's start. */
static bool mount_more(const struct boot_time *t, bool caches, struct s14_text *out)
{
	const struct boot_time_mount *mt = &t->mount;
	const uint32_t outside = mt->mount_us > mt->read_us ? mt->mount_us - mt->read_us : 0u;
	const uint32_t known = mt->alloc_us + mt->glue_us;

	if (!(t->flags & BOOT_TIME_MOUNT_READ))
		return false;
	if (!caches) {
		put(out, "slave mount, outside the reads: %m; %d allocations and %d frees in %m, the "
			 "tags reads' own %m, the rest (yaffs2's scan) %m",
		    (uint64_t)outside, mt->allocs, mt->frees, (uint64_t)mt->alloc_us,
		    (uint64_t)mt->glue_us, (uint64_t)(outside > known ? outside - known : 0u));
		return true;
	}
	put(out, "slave mount, caches: L1DM_DCTL %x8%s, L1IM_ICTL %x8%s; heap %x8-%x8 ", mt->dctl,
	    (mt->dctl & 2u) && (mt->dctl & 0xCu) ? " (on)" : " (off)", mt->ictl,
	    (mt->ictl & 2u) && (mt->ictl & 4u) ? " (on)" : " (off)", mt->heap_lo, mt->heap_hi);
	text_cplb(out, mt->heap_cplb, false);
	put(out, ", static %x8 ", mt->static_at);
	text_cplb(out, mt->static_cplb, false);
	put(out, ", code %x8 ", mt->code_at);
	text_cplb(out, mt->code_cplb, true);
	return true;
}

/* The faster read's way (slot_dma): "; by DMA: whole", or the fall-back to
 * the core's read and its reason, with the receive FIFO's words read off. */
static void slot_dma_text(uint32_t v, struct s14_text *out)
{
	/* BOOT_TIME_SLOT_DMA_CRC to BOOT_TIME_SLOT_DMA_FRAME, in order */
	_Static_assert(BOOT_TIME_SLOT_DMA_FRAME == BOOT_TIME_SLOT_DMA_CRC + 6u, "a reason each");
	static const char why[] =
		"its CRC failed\0done without the words moved\0words left in the receive FIFO\0"
		"a DMA error\0its count stopped\0the start refused\0the frame's command failed";
	const uint32_t how = v & BOOT_TIME_SLOT_DMA_MASK;
	const uint32_t drained = (v >> BOOT_TIME_SLOT_DRAINED_SHIFT) & 0xFFu;

	if (how == 0u)
		return;
	if (how == BOOT_TIME_SLOT_DMA_WHOLE) {
		s14_text_str(out, "; by DMA: whole");
		return;
	}
	put(out, "; by DMA: fell back to the core, %s",
	    how <= BOOT_TIME_SLOT_DMA_FRAME ? name_in(why, how - BOOT_TIME_SLOT_DMA_CRC) : "a failure");
	if (drained != 0u)
		put(out, ", %d words read off the receive FIFO", drained);
}

/* The front's reports in its version 2 record (S-01.023). */
static bool front_reports(const struct boot_time *t, struct s14_text *out)
{
	if (!(t->flags & BOOT_TIME_FRONT_REPORTS))
		return false;
	put(out, "front: longest gap between PSoC link reads %d us, %d reads found the receiver "
		 "overrun, %d frames sent without a TE edge",
	    t->front_gap_us, t->front_overruns, t->front_te_misses);
	return true;
}

static void text_slot(struct s14_text *out, uint32_t slot)
{
	if (slot > 1u)
		s14_text_str(out, "no slot");
	else
		put(out, "Slot %d", slot + 1u);
}

/*
 * How the running slot was found, and what each side spent. The record has
 * no stamp for the front's own step, so its span from the key read to the
 * jump is shown, which holds the image check, the start log's step, the
 * running slot and the drain; the main image's inference is the time of its
 * comparison run (BOOT_TIME_SLOT_TIMED).
 */
static bool slot_line(const struct boot_time *t, uint32_t cpu, struct s14_text *out)
{
	if (!reached(t, BOOT_TIME_SLOT_FOUND))
		return false;
	if (!(t->flags & BOOT_TIME_SLOT_GIVEN)) {
		s14_text_str(out, "running slot: inferred by the main image from the slots' streams");
		return true;
	}
	s14_text_str(out, "running slot: ");
	text_slot(out, t->slot_given);
	s14_text_str(out, ", from the front's record");
	if (reached(t, BOOT_TIME_KEYS) && reached(t, BOOT_TIME_FRONT_HANDOFF))
		put(out, "; front, key read to jump (image check, start log, running slot, drain) %m",
		    span_us(t, BOOT_TIME_KEYS, BOOT_TIME_FRONT_HANDOFF, cpu));
	if (t->flags & BOOT_TIME_SLOT_TIMED) {
		put(out, "; main image's inference from both slots, timed only, not used: %m, ",
		    t->slot_infer / cpu);
		text_slot(out, t->slot_inferred);
	}
	return true;
}

bool boot_time_line(const struct boot_time *t, unsigned k, uint32_t cycles_per_us,
		    struct s14_text *out)
{
	const uint32_t cpu = cycles_per_us != 0u ? cycles_per_us : 1u;

	s14_text_init(out);
	if (k < BOOT_TIME_COUNT)
		return milestone(t, k, cpu, out);
	switch (k - BOOT_TIME_COUNT) {
	case 0:
		if (!reached(t, BOOT_TIME_PSOC_OPEN))
			return false;
		s14_text_str(out, (t->flags & BOOT_TIME_GREETED) ? "psoc greeted" :
								    "psoc not greeted");
		if (!(t->flags & BOOT_TIME_BATTERY_RAN)) {
			s14_text_str(out, ", battery wait did not run");
		} else if (reached(t, BOOT_TIME_BATTERY_START) &&
			   reached(t, BOOT_TIME_BATTERY_END)) {
			put(out, ", battery wait ran %m",
			    span_us(t, BOOT_TIME_BATTERY_START, BOOT_TIME_BATTERY_END, cpu));
		} else {
			s14_text_str(out, ", battery wait ran, not ended");
		}
		return true;
	case 1:
		if (!reached(t, BOOT_TIME_FRONT_HANDOFF))
			return false;
		put(out, "front: greeting %s, charge wait %s",
		    (t->flags & BOOT_TIME_GREET_WAITED) ? "waited for" : "not waited for",
		    (t->flags & BOOT_TIME_CHARGE_WAITED) ? "ran" : "did not run");
		return true;
	case 2: {
		uint64_t us;

		if (!reached(t, BOOT_TIME_COPY_START) || !reached(t, BOOT_TIME_COPY_LAST))
			return false;
		us = span_us(t, BOOT_TIME_COPY_START, BOOT_TIME_COPY_LAST, cpu);
		put(out, "os copy %d bytes in %m", t->bytes, us);
		if (us != 0u)
			put(out, ", %d kB/s", (uint32_t)((uint64_t)t->bytes * 1000u / us));
		if (t->flags & BOOT_TIME_COPY_LINK) {
			s14_text_str(out, ", link rung ");
			text_rung(out, t->copy_link & 0xFFu);
			if ((t->copy_link >> 8 & 0xFFu) != (t->copy_link & 0xFFu)) {
				s14_text_str(out, " to rung ");
				text_rung(out, t->copy_link >> 8 & 0xFFu);
			}
			put(out, ", ceiling %d, %d fall-backs", t->copy_link >> 16 & 0xFFu, t->copy_fell);
			copy_quad(out, t);
		}
		return true;
	}
	case 3:
		if (!reached(t, BOOT_TIME_STREAM_END))
			return false;
		put(out, (t->flags & BOOT_TIME_LINK_FC) ?
			 "stream: %d sessions began with the ready line high, %m beyond the bytes' own "
			 "time" : "stream: %d bytes found the ready line high, waited %m",
		    t->stream_high, c2us(t->stream_wait, cpu));
		return true;
	case 4:
		if (reached(t, BOOT_TIME_STREAM_END) && (t->flags & BOOT_TIME_LINK_DMA))
			return dma_line(t, false, out);
		if (!reached(t, BOOT_TIME_STREAM_END) || (t->flags & BOOT_TIME_LINK_FC))
			return false;
		put(out, "ready line after a byte: %d high at the first sample, %d raised later, the "
			 "latest at %h us, longest hold %h us",
		    t->line_high_first, t->line_late, c2h(t->line_late_max, cpu),
		    c2h(t->line_hold_max, cpu));
		return true;
	case 5: {
		unsigned i;

		if (reached(t, BOOT_TIME_STREAM_END) && (t->flags & BOOT_TIME_LINK_DMA))
			return dma_line(t, true, out);
		if (!reached(t, BOOT_TIME_STREAM_END) || (t->flags & BOOT_TIME_LINK_FC) ||
		    t->line_window == 0u)
			return false;
		put(out, "raises in %d steps of %h us:", (uint32_t)BOOT_TIME_RAISE_STEPS,
		    c2h(t->line_window / BOOT_TIME_RAISE_STEPS, cpu));
		for (i = 0; i < BOOT_TIME_RAISE_STEPS; i++)
			put(out, " %d", t->line_raise[i]);
		return true;
	}
	case 6:
	case 7: {
		const bool late = k - BOOT_TIME_COUNT == 7u;
		const uint32_t n = late ? t->line_late : t->line_high_first;
		const uint32_t *at = late ? t->line_late_at : t->line_high_at;
		unsigned i;

		if (!reached(t, BOOT_TIME_STREAM_END) || (t->flags & BOOT_TIME_LINK_FC) || n == 0u)
			return false;
		s14_text_str(out, late ? "first late raises, after slot bytes:" :
					 "first high at the first sample, after slot bytes:");
		for (i = 0; i < n && i < BOOT_TIME_FIRST; i++)
			put(out, " %d", at[i]);
		return true;
	}
	case 8:
	case 9:
	case 10:
	case 11:
	case 12:
	case 13:
	case 14:
		return stream_line(t, k - BOOT_TIME_COUNT - 8u, cpu, out);
	case 15:
		if (t->slot_fast + t->slot_fast_failed == 0u)
			return false;
		put(out, "slot read at the faster rate: %d whole, %d failed and read again, %d failed "
			 "the CRC and read again, %d of those then good; %d kept by a trial mark's CRC",
		    t->slot_fast, t->slot_fast_failed, t->slot_reread, t->slot_reread_good,
		    t->slot_trial);
		if (t->flags & BOOT_TIME_SLOT_CRC_HW)
			s14_text_str(out, "; its CRC by the peripheral");
		slot_dma_text(t->slot_dma, out);
		return true;
	case 16:
		return power_line(t, out);
	case 17:
		return load_line(t, cpu, out);
	case 18:
		return start_line(t, cpu, out);
	case 19:
	case 20:
		return menu_line(t, k - BOOT_TIME_COUNT == 20u, cpu, out);
	case 21:
	case 22:
		return mount_line(t, k - BOOT_TIME_COUNT == 22u, out);
	case 23:
	case 24:
		return mount_more(t, k - BOOT_TIME_COUNT == 24u, out);
	case 25:
		return mount_how(t, out);
	case 26:
		return front_reports(t, out);
	case 27:
		return slot_line(t, cpu, out);
	case 28:
		s14_text_str(out, "end of report");
		return true;
	default:
		return false;
	}
}

void boot_time_scan_line(struct s14_text *out, uint32_t request_ms, const uint32_t *slave)
{
	s14_text_init(out);
	put(out, "scan before a write: %d ms for the request", request_ms);
	if (slave != NULL)
		put(out, "; the slave's scan %d ms, %d reads, way %d", slave[0] / 1000u, slave[1],
		    slave[2]);
	else
		s14_text_str(out, "; no figures from the slave");
}

/* The record's words: the header and the counts, each count low word
 * first, then the milestones, then the check. */
#define HDR_WORDS BOOT_TIME_WORDS

static uint32_t check_of(const uint8_t *rec)
{
	uint32_t sum = 0x5A5A5A5Au;
	size_t i;

	for (i = 0; i + 4u < BOOT_TIME_RECORD; i += 4u)
		sum = (sum << 5 | sum >> 27) ^ wb_get_le32(rec + i);
	return sum;
}

#define COUNT_WORDS (HDR_WORDS - 4u)

/* Where each count is in struct boot_time, in the record's order: the
 * two-word counts, low word first; the one-word counts; then the arrays,
 * the gaps' three words each last. */
#define AT(f) (uint16_t)offsetof(struct boot_time, f)
#define MOUNT(f) AT(mount.f)
#define SPLIT(f) AT(split.f)

static const uint16_t two_at[] = {
	AT(set), AT(stream_wait), SPLIT(xfer), SPLIT(window), SPLIT(sample), SPLIT(select),
	SPLIT(send), AT(slot_infer),
};

static const uint16_t one_at[] = {
	AT(bytes), AT(flags), AT(stream_high),
	AT(line_window), AT(line_high_first), AT(line_late), AT(line_late_max),
	AT(line_hold_max), AT(slot_fast), AT(slot_fast_failed), AT(slot_reread),
	AT(slot_reread_good), AT(slot_trial), AT(slot_dma),
	AT(dma_parts), AT(dma_guard), AT(dma_guard_us), AT(dma_high), AT(dma_fell),
	AT(dma_fell_session), AT(dma_fell_at), AT(dma_slow_bytes), AT(dma_held),
	AT(dma_fcs_samples), AT(dma_fcs_held), AT(dma_fcs_parts), AT(dma_fcs_first),
	AT(power_state), AT(battery), AT(battery_ma), AT(copy_link), AT(copy_fell),
	AT(copy_quad), AT(copy_qbytes), AT(copy_qstate), AT(copy_qtest_us),
	AT(mount_rc), AT(mount_err), AT(greet_tries), AT(front_gap_us),
	AT(front_overruns), AT(front_te_misses), AT(slot_given), AT(slot_inferred),
	MOUNT(valid), MOUNT(cpu),
	MOUNT(mount_us), MOUNT(reads), MOUNT(failed), MOUNT(read_us),
	MOUNT(read_min), MOUNT(read_max), MOUNT(tags_reads), MOUNT(tags_us),
	MOUNT(whole_reads), MOUNT(fell_back), MOUNT(allocs), MOUNT(frees),
	MOUNT(alloc_us), MOUNT(glue_us), MOUNT(dctl), MOUNT(ictl),
	MOUNT(dcplb_dflt), MOUNT(icplb_dflt), MOUNT(heap_lo), MOUNT(heap_hi),
	MOUNT(heap_cplb), MOUNT(static_at), MOUNT(static_cplb), MOUNT(code_at),
	MOUNT(code_cplb), MOUNT(way), MOUNT(objects), MOUNT(restore_us),
	MOUNT(restore_reads), MOUNT(check_us), MOUNT(check_reads),
	MOUNT(scan_us), MOUNT(scan_reads),
	SPLIT(bytes), SPLIT(blocks), SPLIT(fills), SPLIT(sessions), SPLIT(xfers), SPLIT(xfer_min),
	SPLIT(xfer_max), SPLIT(send_max), SPLIT(wire_byte), SPLIT(read), SPLIT(reads),
};

static const struct { uint16_t at, n; } many_at[] = {
	{ AT(line_raise), BOOT_TIME_RAISE_STEPS },
	{ AT(line_high_at), BOOT_TIME_FIRST },
	{ AT(line_late_at), BOOT_TIME_FIRST },
	{ MOUNT(phase_us), BOOT_TIME_PHASES },
	{ MOUNT(phase_min), BOOT_TIME_PHASES },
	{ MOUNT(phase_max), BOOT_TIME_PHASES },
	{ SPLIT(gap), 3u * BOOT_TIME_GAPS },
};

_Static_assert(sizeof(struct boot_time_gap) == 12u, "a gap is its three words in order");
_Static_assert(2u * (sizeof two_at / sizeof two_at[0]) + sizeof one_at / sizeof one_at[0] +
	       BOOT_TIME_RAISE_STEPS + 2u * BOOT_TIME_FIRST + 3u * BOOT_TIME_PHASES +
	       3u * BOOT_TIME_GAPS == COUNT_WORDS,
	       "every count has its words in the record");

/* n words at a count's place, to w or, with `unpack`, from it. */
static unsigned words(uint8_t *at, unsigned n, uint32_t *w, bool unpack)
{
	uint32_t *p = (uint32_t *)(void *)at;
	unsigned i;

	for (i = 0; i < n; i++) {
		if (unpack)
			p[i] = w[i];
		else
			w[i] = p[i];
	}
	return n;
}

/* The counts after the header's four words, as words: taken from the
 * struct, or with `unpack` put into it. */
static void counts(struct boot_time *t, uint32_t w[COUNT_WORDS], bool unpack)
{
	uint8_t *const base = (uint8_t *)t;
	unsigned i, k = 0;

	for (i = 0; i < sizeof two_at / sizeof two_at[0]; i++, k += 2u) {
		uint64_t *p = (uint64_t *)(void *)(base + two_at[i]);

		if (unpack) {
			*p = (uint64_t)w[k] | (uint64_t)w[k + 1u] << 32;
		} else {
			w[k] = (uint32_t)*p;
			w[k + 1u] = (uint32_t)(*p >> 32);
		}
	}
	for (i = 0; i < sizeof one_at / sizeof one_at[0]; i++)
		k += words(base + one_at[i], 1u, w + k, unpack);
	for (i = 0; i < sizeof many_at / sizeof many_at[0]; i++)
		k += words(base + many_at[i].at, many_at[i].n, w + k, unpack);
}

void boot_time_pack(const struct boot_time *t, uint32_t cycles_per_us,
		    uint8_t rec[BOOT_TIME_RECORD])
{
	struct boot_time copy = *t;
	uint32_t w[COUNT_WORDS];
	unsigned i;

	wb_put_le32(rec, BOOT_TIME_MAGIC);
	wb_put_le32(rec + 4u, BOOT_TIME_VERSION);
	wb_put_le32(rec + 8u, BOOT_TIME_COUNT);
	wb_put_le32(rec + 12u, cycles_per_us);
	counts(&copy, w, false);
	for (i = 0; i < COUNT_WORDS; i++)
		wb_put_le32(rec + 16u + 4u * i, w[i]);
	for (i = 0; i < BOOT_TIME_COUNT; i++) {
		wb_put_le32(rec + 4u * HDR_WORDS + 8u * i, (uint32_t)t->at[i]);
		wb_put_le32(rec + 4u * HDR_WORDS + 8u * i + 4u, (uint32_t)(t->at[i] >> 32));
	}
	wb_put_le32(rec + (BOOT_TIME_RECORD - 4u), check_of(rec));
}

bool boot_time_unpack(const uint8_t rec[BOOT_TIME_RECORD], struct boot_time *t,
		      uint32_t *cycles_per_us)
{
	uint32_t w[COUNT_WORDS];
	unsigned i;

	if (wb_get_le32(rec) != BOOT_TIME_MAGIC ||
	    wb_get_le32(rec + 4u) != BOOT_TIME_VERSION ||
	    wb_get_le32(rec + 8u) != BOOT_TIME_COUNT ||
	    wb_get_le32(rec + (BOOT_TIME_RECORD - 4u)) != check_of(rec))
		return false;
	*cycles_per_us = wb_get_le32(rec + 12u);
	for (i = 0; i < COUNT_WORDS; i++)
		w[i] = wb_get_le32(rec + 16u + 4u * i);
	counts(t, w, true);
	for (i = 0; i < BOOT_TIME_COUNT; i++)
		t->at[i] = (uint64_t)wb_get_le32(rec + 4u * HDR_WORDS + 8u * i) |
			   (uint64_t)wb_get_le32(rec + 4u * HDR_WORDS + 8u * i + 4u) << 32;
	return true;
}
