/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only RAM fake of the SPI NOR flash (S-02.019), with an op log and
 * the injections of S-02's scenarios: cut after an operation,
 * tear one, fail one, corrupt a program.  Also a model of the
 * factory first stage (S-02.020 to S-02.026), the environment contract.
 */
#ifndef S02_FAKE_FLASH_H
#define S02_FAKE_FLASH_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "waffle/s02_flash.h"
#include "s02_ref_crc.h"

#define FF_LOG_MAX 8192u

enum ff_kind { FF_READ = 'r', FF_ERASE = 'e', FF_PROGRAM = 'p' };

/* How a torn operation picks the bits it reaches: per byte of the op. */
enum ff_tear { FF_TEAR_SEED, FF_TEAR_ZEROS, FF_TEAR_ONES, FF_TEAR_BIT };

struct ff_op {
	char kind;
	uint32_t off;
	uint32_t len;
};

struct fake_flash {
	uint8_t mem[S02_FLASH_SIZE];
	struct ff_op log[FF_LOG_MAX];
	unsigned nlog;
	unsigned nmut;		/* erases and programs attempted */
	unsigned nall;		/* every op attempted */
	/* Injections, 1-based op numbers, 0 = none. */
	unsigned cut_after;	/* power lost after this op completes */
	bool count_reads;	/* cut_after and tear count reads too */
	unsigned tear;		/* this erase or program is torn, then power lost */
	enum ff_tear tear_kind;
	uint32_t tear_a, tear_b;	/* seed, or byte range, or byte.bit */
	unsigned fail;		/* this erase or program reports failure */
	unsigned corrupt;	/* this program stores data AND corrupt_mask */
	bool corrupt_on;	/* and every program after it */
	uint8_t corrupt_mask;
	unsigned fail_read;	/* this read (1-based over reads) fails */
	unsigned garble_read;	/* this read (1-based over reads) returns byte 0
				 * with bit 0 flipped and reports success */
	unsigned ignore;	/* this erase or program reports success and
				 * changes nothing (a protected area) */
	unsigned fail_regions;	/* every read that starts in one of these FF_R_* fails */
	unsigned nreads;
	bool dead;		/* power has been lost */
	bool bad_call;		/* an unaligned erase or a page-crossing program */
};

static inline uint8_t ff_tear_mask(const struct fake_flash *f, uint32_t i, uint32_t *rng)
{
	switch (f->tear_kind) {
	case FF_TEAR_SEED:
		*rng ^= *rng << 13;
		*rng ^= *rng >> 17;
		*rng ^= *rng << 5;
		return (uint8_t)(*rng >> 7);
	case FF_TEAR_ZEROS:	/* mask 0 for bytes a..b-1, 0xff elsewhere */
		return (i >= f->tear_a && i < f->tear_b) ? 0x00u : 0xFFu;
	case FF_TEAR_ONES:
		return (i >= f->tear_a && i < f->tear_b) ? 0xFFu : 0x00u;
	case FF_TEAR_BIT:
		return i == f->tear_a ? (uint8_t)(1u << f->tear_b) : 0x00u;
	default:
		return 0;
	}
}

static inline void ff_log(struct fake_flash *f, char kind, uint32_t off, uint32_t len)
{
	if (f->nlog < FF_LOG_MAX) {
		f->log[f->nlog].kind = kind;
		f->log[f->nlog].off = off;
		f->log[f->nlog].len = len;
	}
	f->nlog++;
}

/* Called after an op completes: loses power if this was the cut point. */
static inline void ff_after(struct fake_flash *f, bool mut)
{
	unsigned n = f->count_reads ? f->nall : (mut ? f->nmut : 0u);

	if (f->cut_after != 0u && n == f->cut_after && (mut || f->count_reads))
		f->dead = true;
}

/* Regions for fail_regions. */
#define FF_R_SELECTOR 0x01u
#define FF_R_RECORD0  0x02u
#define FF_R_SLOT0    0x04u
#define FF_R_RECORD1  0x08u
#define FF_R_SLOT1    0x10u

