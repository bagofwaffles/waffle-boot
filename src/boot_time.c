/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A boot's milestones and their report (see waffle/boot_time.h). */
#include "waffle/boot_time.h"

#include "waffle/bytes.h"

static const char *const names[BOOT_TIME_COUNT] = {
	"init entry", "init clocks", "init ddr", "front entry", "front hand-off",
	"main", "bring-up", "ddr check", "metadata", "psoc open",
	"battery wait start", "battery wait end", "keys", "panel light sent",
	"slot probed", "slot read", "slot checked", "slot found", "stream end",
	"slave ready",
	"bridge linked", "slave done", "pre-load", "os copy start",
	"os copy last read", "os staged", "os checked", "release reply", "held mode",
	"test os entry", "stream ready wait", "stream ready", "stream opened",
	"stream first header", "stream last byte",
};

static bool reached(const struct boot_time *t, unsigned id)
{
	return id < BOOT_TIME_COUNT && (t->set >> id & 1u) != 0u;
}

/* Microseconds as milliseconds with three decimals. */
static void text_ms(struct s14_text *out, uint64_t us)
{
	const uint32_t frac = (uint32_t)(us % 1000u);
	char d[4];

	s14_text_dec(out, (uint32_t)(us / 1000u));
	d[0] = '.';
	d[1] = (char)('0' + frac / 100u);
	d[2] = (char)('0' + frac / 10u % 10u);
	d[3] = (char)('0' + frac % 10u);
	s14_text_bytes(out, (const uint8_t *)d, sizeof d);
	s14_text_str(out, " ms");
}

