/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_BOOT_TIME_H
#define WAFFLE_BOOT_TIME_H

/*
 * A boot's milestones in cycle counts, for a build that measures where the
 * time goes (make BOOT_TIMING=1; waffle/board/opxy_boot.h). Taking a stamp
 * stores the counter and nothing else; the lines are made later, in held
 * mode or by the test OS after a hand-over, so the measurement does not
 * move what it measures. Nothing here is a requirement: log wording and
 * which events are logged are free (S-14.009).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s14_log.h"

/* The master's milestones, in boot order. */
enum boot_time_id {
	BOOT_TIME_INIT_ENTRY = 0,	/* INIT stage entered, clocks not yet set */
	BOOT_TIME_INIT_CLOCKS,		/* INIT: the PLL set */
	BOOT_TIME_INIT_DDR,		/* INIT: the DDR controller up */
	BOOT_TIME_FRONT_ENTRY,		/* the front entered, the stream loaded */
	BOOT_TIME_FRONT_HANDOFF,	/* the front: the image checked, the jump next */
	BOOT_TIME_MAIN,			/* the main image entered */
	BOOT_TIME_BRINGUP,		/* pins, SEC, display hand-shake, flash, UART */
	BOOT_TIME_DDR_CHECK,
	BOOT_TIME_METADATA,
	BOOT_TIME_PSOC_OPEN,		/* greeting answered or given up, power queries sent */
	BOOT_TIME_BATTERY_START,	/* the battery wait's loop began (when it runs) */
	BOOT_TIME_BATTERY_END,
	BOOT_TIME_KEYS,			/* the key status read */
	BOOT_TIME_LOGO,			/* the PSoC message that lights the panel sent, after
					 * any charge wait; the frame is drawn earlier */
	BOOT_TIME_SLOT_PROBED,		/* the slot to read first chosen */
	BOOT_TIME_SLOT_READ,		/* its stream read, once, for the stream */
	BOOT_TIME_SLOT_CHECKED,		/* its CRC computed over that read */
	BOOT_TIME_SLOT_FOUND,		/* compared with memory: the running slot inferred */
	BOOT_TIME_STREAM_END,		/* the slave's boot stream sent */
	BOOT_TIME_SLAVE_READY,		/* the slave ready for the init session */
	BOOT_TIME_LINKED,		/* init, echo and the first pull done */
	BOOT_TIME_SLAVE_DONE,		/* the boot-partition size read */
	BOOT_TIME_PRE_LOAD,		/* PMIC step and hand-over frame done */
	BOOT_TIME_COPY_START,		/* the OS image's copy over the bridge began */
	BOOT_TIME_COPY_LAST,		/* its last eMMC read returned */
	BOOT_TIME_STAGED,		/* copied and its CRC checked */
	BOOT_TIME_CHECKED,		/* the image accepted (S-05.024) */
	BOOT_TIME_RELEASED,		/* the slave's release reply in */
	BOOT_TIME_HELD,			/* held mode entered */
	BOOT_TIME_OS_ENTRY,		/* the test OS entered, after a hand-over */
	/* The slave's stream in steps, numbered after the rest so that those
	 * keep their numbers; the report orders every line by its stamp. */
	BOOT_TIME_STREAM_WAIT,		/* S-09.003's wait for the ready line began */
	BOOT_TIME_STREAM_READY,		/* it ended: the line read low */
	BOOT_TIME_STREAM_OPENED,	/* the opening byte sent (S-09.004) */
	BOOT_TIME_STREAM_HEADER,	/* the first block's header sent; paced by the
					 * controller, the session with its first piece */
	BOOT_TIME_STREAM_LAST,		/* the stream's last byte sent */
	/* The start into the OS in steps (S-05.018), numbered after the rest
	 * for the same reason. */
	BOOT_TIME_HANDOFF,		/* the hand-over begun: the pre-load next */
	BOOT_TIME_PMIC,			/* the pre-load's PMIC step done */
	BOOT_TIME_BOOT_REPORT,		/* the logo redrawn, 0x51 {3C FF 32} queued; the
					 * drain next */
	BOOT_TIME_COPY_CRC,		/* the OS image's CRC computed (S-03.044) */
	BOOT_TIME_RELEASE_SENT,		/* the log tidied: the release request next */
	BOOT_TIME_SERVER_STOP,		/* PA6 low after the release: the slave's
					 * server stopped (S-05.032) */
	BOOT_TIME_SLAVE_OS,		/* PA6 high: the slave's OS started */
	BOOT_TIME_TEARDOWN,		/* the master's teardown begun (S-05.016) */
	BOOT_TIME_KERNEL_CALL,		/* the teardown done: the kernel call next */
	BOOT_TIME_COUNT
};