static inline unsigned ff_region(uint32_t off)
{
	if (off >= S02_SLOT1_OFF)
		return FF_R_SLOT1;
	if (off >= S02_SLOT0_OFF)
		return FF_R_SLOT0;
	if (off >= S02_RECORD1_OFF)
		return FF_R_RECORD1;
	if (off >= S02_RECORD0_OFF)
		return FF_R_RECORD0;
	if (off >= S02_SELECTOR_OFF)
		return FF_R_SELECTOR;
	return 0;
}

static inline int ff_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	struct fake_flash *f = ctx;

	if (f->dead)
		return -1;
	if (off > S02_FLASH_SIZE || len > S02_FLASH_SIZE - off) {
		f->bad_call = true;
		return -1;
	}
	f->nall++;
	f->nreads++;
	ff_log(f, FF_READ, off, len);
	if (f->fail_read != 0u && f->nreads == f->fail_read)
		return -1;
	if ((f->fail_regions & ff_region(off)) != 0u)
		return -1;
	memcpy(buf, f->mem + off, len);
	if (f->garble_read != 0u && f->nreads == f->garble_read && len != 0u)
		buf[0] = (uint8_t)(buf[0] ^ 0x01u);
	ff_after(f, false);
	return 0;
}

static inline int ff_erase(void *ctx, uint32_t off)
{
	struct fake_flash *f = ctx;

	if (f->dead)
		return -1;
	if (off % S02_SECTOR_SIZE != 0u || off >= S02_FLASH_SIZE) {
		f->bad_call = true;
		return -1;
	}
	f->nall++;
	f->nmut++;
	ff_log(f, FF_ERASE, off, S02_SECTOR_SIZE);
	if (f->fail != 0u && f->nmut == f->fail)
		return -1;
	if (f->ignore != 0u && f->nmut == f->ignore)
		return 0;
	if (f->tear != 0u && (f->count_reads ? f->nall : f->nmut) == f->tear) {
		uint32_t i, rng = f->tear_a * 2654435761u + 1u;

		for (i = 0; i < S02_SECTOR_SIZE; i++)	/* bits raised to 1 */
			f->mem[off + i] = (uint8_t)(f->mem[off + i] | ff_tear_mask(f, i, &rng));
		f->dead = true;
		return -1;
	}
	memset(f->mem + off, 0xFF, S02_SECTOR_SIZE);
	ff_after(f, true);
	return 0;
}

static inline int ff_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	struct fake_flash *f = ctx;
	uint32_t i, rng;

	if (f->dead)
		return -1;
	if (len == 0u || len > S02_PAGE_SIZE || off >= S02_FLASH_SIZE ||
	    off / S02_PAGE_SIZE != (off + len - 1u) / S02_PAGE_SIZE) {
		f->bad_call = true;
		return -1;
	}
	f->nall++;
	f->nmut++;
	ff_log(f, FF_PROGRAM, off, len);
	if (f->fail != 0u && f->nmut == f->fail)
		return -1;
	if (f->ignore != 0u && f->nmut == f->ignore)
		return 0;
	if (f->tear != 0u && (f->count_reads ? f->nall : f->nmut) == f->tear) {
		rng = f->tear_a * 2654435761u + 1u;
		for (i = 0; i < len; i++) {
			uint8_t old = f->mem[off + i];
			uint8_t clear = (uint8_t)(old & (uint8_t)~buf[i]);

			clear = (uint8_t)(clear & ff_tear_mask(f, i, &rng));
			f->mem[off + i] = (uint8_t)(old & (uint8_t)~clear);
		}
		f->dead = true;
		return -1;
	}
	for (i = 0; i < len; i++) {
		uint8_t v = buf[i];

		if (f->corrupt != 0u && (f->nmut == f->corrupt ||
					 (f->corrupt_on && f->nmut > f->corrupt)))
			v = (uint8_t)(v & f->corrupt_mask);
		f->mem[off + i] = (uint8_t)(f->mem[off + i] & v);
	}
	ff_after(f, true);
	return 0;
}

