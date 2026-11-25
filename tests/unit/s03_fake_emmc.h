/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only RAM fake of the eMMC as S-03 sees it: two boot partitions of B
 * bytes that erase to zero (S-03.003), a user area that is only logged, an
 * op log, and the injections of S-03's scenarios (a named operation
 * failing on every attempt, a sector or header stored wrongly, a power cut
 * after or inside an erase or write) and, on request, a volatile cache that
 * holds every erase and write until a flush (SC-03-WRITE-CACHE).
 */
#ifndef S03_FAKE_EMMC_H
#define S03_FAKE_EMMC_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "waffle/emmc/emmc.h"
#include "s02_ref_crc.h"

#define FE_B_MAX  0x400000u
#define FE_LOG_MAX 4096u
#define FE_FLUSH_INDEX 32u	/* EXT_CSD[32] FLUSH_CACHE (S-03.068) */
#define FE_CACHE_SPAN 0x100000u	/* the part of a partition the cache mode covers */
#define FE_CACHE_MAX 8u

struct fe_op {
	char kind;		/* s r w e x (switch) */
	unsigned part;
	uint32_t a, b;		/* sector and count; first sector for an erase; index, value */
	int error;
};

enum fe_fail {
	FE_FAIL_NONE = 0, FE_FAIL_SELECT1, FE_FAIL_READ_HEADER1, FE_FAIL_SELECT2, FE_FAIL_ERASE,
	FE_FAIL_WRITE_IMAGE, FE_FAIL_READ_IMAGE, FE_FAIL_WRITE_HEADER, FE_FAIL_READ_HEADER,
	FE_FAIL_WRITE_HEADER_ACTIVE, FE_FAIL_READ_HEADER_ACTIVE, FE_FAIL_SELECT_OTHER,
	FE_FAIL_READ_OTHER, FE_FAIL_DEACTIVATE, FE_FAIL_ERASE_OTHER, FE_FAIL_SELECT0,
	FE_FAIL_FLUSH_IMAGE, FE_FAIL_FLUSH_HEADER, FE_FAIL_FLUSH_FINAL
};

/* An erase or write the eMMC has reported but not committed. */
struct fe_cached {
	unsigned part;
	uint32_t off, n;
	uint8_t data[FE_CACHE_SPAN];
};

struct fake_emmc {
	uint32_t B;
	unsigned sel;
	struct fe_op log[FE_LOG_MAX];
	unsigned nlog;
	unsigned nmut;		/* erases and writes that reached the eMMC */
	/* Injections. */
	enum fe_fail fail;
	unsigned target;	/* the writer's target, for the named failures */
	int corrupt_sector;	/* the target's stored sector stored wrongly (-1 none) */
	int corrupt_header;	/* 1: the first target header write stored wrongly, 2: the second */
	unsigned cut_after;	/* power lost after this erase or write */
	unsigned tear;		/* this erase or write is torn, byte-granular */
	int tear_pattern;	/* 0 all old, 1 all new, 2 first half new, 3 alternate, 4+ seeded */
	bool dead;
	bool user_access;	/* a read, write or erase while the user area was selected */
	bool past_end;		/* an access past a boot partition's end */
	/* Writer progress, for the named failures. */
	unsigned hdr_writes;
	bool failing;
	unsigned fail_left;
	struct fe_op fail_op;
	bool fired;
	uint8_t fail_switch;	/* every switch of this EXT_CSD index fails (0 none) */
	bool erase_ff;		/* an erase leaves 0xFF, not 0 */
	unsigned fail_erase_from;	/* erases of this group and later fail (0 none) */
	unsigned nflush;	/* flushes that succeeded */
	/* The cache mode: g_fe_bp* as the eMMC answers reads, g_fe_dur* as committed. */
	bool cache_on;
	unsigned ncached;
	uint8_t ext_csd[256];
	/* S-03.013's polled switch: 'p' in the log; its wait runs out when set,
	 * and the last limit it was given */
	bool poll_timeout;
	uint32_t poll_limit;
	/* S-03.012: CMD8 reads of EXT_CSD[179] ('c' in the log) */
	unsigned n_ext_reads;
	bool fail_ext_read;
};

static uint8_t g_fe_bp1[FE_B_MAX];
static uint8_t g_fe_bp2[FE_B_MAX];
static uint8_t g_fe_dur1[FE_CACHE_SPAN];
static uint8_t g_fe_dur2[FE_CACHE_SPAN];
static struct fe_cached g_fe_cached[FE_CACHE_MAX];