/* Cycles as microseconds with two decimals. */
static void text_us(struct s14_text *out, uint64_t cycles, uint32_t cpu)
{
	const uint64_t hundredths = cycles * 100u / cpu;
	char d[3];

	s14_text_dec(out, (uint32_t)(hundredths / 100u));
	d[0] = '.';
	d[1] = (char)('0' + hundredths / 10u % 10u);
	d[2] = (char)('0' + hundredths % 10u);
	s14_text_bytes(out, (const uint8_t *)d, sizeof d);
	s14_text_str(out, " us");
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
static uint64_t span_us(const struct boot_time *t, unsigned from, unsigned to,
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
	s14_text_dec(out, id / 10u);
	s14_text_dec(out, id % 10u);
	s14_text_str(out, " ");
	s14_text_str(out, names[id]);
	if (id < BOOT_TIME_INIT_DDR) {
		/* The core still on its reset clock: the count as it stands. */
		s14_text_str(out, ": counter ");
		s14_text_hex(out, (uint32_t)(t->at[id] >> 32), 8u);
		s14_text_hex(out, (uint32_t)t->at[id], 8u);
		return true;
	}
	s14_text_str(out, ": ");
	text_ms(out, t->at[id] / cpu);
	/* nothing is counted from INIT's entry, taken on the reset clock */
	if (prev < BOOT_TIME_COUNT && prev != BOOT_TIME_INIT_ENTRY) {
		s14_text_str(out, ", +");
		text_ms(out, span_us(t, prev, id, cpu));
	}
	if (on < BOOT_TIME_COUNT && !before(t, id, on)) {
		s14_text_str(out, ", since on ");
		text_ms(out, span_us(t, on, id, cpu));
	}
	return true;
}

/* Cycles as milliseconds. */
static void text_cycles_ms(struct s14_text *out, uint64_t cycles, uint32_t cpu)
{
	text_ms(out, cycles / cpu);
}

static void text_part(struct s14_text *out, const char *name, uint64_t cycles, uint32_t cpu)
{
	s14_text_str(out, name);
	text_cycles_ms(out, cycles, cpu);
}

/* The stream's split, its blocks, its bytes, its longest waits (two lines)
 * and the measuring's own cost: lines 0 to 6. */
static bool stream_line(const struct boot_time *t, unsigned k, uint32_t cpu,
			struct s14_text *out)
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

		s14_text_str(out, "stream split: ");
		text_cycles_ms(out, whole, cpu);
		text_part(out, " in all, ready wait ", open, cpu);
		text_part(out, ", transfers ", sp->xfer, cpu);
		if (fc)
			text_part(out, ", sends ", sp->send, cpu);
		else
			text_part(out, ", windows ", sp->window, cpu);
		text_part(out, ", samples ", sp->sample, cpu);
		return true;
	}
	case 1: {
		const uint64_t whole = t->at[BOOT_TIME_STREAM_END] - t->at[BOOT_TIME_STREAM_WAIT];
		/* paced by the controller, the waits are inside the sends */
		const uint64_t parts = t->at[BOOT_TIME_STREAM_READY] - t->at[BOOT_TIME_STREAM_WAIT] +
				       sp->xfer + sp->window + sp->sample + sp->select + sp->send +
				       (fc ? 0u : t->stream_wait);

		s14_text_str(out, "stream split: ");
		if (fc)
			text_part(out, "beyond the wire in the sends ", t->stream_wait, cpu);
		else
			text_part(out, "waits on a high line ", t->stream_wait, cpu);
		text_part(out, ", chip select ", sp->select, cpu);
		if (parts <= whole) {
			text_part(out, ", the rest ", whole - parts, cpu);
		} else {
			s14_text_str(out, ", the rest -");
			text_cycles_ms(out, parts - whole, cpu);
		}
		return true;
	}
	case 2:
		s14_text_str(out, "stream blocks: ");
		s14_text_dec(out, sp->blocks);
		s14_text_str(out, " blocks, ");
		s14_text_dec(out, sp->fills);
		s14_text_str(out, " fill or header-only, ");
		s14_text_dec(out, sp->sessions);
		s14_text_str(out, " sessions, ");
		s14_text_dec(out, sp->bytes);
		s14_text_str(out, " bytes");
		return true;
	case 3: {
		const uint64_t wire = (uint64_t)sp->wire_byte * sp->xfers;

		if (fc) {
			s14_text_str(out, "stream sends: ");
			s14_text_dec(out, sp->sessions);
			text_part(out, " sessions in ", sp->send, cpu);
			text_part(out, ", the longest ", sp->send_max, cpu);
			text_part(out, "; on the wire alone ", (uint64_t)sp->wire_byte * sp->bytes, cpu);
			return true;
		}
		if (sp->xfers == 0u)
			return false;
		s14_text_str(out, "stream bytes: ");
		s14_text_dec(out, sp->xfers);
		s14_text_str(out, " transfers less a counter read, avg ");
		{
			const uint64_t avg = sp->xfer / sp->xfers;

			text_us(out, avg > sp->read ? avg - sp->read : 0u, cpu);
		}
		s14_text_str(out, ", min ");
		text_us(out, sp->xfer_min > sp->read ? sp->xfer_min - sp->read : 0u, cpu);
		s14_text_str(out, ", max ");
		text_us(out, sp->xfer_max > sp->read ? sp->xfer_max - sp->read : 0u, cpu);
		text_part(out, "; on the wire alone ", wire, cpu);
		text_part(out, ", in this build ", sp->xfer, cpu);
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
			s14_text_str(out, i == from ? " " : "; ");
			text_cycles_ms(out, sp->gap[i].cycles, cpu);
			s14_text_str(out, " at byte ");
			s14_text_dec(out, sp->gap[i].at);
			if (sp->gap[i].block == BOOT_TIME_NO_BLOCK) {
				s14_text_str(out, ", no block yet");
			} else {
				s14_text_str(out, ", block ");
				s14_text_dec(out, sp->gap[i].block);
			}
		}
		return true;
	}
	case 6:
		s14_text_str(out, "stream measuring: ");
		s14_text_dec(out, sp->reads);
		s14_text_str(out, " counter reads added, ");
		s14_text_dec(out, sp->read);
		s14_text_str(out, " cycles a read, at most ");
		text_cycles_ms(out, (uint64_t)sp->reads * sp->read, cpu);
		return true;
	default:
		return false;
	}
}

/* Fed by DMA, in place of the lines about the line after a byte: the parts
 * and their guards, then the fall-back. The causes are S09_PART_*'s. */
