/* SPDX-License-Identifier: GPL-2.0-or-later */
/* S-01 memory-map checks. */
#include "waffle/s01_memmap.h"
#include "waffle/s05_handoff.h"

static const struct s01_range spaces[] = {
	{S01_L2_START, S01_L2_END},
	{S01_L1_DATA_A_START, S01_L1_DATA_A_END},
	{S01_L1_DATA_B_START, S01_L1_DATA_B_END},
	{S01_L1_CODE_START, S01_L1_CODE_END},
	{S01_L1_SCRATCH_START, S01_L1_SCRATCH_END},
	{S01_DDR_START, S01_DDR_END},
};

static const struct s01_range reserve[] = {
	{S01_RESERVE_L2_START, S01_RESERVE_L2_END},
	{S01_RESERVE_DDR_START, S01_RESERVE_DDR_END},
};

static const struct s01_range staging = {S01_STAGING_START, S01_STAGING_END};
static const struct s01_range boot_reserve = {S01_BOOT_RESERVE_START, S01_BOOT_RESERVE_END};
static const struct s01_range record = {S01_RECORD_START, S01_RECORD_END};
static const struct s01_range reboot_record = {S01_RECORD_START, S01_RECORD_FLOOR};
static const struct s01_range display_record = {S01_DISPLAY_RECORD_START, S01_DISPLAY_RECORD_END};

static bool inside(struct s01_range r, struct s01_range s)
{
	return s.start <= r.start && r.end <= s.end;
}

bool s01_meets(struct s01_range a, struct s01_range b)
{
	return (uint64_t)a.start < b.end && (uint64_t)b.start < a.end;
}

/* S-01.012 */
bool s01_inside_spaces(struct s01_range r)
{
	size_t i;

	for (i = 0; i < sizeof spaces / sizeof spaces[0]; i++)
		if (inside(r, spaces[i]))
			return true;
	return false;
}

bool s01_inside_l1(struct s01_range r)
{
	size_t i;

	for (i = 1; i <= 4u; i++)
		if (inside(r, spaces[i]))
			return true;
	return false;
}

/* S-01.005 */
bool s01_inside_on_chip(struct s01_range r)
{
	return inside(r, spaces[0]) || s01_inside_l1(r);
}

/* S-01.013 */
bool s01_meets_reserve(struct s01_range r)
{
	return s01_meets(r, reserve[0]) || s01_meets(r, reserve[1]);
}

/* S-01.017 (and S-05.024's destination rule), in the kernel model's order. */
enum s01_os_dest s01_os_destination(uint32_t target, uint32_t count)
{
	struct s01_range r;

	r.start = target;
	r.end = (uint64_t)target + count;
	if (!s01_inside_spaces(r))
		return S01_OS_SPACE;
	if (s01_meets(r, staging))
		return S01_OS_STAGING;
	if (s01_meets_reserve(r))
		return S01_OS_RESERVE;
	if (s01_meets(r, boot_reserve))		/* S-01.008 */
		return S01_OS_BOOT_RESERVE;
	if (s01_meets(r, record))		/* S-01.008 */
		return S01_OS_RECORD;
	return S01_OS_OK;
}

static struct s01_range of(const struct s01_region *g)
{
	struct s01_range r;

	r.start = g->start;
	r.end = g->end;
	return r;
}

static bool own(const struct s01_region *g)
{
	return g->kind != S01_HELD_BUFFER;
}

/* S-01.011, S-01.013-.016, S-01.019 (check_layout.py) */
uint32_t s01_layout_check(const struct s01_region *r, size_t n)
{
	uint32_t fail = 0;
	size_t i, j;

	for (i = 0; i < n; i++) {
		struct s01_range a = of(&r[i]);

		if (r[i].handover && !(inside(a, reserve[0]) || inside(a, reserve[1])))
			fail |= S01_RULE_HANDOVER_IN_RESERVE;
		if (r[i].handover && s01_meets(a, staging))
			fail |= S01_RULE_HANDOVER_NOT_STAGING;
		if (r[i].handover && r[i].kind == S01_HEAP)
			fail |= S01_RULE_HANDOVER_NO_HEAP;
		if (own(&r[i]) && s01_meets(a, staging))
			fail |= S01_RULE_STAGING_CLEAR;
		if (s01_meets(a, reboot_record))	/* S-01.007 */
			fail |= S01_RULE_RECORD_CLEAR;
		if (own(&r[i]) && s01_meets(a, display_record))
			fail |= S01_RULE_DISPLAY_RECORD_CLEAR;
		if (!s01_inside_spaces(a))
			fail |= S01_RULE_INSIDE_SPACES;
		if (r[i].kind == S01_HEAP && r[i].end > S01_STAGING_START)
			fail |= S01_RULE_HEAP_CLEAR;
		for (j = 0; j < n; j++) {
			if (r[j].kind != S01_HELD_BUFFER || !s01_meets(a, of(&r[j])))
				continue;
			if (own(&r[i]))
				fail |= S01_RULE_HELD_BUFFERS_CLEAR;
			if (r[i].kind == S01_HEAP)
				fail |= S01_RULE_HEAP_CLEAR;
		}
	}
	return fail;
}

/* --- Stream checks (S-01.005, .008, .009, .010, .017, .020) --- */

static struct s01_range dest_of(const struct s05_block *b)
{
	struct s01_range r;

	r.start = b->target;
	r.end = (uint64_t)b->target + b->count;
	return r;
}

