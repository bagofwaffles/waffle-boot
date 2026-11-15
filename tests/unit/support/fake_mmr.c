/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fake_mmr.h"

#include <stdio.h>
#include <string.h>

#define MAX_REGS   512u
#define MAX_QUEUED 256u
#define MAX_LOG    8192u

struct reg {
	wb_mmr_addr addr;
	uint32_t value;
};

static struct reg regs[MAX_REGS];
static size_t nregs;

static struct reg queued[MAX_QUEUED];
static size_t nqueued;

static struct fake_mmr_access log_buf[MAX_LOG];
static size_t nlog;

static fake_mmr_hook hook_fn;
static void *hook_ctx;
static unsigned errors;

void fake_mmr_reset(void)
{
	nregs = 0;
	nqueued = 0;
	nlog = 0;
	hook_fn = NULL;
	hook_ctx = NULL;
	errors = 0;
}

static struct reg *find_reg(wb_mmr_addr addr, bool create)
{
	size_t i;

	for (i = 0; i < nregs; i++)
		if (regs[i].addr == addr)
			return &regs[i];
	if (!create)
		return NULL;
	if (nregs == MAX_REGS) {
		errors++;
		return NULL;
	}
	regs[nregs].addr = addr;
	regs[nregs].value = 0;
	return &regs[nregs++];
}

void fake_mmr_set(wb_mmr_addr addr, uint32_t value)
{
	struct reg *r = find_reg(addr, true);

	if (r)
		r->value = value;
}

uint32_t fake_mmr_get(wb_mmr_addr addr)
{
	const struct reg *r = find_reg(addr, false);

	return r ? r->value : 0;
}

void fake_mmr_queue_read(wb_mmr_addr addr, uint32_t value)
{
	if (nqueued == MAX_QUEUED) {
		errors++;
		return;
	}
	queued[nqueued].addr = addr;
	queued[nqueued].value = value;
	nqueued++;
}

static bool pop_queued(wb_mmr_addr addr, uint32_t *value)
{
	size_t i;

	for (i = 0; i < nqueued; i++) {
		if (queued[i].addr == addr) {
			*value = queued[i].value;
			memmove(&queued[i], &queued[i + 1],
				(nqueued - i - 1) * sizeof(queued[0]));
			nqueued--;
			return true;
		}
	}
	return false;
}

void fake_mmr_set_hook(fake_mmr_hook hook, void *ctx)
{
	hook_fn = hook;
	hook_ctx = ctx;
}

static void record(enum fake_mmr_op op, unsigned width, wb_mmr_addr addr,
		   uint32_t value)
{
	if (nlog == MAX_LOG) {
		errors++;
		return;
	}
	log_buf[nlog].op = op;
	log_buf[nlog].width = width;
	log_buf[nlog].addr = addr;
	log_buf[nlog].value = value;
	nlog++;
}

static uint32_t width_mask(unsigned width)
{
	return width == 32u ? 0xffffffffu : (1u << width) - 1u;
}

static uint32_t do_read(unsigned width, wb_mmr_addr addr)
{
	uint32_t value;

	if (addr % (width / 8u) != 0)
		errors++;
	if (!pop_queued(addr, &value))
		value = fake_mmr_get(addr);
	if (hook_fn)
		(void)hook_fn(hook_ctx, FAKE_MMR_READ, width, addr, &value);
	value &= width_mask(width);
	record(FAKE_MMR_READ, width, addr, value);
	return value;
}

static void do_write(unsigned width, wb_mmr_addr addr, uint32_t value)
{
	uint32_t stored = value;

	if (addr % (width / 8u) != 0)
		errors++;
	record(FAKE_MMR_WRITE, width, addr, value);
	if (hook_fn && !hook_fn(hook_ctx, FAKE_MMR_WRITE, width, addr, &stored))
		return;
	fake_mmr_set(addr, stored & width_mask(width));
}

uint8_t wb_mmr_read8(wb_mmr_addr addr)
{
	return (uint8_t)do_read(8u, addr);
}

uint16_t wb_mmr_read16(wb_mmr_addr addr)
{
	return (uint16_t)do_read(16u, addr);
}

uint32_t wb_mmr_read32(wb_mmr_addr addr)
{
	return do_read(32u, addr);
}

void wb_mmr_write8(wb_mmr_addr addr, uint8_t val)
{
	do_write(8u, addr, val);
}

void wb_mmr_write16(wb_mmr_addr addr, uint16_t val)
{
	do_write(16u, addr, val);
}

void wb_mmr_write32(wb_mmr_addr addr, uint32_t val)
{
	do_write(32u, addr, val);
}

void wb_mmr_sync(void)
{
	record(FAKE_MMR_SYNC, 0u, 0u, 0u);
}

void fake_mmr_record_csync(void)
{
	record(FAKE_MMR_CSYNC, 0u, 0u, 0u);
}

size_t fake_mmr_count(void)
{
	return nlog;
}

const struct fake_mmr_access *fake_mmr_log(void)
{
	return log_buf;
}

void fake_mmr_clear_log(void)
{
	nlog = 0;
}

size_t fake_mmr_writes(struct fake_mmr_access *out, size_t max)
{
	size_t i, n = 0;

	for (i = 0; i < nlog && n < max; i++)
		if (log_buf[i].op == FAKE_MMR_WRITE)
			out[n++] = log_buf[i];
	return n;
}

unsigned fake_mmr_errors(void)
{
	return errors;
}

static const char *op_name(enum fake_mmr_op op)
{
	switch (op) {
	case FAKE_MMR_READ:
		return "read";
	case FAKE_MMR_WRITE:
		return "write";
	case FAKE_MMR_SYNC:
		return "sync";
	case FAKE_MMR_CSYNC:
		return "csync";
	default:
		return "?";
	}
}

static void print_access(const char *what, const struct fake_mmr_access *a)
{
	printf("    %s: %s%u 0x%08lx = 0x%08lx\n", what, op_name(a->op),
	       a->width, (unsigned long)a->addr, (unsigned long)a->value);
}

static bool same(const struct fake_mmr_access *a,
		 const struct fake_mmr_access *b)
{
	return a->op == b->op && a->width == b->width && a->addr == b->addr &&
	       a->value == b->value;
}

static bool compare(const char *name, const struct fake_mmr_access *got,
		    size_t ngot, const struct fake_mmr_access *seq, size_t n)
{
	size_t i;

	for (i = 0; i < ngot && i < n; i++) {
		if (!same(&got[i], &seq[i])) {
			printf("  %s: access %zu differs\n", name, i);
			print_access("expected", &seq[i]);
			print_access("got     ", &got[i]);
			return false;
		}
	}
	if (ngot != n) {
		printf("  %s: expected %zu accesses, got %zu\n", name, n, ngot);
		if (i < ngot)
			print_access("first extra", &got[i]);
		else
			print_access("first missing", &seq[i]);
		return false;
	}
	return true;
}

bool fake_mmr_expect(const char *name, const struct fake_mmr_access *seq,
		     size_t n)
{
	return compare(name, log_buf, nlog, seq, n);
}

bool fake_mmr_expect_writes(const char *name,
			    const struct fake_mmr_access *seq, size_t n)
{
	static struct fake_mmr_access writes[MAX_LOG];
	size_t nw = fake_mmr_writes(writes, MAX_LOG);

	return compare(name, writes, nw, seq, n);
}