static bool dma_line(const struct boot_time *t, bool fell, struct s14_text *out)
{
	static const char *const cause[] = {
		"none", "the slave's hold", "a DMA error", "the controller stopped with the line low",
		"no end of the transmit with the line low", "the driver refused the part",
	};

	if (!fell) {
		s14_text_str(out, "stream by DMA: ");
		s14_text_dec(out, t->dma_parts);
		s14_text_str(out, " parts, guard ");
		s14_text_dec(out, t->dma_guard);
		s14_text_str(out, " us, ");
		text_ms(out, t->dma_guard_us);
		s14_text_str(out, " in the guards, ");
		s14_text_dec(out, t->dma_high);
		s14_text_str(out, " parts began with the ready line high");
		return true;
	}
	if (t->dma_fell == 0u) {
		s14_text_str(out, t->dma_held ? "stream fall-back: none; the slave held the line past T_STREAM" :
						"stream fall-back: none");
		return true;
	}
	s14_text_str(out, "stream fall-back at byte ");
	s14_text_dec(out, t->dma_fell_at);
	s14_text_str(out, " of session ");
	s14_text_dec(out, t->dma_fell_session);
	s14_text_str(out, ": ");
	s14_text_str(out, t->dma_fell < sizeof cause / sizeof cause[0] ? cause[t->dma_fell] : "?");
	s14_text_str(out, ", then ");
	s14_text_dec(out, t->dma_slow_bytes);
	s14_text_str(out, " bytes one at a time");
	if (t->dma_held)
		s14_text_str(out, "; the slave held the line past T_STREAM");
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
			s14_text_str(out, ", battery wait ran ");
			text_ms(out, span_us(t, BOOT_TIME_BATTERY_START,
					     BOOT_TIME_BATTERY_END, cpu));
		} else {
			s14_text_str(out, ", battery wait ran, not ended");
		}
		return true;
	case 1:
		if (!reached(t, BOOT_TIME_FRONT_HANDOFF))
			return false;
		s14_text_str(out, (t->flags & BOOT_TIME_GREET_WAITED) ?
					  "front: greeting waited for" :
					  "front: greeting not waited for");
		s14_text_str(out, (t->flags & BOOT_TIME_CHARGE_WAITED) ?
					  ", charge wait ran" : ", charge wait did not run");
		return true;
	case 2: {
		uint64_t us;

		if (!reached(t, BOOT_TIME_COPY_START) || !reached(t, BOOT_TIME_COPY_LAST))
			return false;
		us = span_us(t, BOOT_TIME_COPY_START, BOOT_TIME_COPY_LAST, cpu);
		s14_text_str(out, "os copy ");
		s14_text_dec(out, t->bytes);
		s14_text_str(out, " bytes in ");
		text_ms(out, us);
		if (us != 0u) {
			s14_text_str(out, ", ");
			s14_text_dec(out, (uint32_t)((uint64_t)t->bytes * 1000u / us));
			s14_text_str(out, " kB/s");
		}
		return true;
	}
	case 3:
		if (!reached(t, BOOT_TIME_STREAM_END))
			return false;
		s14_text_str(out, "stream: ");
		s14_text_dec(out, t->stream_high);
		if (t->flags & BOOT_TIME_LINK_FC) {
			s14_text_str(out, " sessions began with the ready line high, ");
			text_ms(out, t->stream_wait / cpu);
			s14_text_str(out, " beyond the bytes' own time");
			return true;
		}
		s14_text_str(out, " bytes found the ready line high, waited ");
		text_ms(out, t->stream_wait / cpu);
		return true;
	case 4:
		if (reached(t, BOOT_TIME_STREAM_END) && (t->flags & BOOT_TIME_LINK_DMA))
			return dma_line(t, false, out);
		if (!reached(t, BOOT_TIME_STREAM_END) || (t->flags & BOOT_TIME_LINK_FC))
			return false;
		s14_text_str(out, "ready line after a byte: ");
		s14_text_dec(out, t->line_high_first);
		s14_text_str(out, " high at the first sample, ");
		s14_text_dec(out, t->line_late);
		s14_text_str(out, " raised later, the latest at ");
		text_us(out, t->line_late_max, cpu);
		s14_text_str(out, ", longest hold ");
		text_us(out, t->line_hold_max, cpu);
		return true;
	case 5: {
		unsigned i;

		if (reached(t, BOOT_TIME_STREAM_END) && (t->flags & BOOT_TIME_LINK_DMA))
			return dma_line(t, true, out);
		if (!reached(t, BOOT_TIME_STREAM_END) || (t->flags & BOOT_TIME_LINK_FC) ||
		    t->line_window == 0u)
			return false;
		s14_text_str(out, "raises in ");
		s14_text_dec(out, BOOT_TIME_RAISE_STEPS);
		s14_text_str(out, " steps of ");
		text_us(out, t->line_window / BOOT_TIME_RAISE_STEPS, cpu);
		s14_text_str(out, ":");
		for (i = 0; i < BOOT_TIME_RAISE_STEPS; i++) {
			s14_text_str(out, " ");
			s14_text_dec(out, t->line_raise[i]);
		}
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
		for (i = 0; i < n && i < BOOT_TIME_FIRST; i++) {
			s14_text_str(out, " ");
			s14_text_dec(out, at[i]);
		}
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
		s14_text_str(out, "slot read at the faster rate: ");
		s14_text_dec(out, t->slot_fast);
		s14_text_str(out, " whole, ");
		s14_text_dec(out, t->slot_fast_failed);
		s14_text_str(out, " failed and read again, ");
		s14_text_dec(out, t->slot_reread);
		s14_text_str(out, " failed the CRC and read again, ");
		s14_text_dec(out, t->slot_reread_good);
		s14_text_str(out, " of those then good; ");
		s14_text_dec(out, t->slot_trial);
		s14_text_str(out, " kept by a trial mark's CRC");
		return true;
	case 16:
		s14_text_str(out, "end of report");
		return true;
	default:
		return false;
	}
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