/* flags */
#define BOOT_TIME_GREETED     0x1u	/* the PSoC answered the greeting */
#define BOOT_TIME_BATTERY_RAN 0x2u	/* the battery wait's loop ran */
#define BOOT_TIME_GREET_WAITED  0x4u	/* the front waited for the greeting (S-07.021) */
#define BOOT_TIME_CHARGE_WAITED 0x8u	/* the front's charge wait ran (S-07.054) */
#define BOOT_TIME_LINK_FC       0x10u	/* the stream was paced by the controller */
#define BOOT_TIME_LINK_DMA      0x20u	/* and fed by DMA (S-09.009, S-13b.023) */
#define BOOT_TIME_POWER_KNOWN   0x40u	/* power_state holds a reported state */
#define BOOT_TIME_BATTERY_KNOWN 0x80u	/* battery holds a battery report */
#define BOOT_TIME_COPY_LINK     0x100u	/* copy_link holds the copy's link */
#define BOOT_TIME_PA6_POLLED    0x200u	/* a developer hand-over's line window took
					 * the slave's server stop and OS start */

#define BOOT_TIME_RAISE_STEPS 8u
#define BOOT_TIME_FIRST 16u
#define BOOT_TIME_GAPS 8u

/* A wait on the slave's ready line: its cycles, the slot's bytes sent by
 * then, and the block of the last of them (BOOT_TIME_NO_BLOCK: none yet). */
struct boot_time_gap {
	uint32_t cycles, at, block;
};
#define BOOT_TIME_NO_BLOCK UINT32_MAX

/*
 * The stream's time from S-09.003's wait to its end, split by where it went,
 * in cycles: the wait itself is the two milestones' difference; then inside
 * the byte transfers (from the write to the byte in), in the ready line's
 * window after each, in the sample of the line that decides the next byte,
 * in waits on a line found high there (struct boot_time's stream_wait), in
 * the chip select's changes, and, paced by the controller, in its sends.
 * What is left over is the loop's own: the report gives it as the rest.
 */
struct boot_time_split {
	uint64_t xfer, window, sample, select, send;
	uint32_t bytes;		/* the slot's bytes sent */
	uint32_t blocks;	/* blocks begun */
	uint32_t fills;		/* of those, fill or header-only blocks */
	uint32_t sessions;	/* chip-select sessions after the opening one */
	uint32_t xfers;		/* bytes through a single transfer, the opening one too */
	uint32_t xfer_min, xfer_max;	/* a transfer's shortest and longest */
	uint32_t send_max;	/* the longest send of a session */
	uint32_t wire_byte;	/* a byte's cycles at the link's clock */
	uint32_t read;		/* a counter read's cost: two back to back */
	uint32_t reads;		/* counter reads the split adds */
	struct boot_time_gap gap[BOOT_TIME_GAPS];	/* the longest waits, longest first */
};

