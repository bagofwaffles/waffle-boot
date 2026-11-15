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
	BOOT_TIME_COUNT
};

/* flags */
#define BOOT_TIME_GREETED     0x1u	/* the PSoC answered the greeting */
#define BOOT_TIME_BATTERY_RAN 0x2u	/* the battery wait's loop ran */
#define BOOT_TIME_GREET_WAITED  0x4u	/* the front waited for the greeting (S-07.021) */
#define BOOT_TIME_CHARGE_WAITED 0x8u	/* the front's charge wait ran (S-07.054) */
#define BOOT_TIME_LINK_FC       0x10u	/* the stream was paced by the controller */

#define BOOT_TIME_RAISE_STEPS 8u
#define BOOT_TIME_FIRST 16u

struct boot_time {
	uint64_t at[BOOT_TIME_COUNT];	/* the cycle counter at each milestone */
	uint32_t set;			/* bit n: milestone n was reached */
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
};

_Static_assert(BOOT_TIME_COUNT <= 32u, "a bit of set for each milestone");

/* The cost of a stamp: one store of the count and one bit. */
static inline void boot_time_mark(struct boot_time *t, unsigned id, uint64_t cycles)
{
	if (id < BOOT_TIME_COUNT) {
		t->at[id] = cycles;
		t->set |= 1u << id;
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
 * The report's lines, k = 0 to BOOT_TIME_LINES - 1. First a line per
 * milestone reached, in the order of their stamps, each with its number,
 * its time and the time since the line before; after a charge wait (the
 * unit charged with its switch off, for any length of time) each later
 * line also has its time since that wait's end, when the switch went on.
 * Then the PSoC and battery-wait outcome, the front's waits, the copy's
 * rate, the stream's pacing, the ready line after a byte (four lines), the
 * stream's faster reads, and an end line. A line k with nothing to say
 * leaves *out empty and returns false. cycles_per_us converts counts after
 * INIT's clock set-up; counts taken before it are shown as they are.
 */
#define BOOT_TIME_LINES (BOOT_TIME_COUNT + 10u)
bool boot_time_line(const struct boot_time *t, unsigned k, uint32_t cycles_per_us,
		    struct s14_text *out);

/*
 * A copy that crosses a hand-over: a fixed layout of little-endian words,
 * with a magic, the counts, and a check. pack() writes BOOT_TIME_RECORD
 * bytes; unpack() refuses a record whose magic, version or check is wrong.
 */
#define BOOT_TIME_MAGIC   0x4D495442u	/* "BTIM" */
#define BOOT_TIME_VERSION 6u
#define BOOT_TIME_WORDS   (20u + BOOT_TIME_RAISE_STEPS + 2u * BOOT_TIME_FIRST)
#define BOOT_TIME_RECORD  (BOOT_TIME_WORDS * 4u + 8u * BOOT_TIME_COUNT + 4u)
void boot_time_pack(const struct boot_time *t, uint32_t cycles_per_us,
		    uint8_t rec[BOOT_TIME_RECORD]);
bool boot_time_unpack(const uint8_t rec[BOOT_TIME_RECORD], struct boot_time *t,
		      uint32_t *cycles_per_us);

#endif /* WAFFLE_BOOT_TIME_H */