void s01_stream_begin(struct s01_stream *s, enum s01_profile p,
		      const struct s01_range *fs_stack)
{
	s->p = p;
	s->has_fs_stack = fs_stack != NULL;
	s->fs_stack.start = fs_stack != NULL ? fs_stack->start : 0u;
	s->fs_stack.end = fs_stack != NULL ? fs_stack->end : 0u;
	s->hn = 0;
	s->skip = 0;
	s->fail = 0;
	s->pre = 0;
	s->inits = 0;
	s->in_final = false;
	s->final = false;
	s->bad = false;
}

/* One block's rules, as its header arrives. */
static void block(struct s01_stream *s, const struct s05_block *b)
{
	static const struct s01_range ddr = {S01_DDR_START, S01_DDR_END};
	struct s01_range r = dest_of(b);
	bool has = s05_block_has_destination(b);
	enum s01_profile p = s->p;

	if (has && !s01_inside_spaces(r))
		s->fail |= S01_SRULE_INSIDE_SPACES;
	if (has && s01_meets(r, boot_reserve))
		s->fail |= S01_SRULE_ROM_BOOT_RESERVE;
	if (has && s01_meets(r, record))
		s->fail |= S01_SRULE_RECORD_AND_ROM_STACK;
	if ((p == S01_FIRST_STAGE || p == S01_BOOTLOADER) &&
	    (b->code & 0xFu) != S01_SPI_MASTER_BCODE)
		s->fail |= S01_SRULE_BCODE;
	if (p == S01_FIRST_STAGE && has && !s01_inside_on_chip(r))
		s->fail |= S01_SRULE_ON_CHIP_ONLY;
	/* S-01.010: the blocks up to and including the first INIT block;
	 * applied at the end when there is exactly one. */
	if (p == S01_BOOTLOADER && s->inits == 0u) {
		if (has && !s01_inside_l1(r))
			s->pre |= S01_SRULE_INIT_IN_L1;
		if (has && s01_meets(r, ddr))
			s->pre |= S01_SRULE_INIT_BEFORE_DDR;
		if ((b->flags & S05_FLAG_INIT) &&
		    (b->target < S01_L1_CODE_START || b->target >= S01_L1_CODE_END))
			s->pre |= S01_SRULE_INIT_IN_L1;
	}
	if (b->flags & S05_FLAG_INIT)
		s->inits++;
	if (p == S01_BOOTLOADER && s->has_fs_stack && has && s01_meets(r, s->fs_stack))
		s->fail |= S01_SRULE_SPARES_FS_STACK;
	if (p == S01_OS && has && s01_meets_reserve(r))
		s->fail |= S01_SRULE_OUTSIDE_RESERVE;
	if (p == S01_OS && has && s01_meets(r, staging))
		s->fail |= S01_SRULE_OUTSIDE_STAGING;
}

void s01_stream_feed(struct s01_stream *s, const uint8_t *p, size_t n)
{
	while (n > 0u && !s->final && !s->bad) {
		struct s05_block b;
		size_t k;

		if (s->skip > 0u) {		/* a payload */
			k = s->skip < n ? (size_t)s->skip : n;
			s->skip -= k;
			p += k;
			n -= k;
		} else {			/* a header */
			k = sizeof s->hdr - s->hn;
			if (k > n)
				k = n;
			for (size_t i = 0; i < k; i++)
				s->hdr[s->hn + i] = p[i];
			s->hn += (unsigned)k;
			p += k;
			n -= k;
			if (s->hn < sizeof s->hdr)
				break;
			s->hn = 0;
			if (!s05_ldr_header_ok(s->hdr)) {
				s->bad = true;
				break;
			}
			s05_ldr_decode(s->hdr, &b);
			block(s, &b);
			/* data and IGNORE blocks carry their payload */
			s->skip = (b.flags & S05_FLAG_FILL) ? 0u : b.count;
			s->in_final = (b.flags & S05_FLAG_FINAL) != 0u;
		}
		/* the FINAL block ends with its payload */
		if (s->in_final && s->skip == 0u)
			s->final = true;
	}
}

uint32_t s01_stream_end(const struct s01_stream *s)
{
	uint32_t fail = s->fail;

	/* parsed to a FINAL block within the length, or nothing */
	if (!s->final)
		return S01_SRULE_PARSE;
	if (s->p == S01_BOOTLOADER) {
		if (s->inits != 1u)
			fail |= S01_SRULE_ONE_INIT_BLOCK;
		else
			fail |= s->pre;
	}
	return fail;
}

uint32_t s01_stream_check(enum s01_profile p, const uint8_t *img, uint32_t len,
			  const struct s01_range *fs_stack)
{
	struct s01_stream s;
	uint32_t fail;

	s01_stream_begin(&s, p, fs_stack);
	s01_stream_feed(&s, img, len);
	fail = s01_stream_end(&s);
	if (fail != S01_SRULE_PARSE && p == S01_OS && len > S01_STAGING_MAX)
		fail |= S01_SRULE_FITS_STAGING;
	return fail;
}

/* S-02.091 */
void s01_bootable_begin(struct s01_stream *s, enum s01_profile p)
{
	static const struct s01_range factory_fs_stack = {
		S01_FACTORY_FS_STACK_START, S01_FACTORY_FS_STACK_END
	};

	s01_stream_begin(s, p, p == S01_BOOTLOADER ? &factory_fs_stack : NULL);
}

bool s01_bootable(enum s01_profile p, const uint8_t *img, uint32_t len)
{
	struct s01_stream s;

	s01_bootable_begin(&s, p);
	s01_stream_feed(&s, img, len);
	return s01_stream_end(&s) == 0u;
}