struct boot_time {
	uint64_t at[BOOT_TIME_COUNT];	/* the cycle counter at each milestone */
	uint64_t set;			/* bit n: milestone n was reached */
	uint32_t bytes;			/* bytes the OS image's copy read */
	uint32_t flags;
	/* The slave's stream: the bytes whose first sample of the ready line
	 * found it high, and the cycles waited on those. Paced by the
	 * controller (BOOT_TIME_LINK_FC), where no byte is seen: the sessions
	 * that began with the line high, and the cycles the sessions took
	 * beyond their bytes' own time on the wire. */
	uint32_t stream_high;
	uint64_t stream_wait;
	/* The ready line watched through a window after each byte (struct
	 * boot_time_watch), counts in cycles. */
	uint32_t line_window;		/* the window's length */
	uint32_t line_high_first;	/* bytes: high at the first sample */
	uint32_t line_late;		/* bytes: low at the first, high later in the window */
	uint32_t line_late_max;		/* the latest such raise, from the byte's end */
	uint32_t line_hold_max;		/* the longest the line stayed high */
	uint32_t line_raise[BOOT_TIME_RAISE_STEPS];	/* raises by their delay, in equal
							 * steps of the window */
	/* Where the first of each were: the slot's bytes sent by then, so 16
	 * for a raise after a first block's header. */
	uint32_t line_high_at[BOOT_TIME_FIRST];
	uint32_t line_late_at[BOOT_TIME_FIRST];
	/* The stream's reads by the flash's faster read: those that came
	 * whole, those that failed and were read again at the usual rate,
	 * those whose CRC failed and were read again, and of those the ones
	 * whose CRC was then good, which says the faster read is marginal. */
	uint32_t slot_fast, slot_fast_failed, slot_reread, slot_reread_good;
	uint32_t slot_trial;	/* faster reads a trial mark's CRC confirmed: kept */
	/* Fed by DMA (BOOT_TIME_LINK_DMA, struct s09_fast_stats): the parts the
	 * controller sent whole, the guard's length and the time in the guards
	 * (us), the parts whose guard ended with the line high, the fall-back's
	 * cause (S09_PART_*, 0 for none), its session and the slot's bytes
	 * clocked before it, the bytes sent one at a time after it, and 1 when
	 * the slave held the line past T_STREAM. */
	uint32_t dma_parts, dma_guard, dma_guard_us, dma_high;
	uint32_t dma_fell, dma_fell_session, dma_fell_at, dma_slow_bytes, dma_held;
	/* SPI0_STAT read in the parts' waits, those with FCS set (the slave's
	 * line holding the controller), the parts that saw it, and the slot's
	 * bytes before the first of them plus one, 0 for none. */
	uint32_t dma_fcs_samples, dma_fcs_held, dma_fcs_parts, dma_fcs_first;
	/* The power at the start, as the PSoC last reported it before the
	 * hand-off to the main image (S-07.050, S-07.053): the state (0x10 on,
	 * 0x16 charging with the switch off), and the battery report's status
	 * byte (0 none, 2 charging), charge in % << 8 and mV << 16. */
	uint32_t power_state, battery, battery_ma;
	/* The OS image's copy over the bridge (S-09.075): the rung at its start,
	 * at its end << 8 and the link's ceiling << 16; fall-backs during it. */
	uint32_t copy_link, copy_fell;
	struct boot_time_split split;
};

_Static_assert(BOOT_TIME_COUNT <= 64u, "a bit of set for each milestone");

/* The cost of a stamp: one store of the count and one bit. */
static inline void boot_time_mark(struct boot_time *t, unsigned id, uint64_t cycles)
{
	if (id < BOOT_TIME_COUNT) {
		t->at[id] = cycles;
		t->set |= (uint64_t)1 << id;
	}
}

/*
 * The slave's ready line after a stream byte. boot_time_watch_byte()
 * when a byte has ended, with the slot's bytes sent by then; then boot_time_watch_sample() for every sample of
 * the line, with the cycles since that end and whether the sample is one of
 * the window's. Counted into the boot_time: a byte whose first sample is
 * high; a byte whose line rises later in the window, with the latest such
 * raise and each raise's step of the window; and the longest the line stays
 * high, whenever its fall is seen.
 */
struct boot_time_watch {
	bool active, first, rose, held;
	uint64_t rose_at;
	uint32_t sent;
};

void boot_time_watch_byte(struct boot_time_watch *w, uint32_t sent);
void boot_time_watch_sample(struct boot_time *t, struct boot_time_watch *w, bool high,
			    uint64_t since_byte, bool in_window);