static inline uint8_t *fe_part(struct fake_emmc *e, unsigned part)
{
	(void)e;
	return part == S03_PART_BP1 ? g_fe_bp1 : g_fe_bp2;
}

static inline void fe_log(struct fake_emmc *e, char kind, unsigned part, uint32_t a, uint32_t b, int err)
{
	if (e->nlog < FE_LOG_MAX) {
		e->log[e->nlog].kind = kind;
		e->log[e->nlog].part = part;
		e->log[e->nlog].a = a;
		e->log[e->nlog].b = b;
		e->log[e->nlog].error = err;
	}
	e->nlog++;
}

/* The named failure: true when this op is the one that fails (all its attempts). */
static inline bool fe_fails(struct fake_emmc *e, char kind, unsigned part, uint32_t a, uint32_t b)
{
	unsigned other = e->target == S03_PART_BP1 ? S03_PART_BP2 : S03_PART_BP1;
	bool hit = false;

	if (e->failing) {
		if (e->fail_op.kind == kind && e->fail_op.part == part && e->fail_op.a == a &&
		    e->fail_op.b == b && e->fail_left > 0u) {
			e->fail_left--;
			return true;
		}
		e->failing = false;
	}
	if (e->fired || e->fail == FE_FAIL_NONE)
		return false;
	switch (e->fail) {
	case FE_FAIL_SELECT1: hit = kind == 's' && a == 1u; break;
	case FE_FAIL_READ_HEADER1: hit = kind == 'r' && part == 1u && a == 0u; break;
	case FE_FAIL_SELECT2: hit = kind == 's' && a == 2u; break;
	case FE_FAIL_ERASE: hit = kind == 'e' && part == e->target; break;
	case FE_FAIL_WRITE_IMAGE: hit = kind == 'w' && part == e->target && a >= 1u; break;
	case FE_FAIL_READ_IMAGE: hit = kind == 'r' && part == e->target && a >= 1u; break;
	case FE_FAIL_WRITE_HEADER:
		hit = kind == 'w' && part == e->target && a == 0u && e->hdr_writes == 0u; break;
	case FE_FAIL_READ_HEADER:
		hit = kind == 'r' && part == e->target && a == 0u && e->hdr_writes == 1u; break;
	case FE_FAIL_WRITE_HEADER_ACTIVE:
		hit = kind == 'w' && part == e->target && a == 0u && e->hdr_writes == 1u; break;
	case FE_FAIL_READ_HEADER_ACTIVE:
		hit = kind == 'r' && part == e->target && a == 0u && e->hdr_writes == 2u; break;
	case FE_FAIL_SELECT_OTHER: hit = kind == 's' && a == other && e->hdr_writes >= 2u; break;
	case FE_FAIL_READ_OTHER: hit = kind == 'r' && part == other && e->hdr_writes >= 2u; break;
	case FE_FAIL_DEACTIVATE: hit = kind == 'w' && part == other; break;
	case FE_FAIL_ERASE_OTHER: hit = kind == 'e' && part == other; break;
	case FE_FAIL_SELECT0: hit = kind == 's' && a == 0u; break;
	case FE_FAIL_FLUSH_IMAGE: hit = kind == 'x' && a == FE_FLUSH_INDEX && e->nflush == 0u; break;
	case FE_FAIL_FLUSH_HEADER: hit = kind == 'x' && a == FE_FLUSH_INDEX && e->nflush == 1u; break;
	case FE_FAIL_FLUSH_FINAL: hit = kind == 'x' && a == FE_FLUSH_INDEX && e->nflush == 2u; break;
	case FE_FAIL_NONE:
	default: break;
	}
	if (!hit)
		return false;
	e->fired = true;
	e->failing = true;
	e->fail_left = S03_RETRIES - 1u;
	e->fail_op.kind = kind;
	e->fail_op.part = part;
	e->fail_op.a = a;
	e->fail_op.b = b;
	return true;
}

static inline int fe_select(void *ctx, unsigned part)
{
	struct fake_emmc *e = ctx;

	if (e->dead)
		return -1;
	if (fe_fails(e, 's', 0, part, 0)) {
		fe_log(e, 's', 0, part, 0, 1);
		return -1;
	}
	fe_log(e, 's', 0, part, 0, 0);
	e->sel = part;
	return 0;
}

