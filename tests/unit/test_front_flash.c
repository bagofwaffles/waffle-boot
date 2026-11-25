/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's SPI flash (src/front/hw/flash.c, built here for the host
 * since the front links it for the target only), against a model of the
 * part on SPI2: without an error the frames of S-13b.013-.015 (the run's
 * first frame an RDSR, WREN before each SE and PP, RDSR until WIP = 0);
 * S-13b.017, a frame whose transfer ends with a controller error (a receive
 * overrun) sent again from its first frame, the WREN included, after an
 * RDSR has read WIP = 0, up to three tries once the front has erased or
 * programmed, one try before; a timeout never tried again. A part that
 * never reads ready: FR_FLASH_BUSY after one bounded wait, and one such
 * wait in the first stage's fingerprint.
 */
#include <stdio.h>
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "front/hw/flash.c"
#include "front/start/device.h"
#include "front/handoff.h"

#define N 2u
#define SIZE 0x100000u

/* The part and the controller's FIFOs, one word in flight. */
static struct {
	bool selected, mute, wel, wip;
	uint8_t frame[300];
	size_t flen;
	uint32_t rx;
	bool rx_full;
	uint32_t stat;
	unsigned busy_polls;	/* RDSRs that still read WIP = 1 */
	uint8_t mem[SIZE];
	char ops[128];		/* R READ, S RDSR, W WREN, E SE, P PP */
	unsigned nops, violations;
	/* a receive overrun in the next fail_left frames with opcode
	 * fail_op, in their last transfer's first word */
	uint8_t fail_op;
	unsigned fail_left;
} d;

static uint8_t reply(uint8_t tx)
{
	const size_t pos = d.flen;
	uint32_t a;

	if (pos < sizeof d.frame)
		d.frame[pos] = tx;
	d.flen++;
	if (pos == 0u)
		return 0xFFu;
	switch (d.frame[0]) {
	case OP_RDSR:
		return d.wip ? SR_WIP : 0u;
	case OP_READ:
		if (pos < 4u)
			return 0xFFu;
		a = (uint32_t)d.frame[1] << 16 | (uint32_t)d.frame[2] << 8 | d.frame[3];
		return d.mem[(a + pos - 4u) % SIZE];
	default:
		return 0xFFu;
	}
}

static void end_frame(void)
{
	const uint8_t op = d.frame[0];
	const uint32_t a = (uint32_t)d.frame[1] << 16 | (uint32_t)d.frame[2] << 8 | d.frame[3];
	char c = '?';

	if (d.flen == 0u)
		return;
	if (d.wip && op != OP_RDSR)
		d.violations++;			/* S-13b.015 */
	switch (op) {
	case OP_RDSR:
		c = 'S';
		if (d.wip && d.busy_polls > 0u && --d.busy_polls == 0u)
			d.wip = false;
		break;
	case OP_WREN:
		c = 'W';
		d.wel = d.flen == 1u;
		break;
	case OP_SE:
	case OP_PP:
		c = op == OP_SE ? 'E' : 'P';
		if (!d.wel)
			d.violations++;		/* S-13b.014 */
		if (op == OP_SE) {
			memset(d.mem + (a & ~0xFFFu), 0xFF, 0x1000u);
		} else {
			for (size_t i = 4u; i < d.flen && i < sizeof d.frame; i++)
				d.mem[(a + i - 4u) % SIZE] &= d.frame[i];
		}
		d.wel = false;
		d.wip = true;
		d.busy_polls = 2u;
		break;
	case OP_READ:
		c = 'R';
		d.wel = false;
		break;
	default:
		d.violations++;
		break;
	}
	if (d.nops + 1u < sizeof d.ops)
		d.ops[d.nops++] = c;
	d.flen = 0u;
}

