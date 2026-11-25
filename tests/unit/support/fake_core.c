/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fake_core.h"

#include <stdbool.h>

#include "fake_mmr.h"
#include "waffle/drivers/bf70x/sec.h"

static uint32_t imask;
static uint64_t cycles;
static uint64_t cycle_step;
static uint64_t idle_step;
static unsigned idles, flushes, flushinvs;
static unsigned cycle_starts, early_reads;
static unsigned bp_ons;
static size_t bp_on_at;
static uint32_t last_line;
static void (*cycle_hook)(void *ctx);
static void *cycle_hook_ctx;
static void (*idle_hook)(void *ctx);
static void *idle_hook_ctx;
static bool in_hook;

void fake_core_reset(void)
{
	imask = 0;
	cycles = 0;
	cycle_step = 1;
	idle_step = 1;
	idles = 0;
	flushes = 0;
	flushinvs = 0;
	cycle_starts = 0;
	early_reads = 0;
	bp_ons = 0;
	bp_on_at = 0;
	last_line = 0;
	cycle_hook = NULL;
	cycle_hook_ctx = NULL;
	idle_hook = NULL;
	idle_hook_ctx = NULL;
}

void fake_core_set_idle_hook(void (*hook)(void *ctx), void *ctx)
{
	idle_hook = hook;
	idle_hook_ctx = ctx;
}

void fake_core_set_cycle_hook(void (*hook)(void *ctx), void *ctx)
{
	cycle_hook = hook;
	cycle_hook_ctx = ctx;
}

void fake_core_set_imask(uint32_t mask)
{
	imask = mask;
}

uint32_t fake_core_imask(void)
{
	return imask;
}

void fake_core_set_cycles(uint64_t c)
{
	cycles = c;
}

uint64_t fake_core_peek_cycles(void)
{
	return cycles;
}

void fake_core_set_cycle_step(uint64_t step)
{
	cycle_step = step;
}

void fake_core_set_idle_step(uint64_t step)
{
	idle_step = step;
}

unsigned fake_core_idles(void)
{
	return idles;
}

unsigned fake_core_flushes(void)
{
	return flushes;
}

unsigned fake_core_flushinvs(void)
{
	return flushinvs;
}

uint32_t fake_core_last_line(void)
{
	return last_line;
}

void wb_core_csync(void)
{
	fake_mmr_record_csync();
}

uint32_t wb_core_cli(void)
{
	uint32_t old = imask;

	imask &= 0x1fu;		/* CLI clears IMASK[15:5] */
	return old;
}

void wb_core_sti(uint32_t mask)
{
	imask = mask;
}

unsigned fake_core_cycle_starts(void)
{
	return cycle_starts;
}

unsigned fake_core_early_cycle_reads(void)
{
	return early_reads;
}

void wb_core_cycles_start(void)
{
	cycle_starts++;
}

uint64_t wb_core_cycles(void)
{
	uint64_t now = cycles;

	if (cycle_starts == 0)
		early_reads++;

	cycles += cycle_step;
	if (cycle_hook && !in_hook) {
		in_hook = true;
		cycle_hook(cycle_hook_ctx);
		in_hook = false;
	}
	return now;
}

void wb_core_cycles_add(uint64_t delta)
{
	cycles += delta;
}

unsigned fake_core_bp_ons(void)
{
	return bp_ons;
}

size_t fake_core_bp_on_at(void)
{
	return bp_on_at;
}

void wb_core_bp_on(void)
{
	bp_ons++;
	bp_on_at = fake_mmr_count();
}

void wb_core_idle(void)
{
	idles++;
	cycles += idle_step;
	if (idle_hook)
		idle_hook(idle_hook_ctx);
}

void wb_core_flush_line(uint32_t addr)
{
	flushes++;
	last_line = addr;
}

void wb_core_flushinv_line(uint32_t addr)
{
	flushinvs++;
	last_line = addr;
}

/* The IVG11 entry is assembly on the target; tests only need its address. */
void wb_sec_entry(void)
{
}
