/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_TIMER_H
#define WAFFLE_DRIVERS_BF70X_TIMER_H

/*
 * Timers: the general-purpose timers of TIMER0 (ADSP-BF70x Hardware
 * Reference chapter "General-Purpose Timer (TIMER)"), the core timer
 * (Blackfin+ PRM chapter "Core Timer (TMR)") and the watchdog (HRM chapter
 * "Watchdog Timer (WDOG)").
 *
 * The time base is the cycle counter (waffle/drivers/bf70x_time.h, S-13a.031),
 * not a GP timer: the GP timers here give periodic or one-shot events in
 * SCLK0 cycles, for interrupts through the SEC or for polled delays. No
 * timer output drives a pin (OUTDIS, S-13a.032).
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x_time.h"

#define TIMER0_BASE          0x20043000u
#define TIMER0_RUN           (TIMER0_BASE + 0x04u)
#define TIMER0_RUN_SET       (TIMER0_BASE + 0x08u)
#define TIMER0_RUN_CLR       (TIMER0_BASE + 0x0Cu)
#define TIMER0_STOP_CFG      (TIMER0_BASE + 0x10u)
#define TIMER0_STOP_CFG_SET  (TIMER0_BASE + 0x14u)
#define TIMER0_STOP_CFG_CLR  (TIMER0_BASE + 0x18u)
#define TIMER0_DATA_IMSK     (TIMER0_BASE + 0x1Cu)
#define TIMER0_STAT_IMSK     (TIMER0_BASE + 0x20u)
#define TIMER0_DATA_ILAT     (TIMER0_BASE + 0x2Cu)	/* W1C */
#define TIMER0_STAT_ILAT     (TIMER0_BASE + 0x30u)	/* W1C */
#define TIMER0_ERR_TYPE      (TIMER0_BASE + 0x34u)
#define TIMER0_TMR_CFG(n)    (TIMER0_BASE + 0x60u + 0x20u * (n))
#define TIMER0_TMR_CNT(n)    (TIMER0_BASE + 0x64u + 0x20u * (n))
#define TIMER0_TMR_PER(n)    (TIMER0_BASE + 0x68u + 0x20u * (n))
#define TIMER0_TMR_WID(n)    (TIMER0_BASE + 0x6Cu + 0x20u * (n))
#define TIMER0_TMR_DLY(n)    (TIMER0_BASE + 0x70u + 0x20u * (n))

#define WB_GPTIMERS          8u

/* TIMER_TMR[n]_CFG. */
#define TIMER_CFG_TMODE_SHIFT   0u
#define TIMER_CFG_TMODE_PWM     (12u << 0)	/* continuous PWMOUT */
#define TIMER_CFG_TMODE_PULSE   (13u << 0)	/* single pulse PWMOUT */
/*
 * Only the PWMOUT modes are used. Silicon anomaly 19000043 (the first
 * EXTCLK-mode interrupt comes one edge late) would need PER = n - 1 for
 * the first period if EXTCLK mode is ever added.
 */
#define TIMER_CFG_IRQ_EDGE      (0u << 4)
#define TIMER_CFG_IRQ_DLY       (1u << 4)
#define TIMER_CFG_IRQ_WIDDLY    (2u << 4)
#define TIMER_CFG_IRQ_PER       (3u << 4)
#define TIMER_CFG_SLAVETRIG     (1u << 6)
#define TIMER_CFG_PULSEHI       (1u << 7)
#define TIMER_CFG_CLKSEL_SCLK0  (0u << 8)
#define TIMER_CFG_TINSEL        (1u << 10)
#define TIMER_CFG_OUTDIS        (1u << 11)
#define TIMER_CFG_EMURUN        (1u << 15)

/* Core timer TCNTL (PRM). INT is cleared by writing 0. */
#define TMR_TCNTL_PWR        (1u << 0)
#define TMR_TCNTL_EN         (1u << 1)
#define TMR_TCNTL_AUTORLD    (1u << 2)
#define TMR_TCNTL_INT        (1u << 3)

/* Watchdog. */
#define WDOG0_BASE           0x20046000u
#define WDOG0_CTL            (WDOG0_BASE + 0x00u)
#define WDOG0_CNT            (WDOG0_BASE + 0x04u)
#define WDOG0_STAT           (WDOG0_BASE + 0x08u)
#define WDOG_CTL_WDEN_SHIFT  4u
#define WDOG_CTL_WDEN_MASK   (0xFFu << 4)
#define WDOG_CTL_DISABLE     (0xADu << 4)	/* any other WDEN enables */
#define WDOG_CTL_WDRO        (1u << 15)	/* W1C */

#define WB_TIMER_OK       0
#define WB_TIMER_ERR_ARG (-1)
#define WB_TIMER_TIMEOUT (-2)

/* Stop every timer and mask their interrupts (OSENTRY: TIMER0_RUN = 0). */
void wb_gptimer_stop_all(void);

/* Core timer: stopped and powered down. */
void wb_coretmr_stop(void);

/* Watchdog. disable() also clears a rollover. */
void wb_wdog_disable(void);
void wb_wdog_start(uint32_t count);	/* count >= 1 SCLK0 cycles */

#endif /* WAFFLE_DRIVERS_BF70X_TIMER_H */