static inline bool fe_range_ok(struct fake_emmc *e, uint32_t sector, uint32_t count)
{
	if (e->sel == S03_PART_USER) {
		e->user_access = true;
		return false;
	}
	if ((uint64_t)(sector + (uint64_t)count) * S03_SECTOR > e->B) {
		e->past_end = true;
		return false;
	}
	return true;
}

static inline int fe_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	struct fake_emmc *e = ctx;

	if (e->dead)
		return -1;
	if (fe_fails(e, 'r', e->sel, sector, count)) {
		fe_log(e, 'r', e->sel, sector, count, 1);
		return -1;
	}
	fe_log(e, 'r', e->sel, sector, count, 0);
	if (!fe_range_ok(e, sector, count))
		return -1;
	memcpy(buf, fe_part(e, e->sel) + (size_t)sector * S03_SECTOR, (size_t)count * S03_SECTOR);
	return 0;
}

/* The cache mode: an erase or write (src NULL: zeros) reported, not committed. */
static inline void fe_cache(struct fake_emmc *e, uint32_t off, uint32_t n, const uint8_t *src)
{
	struct fe_cached *c;

	if (!e->cache_on)
		return;
	if (e->ncached >= FE_CACHE_MAX || off + n > FE_CACHE_SPAN) {
		e->past_end = true;
		return;
	}
	c = &g_fe_cached[e->ncached++];
	c->part = e->sel;
	c->off = off;
	c->n = n;
	if (src != NULL)
		memcpy(c->data, src, n);
	else
		memset(c->data, 0, n);
}

/* Commit one cached operation into `bp1` and `bp2`. */
static inline void fe_commit(const struct fe_cached *c, uint8_t *bp1, uint8_t *bp2)
{
	memcpy((c->part == S03_PART_BP1 ? bp1 : bp2) + c->off, c->data, c->n);
}

/* The eMMC as a power cut leaves it, having committed the cached operations
 * of `mask` (bit i: the i-th since the last flush) and lost the others. */
static inline void fe_cache_lose(const struct fake_emmc *e, unsigned mask)
{
	unsigned i;

	memcpy(g_fe_bp1, g_fe_dur1, FE_CACHE_SPAN);
	memcpy(g_fe_bp2, g_fe_dur2, FE_CACHE_SPAN);
	for (i = 0; i < e->ncached; i++)
		if (mask & (1u << i))
			fe_commit(&g_fe_cached[i], g_fe_bp1, g_fe_bp2);
}

/* Store n bytes over dst from src, torn by the pattern when tearing. */
static inline void fe_store(const struct fake_emmc *e, uint8_t *dst, const uint8_t *src, uint32_t n, bool torn)
{
	uint32_t i, x = (uint32_t)e->tear_pattern * 2654435761u + 7u;

	if (!torn) {
		memcpy(dst, src, n);
		return;
	}
	for (i = 0; i < n; i++) {
		bool take;

		switch (e->tear_pattern) {
		case 0: take = false; break;
		case 1: take = true; break;
		case 2: take = i < n / 2u; break;
		case 3: take = (i & 1u) != 0u; break;
		default:
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			take = (x & 1u) != 0u;
			break;
		}
		if (take)
			dst[i] = src[i];
	}
}

static inline int fe_write(void *ctx, uint32_t sector, uint32_t count, const uint8_t *buf)
{
	struct fake_emmc *e = ctx;
	uint8_t *p;
	bool torn;

	if (e->dead)
		return -1;
	if (fe_fails(e, 'w', e->sel, sector, count)) {
		/* A failed write stores nothing, except the target's header (stored in full). */
		fe_log(e, 'w', e->sel, sector, count, 1);
		if (e->sel == e->target && sector == 0u && fe_range_ok(e, sector, count)) {
			memcpy(fe_part(e, e->sel), buf, S03_SECTOR);
			if (e->fail_left == S03_RETRIES - 1u)	/* the first attempt */
				e->hdr_writes++;
		}
		return -1;
	}
	fe_log(e, 'w', e->sel, sector, count, 0);
	if (!fe_range_ok(e, sector, count))
		return -1;
	e->nmut++;
	torn = e->tear != 0u && e->nmut == e->tear;
	p = fe_part(e, e->sel) + (size_t)sector * S03_SECTOR;
	fe_store(e, p, buf, count * S03_SECTOR, torn);
	fe_cache(e, sector * S03_SECTOR, count * S03_SECTOR, p);
	if (e->sel == e->target && sector == 0u) {
		e->hdr_writes++;
		if (!torn && e->corrupt_header == (int)e->hdr_writes)
			p[0x05] ^= 0x01u;		/* stored wrongly; the active byte is not touched */
	}
	if (!torn && e->sel == e->target && e->corrupt_sector >= 1) {
		uint32_t s = (uint32_t)e->corrupt_sector;

		if (s >= sector && s < sector + count) {
			p[(size_t)(s - sector) * S03_SECTOR + 3u] ^= 0x10u;
			e->corrupt_sector = -1;
		}
	}
	if (torn || (e->cut_after != 0u && e->nmut == e->cut_after)) {
		e->dead = true;
		return -1;
	}
	return 0;
}