/*
 * The stream's bytes as they go, which finds its blocks (S-09.005): a
 * 16-byte header, then its count of bytes unless it is a fill or its count
 * is 0. boot_time_walk_byte() for a byte, boot_time_walk() for a run; the
 * slot's bytes only, the opening byte not. A byte inside a block's payload
 * costs a test and two counts.
 */
struct boot_time_walk {
	uint8_t head[16];
	uint32_t have;		/* the header's bytes so far */
	uint32_t left;		/* the payload's bytes still to come */
};

void boot_time_walk_head(struct boot_time *t, struct boot_time_walk *w, uint8_t b);
void boot_time_walk(struct boot_time *t, struct boot_time_walk *w, const uint8_t *d,
		    uint32_t n);

static inline void boot_time_walk_byte(struct boot_time *t, struct boot_time_walk *w,
				       uint8_t b)
{
	t->split.bytes++;
	if (w->left != 0u)
		w->left--;
	else
		boot_time_walk_head(t, w, b);
}

/* A single transfer's cycles: summed, counted, the shortest and longest
 * kept. xfer_min starts at UINT32_MAX. */
static inline void boot_time_xfer(struct boot_time *t, uint64_t cycles)
{
	const uint32_t c = cycles > UINT32_MAX ? UINT32_MAX : (uint32_t)cycles;

	t->split.xfer += cycles;
	t->split.xfers++;
	if (c < t->split.xfer_min)
		t->split.xfer_min = c;
	if (c > t->split.xfer_max)
		t->split.xfer_max = c;
}

/* A wait of `cycles` after `at` of the slot's bytes, kept when it is among
 * the BOOT_TIME_GAPS longest; of equal ones the earlier stays first. */
void boot_time_gap(struct boot_time *t, uint64_t cycles, uint32_t at);

/*
 * The report's lines, k = 0 to BOOT_TIME_LINES - 1. First a line per
 * milestone reached, in the order of their stamps, each with its number,
 * its time and the time since the line before; after a charge wait (the
 * unit charged with its switch off, for any length of time) each later
 * line also has its time since that wait's end, when the switch went on.
 * Then the PSoC and battery-wait outcome, the front's waits, the copy's
 * rate, the stream's pacing, the ready line after a byte (four lines), the
 * stream's split, blocks, bytes, longest waits and the measuring's own cost
 * (seven lines), the stream's faster reads, the power at the start, the start
 * into the OS in spans (two lines), and an end line. A line k with nothing to say
 * leaves *out empty and returns false. cycles_per_us converts counts after
 * INIT's clock set-up; counts taken before it are shown as they are.
 */
#define BOOT_TIME_LINES (BOOT_TIME_COUNT + 20u)
bool boot_time_line(const struct boot_time *t, unsigned k, uint32_t cycles_per_us,
		    struct s14_text *out);

/*
 * A copy that crosses a hand-over: a fixed layout of little-endian words,
 * with a magic, the counts, and a check. pack() writes BOOT_TIME_RECORD
 * bytes; unpack() refuses a record whose magic, version or check is wrong.
 */
#define BOOT_TIME_MAGIC   0x4D495442u	/* "BTIM" */
#define BOOT_TIME_VERSION 10u
#define BOOT_TIME_WORDS   (60u + BOOT_TIME_RAISE_STEPS + 2u * BOOT_TIME_FIRST + \
			   3u * BOOT_TIME_GAPS)
#define BOOT_TIME_RECORD  (BOOT_TIME_WORDS * 4u + 8u * BOOT_TIME_COUNT + 4u)
void boot_time_pack(const struct boot_time *t, uint32_t cycles_per_us,
		    uint8_t rec[BOOT_TIME_RECORD]);
bool boot_time_unpack(const uint8_t rec[BOOT_TIME_RECORD], struct boot_time *t,
		      uint32_t *cycles_per_us);

#endif /* WAFFLE_BOOT_TIME_H */