static bool hook(void *ctx, enum fake_mmr_op op, unsigned width, wb_mmr_addr addr,
		 uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_READ) {
		if (addr == SPI_STAT(N))
			*value = d.stat | (d.rx_full ? 0u : SPI_STAT_RFE);
		else if (addr == SPI_RFIFO(N) && d.rx_full) {
			*value = d.rx;
			d.rx_full = false;
		}
		return true;
	}
	if (op != FAKE_MMR_WRITE)
		return true;
	if (addr == SPI_STAT(N)) {
		d.stat &= ~(*value & SPI_STAT_W1C);
	} else if (addr == SPI_TFIFO(N)) {
		if (!d.selected)
			d.violations++;
		if (d.mute)
			return true;
		d.rx = reply((uint8_t)*value);
		d.rx_full = true;
		if (d.fail_left > 0u && d.frame[0] == d.fail_op &&
		    d.flen - 1u == (d.fail_op == OP_WREN ? 0u : d.fail_op == OP_SE ? 3u : 4u)) {
			d.stat |= SPI_STAT_ROR;
			d.fail_left--;
		}
	} else if (addr == SPI_SLVSEL(N)) {
		const bool on = (*value & SPI_SLVSEL_SSE(FLASH_SEL)) &&
				!(*value & SPI_SLVSEL_SSEL(FLASH_SEL));

		if (d.selected && !on)
			end_frame();
		d.selected = on;
	}
	return true;
}

/* A fresh run of the front: its flash started, nothing written yet. */
static void start(void)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(1000u);
	memset(&d, 0, sizeof d);
	memset(d.mem, 0xFF, sizeof d.mem);
	fake_mmr_set_hook(hook, NULL);
	writing = false;
	ctl_err = false;
	fr_hw_flash_start();
}

static bool ops_are(const char *want)
{
	if (strcmp(d.ops, want) == 0)
		return true;
	printf("  ops %s, want %s\n", d.ops, want);
	return false;
}

static void clear_ops(void)
{
	memset(d.ops, 0, sizeof d.ops);
	d.nops = 0u;
}

static const struct fr_flash *const f = &fr_hw_flash;

/* No error: the frames as they were. */
static void test_frames(void)
{
	static const uint8_t data[2] = { 0x12, 0x34 };
	uint8_t b[2];

	start();
	d.mem[0x100] = 0xA5u;
	CHECK(f->read(f->ctx, 0x100u, b, 2u) == FR_FLASH_OK && b[0] == 0xA5u);
	CHECK(ops_are("SR"));			/* the run's first frame an RDSR */
	clear_ops();
	CHECK(f->erase(f->ctx, 0x3000u) == FR_FLASH_OK);
	CHECK(f->program(f->ctx, 0x3010u, data, 2u) == FR_FLASH_OK);
	CHECK(f->read(f->ctx, 0x3010u, b, 2u) == FR_FLASH_OK);
	CHECK(b[0] == 0x12u && b[1] == 0x34u);
	CHECK(ops_are("WESSSWPSSSR"));
	CHECK(d.violations == 0u && !busy);
}

/* A controller error once in an SE, a PP and a WREN: sent again from the
 * WREN after an RDSR has read WIP = 0, and the operation completes. */
static void test_write_once(void)
{
	static const uint8_t data[1] = { 0x5Au };

	start();
	d.mem[0x3000] = 0u;
	d.fail_op = OP_SE;
	d.fail_left = 1u;
	CHECK(f->erase(f->ctx, 0x3000u) == FR_FLASH_OK);
	CHECK(d.mem[0x3000] == 0xFFu);
	CHECK(ops_are("SWESSSWESSS"));
	CHECK(d.violations == 0u);

	start();
	d.fail_op = OP_PP;
	d.fail_left = 1u;
	CHECK(f->program(f->ctx, 0x3000u, data, 1u) == FR_FLASH_OK);
	CHECK(d.mem[0x3000] == 0x5Au);
	CHECK(ops_are("SWPSSSWPSSS"));
	CHECK(d.violations == 0u);

	start();
	d.fail_op = OP_WREN;
	d.fail_left = 1u;
	CHECK(f->erase(f->ctx, 0x3000u) == FR_FLASH_OK);
	CHECK(ops_are("SWSWESSS"));
	CHECK(d.violations == 0u);
}

/* On every try: three, then the failure, the part left busy, so the next
 * operation waits on RDSR first. */