static inline int fe_erase(void *ctx, uint32_t first)
{
	struct fake_emmc *e = ctx;
	uint32_t group = first / S03_GROUP_SECTORS, n;
	static const uint8_t zeros[S03_GROUP_SECTORS * S03_SECTOR];
	bool torn;

	if (e->dead)
		return -1;
	if (first % S03_GROUP_SECTORS != 0u) {
		e->past_end = true;
		return -1;
	}
	if (fe_fails(e, 'e', e->sel, group, 0) ||
	    (e->fail_erase_from != 0u && group >= e->fail_erase_from)) {
		fe_log(e, 'e', e->sel, group, 0, 1);
		return -1;
	}
	fe_log(e, 'e', e->sel, group, 0, 0);
	if (e->sel == S03_PART_USER) {
		e->user_access = true;
		return 0;
	}
	if ((uint64_t)first * S03_SECTOR >= e->B) {
		e->past_end = true;
		return -1;
	}
	e->nmut++;
	torn = e->tear != 0u && e->nmut == e->tear;
	n = S03_GROUP_SECTORS * S03_SECTOR;
	if ((uint64_t)first * S03_SECTOR + n > e->B)
		n = e->B - first * S03_SECTOR;
	fe_store(e, fe_part(e, e->sel) + (size_t)first * S03_SECTOR, zeros, n, torn);
	if (e->erase_ff && !torn)
		memset(fe_part(e, e->sel) + (size_t)first * S03_SECTOR, 0xFF, n);
	fe_cache(e, first * S03_SECTOR, n, NULL);
	if (torn || (e->cut_after != 0u && e->nmut == e->cut_after)) {
		e->dead = true;
		return -1;
	}
	return 0;
}

static inline int fe_switch(void *ctx, uint8_t index, uint8_t value)
{
	struct fake_emmc *e = ctx;

	if (e->dead)
		return -1;
	if (e->fail_switch != 0u && index == e->fail_switch) {
		fe_log(e, 'x', 0, index, value, 1);
		return -1;
	}
	if (fe_fails(e, 'x', 0, index, value)) {
		fe_log(e, 'x', 0, index, value, 1);
		return -1;
	}
	fe_log(e, 'x', 0, index, value, 0);
	e->ext_csd[index] = value;
	if (index == 179u)
		e->sel = value & 0x07u;	/* PARTITION_ACCESS (S-03.012) */
	if (index == FE_FLUSH_INDEX) {
		unsigned i;

		e->nflush++;
		for (i = 0; i < e->ncached; i++)
			fe_commit(&g_fe_cached[i], g_fe_dur1, g_fe_dur2);
		e->ncached = 0;
	}
	return 0;
}

/* S-03.013: CMD6, then CMD13 until ready, for at most limit_ms. */
static inline int fe_switch_poll(void *ctx, uint8_t index, uint8_t value, uint32_t limit_ms)
{
	struct fake_emmc *e = ctx;

	if (e->dead)
		return -1;
	e->poll_limit = limit_ms;
	if (e->fail_switch != 0u && index == e->fail_switch) {
		fe_log(e, 'p', 0, index, value, 1);
		return -1;
	}
	fe_log(e, 'p', 0, index, value, e->poll_timeout ? 2 : 0);
	e->ext_csd[index] = value;
	return e->poll_timeout ? 1 : 0;
}

