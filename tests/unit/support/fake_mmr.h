/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_TESTS_FAKE_MMR_H
#define WAFFLE_TESTS_FAKE_MMR_H

/*
 * Host fake for the register access layer (waffle/drivers/mmr.h).
 *
 * It keeps a sparse register file (every register reads 0 until written
 * or preset), records every access in order, and lets a test:
 *   - preset a register's value (fake_mmr_set), not recorded;
 *   - queue values that successive reads of a register return before
 *     falling back to the register file (fake_mmr_queue_read), e.g. a
 *     status register that is busy twice and then ready;
 *   - install a hook that models a register's behavior (write-1-to-clear
 *     bits, self-clearing bits, a status bit that follows a write);
 *   - compare the recorded accesses with an expected sequence.
 *
 * Misaligned accesses, register file or queue overflow and log overflow
 * are counted as errors (fake_mmr_errors), so a test fails on them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/drivers/mmr.h"

enum fake_mmr_op {
	FAKE_MMR_READ,
	FAKE_MMR_WRITE,
	FAKE_MMR_SYNC,
	FAKE_MMR_CSYNC,		/* core sync, from the core fake (fake_core.h) */
};

struct fake_mmr_access {
	enum fake_mmr_op op;
	unsigned width;		/* 8, 16 or 32; 0 for FAKE_MMR_SYNC */
	wb_mmr_addr addr;
	uint32_t value;		/* value read or written */
};

/* Shorthands for expected sequences. */
#define FAKE_R32(a, v) { FAKE_MMR_READ, 32u, (a), (v) }
#define FAKE_W32(a, v) { FAKE_MMR_WRITE, 32u, (a), (v) }
#define FAKE_R16(a, v) { FAKE_MMR_READ, 16u, (a), (v) }
#define FAKE_W16(a, v) { FAKE_MMR_WRITE, 16u, (a), (v) }
#define FAKE_R8(a, v)  { FAKE_MMR_READ, 8u, (a), (v) }
#define FAKE_W8(a, v)  { FAKE_MMR_WRITE, 8u, (a), (v) }
#define FAKE_SYNC      { FAKE_MMR_SYNC, 0u, 0u, 0u }
#define FAKE_CSYNC     { FAKE_MMR_CSYNC, 0u, 0u, 0u }

/*
 * A hook sees every read and write. For a read, *value holds what the
 * fake would return and the hook may change it. For a write, *value holds
 * the value written and the hook may change what is stored. Returning
 * false leaves the register file untouched for a write (the hook has
 * stored what it wants with fake_mmr_set).
 */
typedef bool (*fake_mmr_hook)(void *ctx, enum fake_mmr_op op,
			      unsigned width, wb_mmr_addr addr,
			      uint32_t *value);

/* Forget everything: registers, queues, log, hook and error count. */
void fake_mmr_reset(void);

/* Set or read a register file entry without recording an access. */
void fake_mmr_set(wb_mmr_addr addr, uint32_t value);
uint32_t fake_mmr_get(wb_mmr_addr addr);

/* Queue a value for the next not-yet-answered read of addr. */
void fake_mmr_queue_read(wb_mmr_addr addr, uint32_t value);

void fake_mmr_set_hook(fake_mmr_hook hook, void *ctx);

/* The recorded accesses, oldest first. */
size_t fake_mmr_count(void);
const struct fake_mmr_access *fake_mmr_log(void);
void fake_mmr_clear_log(void);

/* Writes only, oldest first; returns how many were copied into out. */
size_t fake_mmr_writes(struct fake_mmr_access *out, size_t max);

/* Record a core sync in the access log (used by the core fake). */
void fake_mmr_record_csync(void);

/* Number of misuse errors seen since the last reset. */
unsigned fake_mmr_errors(void);

/*
 * Compare the whole log (or, for fake_mmr_expect_writes, only its writes)
 * with an expected sequence. On a mismatch print the first difference,
 * prefixed with name, and return false.
 */
bool fake_mmr_expect(const char *name, const struct fake_mmr_access *seq,
		     size_t n);
bool fake_mmr_expect_writes(const char *name,
			    const struct fake_mmr_access *seq, size_t n);

#endif /* WAFFLE_TESTS_FAKE_MMR_H */