static void test_write_every_try(void)
{
	uint8_t b;

	start();
	d.fail_op = OP_SE;
	d.fail_left = 100u;
	CHECK(f->erase(f->ctx, 0x3000u) == FR_FLASH_IO);
	CHECK(ops_are("SWESSSWESSSWE"));
	CHECK(busy);
	d.fail_left = 0u;
	CHECK(f->read(f->ctx, 0u, &b, 1u) == FR_FLASH_OK);
	CHECK(ops_are("SWESSSWESSSWESSSR"));
	CHECK(d.violations == 0u && !busy);
}

/* A timeout is not a controller error: no second try. */
static void test_timeout(void)
{
	start();
	(void)f->read(f->ctx, 0u, d.mem, 1u);	/* the run's first RDSR */
	clear_ops();
	fake_mmr_clear_log();
	d.mute = true;
	CHECK(f->erase(f->ctx, 0x3000u) == FR_FLASH_IO);
	CHECK(busy);
	d.mute = false;
	CHECK(d.nops == 0u);			/* the muted WREN ended no frame */
	{
		unsigned tfifo = 0u;
		const struct fake_mmr_access *log = fake_mmr_log();

		for (size_t i = 0; i < fake_mmr_count(); i++)
			if (log[i].op == FAKE_MMR_WRITE && log[i].addr == SPI_TFIFO(N))
				tfifo++;
		CHECK(tfifo == 1u);		/* the WREN's one byte, once */
	}
}

/* READs: one try before the front's first erase or program; afterwards up
 * to three, each after an RDSR that reads WIP = 0. */
static void test_reads(void)
{
	uint8_t b;

	start();
	d.fail_op = OP_READ;
	d.fail_left = 1u;
	CHECK(f->read(f->ctx, 0u, &b, 1u) == FR_FLASH_IO);
	CHECK(ops_are("SR"));

	start();
	CHECK(f->erase(f->ctx, 0x3000u) == FR_FLASH_OK);
	clear_ops();
	d.mem[0x3000] = 0x77u;
	d.fail_op = OP_READ;
	d.fail_left = 1u;
	CHECK(f->read(f->ctx, 0x3000u, &b, 1u) == FR_FLASH_OK && b == 0x77u);
	CHECK(ops_are("RSR"));
	clear_ops();
	d.fail_left = 100u;
	CHECK(f->read(f->ctx, 0x3000u, &b, 1u) == FR_FLASH_IO);
	CHECK(ops_are("RSRSR"));
	CHECK(d.violations == 0u);
}

static unsigned rdsrs(void)
{
	unsigned n = 0;

	for (unsigned i = 0; i < d.nops; i++)
		n += d.ops[i] == 'S' ? 1u : 0u;
	return n;
}

/* A part that never reads ready (WIP stuck, or every bit read 1): the
 * read's wait gives up after its bound with FR_FLASH_BUSY and no READ;
 * the first stage's fingerprint then costs that one wait and no more. */
static void test_stuck(void)
{
	struct fr_ident id;
	uint8_t b;
	unsigned one;

	start();
	fake_core_set_cycle_step(50000000u);	/* 0.125 s a read of the counter */
	d.wip = true;
	CHECK(f->read(f->ctx, 0u, &b, 1u) == FR_FLASH_BUSY);
	one = rdsrs();
	CHECK(one >= 2u && one == d.nops && busy);
	start();
	fake_core_set_cycle_step(50000000u);
	d.wip = true;
	fr_identify(f, &id);
	CHECK(rdsrs() == one && d.nops == one);
	CHECK(id.dev == &fr_dev_opxy && id.how == FR_HO_HOW_UNKNOWN && id.n == 0u);
	/* a READ whose transfer fails is not FR_FLASH_BUSY */
	start();
	d.fail_op = OP_READ;
	d.fail_left = 1u;
	CHECK(f->read(f->ctx, 0u, &b, 1u) == FR_FLASH_IO);
	CHECK(d.violations == 0u);
}

int main(void)
{
	RUN(test_frames);
	RUN(test_write_once);
	RUN(test_write_every_try);
	RUN(test_timeout);
	RUN(test_reads);
	RUN(test_stuck);
	return check_summary("test_front_flash");
}