/* The counts after the header's four words, in the record's order, as words:
 * taken from the struct, or with `unpack` put into it. */
#define COUNT_WORDS (HDR_WORDS - 4u)

static void counts(struct boot_time *t, uint32_t w[COUNT_WORDS], bool unpack)
{
	struct boot_time_split *sp = &t->split;
	uint64_t *const two[] = {
		&t->set, &t->stream_wait, &sp->xfer, &sp->window, &sp->sample, &sp->select,
		&sp->send,
	};
	uint32_t *const one[] = {
		&t->bytes, &t->flags, &t->stream_high,
		&t->line_window, &t->line_high_first, &t->line_late, &t->line_late_max,
		&t->line_hold_max, &t->slot_fast, &t->slot_fast_failed, &t->slot_reread,
		&t->slot_reread_good, &t->slot_trial,
		&t->dma_parts, &t->dma_guard, &t->dma_guard_us, &t->dma_high, &t->dma_fell,
		&t->dma_fell_session, &t->dma_fell_at, &t->dma_slow_bytes, &t->dma_held,
		&sp->bytes, &sp->blocks, &sp->fills, &sp->sessions, &sp->xfers, &sp->xfer_min,
		&sp->xfer_max, &sp->send_max, &sp->wire_byte, &sp->read, &sp->reads,
	};
	const struct { uint32_t *p; unsigned n; } many[] = {
		{ t->line_raise, BOOT_TIME_RAISE_STEPS },
		{ t->line_high_at, BOOT_TIME_FIRST },
		{ t->line_late_at, BOOT_TIME_FIRST },
	};
	unsigned i, j, k = 0;

	_Static_assert(2u * (sizeof two / sizeof two[0]) + sizeof one / sizeof one[0] +
		       BOOT_TIME_RAISE_STEPS + 2u * BOOT_TIME_FIRST + 3u * BOOT_TIME_GAPS ==
		       COUNT_WORDS, "every count has its words in the record");
	for (i = 0; i < sizeof two / sizeof two[0]; i++, k += 2u) {
		if (unpack) {
			*two[i] = (uint64_t)w[k] | (uint64_t)w[k + 1u] << 32;
		} else {
			w[k] = (uint32_t)*two[i];
			w[k + 1u] = (uint32_t)(*two[i] >> 32);
		}
	}
	for (i = 0; i < sizeof one / sizeof one[0]; i++, k++) {
		if (unpack)
			*one[i] = w[k];
		else
			w[k] = *one[i];
	}
	for (i = 0; i < sizeof many / sizeof many[0]; i++)
		for (j = 0; j < many[i].n; j++, k++) {
			if (unpack)
				many[i].p[j] = w[k];
			else
				w[k] = many[i].p[j];
		}
	for (i = 0; i < BOOT_TIME_GAPS; i++, k += 3u) {
		struct boot_time_gap *g = &sp->gap[i];

		if (unpack) {
			g->cycles = w[k];
			g->at = w[k + 1u];
			g->block = w[k + 2u];
		} else {
			w[k] = g->cycles;
			w[k + 1u] = g->at;
			w[k + 2u] = g->block;
		}
	}
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