/* S-03.012: an EXT_CSD read of PART_CONFIG. */
static inline int fe_read_part_config(void *ctx, uint8_t *value)
{
	struct fake_emmc *e = ctx;

	if (e->dead || e->fail_ext_read)
		return -1;
	fe_log(e, 'c', 0, 179u, e->ext_csd[179], 0);
	e->n_ext_reads++;
	*value = e->ext_csd[179];
	return 0;
}

/* Power on again: injections and progress cleared, the contents kept. */
static inline void fe_reset(struct fake_emmc *e)
{
	e->sel = S03_PART_USER;
	e->nlog = 0;
	e->nmut = 0;
	e->fail = FE_FAIL_NONE;
	e->target = 0;
	e->corrupt_sector = -1;
	e->corrupt_header = 0;
	e->cut_after = 0;
	e->tear = 0;
	e->tear_pattern = 0;
	e->dead = false;
	e->user_access = false;
	e->past_end = false;
	e->hdr_writes = 0;
	e->failing = false;
	e->fail_left = 0;
	e->fired = false;
	e->fail_switch = 0;
	e->erase_ff = false;
	e->fail_erase_from = 0;
	e->nflush = 0;
	e->poll_timeout = false;
	e->poll_limit = 0;
	e->n_ext_reads = 0;
	e->fail_ext_read = false;
}

/* The cache mode on, everything on the eMMC committed so far. */
static inline void fe_cache_start(struct fake_emmc *e)
{
	e->cache_on = true;
	e->ncached = 0;
	memcpy(g_fe_dur1, g_fe_bp1, FE_CACHE_SPAN);
	memcpy(g_fe_dur2, g_fe_bp2, FE_CACHE_SPAN);
}

static inline void fe_init(struct fake_emmc *e, uint32_t B)
{
	e->B = B;
	memset(g_fe_bp1, 0, sizeof g_fe_bp1);
	memset(g_fe_bp2, 0, sizeof g_fe_bp2);
	memset(e->ext_csd, 0, sizeof e->ext_csd);
	e->cache_on = false;
	e->ncached = 0;
	fe_reset(e);
}

static uint8_t g_fe_scratch[8u * S03_SECTOR];

static inline struct s03_env fe_env(struct fake_emmc *e, struct s03_emmc_ops *ops)
{
	struct s03_env env;

	ops->ctx = e;
	ops->select = fe_select;
	ops->read = fe_read;
	ops->write = fe_write;
	ops->erase = fe_erase;
	ops->switch_byte = fe_switch;
	env.ops = ops;
	env.crc32 = ref_crc32;
	env.boot_size = e->B;
	env.scratch = g_fe_scratch;
	env.scratch_len = sizeof g_fe_scratch;
	env.max_xfer = 0;
	env.crc32_fast = NULL;
	return env;
}

/* Put a header and an image straight into a partition (the vectors' install). */
static inline void fe_install(struct fake_emmc *e, unsigned part, const uint8_t *image,
			      uint32_t length, uint32_t format, uint8_t active, uint8_t encrypted,
			      uint8_t pad, uint8_t reserved)
{
	uint8_t *p = fe_part(e, part);
	uint32_t crc = ref_crc32(0, image, length), i, end;

	memset(p, 0, S03_SECTOR);
	memset(p + 0x0E, reserved, S03_SECTOR - 0x0E);
	for (i = 0; i < 4u; i++) {
		p[i] = (uint8_t)(format >> (8u * i));
		p[4u + i] = (uint8_t)(length >> (8u * i));
		p[8u + i] = (uint8_t)(crc >> (8u * i));
	}
	p[0x0C] = active;
	p[0x0D] = encrypted;
	memcpy(p + S03_SECTOR, image, length);
	end = s03_sectors(length) * S03_SECTOR;
	for (i = length; i < end; i++)
		p[S03_SECTOR + i] = pad;
}

/* A header alone, over an otherwise erased partition. */
static inline void fe_header_only(struct fake_emmc *e, unsigned part, uint32_t format,
				  uint32_t length, uint32_t crc, uint8_t active, uint8_t encrypted)
{
	uint8_t *p = fe_part(e, part);
	unsigned i;

	memset(p, 0, S03_SECTOR);
	for (i = 0; i < 4u; i++) {
		p[i] = (uint8_t)(format >> (8u * i));
		p[4u + i] = (uint8_t)(length >> (8u * i));
		p[8u + i] = (uint8_t)(crc >> (8u * i));
	}
	p[0x0C] = active;
	p[0x0D] = encrypted;
}

#endif /* S03_FAKE_EMMC_H */
