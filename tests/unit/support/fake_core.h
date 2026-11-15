/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_TESTS_FAKE_CORE_H
#define WAFFLE_TESTS_FAKE_CORE_H

/*
 * Host fake for the core operations (waffle/drivers/bf70x_core.h).
 *
 * - csync is recorded in the register access log (fake_mmr.h), so a test
 *   sees it in order with the register accesses;
 * - IMASK is a variable: cli returns it and clears bits 15:5, sti loads it;
 * - the cycle counter advances by a set step on every read, so polling
 *   loops see time pass; a test can also set it;
 * - wb_core_cycles_start() (SYSCFG.CCEN) is counted, and so is every
 *   counter read made before the first one; the counter advances either
 *   way, so tests that do not start it keep working;
 * - idle advances the counter by the idle step (the time an interrupt
 *   would take to arrive), then runs the idle hook, if any (a test can
 *   leave a function that never returns through it);
 * - wb_core_cycles_add() adds to the counter and is counted;
 * - wb_core_bp_on() (SYSCFG.BPEN) is counted, with the register access
 *   log's length at the last call;
 * - a hook may run on every counter read, as an interrupt would arrive
 *   while a loop polls the time (not from inside itself);
 * - cache line operations are counted with their last address.
 * It also stands in for wb_sec_entry, the target's IVG11 assembly entry.
 */

#include <stddef.h>
#include <stdint.h>

#include "waffle/drivers/bf70x_core.h"

void fake_core_reset(void);

void fake_core_set_imask(uint32_t mask);
uint32_t fake_core_imask(void);

void fake_core_set_cycles(uint64_t cycles);
uint64_t fake_core_peek_cycles(void);
/* Added on every wb_core_cycles() read (default 1) and every idle. */
void fake_core_set_cycle_step(uint64_t step);
void fake_core_set_idle_step(uint64_t step);

void fake_core_set_cycle_hook(void (*hook)(void *ctx), void *ctx);
void fake_core_set_idle_hook(void (*hook)(void *ctx), void *ctx);

unsigned fake_core_idles(void);
unsigned fake_core_flushes(void);	/* FLUSH */
unsigned fake_core_flushinvs(void);	/* FLUSHINV */
uint32_t fake_core_last_line(void);
unsigned fake_core_cycle_starts(void);	/* wb_core_cycles_start() calls */
unsigned fake_core_early_cycle_reads(void);	/* reads before the first */
unsigned fake_core_bp_ons(void);		/* wb_core_bp_on() calls */
size_t fake_core_bp_on_at(void);	/* fake_mmr_count() at the last */

#endif /* WAFFLE_TESTS_FAKE_CORE_H */