/* Clear the log and the injections; power on.  The contents stay. */
static inline void ff_reset(struct fake_flash *f)
{
	f->nlog = 0;
	f->nmut = 0;
	f->nall = 0;
	f->nreads = 0;
	f->cut_after = 0;
	f->count_reads = false;
	f->tear = 0;
	f->tear_kind = FF_TEAR_SEED;
	f->tear_a = 0;
	f->tear_b = 0;
	f->fail = 0;
	f->corrupt = 0;
	f->corrupt_on = false;
	f->corrupt_mask = 0xFF;
	f->fail_read = 0;
	f->garble_read = 0;
	f->ignore = 0;
	f->fail_regions = 0;
	f->dead = false;
	f->bad_call = false;
}

static inline void ff_init(struct fake_flash *f)
{
	memset(f->mem, 0xFF, sizeof f->mem);
	ff_reset(f);
}

static uint8_t g_ff_scratch[2u * S02_SECTOR_SIZE];

static inline struct s02_env ff_env(struct fake_flash *f, struct s02_flash_ops *ops)
{
	struct s02_env env;

	ops->ctx = f;
	ops->read = ff_read;
	ops->erase = ff_erase;
	ops->program = ff_program;
	env.flash = ops;
	env.crc32 = ref_crc32;
	env.scratch = g_ff_scratch;
	env.scratch_len = S02_SECTOR_SIZE;
	return env;
}

/* --- Building slots in the vectors' states (S-02.016) --- */

static inline void ff_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Fill a slot with a seeded pseudo-random image of n bytes. */
static inline void ff_fill_image(uint8_t *dst, uint32_t n, uint32_t seed)
{
	uint32_t x = seed * 2654435761u + 0x9E3779B9u, i;

	for (i = 0; i < n; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		dst[i] = (uint8_t)(x >> 11);
	}
}

static inline void ff_ldr_header(uint8_t *h, uint32_t flags, uint32_t target, uint32_t count)
{
	const uint32_t w[4] = {0xAD000004u | flags, target, count, 0u};
	uint32_t i;
	uint8_t x = 0;

	for (i = 0; i < 16u; i++)
		h[i] = (uint8_t)(w[i / 4u] >> (8u * (i % 4u)));
	for (i = 0; i < 16u; i++)
		x ^= h[i];
	h[2] ^= x;	/* the checksum byte makes the XOR 0 */
}

/*
 * An image of n bytes that is a bootable stream (S-02.091) for a slot and
 * for the first stage: an INIT block of 16 bytes into L1 code and a FINAL
 * block with no payload, then ff_fill_image() bytes after the FINAL block.
 * Below FF_BOOT_MIN bytes it is not one.
 */
#define FF_BOOT_MIN 48u
static inline void ff_boot_image(uint8_t *dst, uint32_t n, uint32_t seed)
{
	uint8_t s[FF_BOOT_MIN];

	ff_fill_image(dst, n, seed);
	ff_ldr_header(s, 0x0800u, 0x11A00000u, 16u);	/* INIT */
	ff_fill_image(s + 16, 16u, seed);
	ff_ldr_header(s + 32, 0x8000u, 0x11A00000u, 0u);	/* FINAL */
	memcpy(dst, s, n < FF_BOOT_MIN ? n : FF_BOOT_MIN);
}

/*
 * Put a slot in one of the vectors' states: good, badcrc, badtag, erased,
 * badlen, hugelen, empty (S-02.016), and "blank" (slot and record erased).
 */
