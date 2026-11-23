/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-02.095: the boot menu's mark in a trial start (opxy_trial_reached(),
 * src/board/opxy_front.c): byte 0 of the tried slot's last sector, 0xFC (the
 * front's trial start mark) to 0xF0, once, at held mode's first pass, only
 * in a start of a tried slot that the selector does not name, and only over
 * a sector that holds the front's mark alone. And that the front reads the
 * marked sector as a valid log, with no trial pending (test_front_startlog
 * has the front's own cases).
 */
#include "support/check.h"

#include <string.h>

#include "front/fr_startlog.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"
#include "waffle/board/opxy_boot.h"

static struct opxy_master m;
static uint8_t mem[S02_FLASH_SIZE];
static uint8_t scratch[512];
static unsigned programs, erases;

static int ram_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if ((uint64_t)off + len > sizeof mem)
		return -1;
	memcpy(buf, mem + off, len);
	return 0;
}

static int ram_erase(void *ctx, uint32_t off)
{
	(void)ctx;
	(void)off;
	erases++;
	return -1;
}

static int ram_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	(void)ctx;
	programs++;
	for (uint32_t i = 0; i < len; i++)
		mem[off + i] &= buf[i];
	return 0;
}

static const struct s02_flash_ops ops = { NULL, ram_read, ram_erase, ram_program };

/* Slot 1 tried and started by its front, the selector naming slot 0. */
static void setup(void)
{
	fake_core_reset();		/* the board's core and registers are the fakes' */
	fake_mmr_reset();
	memset(mem, 0xFF, sizeof mem);
	mem[S02_SELECTOR_OFF] = 0x00u;
	mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	mem[FR_LOG_OFF(1)] = OPXY_TRIAL_STARTED_BYTE;
	memset(&m, 0, sizeof m);
	m.flash.flash = &ops;
	m.flash.scratch = scratch;
	m.flash.scratch_len = sizeof scratch;
	m.front = true;
	m.running_established = true;
	m.running_slot = 1u;
	m.running_trial = true;
	programs = erases = 0;
}

static void test_marks_once(void)
{
	setup();
	opxy_trial_reached(&m);
	CHECK(mem[FR_LOG_OFF(1)] == OPXY_TRIAL_REACHED_BYTE);
	CHECK(programs == 1u && erases == 0u);
	CHECK(m.trial_reached);	/* made and read back */
	opxy_trial_reached(&m);
	CHECK(programs == 1u);
	/* slot 0's log untouched */
	CHECK(mem[FR_LOG_OFF(0)] == 0xFFu);
}

/* A program that leaves the byte otherwise: no going on (opxy_trial_continue()) */
static int bad_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	(void)ctx;
	(void)buf;
	(void)len;
	programs++;
	mem[off] &= 0xF8u;
	return 0;
}

static void test_not_read_back(void)
{
	static const struct s02_flash_ops bad = { NULL, ram_read, ram_erase, bad_program };

	setup();
	m.flash.flash = &bad;
	opxy_trial_reached(&m);
	CHECK(programs == 1u && mem[FR_LOG_OFF(1)] != OPXY_TRIAL_REACHED_BYTE && !m.trial_reached);
}

static void test_not_a_trial_start(void)
{
	setup();
	m.running_trial = false;		/* the record is not tried */
	opxy_trial_reached(&m);
	CHECK(programs == 0u && !m.trial_reached);

	setup();
	m.front = false;			/* no front's record */
	opxy_trial_reached(&m);
	CHECK(programs == 0u);

	setup();
	mem[S02_SELECTOR_OFF] = 0x01u;		/* the selector names the running slot */
	opxy_trial_reached(&m);
	CHECK(programs == 0u);

	setup();
	mem[FR_LOG_OFF(1)] = 0xFDu;		/* the front made no mark */
	opxy_trial_reached(&m);
	CHECK(programs == 0u);

	setup();
	mem[FR_LOG_OFF(1)] = 0xF9u;		/* spent */
	opxy_trial_reached(&m);
	CHECK(programs == 0u);

	setup();
	mem[FR_LOG_OFF(1) + 3000u] = 0x7Fu;	/* something else in the sector */
	opxy_trial_reached(&m);
	CHECK(programs == 0u);

	setup();
	m.running_slot = 2u;
	opxy_trial_reached(&m);
	CHECK(programs == 0u);
}

/* Slot 0 as the tried slot, selector naming slot 1 (index 0x01). */
static void test_slot0(void)
{
	uint32_t off = 0;

	setup();
	mem[FR_LOG_OFF(1)] = 0xFFu;
	mem[FR_LOG_OFF(0)] = OPXY_TRIAL_STARTED_BYTE;
	mem[S02_SELECTOR_OFF] = 0x01u;
	m.running_slot = 0u;
	CHECK(opxy_trial_reached_at(&m.flash, 0u, &off) && off == FR_LOG_OFF(0));
	opxy_trial_reached(&m);
	CHECK(mem[FR_LOG_OFF(0)] == OPXY_TRIAL_REACHED_BYTE);
}

/* The front's reading of a marked sector, once that slot is selected (after
 * the commit): a valid log, entries 0 and 1 finished, the next start entry 2;
 * and, while the other slot is selected, no trial pending in it. */
static int fr_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	return ram_read(ctx, off, buf, len);
}

static void test_front_reads_it(void)
{
	struct fr_flash f;
	struct fr_log l;
	unsigned slot = 2u;

	setup();
	opxy_trial_reached(&m);
	/* the tried slot's record, as a trial leaves it: the tag and a length */
	mem[0xF000 + 4] = 0x00u;
	mem[0xF000 + 5] = 0x10u;
	mem[0xF000 + 6] = 0x00u;
	mem[0xF000 + 7] = 0x00u;
	mem[0xF000 + 8] = (uint8_t)S02_RECORD_TAG;
	mem[0xF000 + 9] = (uint8_t)(S02_RECORD_TAG >> 8);
	mem[0xF000 + 10] = 0x00u;
	mem[0xF000 + 11] = 0x00u;
	memset(&f, 0, sizeof f);
	f.read = fr_read;
	fr_log_scan(&f, &l);
	CHECK(l.slot == 0u);
	CHECK(!fr_log_trial_pending(&f, &l, &slot));
	mem[S02_SELECTOR_OFF] = 0x01u;		/* committed: slot 1 named */
	mem[S02_SELECTOR_OFF + 1u] = 0x00u;
	fr_log_scan(&f, &l);
	CHECK(l.slot == 1u);
	CHECK(l.state == FR_LOG_OK);
	CHECK(l.next == 2u && l.unfinished == 0u);
}

int main(void)
{
	RUN(test_marks_once);
	RUN(test_not_a_trial_start);
	RUN(test_not_read_back);
	RUN(test_slot0);
	RUN(test_front_reads_it);
	return check_summary("test_front_trial_mark");
}
