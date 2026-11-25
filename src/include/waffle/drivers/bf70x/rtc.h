/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_RTC_H
#define WAFFLE_DRIVERS_BF70X_RTC_H

/*
 * RTC0 (ADSP-BF70x Hardware Reference chapter "Real-Time Clock (RTC)"):
 * held mode's set-up (S-13a.071) and the count register RTC0_CLK, read and
 * written in S-04.070's encoding, with the write protocol of S-13a.072.
 *
 * The OS owns the RTC (S-13a.070): until wb_rtc_held_start() a struct
 * wb_rtc makes no RTC0 access, and the boot path does not call it.
 *
 * Writes to the 1 Hz registers (CLK, ALM, STPWTCH, INIT) go through a
 * shadow register and complete at the RTC's next 1 Hz edge, up to 1 s
 * later; STAT.WRPEND is set until then and a second write before it clears
 * is ignored. A CLK write here waits for a seconds event (the HRM: writes
 * are issued just after one), writes, and waits for WRPEND to clear, so a
 * read that follows gets the new time (S-10b.063). It can take up to about
 * 2 s; S-10b states no response time for a DateTime set, so this is free.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x/time.h"

#define RTC0_BASE       0x200D1280u
#define RTC0_CLK        (RTC0_BASE + 0x00u)
#define RTC0_ALM        (RTC0_BASE + 0x04u)
#define RTC0_IEN        (RTC0_BASE + 0x08u)
#define RTC0_STAT       (RTC0_BASE + 0x0Cu)	/* clear on read */
#define RTC0_STPWTCH    (RTC0_BASE + 0x10u)
#define RTC0_INIT       (RTC0_BASE + 0x18u)
#define RTC0_INITSTAT   (RTC0_BASE + 0x1Cu)

/* RTC_CLK (S-04.070) */
#define RTC_CLK_SEC_MASK    0x3Fu
#define RTC_CLK_MIN_SHIFT   6u
#define RTC_CLK_MIN_MASK    (0x3Fu << 6)
#define RTC_CLK_HOUR_SHIFT  12u
#define RTC_CLK_HOUR_MASK   (0x1Fu << 12)
#define RTC_CLK_DAY_SHIFT   17u

/* RTC_IEN */
#define RTC_IEN_WRDONE      (1u << 0)
#define RTC_IEN_SEC         (1u << 1)
#define RTC_IEN_MIN         (1u << 2)
#define RTC_IEN_HOUR        (1u << 3)
#define RTC_IEN_DAY         (1u << 4)
#define RTC_IEN_ALM         (1u << 5)
#define RTC_IEN_DAYALM      (1u << 6)
#define RTC_IEN_SW          (1u << 7)
#define RTC_IEN_CLKFAIL     (1u << 8)
#define RTC_IEN_EMUDIS      (1u << 9)

/* RTC_STAT; every bit but WRPEND, ALM and DAYALM clears when read */
#define RTC_STAT_WRPEND     (1u << 0)
#define RTC_STAT_WRDONE     (1u << 1)
#define RTC_STAT_SEC        (1u << 2)
#define RTC_STAT_MIN        (1u << 3)
#define RTC_STAT_HOUR       (1u << 4)
#define RTC_STAT_DAY        (1u << 5)
#define RTC_STAT_ALM        (1u << 6)
#define RTC_STAT_DAYALM     (1u << 7)
#define RTC_STAT_SWEXP      (1u << 8)
#define RTC_STAT_CLKFAIL    (1u << 9)
#define RTC_STAT_WRZONE     (1u << 10)

/* RTC_INIT */
#define RTC_INIT_CAL_MASK   0xFu
#define RTC_INIT_PWDN       (1u << 4)	/* 1: oscillator powered down */
#define RTC_INIT_RDEN       (1u << 5)	/* 1: output bus disabled */

/* RTC_INITSTAT */
#define RTC_INITSTAT_ALMPND    (1u << 0)
#define RTC_INITSTAT_DAYALMPND (1u << 1)
#define RTC_INITSTAT_PWDN      (1u << 2)	/* 1: oscillator running */

/* How long a wait for one 1 Hz edge may take: the edge, with margin. */
#define WB_RTC_EDGE_US      1200000u

#define WB_RTC_OK           0
#define WB_RTC_ERR_STATE   (-1)		/* not set up */
#define WB_RTC_ERR_TIMEOUT (-2)		/* no seconds event, or WRPEND stuck */
#define WB_RTC_ERR_VALUE   (-3)		/* not a valid time of day */

struct wb_rtc {
	const struct wb_time *t;	/* NULL until wb_rtc_held_start() */
	bool ticked;			/* a seconds event since the set-up */
	bool no_tick;			/* the wait for the first one timed out */
};

/*
 * S-13a.071, as held mode starts: once no write is pending, RTC0_INIT is
 * read and written back with its CAL, PWDN = 0 and RDEN = 0, and RTC0_IEN
 * is cleared (the RTC is polled, no interrupt is used); then waits up to
 * WB_RTC_EDGE_US for the INIT write to complete (the HRM: before other
 * writes; HELD is observed once the RTC is set up), and goes on if it
 * does not, as an oscillator just powered up may not tick yet. Returns
 * WB_RTC_ERR_TIMEOUT, writing nothing, when an earlier write stays
 * pending; r is then not set up.
 */
int wb_rtc_held_start(struct wb_rtc *r, const struct wb_time *t);

/*
 * RTC0_CLK as S-04.070 encodes it. CLK reads 0 until the first seconds
 * event after the core's power-up, so the first read waits for one (up to
 * WB_RTC_EDGE_US); once that wait has timed out, later reads only look
 * for one, without waiting.
 */
int wb_rtc_read(struct wb_rtc *r, uint32_t *clk);

/*
 * RTC0_CLK as it reads now, 0 included, without waiting for a seconds
 * event and without reading RTC0_STAT (whose events a read clears).
 * WB_RTC_ERR_STATE until wb_rtc_held_start() (S-13a.070).
 */
int wb_rtc_peek(const struct wb_rtc *r, uint32_t *clk);

/*
 * Sets RTC0_CLK to clk (S-04.070) and returns once the write completed.
 * Refuses a second, minute or hour field out of range (the HRM forbids
 * writing one) with WB_RTC_ERR_VALUE, writing nothing.
 */
int wb_rtc_write(struct wb_rtc *r, uint32_t clk);

#endif /* WAFFLE_DRIVERS_BF70X_RTC_H */
