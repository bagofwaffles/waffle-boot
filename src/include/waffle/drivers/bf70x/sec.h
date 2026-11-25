/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_SEC_H
#define WAFFLE_DRIVERS_BF70X_SEC_H

/*
 * Interrupts: the core event controller (CEC) and the System Event
 * Controller (SEC), ADSP-BF70x Hardware Reference chapter "System Event
 * Controller (SEC)" and Blackfin+ PRM "Events and Interrupts".
 *
 * Every SEC source is delivered to core 0 at IVG11, and main runs at IVG15
 * (S-13a.051). wb_sec_init() resets and enables the SEC once, before any
 * source is configured; later calls never write a RESET bit (S-13a.050).
 */

#include <stdbool.h>
#include <stdint.h>

#define SEC0_BASE       0x20004000u
#define SEC0_GCTL       (SEC0_BASE + 0x000u)
#define SEC0_GSTAT      (SEC0_BASE + 0x004u)
#define SEC0_RAISE      (SEC0_BASE + 0x008u)
#define SEC0_END        (SEC0_BASE + 0x00Cu)
#define SEC0_FCTL       (SEC0_BASE + 0x010u)
#define SEC0_CCTL0      (SEC0_BASE + 0x400u)
#define SEC0_CSTAT0     (SEC0_BASE + 0x404u)
#define SEC0_CPND0      (SEC0_BASE + 0x408u)
#define SEC0_CACT0      (SEC0_BASE + 0x40Cu)
#define SEC0_CPMSK0     (SEC0_BASE + 0x410u)
#define SEC0_CGMSK0     (SEC0_BASE + 0x414u)
#define SEC0_CPLVL0     (SEC0_BASE + 0x418u)
#define SEC0_CSID0      (SEC0_BASE + 0x41Cu)
#define SEC0_SCTL(n)    (SEC0_BASE + 0x800u + 8u * (n))
#define SEC0_SSTAT(n)   (SEC0_BASE + 0x804u + 8u * (n))

/* SEC_GCTL, SEC_CCTL[n]. */
#define SEC_CTL_EN      (1u << 0)
#define SEC_CTL_RESET   (1u << 1)
#define SEC_GCTL_LOCK   (1u << 31)

/* SEC_FCTL. */
#define SEC_FCTL_EN     (1u << 0)
#define SEC_FCTL_RESET  (1u << 1)
#define SEC_FCTL_FOEN   (1u << 4)
#define SEC_FCTL_TOEN   (1u << 5)
#define SEC_FCTL_SREN   (1u << 6)
#define SEC_FCTL_FIEN   (1u << 7)

/* SEC_CSTAT[n]. */
#define SEC_CSTAT_SIDV  (1u << 10)

/* SEC_SCTL[n]. */
#define SEC_SCTL_IEN    (1u << 0)
#define SEC_SCTL_FEN    (1u << 1)
#define SEC_SCTL_SEN    (1u << 2)
#define SEC_SCTL_ES     (1u << 3)	/* edge sensitive */
#define SEC_SCTL_ERREN  (1u << 4)
#define SEC_SCTL_PRIO_SHIFT 8u
#define SEC_SCTL_PRIO_MASK  (0xFFu << 8)
#define SEC_SCTL_CTG_SHIFT  24u	/* core target: 0 = core 0 */
#define SEC_SCTL_LOCK   (1u << 31)

/* SEC_SSTAT[n]. */
#define SEC_SSTAT_ERR   (1u << 1)
#define SEC_SSTAT_PND   (1u << 8)
#define SEC_SSTAT_ACT   (1u << 9)

/* Number of interrupt sources (HRM, SEC interrupt list). */
#define WB_SEC_SOURCES  106u

/* Source IDs used by the drivers here (HRM, SEC interrupt list). */
#define WB_SID_CGU0_EVT     2u
#define WB_SID_WDOG0        3u
#define WB_SID_TIMER0_TMR(n) (11u + (n))
#define WB_SID_TIMER0_STAT  19u

typedef void (*wb_irq_handler)(void *ctx, uint32_t sid);

#define WB_SEC_OK          0
#define WB_SEC_ERR_SID    (-1)
#define WB_SEC_ERR_AGAIN  (-2)	/* sources already configured: no reset */

/* Core event vectors and IMASK. */
void wb_cec_set_vector(unsigned ivg, uint32_t addr);
void wb_cec_enable(unsigned ivg);
void wb_cec_disable(unsigned ivg);

/*
 * Reset the SEC (only if no source has been configured since start-up),
 * enable it and its core interface, forget all handlers, point EVT11 at
 * wb_sec_entry, turn branch prediction on (S-13a.042: after the image's
 * last vector write) and unmask IVG11. Returns WB_SEC_ERR_AGAIN, having only
 * made sure the SEC and core interface are enabled, if sources were
 * already configured.
 */
int wb_sec_init(void);

/*
 * Configure a source as an interrupt to core 0 at the given priority (0 is
 * the highest), with its handler. The source stays disabled until
 * wb_sec_enable(). edge selects edge-sensitive (SEC_SCTL.ES).
 */
int wb_sec_attach(uint32_t sid, uint8_t prio, bool edge, wb_irq_handler fn,
		  void *ctx);
int wb_sec_enable(uint32_t sid);
int wb_sec_disable(uint32_t sid);

/*
 * Service one interrupt: read and acknowledge SEC_CSID0, run the handler,
 * write the ID to SEC_END. A source without a handler is disabled. Called
 * from wb_sec_entry at IVG11.
 */
void wb_sec_dispatch(void);

/*
 * Route a source to the fault interface with a system reset as the action
 * (SEC_SCTL.FEN and SEN, SEC_FCTL.EN and SREN), for the watchdog of the
 * hard-reset path (S-13a.061). No RESET bit is written.
 */
int wb_sec_fault_reset(uint32_t sid);

/* The IVG11 entry (src/drivers/bf70x/sec_entry.S). */
void wb_sec_entry(void);

#endif /* WAFFLE_DRIVERS_BF70X_SEC_H */