static inline void ff_set_slot(struct fake_flash *f, unsigned slot, const char *state,
			       uint32_t n, uint32_t seed)
{
	uint32_t base = s02_slot_base(slot);
	uint8_t *rec = f->mem + s02_record_off(slot);
	uint32_t crc;

	memset(f->mem + base, 0xFF, S02_SLOT_CAPACITY);
	memset(rec, 0xFF, S02_SECTOR_SIZE);
	if (strcmp(state, "blank") == 0)
		return;
	ff_fill_image(f->mem + base, n, seed);
	crc = ref_crc32(0, f->mem + base, n);
	ff_put32(rec, crc);
	ff_put32(rec + 4, n);
	ff_put32(rec + 8, S02_RECORD_TAG);
	if (strcmp(state, "badcrc") == 0) {
		f->mem[base + 0x200u] = (uint8_t)~f->mem[base + 0x200u];
	} else if (strcmp(state, "badtag") == 0) {
		ff_put32(rec + 8, 0x0000B005u);
	} else if (strcmp(state, "erased") == 0) {
		memset(rec, 0xFF, S02_RECORD_SIZE);
	} else if (strcmp(state, "badlen") == 0) {
		ff_put32(rec + 4, 0x78001u);
		ff_put32(rec, crc ^ 1u);
	} else if (strcmp(state, "hugelen") == 0) {
		ff_put32(rec + 4, 0x80000000u);
	} else if (strcmp(state, "empty") == 0) {
		ff_put32(rec, 0);
		ff_put32(rec + 4, 0);
	}
}

static inline void ff_set_selector(struct fake_flash *f, uint8_t index, uint8_t verified)
{
	memset(f->mem + S02_SELECTOR_OFF, 0xFF, S02_SECTOR_SIZE);
	f->mem[S02_SELECTOR_OFF] = index;
	f->mem[S02_SELECTOR_OFF + 1u] = verified;
}

/* S-02.016, judged directly on the memory (the reference for tests). */
static inline bool ff_slot_good(const struct fake_flash *f, unsigned slot)
{
	const uint8_t *r = f->mem + s02_record_off(slot);
	uint32_t crc = (uint32_t)r[0] | ((uint32_t)r[1] << 8) |
		       ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
	uint32_t len = (uint32_t)r[4] | ((uint32_t)r[5] << 8) |
		       ((uint32_t)r[6] << 16) | ((uint32_t)r[7] << 24);
	uint32_t tag = (uint32_t)r[8] | ((uint32_t)r[9] << 8) |
		       ((uint32_t)r[10] << 16) | ((uint32_t)r[11] << 24);

	if (tag != S02_RECORD_TAG || len < 1u || len > S02_SLOT_CAPACITY)
		return false;
	return ref_crc32(0, f->mem + s02_slot_base(slot), len) == crc;
}

/*
 * The factory first stage (S-02.020 to S-02.023), through the flash ops so
 * that its own selector write can be torn.  It never sees hugelen records in
 * the tests that use it.  Returns the slot booted.
 */
static inline unsigned ff_factory_first_stage(const struct s02_env *env)
{
	uint8_t sel[2], raw[S02_RECORD_SIZE], w[2];
	struct s02_record rec;
	unsigned slot;
	uint32_t crc;
	bool pass = false;

	if (env->flash->read(env->flash->ctx, S02_SELECTOR_OFF, sel, 2u) != 0)
		return 1u;
	slot = s02_selected_slot(sel[0]);
	if (sel[1] == 0x01u)
		return slot;			/* S-02.020 */
	if (env->flash->read(env->flash->ctx, s02_record_off(slot), raw,
			     S02_RECORD_SIZE) == 0) {
		s02_record_decode(raw, &rec);
		if (s02_tag_ok(rec.tag) &&	/* S-02.021: no length test */
		    s02_flash_crc(env, s02_slot_base(slot), rec.length, &crc) == S02_OK)
			pass = crc == rec.crc32;
	}
	w[0] = pass ? sel[0] : (sel[0] == 0x00u ? 0x01u : 0x00u);	/* S-02.022 */
	w[1] = 0x01u;
	if (env->flash->erase(env->flash->ctx, S02_SELECTOR_OFF) == 0)
		(void)env->flash->program(env->flash->ctx, S02_SELECTOR_OFF, w, 2u);
	return slot;				/* S-02.023: one boot late */
}

#endif /* S02_FAKE_FLASH_H */
