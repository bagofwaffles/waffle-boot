/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SPI flash's slots (see slots/slots.h): the front's own copy of S-02's
 * formats, the slot check, the running-slot inference and the writer, and
 * of S-01's stream rules as S-02.091 applies them to a slot image.
 */
#include "front/slots/slots.h"

#include "front/handoff.h"

/* S-05 §Terms: an LDR block header, {code, target, count, argument}. */
#define LDR_HDR    16u
#define LDR_SIG    0xADu
#define LDR_FINAL  0x8000u
#define LDR_FIRST  0x4000u
#define LDR_IGNORE 0x1000u
#define LDR_INIT   0x0800u
#define LDR_FILL   0x0100u
#define LDR_BCODE  0x4u		/* S-01.020 */

/* Flash reads go through this buffer, a page at a time. */
static uint8_t chunk[FR_PAGE];
static uint8_t descbuf[FR_DESC_MAX];
_Static_assert(FR_DESC_MAX >= FR_PAGE, "descbuf holds a page for fr_slots_same()");

/* A range [lo, hi); every end here fits in 32 bits. */
struct span {
	uint32_t lo, hi;
};

/* S-01.012: L2, the four L1 blocks, DDR. */
static const struct span spaces[6] = {
	{0x08000000u, 0x08040000u},
	{0x11800000u, 0x11808000u},
	{0x11900000u, 0x11908000u},
	{0x11A00000u, 0x11A0C000u},
	{0x11B00000u, 0x11B02000u},
	{0x80000000u, 0x90000000u},
};
#define SP_L1_FIRST 1u
#define SP_L1_CODE  3u
#define SP_L1_LAST  4u
#define SP_DDR      5u

static const struct span boot_reserve = {0x08000000u, 0x08002000u};	/* S-01.008 */
static const struct span record_area = {0x11B01F00u, 0x11B02000u};	/* S-01.008 */
static const struct span boot_call_stack = {0x11807000u, 0x11808000u};	/* rule 4 */

/* The front's windows and the shared block (handoff.h). */
static const struct span front_win[3] = {
	{FR_L1_CODE_START, FR_L1_CODE_END},
	{FR_L1_DATA_START, FR_L1_DATA_END},
	{FR_L2_START, FR_L2_END},
};
#define FW_L1_CODE 0u
#define FW_L2      2u
static const struct span shared = {FR_SHARED_START, FR_SHARED_END};

static bool meets(uint32_t a, uint64_t b, const struct span *s)
{
	return a < s->hi && s->lo < b;
}

static bool inside(uint32_t a, uint64_t b, const struct span *s)
{
	return s->lo <= a && b <= s->hi;
}

static bool holds(uint32_t t, const struct span *s)
{
	return s->lo <= t && t < s->hi;
}

static bool inside_of(uint32_t a, uint64_t b, unsigned first, unsigned last)
{
	for (unsigned i = first; i <= last; i++)
		if (inside(a, b, &spaces[i]))
			return true;
	return false;
}

/* Signature 0xAD and a zero XOR over the 16 bytes. */
static bool header_ok(const uint8_t h[LDR_HDR])
{
	uint8_t x = 0;

	for (unsigned i = 0; i < LDR_HDR; i++)
		x = (uint8_t)(x ^ h[i]);
	return h[3] == LDR_SIG && x == 0u;
}

/* Every read the front makes of the flash, at most a page but for the
 * descriptor's: the link is served between them. */
static int flash_read(const struct fr_flash *f, uint32_t off, uint8_t *buf, uint32_t len)
{
	fr_serve();
	return f->read(f->ctx, off, buf, len) == 0 ? FR_FLASH_OK : FR_FLASH_IO;
}

/* --- Formats (S-02.010 to S-02.016) --- */

unsigned fr_selected_slot(uint8_t index)
{
	return index == 0x00u ? 0u : 1u;
}

int fr_read_selector(const struct fr_flash *f, struct fr_selector *s)
{
	uint8_t raw[2];

	if (flash_read(f, FR_SELECTOR_OFF, raw, 2u) != FR_FLASH_OK)
		return FR_FLASH_IO;
	s->index = raw[0];
	s->verified = raw[1];
	return FR_FLASH_OK;
}

int fr_read_record(const struct fr_flash *f, unsigned k, struct fr_record *r)
{
	uint8_t raw[12];

	if (k > 1u || flash_read(f, FR_RECORD_OFF(k), raw, sizeof raw) != FR_FLASH_OK)
		return FR_FLASH_IO;
	r->crc32 = fr_le32(raw);
	r->length = fr_le32(raw + 4);
	r->tag = fr_le32(raw + 8);
	return FR_FLASH_OK;
}

/* S-02.016 items 1 and 2: judged without a read of the slot. */
static bool plausible(const struct fr_record *r)
{
	return r->tag == FR_TAG && r->length >= 1u && r->length <= FR_SLOT_CAP;
}

static int flash_crc(const struct fr_flash *f, uint32_t off, uint32_t len, uint32_t *crc)
{
	while (len > 0u) {
		uint32_t k = len < FR_PAGE ? len : FR_PAGE;

		if (flash_read(f, off, chunk, k) != FR_FLASH_OK)
			return FR_FLASH_IO;
		*crc = fr_crc32(*crc, chunk, k);
		off += k;
		len -= k;
	}
	return FR_FLASH_OK;
}

bool fr_tried(const struct fr_record *r, uint32_t crc)
{
	return plausible(r) && r->crc32 == ~crc;
}

/* S-02.016 and the trial mark, by one CRC pass over the slot. */
static int check(const struct fr_flash *f, unsigned k, bool *good, bool *tried, struct fr_record *r)
{
	struct fr_record rec;
	uint32_t crc = 0;

	*good = *tried = false;
	if (fr_read_record(f, k, &rec) != FR_FLASH_OK)
		return FR_FLASH_IO;
	if (r != NULL)
		*r = rec;
	if (!plausible(&rec))
		return FR_FLASH_OK;
	if (flash_crc(f, FR_SLOT_BASE(k), rec.length, &crc) != FR_FLASH_OK)
		return FR_FLASH_IO;
	*good = crc == rec.crc32;
	*tried = fr_tried(&rec, crc);
	return FR_FLASH_OK;
}

int fr_check_slot(const struct fr_flash *f, unsigned k, bool *good, struct fr_record *r)
{
	bool tried;

	return check(f, k, good, &tried, r);
}

/* --- The running slot (S-01.002) --- */

/* Bytes compared and bytes equal, and whether the stream reached its
 * FINAL block. */
struct score {
	uint32_t compared, equal;
	bool complete;
};

/* count payload bytes at flash offset src against memory at addr, where
 * mem maps it: the memory map decides what may be compared (S-01.002). */
static int compare_payload(const struct fr_flash *f, const struct fr_mem *mem, uint32_t src,
			   uint32_t addr, uint32_t count, struct score *sc)
{
	uint32_t done = 0;

	if ((uint64_t)addr + count > 0x100000000ull)
		return FR_FLASH_OK;		/* wraps: not a load to compare */
	while (done < count) {
		uint32_t k = count - done < FR_PAGE ? count - done : FR_PAGE;
		const uint8_t *m = mem->map(mem->ctx, addr + done, k);

		if (m != NULL) {
			if (flash_read(f, src + done, chunk, k) != FR_FLASH_OK)
				return FR_FLASH_IO;
			for (uint32_t i = 0; i < k; i++)
				sc->equal += chunk[i] == m[i] ? 1u : 0u;
			sc->compared += k;
		}
		done += k;
	}
	return FR_FLASH_OK;
}

/* Every non-FILL, non-IGNORE payload of slot k's stream against memory. */
static int slot_score(const struct fr_flash *f, const struct fr_mem *mem, unsigned k,
		      struct score *sc)
{
	struct fr_record rec;
	uint8_t h[LDR_HDR];
	uint32_t base = FR_SLOT_BASE(k), len = FR_SLOT_CAP, off = 0;

	sc->compared = sc->equal = 0;
	sc->complete = false;
	if (fr_read_record(f, k, &rec) != FR_FLASH_OK)
		return FR_FLASH_IO;
	if (plausible(&rec))
		len = rec.length;
	while ((uint64_t)off + LDR_HDR <= len) {
		uint32_t flags, target, count;

		if (flash_read(f, base + off, h, LDR_HDR) != FR_FLASH_OK)
			return FR_FLASH_IO;
		if (!header_ok(h))
			break;			/* the stream ends here */
		flags = fr_le32(h) & 0xFFF0u;
		target = fr_le32(h + 4);
		count = fr_le32(h + 8);
		off += LDR_HDR;
		if (!(flags & LDR_FILL)) {
			if ((uint64_t)off + count > len)
				break;
			/* a FILL block's memory is the program's own by now */
			if (count != 0u && !(flags & LDR_IGNORE) &&
			    compare_payload(f, mem, base + off, target, count, sc) != FR_FLASH_OK)
				return FR_FLASH_IO;
			off += count;
		}
		if (flags & LDR_FINAL) {
			sc->complete = true;
			break;
		}
	}
	return FR_FLASH_OK;
}

/*
 * The slot later starts settle on, as the selector and the records read
 * now (S-02.020-S-02.023): the slot the selector names when verified is
 * 0x01, where the first stage starts it with no check, or when that slot
 * is good, where its check passes; otherwise the other slot, where a
 * failed check leaves the selector.
 */
static int settled_slot(const struct fr_flash *f, unsigned *slot)
{
	struct fr_selector sel;
	unsigned s;
	bool good;

	if (fr_read_selector(f, &sel) != FR_FLASH_OK)
		return FR_FLASH_IO;
	s = fr_selected_slot(sel.index);
	if (sel.verified != 0x01u) {
		if (fr_check_slot(f, s, &good, NULL) != FR_FLASH_OK)
			return FR_FLASH_IO;
		if (!good)
			s = 1u - s;
	}
	*slot = s;
	return FR_FLASH_OK;
}

/*
 * The one slot whose stream matched every byte compared, at least one
 * being compared, as s02_running_slot() decides it; where both do, the
 * one whose stream reached its FINAL block: a stream cut short, as a write
 * cut early leaves it, can match a part of the running image. Where both
 * match whole and both reach their FINAL block the slots hold the same
 * image, and either is this one: the answer is settled_slot(), the slot
 * the unit goes on starting, so that the slot a write keeps is the one a
 * start after a cut runs, and *both is set. Neither
 * establishes nothing: memory close to a slot, but loaded from neither,
 * establishes nothing, and a slot that matches only in part never wins
 * over one that matches whole. On a real boot every compared
 * byte matches only because mem maps nothing written since the load
 * (fr_mem_readable()): the ROM writes only its reserve, its scratch and
 * the first stage's stack, INIT and the front only their own windows, the
 * hand-off record and the staging buffer, and main has not run. A stream
 * with two blocks that load the same byte never matches, so is never
 * established.
 */
/* Slot k's stream against the loaded descriptor alone. */
enum desc_vote { D_UNSURE, D_MATCH, D_DIFFER };

/*
 * The descriptor's bytes as slot k's stream loads them, read from the
 * flash with no other payload read: D_DIFFER when one of them differs
 * from memory, which fails slot_score()'s whole match too, or when the
 * stream reaches its FINAL block loading none of them (an image that is
 * not this bootloader's: the running stream loads every one); D_MATCH
 * when the stream loads every one and each is equal; D_UNSURE otherwise
 * (a FILL block or a partial gap over the descriptor, a stream that ends
 * before its FINAL block, no descriptor in memory), which leaves the
 * answer to the whole compare.
 */
static int desc_vote(const struct fr_flash *f, const struct fr_mem *mem, unsigned k,
		     enum desc_vote *v)
{
	const uint8_t *m = mem->map(mem->ctx, FR_DESC_ADDR, FR_DESC_HEAD);
	struct fr_record rec;
	uint8_t h[LDR_HDR];
	uint32_t base = FR_SLOT_BASE(k), len = FR_SLOT_CAP, off = 0, size, seen = 0;
	bool final = false;

	*v = D_UNSURE;
	if (m == NULL || fr_le32(m + FR_DESC_O_MAGIC) != FR_DESC_MAGIC)
		return FR_FLASH_OK;
	size = fr_le16(m + FR_DESC_O_SIZE);
	if (size < FR_DESC_HEAD || size > FR_DESC_MAX ||
	    (m = mem->map(mem->ctx, FR_DESC_ADDR, size)) == NULL)
		return FR_FLASH_OK;
	if (fr_read_record(f, k, &rec) != FR_FLASH_OK)
		return FR_FLASH_IO;
	if (plausible(&rec))
		len = rec.length;
	while ((uint64_t)off + LDR_HDR <= len) {
		uint32_t flags, target, count, lo, hi;

		if (flash_read(f, base + off, h, LDR_HDR) != FR_FLASH_OK)
			return FR_FLASH_IO;
		if (!header_ok(h))
			break;
		flags = fr_le32(h) & 0xFFF0u;
		target = fr_le32(h + 4);
		count = fr_le32(h + 8);
		off += LDR_HDR;
		lo = target > FR_DESC_ADDR ? target : FR_DESC_ADDR;
		hi = (uint64_t)target + count < (uint64_t)FR_DESC_ADDR + size ?
			     (uint32_t)((uint64_t)target + count) : FR_DESC_ADDR + size;
		if (count != 0u && lo < hi && !(flags & LDR_IGNORE)) {
			if (flags & LDR_FILL)
				return FR_FLASH_OK;	/* D_UNSURE */
			if ((uint64_t)off + (lo - target) + (hi - lo) > len)
				break;
			if (flash_read(f, base + off + (lo - target), descbuf, hi - lo) !=
			    FR_FLASH_OK)
				return FR_FLASH_IO;
			for (uint32_t i = 0; i < hi - lo; i++)
				if (descbuf[i] != m[lo - FR_DESC_ADDR + i]) {
					*v = D_DIFFER;
					return FR_FLASH_OK;
				}
			seen += hi - lo;
		}
		if (!(flags & LDR_FILL)) {
			if ((uint64_t)off + count > len)
				break;
			off += count;
		}
		if (flags & LDR_FINAL) {
			final = true;
			break;
		}
	}
	if (seen == size)
		*v = D_MATCH;
	else if (seen == 0u && final)
		*v = D_DIFFER;
	return FR_FLASH_OK;
}

int fr_running_slots(const struct fr_flash *f, const struct fr_mem *mem, unsigned *slot,
		     bool *established, bool *both)
{
	struct score sc[2];
	bool full[2];
	unsigned w;

	*slot = FR_NO_SLOT;
	*established = false;
	*both = false;
	for (unsigned k = 0; k < 2u; k++)
		if (slot_score(f, mem, k, &sc[k]) != FR_FLASH_OK)
			return FR_FLASH_IO;
	for (unsigned k = 0; k < 2u; k++)
		full[k] = sc[k].compared != 0u && sc[k].equal == sc[k].compared;
	if (full[0] && full[1]) {
		if (sc[0].complete && sc[1].complete) {
			if (settled_slot(f, &w) != FR_FLASH_OK)
				return FR_FLASH_IO;
			*slot = w;
			*established = true;
			*both = true;
			return FR_FLASH_OK;
		}
		/* the one whose stream reached its FINAL block */
		if (sc[0].complete != sc[1].complete)
			full[sc[0].complete ? 1u : 0u] = false;
	}
	if (full[0] == full[1])
		return FR_FLASH_OK;
	w = full[0] ? 0u : 1u;
	*slot = w;
	*established = true;
	return FR_FLASH_OK;
}

/*
 * The same answer after fr_image_check() has passed, with less read: the
 * loaded descriptor came from the slot the ROM read, and the check has
 * proved memory against its CRCs. Where one slot's descriptor is the
 * loaded one and the other's differs, the first is the one whose stream
 * matches whole and the second's cannot, so no stream is compared. Equal
 * descriptors (the same image in both slots) or a slot its descriptor does
 * not tell are left to the whole compare. Recovery, which may run on an
 * image that failed its check or was never checked, uses the whole compare.
 */
int fr_running_slots_checked(const struct fr_flash *f, const struct fr_mem *mem, unsigned *slot,
			     bool *established, bool *both)
{
	enum desc_vote dv[2];

	*slot = FR_NO_SLOT;
	*established = false;
	*both = false;
	for (unsigned k = 0; k < 2u; k++)
		if (desc_vote(f, mem, k, &dv[k]) != FR_FLASH_OK)
			return FR_FLASH_IO;
	if ((dv[0] == D_MATCH && dv[1] == D_DIFFER) || (dv[0] == D_DIFFER && dv[1] == D_MATCH)) {
		*slot = dv[0] == D_MATCH ? 0u : 1u;
		*established = true;
		return FR_FLASH_OK;
	}
	return fr_running_slots(f, mem, slot, established, both);
}

/*
 * The slot whose stored logo the start shows, before the image check: the
 * one slot whose descriptor is the loaded one, else the slot the selector
 * names. Only a picture follows from it, each slot's checked on its own.
 */
int fr_logo_slot(const struct fr_flash *f, const struct fr_mem *mem, unsigned *slot)
{
	enum desc_vote dv[2];
	struct fr_selector sel;

	for (unsigned k = 0; k < 2u; k++)
		if (desc_vote(f, mem, k, &dv[k]) != FR_FLASH_OK)
			return FR_FLASH_IO;
	if ((dv[0] == D_MATCH) != (dv[1] == D_MATCH)) {
		*slot = dv[0] == D_MATCH ? 0u : 1u;
		return FR_FLASH_OK;
	}
	if (fr_read_selector(f, &sel) != FR_FLASH_OK)
		return FR_FLASH_IO;
	*slot = fr_selected_slot(sel.index);
	return FR_FLASH_OK;
}

int fr_running_slot(const struct fr_flash *f, const struct fr_mem *mem, unsigned *slot,
		    bool *established)
{
	bool both;

	return fr_running_slots(f, mem, slot, established, &both);
}

int fr_slots_same(const struct fr_flash *f, bool *same)
{
	struct fr_record r[2];

	*same = false;
	for (unsigned k = 0; k < 2u; k++)
		if (fr_read_record(f, k, &r[k]) != FR_FLASH_OK)
			return FR_FLASH_IO;
	if (!plausible(&r[0]) || !plausible(&r[1]) || r[0].length != r[1].length)
		return FR_FLASH_OK;
	/* descbuf is the second page: no stream is scanned meanwhile */
	for (uint32_t off = 0; off < r[0].length;) {
		const uint32_t len = r[0].length - off < FR_PAGE ? r[0].length - off : FR_PAGE;

		if (flash_read(f, FR_SLOT_BASE(0) + off, chunk, len) != FR_FLASH_OK ||
		    flash_read(f, FR_SLOT_BASE(1) + off, descbuf, len) != FR_FLASH_OK)
			return FR_FLASH_IO;
		if (!fr_same(chunk, descbuf, len))
			return FR_FLASH_OK;
		off += len;
	}
	*same = true;
	return FR_FLASH_OK;
}

/* --- S-02.091 and the front, over an image in memory or a slot --- */

struct walk {
	const uint8_t *img;		/* the image in memory, or NULL: */
	const struct fr_flash *f;	/* the flash, from base */
	uint32_t base, n;
};

static int walk_get(const struct walk *w, uint32_t off, uint8_t *buf, uint32_t len)
{
	if (w->img != NULL) {
		fr_copy(buf, w->img + off, len);
		return FR_FLASH_OK;
	}
	return flash_read(w->f, w->base + off, buf, len);
}

static int walk_crc(const struct walk *w, uint32_t off, uint32_t len, uint32_t *crc)
{
	if (w->img != NULL) {
		*crc = fr_crc32(*crc, w->img + off, len);
		return FR_FLASH_OK;
	}
	return flash_crc(w->f, w->base + off, len, crc);
}

#define BAD(rc) (1u << (rc))

/*
 * One pass over the stream from byte 0, to its FINAL block. *rc gets the
 * first rule of S-02.091 the stream fails, in the rules' order, then
 * FR_IMG_NO_FRONT when the front's condition fails (slots/slots.h); *has_desc
 * and *d the one descriptor block, when it parses.
 */
static int scan(const struct walk *w, enum fr_image_rc *rc, struct fr_desc *d, bool *has_desc)
{
	uint8_t h[LDR_HDR];
	uint32_t off = 0, fail = 0, inits = 0, ndesc = 0, fcrc = 0;
	uint32_t first = 0, final = 0, init = 0;
	bool pre = false, has_first = false, front_bad = false, desc_ok = false;

	*has_desc = false;
	*rc = FR_IMG_STREAM;
	for (;;) {
		uint32_t code, flags, target, count;
		uint64_t end;
		bool dest, data;

		/* Rule 1: headers and payloads within the n bytes, to a FINAL. */
		if ((uint64_t)off + LDR_HDR > w->n)
			return FR_FLASH_OK;
		if (walk_get(w, off, h, LDR_HDR) != FR_FLASH_OK)
			return FR_FLASH_IO;
		if (!header_ok(h))
			return FR_FLASH_OK;
		code = fr_le32(h);
		target = fr_le32(h + 4);
		count = fr_le32(h + 8);
		flags = code & 0xFFF0u;
		data = !(flags & LDR_FILL);
		off += LDR_HDR;
		if (data && (uint64_t)off + count > w->n)
			return FR_FLASH_OK;
		end = (uint64_t)target + count;
		dest = count != 0u && !(flags & LDR_IGNORE);	/* S-01 Terms */

		/* Rule 2 (S-01.010): up to and including the first INIT block. */
		if (inits == 0u) {
			if (dest && (!inside_of(target, end, SP_L1_FIRST, SP_L1_LAST) ||
				     meets(target, end, &spaces[SP_DDR])))
				pre = true;
			if ((flags & LDR_INIT) && !holds(target, &spaces[SP_L1_CODE]))
				pre = true;
		}
		if (flags & LDR_INIT) {
			if (inits == 0u)
				init = target;
			inits++;
		}
		/* Rule 3 (S-01.012, S-01.008). */
		if (dest && (!inside_of(target, end, 0u, SP_DDR) ||
			     meets(target, end, &boot_reserve) || meets(target, end, &record_area)))
			fail |= BAD(FR_IMG_SPACES);
		/* Rule 4. */
		if (dest && meets(target, end, &boot_call_stack))
			fail |= BAD(FR_IMG_STACK);
		/* Rule 5 (S-01.020). */
		if ((code & 0xFu) != LDR_BCODE)
			fail |= BAD(FR_IMG_BCODE);

		/* The front: its entry, its descriptor, its payloads. */
		if ((flags & LDR_FIRST) && !has_first) {
			has_first = true;
			first = target;
		}
		if (dest && meets(target, end, &shared)) {
			if (data && target == FR_DESC_ADDR && inside(target, end, &shared)) {
				uint32_t k = count < FR_DESC_MAX ? count : FR_DESC_MAX;

				ndesc++;
				if (walk_get(w, off, descbuf, k) != FR_FLASH_OK)
					return FR_FLASH_IO;
				desc_ok = fr_desc_parse(descbuf, k, d);
			} else {
				front_bad = true;	/* the shared block is the descriptor's */
			}
		} else if (dest) {
			for (unsigned i = 0; i < 3u; i++) {
				if (!meets(target, end, &front_win[i]))
					continue;
				if (data && inside(target, end, &front_win[i])) {
					if (walk_crc(w, off, count, &fcrc) != FR_FLASH_OK)
						return FR_FLASH_IO;
				} else {
					front_bad = true;	/* front bytes the CRC would not see */
				}
				break;
			}
		}
		if (data)
			off += count;
		if (flags & LDR_FINAL) {
			final = target;
			break;
		}
	}
	*has_desc = ndesc == 1u && desc_ok;
	if (inits != 1u || pre)
		fail |= BAD(FR_IMG_INIT);
	for (unsigned r = FR_IMG_INIT; r <= FR_IMG_BCODE; r++) {
		if (fail & BAD(r)) {
			*rc = (enum fr_image_rc)r;
			return FR_FLASH_OK;
		}
	}
	if (!*has_desc || front_bad || !has_first || !holds(first, &front_win[FW_L2]) ||
	    !holds(final, &front_win[FW_L2]) || !holds(init, &front_win[FW_L1_CODE]) ||
	    fcrc != d->front_crc)
		*rc = FR_IMG_NO_FRONT;
	else
		*rc = FR_IMG_OK;
	return FR_FLASH_OK;
}

enum fr_image_rc fr_image_valid(const uint8_t *img, uint32_t n, struct fr_desc *desc)
{
	const struct walk w = {img, NULL, 0u, n};
	struct fr_desc own;
	enum fr_image_rc rc;
	bool has;

	/* S-02.084: an image leaves the settings sector and the log free */
	if (img == NULL || n == 0u || n > FR_IMAGE_MAX)
		return FR_IMG_SIZE;
	(void)scan(&w, &rc, desc != NULL ? desc : &own, &has);
	return rc;
}

/* S-02.095, S-02.093: of slot k's log byte 0, entry 0 reads finished (a
 * trial start marked), or the trial was spent (pending-coded, not 0xFD). */
static int started(const struct fr_flash *f, unsigned k, bool *yes, bool *spent)
{
	uint8_t b;

	*yes = *spent = false;
	if (flash_read(f, FR_LOG_OFF(k), &b, 1u) != FR_FLASH_OK)
		return FR_FLASH_IO;
	*yes = FR_LOG0_E(b) == FR_LOG_E0_FINISHED;
	*spent = FR_LOG0_E(b) == FR_LOG_E0_PENDING && b != FR_LOG0_PENDING;
	return FR_FLASH_OK;
}

int fr_slot_info(const struct fr_flash *f, unsigned k, struct fr_slot_info *out)
{
	struct walk w;
	enum fr_image_rc rc;

	fr_fill(out, 0, sizeof *out);
	if (check(f, k, &out->good, &out->tried, &out->rec) != FR_FLASH_OK)
		return FR_FLASH_IO;
	/* S-02.095: the record gives a log */
	if (out->rec.tag == FR_TAG && out->rec.length >= 1u && out->rec.length <= FR_LOG_IMAGE_MAX) {
		if (flash_read(f, FR_LOG_OFF(k), &out->log0, 1u) != FR_FLASH_OK)
			return FR_FLASH_IO;
		out->has_log = true;
		if (out->tried) {
			out->started = FR_LOG0_E(out->log0) == FR_LOG_E0_FINISHED;
			out->spent = FR_LOG0_E(out->log0) == FR_LOG_E0_PENDING &&
				     out->log0 != FR_LOG0_PENDING;
		}
	}
	/* S-02.084: the stream within the record's length, or within the
	 * slot when the record is not plausible (erased or torn), as
	 * slot_score() reads it: the trusted path boots the stream whatever
	 * the record holds. But for the tag with a length of 2^31 or more,
	 * which no stream is read for: a write's selector erase, cut short,
	 * could leave a first stage checking that record, and its check is
	 * unspecified then (S-02.026, S-16.126).
	 * SPEC-Q: whether the first stage's check of such a record ends. */
	if (!plausible(&out->rec) && out->rec.tag == FR_TAG && out->rec.length >= 0x80000000u)
		return FR_FLASH_OK;
	w.img = NULL;
	w.f = f;
	w.base = FR_SLOT_BASE(k);
	w.n = plausible(&out->rec) ? out->rec.length : FR_SLOT_CAP;
	if (scan(&w, &rc, &out->desc, &out->has_desc) != FR_FLASH_OK)
		return FR_FLASH_IO;
	out->bootable = rc == FR_IMG_OK || rc == FR_IMG_NO_FRONT;
	out->has_front = rc == FR_IMG_OK;
	return FR_FLASH_OK;
}

/* S-02.106 rule 2: a TE build by its image's length and CRC-32. */
static const struct te_row {
	uint32_t n, crc;
	uint16_t v[4];
} te_rows[] = {
	{ 0x5ADB0u, 0x7AF4665Cu, { 1000u, 0u, 12u, 0u } },
	{ 0x5B7ACu, 0x0789B2D9u, { 1000u, 0u, 13u, 0u } },
	{ 0x60000u, 0x5DEE4B51u, { 1000u, 0u, 20u, 0u } },
	{ 0x60CA4u, 0xA930BEF3u, { 1000u, 0u, 23u, 0u } },
};
/* Rule 3: the CRC-32 of the slot's first TE_PREFIX_LEN bytes. */
#define TE_PREFIX_LEN 4468u
#define TE_PREFIX_CRC 0x31D25812u

int fr_slot_kind(const struct fr_flash *f, unsigned k, struct fr_slot_info *s)
{
	uint32_t c, crc = 0;

	s->kind = FR_KIND_NONE;
	if (k > 1u)
		return FR_FLASH_IO;
	if (!s->good && !s->tried)
		return FR_FLASH_OK;
	s->kind = FR_KIND_WB;
	if (s->has_desc)
		return FR_FLASH_OK;
	c = s->tried ? ~s->rec.crc32 : s->rec.crc32;
	for (unsigned i = 0; i < sizeof te_rows / sizeof te_rows[0]; i++)
		if (te_rows[i].n == s->rec.length && te_rows[i].crc == c) {
			s->kind = FR_KIND_TE_BUILD;
			fr_copy(s->te_version, te_rows[i].v, sizeof s->te_version);
			return FR_FLASH_OK;
		}
	s->kind = FR_KIND_UNKNOWN;
	if (s->rec.length < TE_PREFIX_LEN)
		return FR_FLASH_OK;
	if (flash_crc(f, FR_SLOT_BASE(k), TE_PREFIX_LEN, &crc) != FR_FLASH_OK)
		return FR_FLASH_IO;
	if (crc == TE_PREFIX_CRC)
		s->kind = FR_KIND_TE;
	return FR_FLASH_OK;
}

/* --- Selector writes --- */

bool fr_record_fails(const struct fr_record *r)
{
	return r->tag != FR_TAG || (r->length >= 1u && r->length <= FR_SLOT_CAP);
}

unsigned fr_reach(const struct fr_selector *sel, const bool good[2], const struct fr_record *rec)
{
	const unsigned n = fr_selected_slot(sel->index);

	if (sel->verified == 0x01u)
		return good[n] ? FR_REACH_NAMED : FR_REACH_NONE;
	if (good[n])
		return n == 1u || good[1] ? FR_REACH_NAMED : FR_REACH_NONE;
	if (n == 0u && good[1] && (sel->verified & 0xFEu) != 0u && fr_record_fails(rec))
		return FR_REACH_FALLBACK;
	return FR_REACH_NONE;
}

/* --- The recovery writer (S-02.040-S-02.052, S-02.084, S-02.090, S-02.091) --- */

/* S-02.042: the target slot's sectors, its record sector, the selector
 * sector, and nothing else, whatever a caller's state. */
static bool in_footprint(const struct fr_writer *w, uint32_t off, uint32_t len)
{
	const uint64_t end = (uint64_t)off + len;
	const uint32_t base = FR_SLOT_BASE(w->target), rec = FR_RECORD_OFF(w->target);

	if (w->target > 1u || len == 0u)
		return false;
	return (off >= base && end <= (uint64_t)base + FR_SLOT_CAP) ||
	       (off >= rec && end <= (uint64_t)rec + FR_SECTOR) ||
	       (off >= FR_SELECTOR_OFF && end <= (uint64_t)FR_SELECTOR_OFF + FR_SECTOR);
}

static bool w_erase(const struct fr_writer *w, uint32_t off)
{
	return off % FR_SECTOR == 0u && in_footprint(w, off, FR_SECTOR) &&
	       w->f->erase(w->f->ctx, off) == 0;
}

/* Within one page (S-13b.016). */
static bool w_program(const struct fr_writer *w, uint32_t off, const uint8_t *p, uint32_t len)
{
	return len >= 1u && off % FR_PAGE + len <= FR_PAGE && in_footprint(w, off, len) &&
	       w->f->program(w->f->ctx, off, p, len) == 0;
}

static bool stop(struct fr_writer *w, int result)
{
	/* S-02.084: the other slot could not be written: an in-place write
	 * of the running slot may follow in this recovery. */
	if (w->home != NULL && !w->own && w->way == FR_WAY_PLAIN &&
	    (result == FR_WR_FLASH || result == FR_WR_READBACK))
		w->home->other_failed = true;
	/* S-02.084: a write that ends well has left the selector settled. */
	if (w->home != NULL && result == FR_WR_OK && w->act == FR_ACT_NONE)
		w->home->unsettled = false;
	w->result = result;
	w->state = FR_WS_DONE;
	return true;
}

static int refuse(struct fr_writer *w, int result)
{
	(void)stop(w, result);
	return result;
}

/* S-02.093: slot k holds img[0..n) under the trial mark: its record has
 * the tag, the length n and the complement of the image's CRC-32, and its
 * n bytes are the image's. */
static int tried_image(const struct fr_flash *f, unsigned k, const uint8_t *img, uint32_t n)
{
	struct fr_record r;

	if (fr_read_record(f, k, &r) != FR_FLASH_OK)
		return FR_WR_READ;
	if (!fr_tried(&r, fr_crc32(0, img, n)) || r.length != n)
		return FR_WR_NOT_TRIED;
	for (uint32_t off = 0; off < n;) {
		const uint32_t len = n - off < FR_PAGE ? n - off : FR_PAGE;

		if (flash_read(f, FR_SLOT_BASE(k) + off, chunk, len) != FR_FLASH_OK)
			return FR_WR_READ;
		if (!fr_same(chunk, img + off, len))
			return FR_WR_NOT_TRIED;
		off += len;
	}
	return FR_WR_OK;
}

/* The running slot as the plan reads it; off the stack: one writer, no
 * interrupts. */
static struct fr_slot_info cur;

static unsigned decided(struct fr_plan *plan, uint8_t rc)
{
	plan->refusal = rc;
	return 0;
}

/*
 * S-02.093 flag bit 2: the slot that holds the image as tried, by the
 * rule's two places. From the old front: the start condition with A's
 * front intact in the flash, and B tried; B is selected. From the tried
 * slot's own front, in a trial start: A tried, the selector naming B with
 * verified 0x01, and B good; A is selected. Neither once this recovery has
 * begun a write of A, written the selector to B or begun a trial. From
 * the old front B must also have started: its own front marked a trial
 * start in its log (else FR_WR_NOT_STARTED), so that a front that never
 * ran is never selected. From A's own front, a trial a start of B spent is
 * no trial start: refused (FR_WR_NOT_TRIED), so that its log is never
 * made to read finished.
 */
static unsigned select_plan(const struct fr_facts *x, struct fr_plan *plan)
{
	const struct fr_slot_info *A = x->a;
	const unsigned a = x->run->slot, b = 1u - a;
	unsigned s;

	if (!(x->known & FR_KNOW_B))
		return FR_KNOW_B;
	plan->own_good = A->good;
	if (!(x->known & FR_KNOW_SEL))
		return decided(plan, FR_WR_READ);
	if (x->home->moved || x->home->spoiled || (x->home->trial && !x->force))
		return decided(plan, FR_WR_TRIAL_STATE);
	if (x->b_tried && !(x->known & FR_KNOW_LOG))
		return FR_KNOW_LOG;
	s = fr_selected_slot(x->sel.index);
	if (x->sel.verified == 0x01u && s == a && A->has_front && x->b_tried) {
		if (!x->b_started && !x->force)
			return decided(plan, FR_WR_NOT_STARTED);
		plan->target = (uint8_t)b;
	} else if (x->sel.verified == 0x01u && s == b && A->tried && !A->spent && x->b_good) {
		plan->target = (uint8_t)a;
	} else {
		return decided(plan, FR_WR_NOT_TRIED);
	}
	plan->keep = (uint8_t)(1u - plan->target);
	plan->way = FR_WAY_SELECT;
	return decided(plan, FR_WR_OK);
}

unsigned fr_plan_decide(const struct fr_facts *x, struct fr_plan *plan, struct fr_desc *front)
{
	const struct fr_home *home = x->home;
	const struct fr_slot_info *A = x->a;
	const bool flag = (x->flags & FR_BEGIN_F_RUNNING) != 0u;
	const uint8_t v = x->sel.verified;
	unsigned a, b, s;
	bool rescue;

	plan->way = FR_WAY_NONE;
	plan->target = FR_NO_SLOT;
	plan->keep = FR_NO_SLOT;
	plan->refusal = FR_WR_READ;
	plan->own_good = false;
	plan->only_same = false;
	plan->rescue = false;
	plan->trial_ok = false;

	/* S-01.002: A is the slot this recovery runs from. */
	if (!x->run->established || x->run->slot > 1u)
		return decided(plan, FR_WR_NOT_RUNNING);
	if (!(x->known & FR_KNOW_A))
		return FR_KNOW_A;
	if (x->flags & FR_BEGIN_F_SELECT)
		return select_plan(x, plan);
	a = x->run->slot;
	b = 1u - a;
	plan->own_good = A->good;
	/* S-02.084: this recovery began a write of A and A is bad. Then no
	 * start boots A, and the selector's conditions protect nothing. */
	rescue = home->spoiled && !A->good;
	if (home->spoiled && A->good && home->unsettled) {
		/* A was written and read back good, and the write did not end
		 * well after that: the selector may not be settled on A. With
		 * B bad that keeps the state; with B good as well every
		 * selector boots a good slot. */
		if (!(x->known & FR_KNOW_B))
			return FR_KNOW_B;
		rescue = !x->b_good;
	}
	plan->rescue = rescue;
	if (!(x->known & FR_KNOW_SEL))
		return decided(plan, FR_WR_READ);
	s = fr_selected_slot(x->sel.index);
	if (!rescue) {
		/* The selector names A, or B because this recovery wrote it so. */
		if (s != a && !home->moved && !x->force)
			return decided(plan, FR_WR_NOT_SELECTED);
		if (v != 0x01u && !x->force)
			return decided(plan, FR_WR_UNVERIFIED);	/* S-02.090 */
	}

	/* S-02.084: A's front as the flash holds it, so that a start after a
	 * cut reaches this recovery again; once this recovery has begun a
	 * write of A, the front A had. A good A is not needed. */
	if (A->has_front) {
		if (front != NULL)
			*front = A->desc;
	} else if (home->spoiled) {
		if (front != NULL) {
			fr_copy(front->pin, home->pin, FR_SHA256_LEN);
			front->front_crc = home->front_crc;
		}
	} else if (!x->force) {
		return decided(plan, A->good ? FR_WR_FRONT : FR_WR_SELECTED_BAD);
	}
	if ((rescue || s != a || flag) && !(x->known & FR_KNOW_B))
		return FR_KNOW_B;

	if (rescue) {
		/* B is slot 0, named, and programs alone settle the selector
		 * on it: verified has bit 0 set and is not 0x01 */
		const bool by_programs = s == b && b == 0u && (v & 0x01u) != 0u && v != 0x01u;

		if (A->good) {
			/* A is good and B bad; only the selector is open. A is
			 * the only good slot and is not erased: with the flag
			 * its own image settles the selector on it; without,
			 * B is written. */
			plan->way = FR_WAY_PLAIN;
			plan->target = (uint8_t)(flag ? a : b);
			plan->only_same = flag;
		} else if (x->b_good && !(by_programs && !flag) &&
			   (s == b || (v == 0x00u && fr_record_fails(&A->rec)))) {
			/* The selector leads to B already, and is not erased
			 * while A is damaged: A is the slot it does not lead
			 * to (S-02.043). It names B; or it names A with
			 * verified 0x00, where a first stage fails A's check
			 * and falls back to B, and an erase of that value,
			 * cut short, could read verified 0x01 over A's index.
			 * Where programs alone settle it on B, the request
			 * without the flag does that instead (below). */
			plan->way = FR_WAY_PLAIN;
			plan->target = (uint8_t)a;
		} else if (!flag) {
			plan->way = FR_WAY_PLAIN;
			plan->target = (uint8_t)b;
			plan->only_same = x->b_good;
		} else {
			plan->way = x->b_good ? FR_WAY_SELECT_FIRST : FR_WAY_IN_PLACE;
			plan->target = (uint8_t)a;
		}
	} else if (s != a) {
		/* S-02.090: B is selected and every reset boots it. */
		/* forced too: a write of A under a selector that names a bad B
		 * would leave nothing that starts, with no hold */
		if (!x->b_good)
			return decided(plan, FR_WR_SELECTED_BAD);
		plan->way = FR_WAY_PLAIN;
		plan->target = (uint8_t)a;	/* S-02.043 */
	} else if (!flag) {
		plan->way = FR_WAY_PLAIN;
		plan->target = (uint8_t)b;	/* S-02.043 */
		/* S-02.093 flag bit 1: A good, neither written nor moved from
		 * by this recovery */
		plan->trial_ok = (A->good || x->force) && !home->moved && !home->spoiled;
	} else {
		/* S-02.084: in place only where B cannot be selected first,
		 * and only after a write of B has failed in this recovery. */
		if (!x->b_good && !home->other_failed && !x->force)
			return decided(plan, FR_WR_TRY_OTHER);
		plan->way = x->b_good ? FR_WAY_SELECT_FIRST : FR_WAY_IN_PLACE;
		plan->target = (uint8_t)a;
	}
	plan->keep = (uint8_t)(1u - plan->target);
	return decided(plan, FR_WR_OK);
}

int fr_writer_plan(const struct fr_flash *f, const struct fr_running *run,
		   const struct fr_home *home, uint8_t flags, struct fr_plan *plan,
		   struct fr_desc *front)
{
	struct fr_facts x = { .run = run, .home = home, .flags = flags, .a = &cur,
			      .force = (flags & FR_BEGIN_F_FORCE) != 0u };
	unsigned need;
	struct fr_record brec = { 0, 0, 0 };

	fr_fill(front, 0, sizeof *front);
	/* bounded: each pass reads a fact the plan asks for, never one read
	 * before, and only where the plan needs it */
	while ((need = fr_plan_decide(&x, plan, front)) != 0u) {
		const unsigned b = 1u - run->slot;
		bool ok, spent;

		if (need == FR_KNOW_A) {
			ok = fr_read_selector(f, &x.sel) == FR_FLASH_OK &&
			     fr_slot_info(f, run->slot, &cur) == FR_FLASH_OK;
			need |= FR_KNOW_SEL;
		} else if (need == FR_KNOW_B) {
			ok = check(f, b, &x.b_good, &x.b_tried, &brec) == FR_FLASH_OK;
		} else {
			/* S-02.095: only a record that gives a log */
			ok = brec.length > FR_LOG_IMAGE_MAX ||
			     started(f, b, &x.b_started, &spent) == FR_FLASH_OK;
		}
		if (!ok)
			return plan->refusal;	/* FR_WR_READ */
		x.known |= (uint8_t)need;
	}
	return plan->refusal;
}

int fr_writer_begin(struct fr_writer *w, const struct fr_flash *f, const struct fr_running *run,
		    struct fr_home *home, const uint8_t *img, uint32_t n,
		    const uint8_t sha[FR_SHA256_LEN], uint8_t flags)
{
	const bool force = (flags & FR_BEGIN_F_FORCE) != 0u;
	struct fr_plan plan;
	struct fr_desc d, front;
	enum fr_image_rc irc;
	int rc;

	if (w->state > FR_WS_IDLE && w->state < FR_WS_DONE)
		return FR_WR_BUSY;
	fr_fill(w, 0, sizeof *w);
	w->f = f;
	w->img = img;
	w->n = n;
	w->target = FR_NO_SLOT;
	w->way = FR_WAY_NONE;
	w->image_rc = FR_IMG_OK;

	/* The image first: S-02.044, then S-02.091 before the start
	 * condition is read, and the front; then that it is the one
	 * announced. */
	irc = fr_image_valid(img, n, &d);
	if (irc == FR_IMG_SIZE) {
		w->image_rc = FR_IMG_SIZE;
		return refuse(w, FR_WR_SIZE);
	}
	/* S-02.093: forced, a bootable stream with no waffle-boot front */
	if (irc != FR_IMG_OK && !(irc == FR_IMG_NO_FRONT && force)) {
		w->image_rc = (uint8_t)irc;
		return refuse(w, FR_WR_IMAGE);
	}
	fr_sha256_init(&w->sha);
	fr_sha256_add(&w->sha, img, n);
	fr_sha256_end(&w->sha, w->want_sha);
	if (!fr_same(w->want_sha, sha, FR_SHA256_LEN))
		return refuse(w, FR_WR_HASH);
	if (irc == FR_IMG_OK) {
		w->has_version = true;
		fr_copy(w->version, d.bl_version, sizeof w->version);
	}

	/* S-02.084, S-02.090: the start condition, from the flash. */
	rc = fr_writer_plan(f, run, home, flags, &plan, &front);
	if (rc == FR_WR_NOT_STARTED) {
		/* S-02.093: a slot that does not hold this image as tried
		 * (20) before one whose front has not started (21) */
		const int trc = tried_image(f, 1u - run->slot, img, n);

		return refuse(w, trc != FR_WR_OK ? trc : rc);
	}
	if (rc != FR_WR_OK)
		return refuse(w, rc);

	if (plan.way == FR_WAY_SELECT) {
		/* S-02.093: the image the selected slot holds as tried, byte
		 * for byte; no front rule */
		rc = tried_image(f, plan.target, img, n);
		if (rc != FR_WR_OK)
			return refuse(w, rc);
	} else if (irc != FR_IMG_OK || !fr_same(front.pin, d.pin, FR_SHA256_LEN) ||
		   front.front_crc != d.front_crc) {
		/* S-02.084: the image's front is not the one that runs, by its
		 * pin and front CRC: written only for a trial (S-02.093 flag
		 * bit 1), and only where one may go; or, forced, as planned. */
		if (flags & FR_BEGIN_F_TRIAL) {
			if (!plan.trial_ok)
				return refuse(w, FR_WR_TRIAL_STATE);
			plan.way = FR_WAY_TRIAL;
		} else if (!force) {
			return refuse(w, FR_WR_FRONT);
		}
	}
	w->crc = fr_crc32(0, img, n);	/* S-02.046 */
	/* the trial mark: a record sure to fail the first stage's check */
	w->rec_crc = plan.way == FR_WAY_TRIAL ? ~w->crc : w->crc;
	if (plan.only_same) {
		struct fr_record r;

		/* S-02.084: the target is good, and the only good slot. It
		 * is not erased: only the image it holds is taken, for the
		 * read-back and the selector write. */
		if (fr_read_record(f, plan.target, &r) != FR_FLASH_OK)
			return refuse(w, FR_WR_READ);
		if (r.length != n || r.crc32 != w->crc)
			return refuse(w, FR_WR_ONLY_GOOD);
	}

	w->target = plan.target;
	w->way = plan.way;
	w->rescue = plan.rescue;
	w->home = home;
	w->pos = 0;
	w->result = FR_WR_OK;
	if (plan.target == run->slot) {
		/* Once this write erases A, A's own front may be gone from
		 * the flash: a retry goes by the front it had. */
		w->own = true;
		w->front_crc = front.front_crc;
		fr_copy(w->front_pin, front.pin, FR_SHA256_LEN);
	}
	/* a select writes nothing in the slot or its log sector, but from the
	 * tried slot's own front the trial start's mark */
	w->state = plan.only_same ? FR_WS_READBACK :
		   plan.way == FR_WAY_SELECT ? (w->own ? FR_WS_SELECT_LOG : FR_WS_RECORD_ERASE) :
		   plan.way == FR_WAY_SELECT_FIRST ? FR_WS_SELECT_ERASE : FR_WS_SLOT;
	return FR_WR_OK;
}

/* Class 1 (S-02.045): one sector of the image, erased, then programmed a
 * page at a time; nothing past n, no sector wholly beyond the image. */
static void slot_step(struct fr_writer *w)
{
	const uint32_t base = FR_SLOT_BASE(w->target);
	const uint32_t end = w->n - w->pos < FR_SECTOR ? w->n : w->pos + FR_SECTOR;

	if (w->pos == 0u && w->own) {
		/* the running slot's first erase is next */
		fr_copy(w->home->pin, w->front_pin, FR_SHA256_LEN);
		w->home->front_crc = w->front_crc;
		w->home->spoiled = true;
		w->home->unsettled = true;
	}
	/* S-02.093: no select in this recovery once a trial is begun */
	if (w->pos == 0u && w->way == FR_WAY_TRIAL)
		w->home->trial = true;
	if (!w_erase(w, base + w->pos)) {
		(void)stop(w, FR_WR_FLASH);
		return;
	}
	for (uint32_t off = w->pos; off < end;) {
		uint32_t k = end - off < FR_PAGE ? end - off : FR_PAGE;

		if (!w_program(w, base + off, w->img + off, k)) {
			(void)stop(w, FR_WR_FLASH);
			return;
		}
		off += k;
	}
	w->pos = end;
	if (w->pos == w->n)
		w->state = FR_WS_LOG_ERASE;
}

/* Byte 0 of the target's log sector reads v. */
static bool log0_reads(const struct fr_writer *w, uint8_t v)
{
	uint8_t b;

	return flash_read(w->f, FR_LOG_OFF(w->target), &b, 1u) == FR_FLASH_OK && b == v;
}

/* S-02.095: byte 0 of the target's log sector programmed v, read back. */
static bool log0_put(const struct fr_writer *w, uint8_t v)
{
	return w_program(w, FR_LOG_OFF(w->target), &v, 1u) && log0_reads(w, v);
}

/* Class 4 (S-02.047): the record, then 4 KiB of the n bytes a step. */
static void readback_step(struct fr_writer *w)
{
	const uint32_t base = FR_SLOT_BASE(w->target);
	const uint32_t end = w->n - w->pos < FR_SECTOR ? w->n : w->pos + FR_SECTOR;
	struct fr_record r;

	if (w->pos == 0u) {
		if (fr_read_record(w->f, w->target, &r) != FR_FLASH_OK) {
			(void)stop(w, FR_WR_FLASH);
			return;
		}
		/* a good slot (S-02.016) of the n bytes, whose crc32 is the image's */
		if (r.tag != FR_TAG || r.length != w->n || r.crc32 != w->rec_crc) {
			(void)stop(w, FR_WR_READBACK);
			return;
		}
		/* a trial's entry 0 reads pending */
		if (w->way == FR_WAY_TRIAL && !log0_reads(w, FR_LOG0_PENDING)) {
			(void)stop(w, FR_WR_READBACK);
			return;
		}
		w->rb_crc = 0;
		fr_sha256_init(&w->sha);
	}
	for (uint32_t off = w->pos; off < end;) {
		uint32_t k = end - off < FR_PAGE ? end - off : FR_PAGE;

		if (flash_read(w->f, base + off, chunk, k) != FR_FLASH_OK) {
			(void)stop(w, FR_WR_FLASH);
			return;
		}
		if (!fr_same(chunk, w->img + off, k)) {
			(void)stop(w, FR_WR_READBACK);
			return;
		}
		w->rb_crc = fr_crc32(w->rb_crc, chunk, k);
		fr_sha256_add(&w->sha, chunk, k);
		off += k;
	}
	w->pos = end;
	if (w->pos < w->n)
		return;
	if (w->rb_crc != w->crc) {
		(void)stop(w, FR_WR_READBACK);
		return;
	}
	fr_sha256_end(&w->sha, w->readback_sha);
	w->state = FR_WS_SELECTOR_ERASE;
}

/* The selector as the flash now holds it is {index, verified}. */
static bool selector_reads(const struct fr_writer *w, uint8_t index, uint8_t verified)
{
	uint8_t raw[2];

	return flash_read(w->f, FR_SELECTOR_OFF, raw, 2u) == FR_FLASH_OK &&
	       raw[0] == index && raw[1] == verified;
}

/*
 * The erased selector programmed {index, verified} a byte at a time: the
 * index byte first, alone, read back before the verified byte follows.
 *
 * So while the running slot is damaged (w->rescue), and in every settling
 * (settle()). A program cut
 * short clears some of its bits (S-02 Terms), so the two bytes written at
 * once could be left as the damaged slot's index with verified 0x01,
 * which the first stage trusts for ever (S-02.020). Written one by
 * one, a torn index names one slot or the other with verified still 0xFF,
 * which a first stage checks (S-02.021), and verified is written only
 * under the whole index of a slot read good. A trial's settling back on
 * the running slot (S-02.093) is written so too: the other slot is tried.
 */
static bool selector_bytes(const struct fr_writer *w, uint8_t index, uint8_t verified)
{
	const uint8_t raw[2] = {index, verified};
	uint8_t back;

	return w_program(w, FR_SELECTOR_OFF, raw, 1u) &&
	       flash_read(w->f, FR_SELECTOR_OFF, &back, 1u) == FR_FLASH_OK && back == index &&
	       w_program(w, FR_SELECTOR_OFF + 1u, raw + 1, 1u) &&
	       selector_reads(w, index, verified);
}

/* The erased selector programmed {index, verified} and read back: a byte
 * at a time where selector_bytes() says, else in one program. */
static bool selector_put(const struct fr_writer *w, uint8_t index, uint8_t verified)
{
	const uint8_t raw[2] = {index, verified};

	if (w->rescue || w->way == FR_WAY_TRIAL || w->bytewise)
		return selector_bytes(w, index, verified);
	return w_program(w, FR_SELECTOR_OFF, raw, 2u) && selector_reads(w, index, verified);
}

/*
 * S-02.084, with the running slot damaged: the selector settled on slot
 * t, which was just read good, so that every first stage follows it to t
 * with no selector write of its own.
 *
 * Nothing is written where it names t with verified 0x01, or names slot 1
 * as t. For slot 0, where verified has bit 0 set, programs alone reach
 * {0x00, 0x01} and no erase is made: a program only clears bits, so cut
 * short it leaves the index naming the slot it named or slot 0, and
 * verified short of 0x01 or at it; an erase cut short could set the bits
 * of another index under a verified it left at 0x01. Else the sector is
 * erased and {t, 0x01} programmed, the index first, read back, then
 * verified (S-02.084, S-02.048), whatever way the writer runs.
 */
static bool settle(const struct fr_writer *w, unsigned t)
{
	static const uint8_t zero = 0x00u, one = 0x01u;
	struct fr_selector sel;
	uint8_t back;

	if (fr_read_selector(w->f, &sel) != FR_FLASH_OK)
		return false;
	if (fr_selected_slot(sel.index) == t && (sel.verified == 0x01u || t == 1u))
		return true;
	if (t == 0u && (sel.verified & 0x01u) != 0u) {
		if (sel.index != 0x00u &&
		    !(w_program(w, FR_SELECTOR_OFF, &zero, 1u) &&
		      flash_read(w->f, FR_SELECTOR_OFF, &back, 1u) == FR_FLASH_OK && back == 0x00u))
			return false;
		if (sel.verified != 0x01u && !w_program(w, FR_SELECTOR_OFF + 1u, &one, 1u))
			return false;
		return selector_reads(w, 0x00u, 0x01u);
	}
	return w_erase(w, FR_SELECTOR_OFF) && selector_bytes(w, (uint8_t)t, 0x01u);
}

/* S-02.093: a selector step of the trial failed: the selector settled back
 * on the running slot, which is good, before status 11. */
static bool trial_back(struct fr_writer *w)
{
	(void)settle(w, 1u - w->target);
	return stop(w, FR_WR_SELECTOR);
}


/* --- The actions on a slot's image as it lies (S-02.084) --- */

static void op(struct fr_act_plan *p, uint8_t o)
{
	if (p->n_ops < FR_ACT_OPS)
		p->ops[p->n_ops++] = o;
}

static void refused(struct fr_act_plan *p, uint8_t rc)
{
	p->status = rc;
	p->n_ops = 0;
}

/* The warning's tier for slot t, against the running slot's front: as the
 * flash holds it, or as this recovery keeps it once it began a write of
 * that slot. */
static uint8_t tier_of(const struct fr_slot_info *t, const struct fr_slot_info *a,
		       const struct fr_home *home)
{
	const uint8_t *pin = a->has_front ? a->desc.pin : home->spoiled ? home->pin : NULL;
	const uint32_t crc = a->has_front ? a->desc.front_crc : home->front_crc;

	if (!t->good && !t->tried)
		return FR_TIER_BAD;
	if (!t->has_front)
		return FR_TIER_FOREIGN;
	if (pin == NULL || !fr_same(pin, t->desc.pin, FR_SHA256_LEN) || crc != t->desc.front_crc)
		return FR_TIER_FRONT;
	return FR_TIER_SAME;
}

/* A slot made good again or switched to keeps no trial's entry. */
static void log_clean(struct fr_act_plan *p, const struct fr_slot_info *t)
{
	if (t->has_log && FR_LOG0_E(t->log0) == FR_LOG_E0_PENDING)
		op(p, FR_OP_LOG_ERASE);
}

/* A tried slot's record made right, after its bytes are confirmed. */
static void record_right(struct fr_act_plan *p, const struct fr_slot_info *t, bool own)
{
	op(p, FR_OP_CRC);
	if (own && t->has_log && t->log0 == FR_LOG0_PENDING)
		op(p, FR_OP_LOG_MARK);		/* S-02.095: the trial start's mark */
	else
		log_clean(p, t);
	op(p, FR_OP_REC_ERASE);
	op(p, FR_OP_REC_PUT);
	op(p, FR_OP_REC_CHECK);
}

void fr_act_decide(const struct fr_act_facts *x, struct fr_act_plan *p)
{
	const struct fr_slot_info *A = x->a, *B = x->b;
	const struct fr_home *home = x->home;
	unsigned a, b, s;
	bool damaged, at_home, trusted;

	fr_fill(p, 0, sizeof *p);
	p->target = FR_NO_SLOT;
	p->way = FR_WAY_NONE;
	p->tier = FR_TIER_NONE;
	p->status = FR_WR_READ;
	/* S-01.002: the slots are told apart by the running one. */
	if (!x->run->established || x->run->slot > 1u) {
		refused(p, FR_WR_NOT_RUNNING);
		return;
	}
	if (!x->sel_ok)
		return;
	a = x->run->slot;
	b = 1u - a;
	s = fr_selected_slot(x->sel.index);
	damaged = home->spoiled && !A->good;
	at_home = s == a && x->sel.verified == 0x01u;
	trusted = s == b && x->sel.verified == 0x01u;
	p->status = FR_WR_OK;
	p->target = (uint8_t)b;

	switch (x->act) {
	case FR_ACT_TRY:
		p->way = FR_WAY_TRY;
		p->tier = tier_of(B, A, home);
		/* the trial start comes back to A: not once A or the selector
		 * was written by this recovery */
		if (home->moved || home->spoiled) {
			refused(p, FR_WR_TRIAL_STATE);
			return;
		}
		if (trusted) {
			refused(p, FR_WR_ALREADY);
			return;
		}
		if (!x->force) {
			if (!at_home) {
				refused(p, s != a ? FR_WR_NOT_SELECTED : FR_WR_UNVERIFIED);
				return;
			}
			if (!A->good || !A->has_front) {
				refused(p, FR_WR_TRIAL_STATE);
				return;
			}
			if (!B->good && !B->tried) {
				refused(p, FR_WR_TARGET_BAD);
				return;
			}
		}
		if (B->good || B->tried) {
			if (B->has_log) {
				op(p, FR_OP_LOG_ERASE);
				op(p, FR_OP_LOG_PENDING);
			}
			if (B->good) {
				p->mark = true;
				op(p, FR_OP_REC_ERASE);
				op(p, FR_OP_REC_PUT);
			}
		} else if (!fr_record_fails(&B->rec)) {
			/* S-02.021, S-02.026: a record the check may pass, or
			 * not end on, is erased: another tag fails it unread */
			op(p, FR_OP_REC_ERASE);
		}
		op(p, FR_OP_REC_CHECK);
		op(p, FR_OP_TRIAL_ERASE);
		if (b == 0u) {
			op(p, FR_OP_TRIAL_VERIFIED);
			op(p, FR_OP_TRIAL_INDEX);
		}
		return;
	case FR_ACT_SWITCH:
		p->way = FR_WAY_SWITCH;
		p->tier = tier_of(B, A, home);
		if (trusted) {
			refused(p, FR_WR_ALREADY);
			return;
		}
		/* S-02.091: the ROM would not start it, on any reset */
		if (!B->bootable) {
			refused(p, FR_WR_IMAGE);
			return;
		}
		if (!x->force) {
			if (home->moved || home->spoiled) {
				refused(p, FR_WR_TRIAL_STATE);
				return;
			}
			if (!at_home) {
				refused(p, s != a ? FR_WR_NOT_SELECTED : FR_WR_UNVERIFIED);
				return;
			}
			if (!B->good && !B->tried) {
				refused(p, FR_WR_TARGET_BAD);
				return;
			}
			if (B->tried && !B->started) {
				refused(p, FR_WR_NOT_STARTED);
				return;
			}
			if (p->tier != FR_TIER_SAME) {
				refused(p, FR_WR_FRONT);
				return;
			}
		}
		if (B->tried)
			record_right(p, B, false);
		else if (B->good)
			log_clean(p, B);
		if (damaged) {
			op(p, FR_OP_SETTLE);
		} else {
			/* S-02.048 for a good slot; a bad one is never checked */
			p->sel_verified = (uint8_t)(B->good || B->tried ? 0x00u : 0x01u);
			op(p, FR_OP_SEL_ERASE);
			op(p, FR_OP_SEL_PUT);
		}
		return;
	case FR_ACT_KEEP:
		p->way = FR_WAY_KEEP;
		if (B->tried) {
			p->tier = tier_of(B, A, home);
			if (!x->force) {
				if (home->moved || home->spoiled || home->trial) {
					refused(p, FR_WR_TRIAL_STATE);
					return;
				}
				if (!at_home || !A->has_front) {
					refused(p, FR_WR_NOT_TRIED);
					return;
				}
				if (!B->started) {
					refused(p, FR_WR_NOT_STARTED);
					return;
				}
			}
			record_right(p, B, false);
		} else if (A->tried) {
			/* a trial start: the running slot is the tried one */
			p->target = (uint8_t)a;
			p->tier = FR_TIER_SAME;
			if (!x->force) {
				if (home->moved || home->spoiled || home->trial) {
					refused(p, FR_WR_TRIAL_STATE);
					return;
				}
				if (!trusted || !B->good || A->spent) {
					refused(p, FR_WR_NOT_TRIED);
					return;
				}
			}
			record_right(p, A, true);
		} else {
			refused(p, FR_WR_NOT_TRIED);
			return;
		}
		if (damaged) {
			op(p, FR_OP_SETTLE);
		} else {
			p->sel_verified = 0x00u;
			op(p, FR_OP_SEL_ERASE);
			op(p, FR_OP_SEL_PUT);
		}
		return;
	case FR_ACT_PUT_BACK:
		p->way = FR_WAY_PUT_BACK;
		if (!B->tried) {
			refused(p, FR_WR_NOT_TRIED);
			return;
		}
		/* a try not yet started: B made good under that selector
		 * would pass the first stage's check and stay selected */
		if (s == b && x->sel.verified != 0x01u && !damaged)
			op(p, FR_OP_SETTLE_BACK);
		record_right(p, B, false);
		return;
	default:
		refused(p, FR_WR_READ);
		p->target = FR_NO_SLOT;
		return;
	}
}

/* The other slot as an action reads it (cur is the running one). */
static struct fr_slot_info oth;

int fr_act_begin(struct fr_writer *w, const struct fr_flash *f, const struct fr_running *run,
		 struct fr_home *home, uint8_t act, bool force, struct fr_act_plan *plan)
{
	struct fr_act_facts x = { .run = run, .home = home, .a = &cur, .b = &oth,
				  .act = act, .force = force };
	struct fr_act_plan own;
	const struct fr_slot_info *t;
	bool ok = true;

	if (plan == NULL)
		plan = &own;
	if (w->state > FR_WS_IDLE && w->state < FR_WS_DONE)
		return FR_WR_BUSY;
	fr_fill(w, 0, sizeof *w);
	w->f = f;
	w->target = FR_NO_SLOT;
	w->way = FR_WAY_NONE;
	w->image_rc = FR_IMG_OK;
	w->act = act;
	fr_fill(&cur, 0, sizeof cur);
	fr_fill(&oth, 0, sizeof oth);
	if (run->established && run->slot <= 1u) {
		x.sel_ok = fr_read_selector(f, &x.sel) == FR_FLASH_OK;
		ok = x.sel_ok && fr_slot_info(f, run->slot, &cur) == FR_FLASH_OK &&
		     fr_slot_info(f, 1u - run->slot, &oth) == FR_FLASH_OK;
	}
	fr_act_decide(&x, plan);
	if (!ok && plan->status != FR_WR_NOT_RUNNING)
		refused(plan, FR_WR_READ);
	if (plan->status != FR_WR_OK)
		return refuse(w, plan->status);
	t = plan->target == run->slot ? &cur : &oth;
	w->target = plan->target;
	w->way = plan->way;
	w->home = home;
	w->own = plan->target == run->slot;
	w->n = t->rec.length;
	/* the record put: the trial mark of a good slot's CRC, or the right
	 * record of a tried slot's */
	w->crc = t->good ? t->rec.crc32 : ~t->rec.crc32;
	w->rec_crc = plan->mark ? ~w->crc : w->crc;
	w->sel_verified = plan->sel_verified;
	w->bytewise = plan->sel_verified == 0x01u;
	w->n_ops = plan->n_ops;
	for (unsigned i = 0; i < plan->n_ops; i++) {
		w->ops[i] = plan->ops[i];
		if (plan->ops[i] == FR_OP_REC_PUT)
			w->rec_put = true;
		if (plan->ops[i] == FR_OP_LOG_PENDING)
			w->log_pending = true;
		/* selector_put()'s index first for every settling, as in the
		 * damaged state's writes: a torn {t, 01} could trust A */
		if (plan->ops[i] == FR_OP_SETTLE || plan->ops[i] == FR_OP_SETTLE_BACK)
			w->bytewise = true;
	}
	w->result = FR_WR_OK;
	w->state = FR_WS_ACT;
	return FR_WR_OK;
}

/* FR_OP_CRC: 4 KiB of the slot's n bytes a step; at the end their CRC-32
 * is the one the record is written for. */
static bool crc_step(struct fr_writer *w)
{
	const uint32_t end = w->n - w->pos < FR_SECTOR ? w->n : w->pos + FR_SECTOR;

	if (w->pos == 0u)
		w->rb_crc = 0;
	if (flash_crc(w->f, FR_SLOT_BASE(w->target) + w->pos, end - w->pos, &w->rb_crc) !=
	    FR_FLASH_OK) {
		(void)stop(w, FR_WR_FLASH);
		return false;
	}
	w->pos = end;
	if (w->pos < w->n)
		return false;
	if (w->rb_crc != w->crc) {
		(void)stop(w, FR_WR_READBACK);
		return false;
	}
	w->pos = 0;
	return true;
}

/* FR_OP_REC_CHECK: the record as written, or, where none was, one the
 * first stage's check is sure to fail; and a pending log reads 0xFD. */
static bool rec_check(const struct fr_writer *w)
{
	struct fr_record r;

	if (fr_read_record(w->f, w->target, &r) != FR_FLASH_OK)
		return false;
	if (w->rec_put ? (r.tag != FR_TAG || r.length != w->n || r.crc32 != w->rec_crc) :
			 (w->act == FR_ACT_TRY && !fr_record_fails(&r)))
		return false;
	return !w->log_pending || log0_reads(w, FR_LOG0_PENDING);
}

/* One operation of an action; the CRC pass a part of it. */
static void act_step(struct fr_writer *w)
{
	static const uint8_t fe = 0xFEu, zero = 0x00u;
	uint8_t raw[12], b;
	bool ok = true;
	int fail = FR_WR_FLASH;

	if (w->ip >= w->n_ops) {
		(void)stop(w, FR_WR_OK);
		return;
	}
	/* S-02.093: no select follows a try in this recovery, unforced */
	if (w->ip == 0u && w->act == FR_ACT_TRY)
		w->home->trial = true;
	switch (w->ops[w->ip]) {
	case FR_OP_SETTLE_BACK:
		ok = settle(w, 1u - w->target);
		fail = FR_WR_SELECTOR;
		break;
	case FR_OP_CRC:
		if (!crc_step(w))
			return;		/* more to read, or stopped */
		break;
	case FR_OP_LOG_ERASE:
		ok = w_erase(w, FR_LOG_OFF(w->target));
		break;
	case FR_OP_LOG_PENDING:
		ok = log0_put(w, FR_LOG0_PENDING);
		break;
	case FR_OP_LOG_MARK:
		ok = flash_read(w->f, FR_LOG_OFF(w->target), &b, 1u) == FR_FLASH_OK &&
		     (b != FR_LOG0_PENDING || log0_put(w, FR_LOG0_MARKED));
		break;
	case FR_OP_REC_ERASE:
		ok = w_erase(w, FR_RECORD_OFF(w->target));
		break;
	case FR_OP_REC_PUT:
		fr_put32(raw, w->rec_crc);
		fr_put32(raw + 4, w->n);
		fr_put32(raw + 8, FR_TAG);
		ok = w_program(w, FR_RECORD_OFF(w->target), raw, 12u);
		break;
	case FR_OP_REC_CHECK:
		ok = rec_check(w);
		fail = FR_WR_READBACK;
		break;
	case FR_OP_SEL_ERASE:
		ok = w_erase(w, FR_SELECTOR_OFF);
		fail = FR_WR_SELECTOR;
		break;
	case FR_OP_SEL_PUT:
		ok = selector_put(w, w->target, w->sel_verified);
		fail = FR_WR_SELECTOR;
		break;
	case FR_OP_SETTLE:
		ok = settle(w, w->target);
		fail = FR_WR_SELECTOR;
		break;
	/* S-02.048: a trial's selector, a byte at a time, each step read
	 * back; a step that fails settles it back on the running slot */
	case FR_OP_TRIAL_ERASE:
		if (!w_erase(w, FR_SELECTOR_OFF) || !selector_reads(w, 0xFFu, 0xFFu)) {
			(void)trial_back(w);
			return;
		}
		break;
	case FR_OP_TRIAL_VERIFIED:
		if (!w_program(w, FR_SELECTOR_OFF + 1u, &fe, 1u) ||
		    !selector_reads(w, 0xFFu, 0xFEu)) {
			(void)trial_back(w);
			return;
		}
		break;
	case FR_OP_TRIAL_INDEX:
		if (!w_program(w, FR_SELECTOR_OFF, &zero, 1u) ||
		    !selector_reads(w, 0x00u, 0xFEu)) {
			(void)trial_back(w);
			return;
		}
		break;
	default:
		ok = false;
		break;
	}
	if (!ok) {
		(void)stop(w, fail);
		return;
	}
	if (++w->ip >= w->n_ops)
		(void)stop(w, FR_WR_OK);
}

void fr_writer_cancel(struct fr_writer *w)
{
	w->way = FR_WAY_NONE;
	w->target = FR_NO_SLOT;
	(void)stop(w, FR_WR_CHANGED);
}

bool fr_writer_step(struct fr_writer *w)
{
	uint8_t raw[12];

	switch (w->state) {
	case FR_WS_SELECT_ERASE:
		/* S-02.084: before a write of the running slot the selector
		 * names the other slot, confirmed good at the write's start
		 * (S-02.050), so that a cut from here on starts it. */
		w->home->moved = true;
		if (w->rescue) {
			if (!settle(w, 1u - w->target))
				return stop(w, FR_WR_SELECT_FIRST);
			w->state = FR_WS_SLOT;
			break;
		}
		if (!w_erase(w, FR_SELECTOR_OFF))
			return stop(w, FR_WR_SELECT_FIRST);
		w->state = FR_WS_SELECT_PROGRAM;
		break;
	case FR_WS_SELECT_PROGRAM:
		if (!selector_put(w, (uint8_t)(1u - w->target), 0x01u))
			return stop(w, FR_WR_SELECT_FIRST);
		w->state = FR_WS_SLOT;
		break;
	case FR_WS_SLOT:
		slot_step(w);
		break;
	case FR_WS_LOG_ERASE:
		/* S-02.045: the slot's last sector, which the image leaves
		 * free (fr_image_valid()), so that the image starts with an
		 * empty start log (S-02.095). */
		if (!w_erase(w, FR_LOG_OFF(w->target)))
			return stop(w, FR_WR_FLASH);
		w->state = w->n <= FR_LOGO_SLOT_OFF ? FR_WS_LOGO_ERASE : FR_WS_SETTINGS_ERASE;
		break;
	case FR_WS_LOGO_ERASE:
		/* The stored logo's header sector, where the image leaves it
		 * free, so that the new image starts with the front's own logo
		 * (display/logo.h); class 1, before the record. SPEC-Q: this erase
		 * has no label yet. */
		if (!w_erase(w, FR_LOGO_OFF(w->target)))
			return stop(w, FR_WR_FLASH);
		w->state = FR_WS_SETTINGS_ERASE;
		break;
	case FR_WS_SETTINGS_ERASE:
		/* S-02.103: the slot's settings sector, so that the new image
		 * starts with its defaults; class 1, before the record. */
		if (!w_erase(w, FR_SETTINGS_OFF(w->target)))
			return stop(w, FR_WR_FLASH);
		w->state = w->way == FR_WAY_TRIAL ? FR_WS_LOG_PENDING : FR_WS_RECORD_ERASE;
		break;
	case FR_WS_LOG_PENDING:
		/* S-02.095: a trial's entry 0 pending, one bit, before the
		 * record; the slot's own front marks it at its trial start */
		if (!log0_put(w, FR_LOG0_PENDING))
			return stop(w, FR_WR_FLASH);
		w->state = FR_WS_RECORD_ERASE;
		break;
	case FR_WS_SELECT_LOG: {
		/* S-02.093: a select from the tried slot's own front marks its
		 * trial start where byte 0 reads exactly pending; else writes
		 * none, so that a spent trial is never made finished */
		uint8_t b;

		if (flash_read(w->f, FR_LOG_OFF(w->target), &b, 1u) != FR_FLASH_OK)
			return stop(w, FR_WR_FLASH);
		if (b == FR_LOG0_PENDING && !log0_put(w, FR_LOG0_MARKED))
			return stop(w, FR_WR_FLASH);
		w->state = FR_WS_RECORD_ERASE;
		break;
	}
	case FR_WS_RECORD_ERASE:	/* class 2 (S-02.046) */
		if (!w_erase(w, FR_RECORD_OFF(w->target)))
			return stop(w, FR_WR_FLASH);
		w->state = FR_WS_RECORD_PROGRAM;
		break;
	case FR_WS_RECORD_PROGRAM:	/* class 3 */
		fr_put32(raw, w->rec_crc);
		fr_put32(raw + 4, w->n);
		fr_put32(raw + 8, FR_TAG);
		if (!w_program(w, FR_RECORD_OFF(w->target), raw, 12u))
			return stop(w, FR_WR_FLASH);
		w->pos = 0;
		w->state = FR_WS_READBACK;
		break;
	case FR_WS_READBACK:
		readback_step(w);
		break;
	case FR_WS_SELECTOR_ERASE:	/* class 5 (S-02.048) */
		/* the read-back's SHA-256 is the one BEGIN announced (S-02.093) */
		if (!fr_same(w->readback_sha, w->want_sha, FR_SHA256_LEN))
			return stop(w, FR_WR_READBACK);
		if (w->rescue) {
			/* S-02.084: the target is read back good, and the
			 * selector is settled on it: {target, 0x01}, for a
			 * slot just read good (S-02.050), so that no first
			 * stage writes the selector after it. */
			return stop(w, settle(w, w->target) ? FR_WR_OK : FR_WR_SELECTOR);
		} else if (w->way == FR_WAY_IN_PLACE) {
			/* S-02.084: the selector reads {target, 0x01}, naming
			 * the slot just read back, and stays as it is: torn,
			 * it could name the other slot, which is bad. */
			return stop(w, FR_WR_OK);
		} else if (w->way == FR_WAY_TRIAL) {
			/* S-02.093: the selector that starts the tried slot
			 * once, a byte at a time, each step read back. Erased,
			 * {FF, FF} names slot 1 unchecked, the end for slot 1;
			 * from the running slot 1's index, an erase cut short
			 * still names slot 1. */
			if (!w_erase(w, FR_SELECTOR_OFF) || !selector_reads(w, 0xFFu, 0xFFu))
				return trial_back(w);
			if (w->target == 1u)
				return stop(w, FR_WR_OK);
			w->state = FR_WS_TRIAL_VERIFIED;
			break;
		}
		if (!w_erase(w, FR_SELECTOR_OFF))
			return stop(w, FR_WR_SELECTOR);
		w->state = FR_WS_SELECTOR_PROGRAM;
		break;
	case FR_WS_SELECTOR_PROGRAM:	/* class 6: {target, 0x00} (S-02.048) */
		if (!selector_put(w, w->target, 0x00u))
			return stop(w, FR_WR_SELECTOR);
		return stop(w, FR_WR_OK);
	case FR_WS_TRIAL_VERIFIED: {
		/* slot 0: verified to 0xFE, one bit and never 0x01, while the
		 * index still names slot 1 */
		static const uint8_t fe = 0xFEu;

		if (!w_program(w, FR_SELECTOR_OFF + 1u, &fe, 1u) || !selector_reads(w, 0xFFu, 0xFEu))
			return trial_back(w);
		w->state = FR_WS_TRIAL_INDEX;
		break;
	}
	case FR_WS_TRIAL_INDEX: {
		/* then the index to 0x00: torn, it names slot 1 or slot 0,
		 * under verified 0xFE */
		static const uint8_t zero = 0x00u;

		if (!w_program(w, FR_SELECTOR_OFF, &zero, 1u) || !selector_reads(w, 0x00u, 0xFEu))
			return trial_back(w);
		return stop(w, FR_WR_OK);
	}
	case FR_WS_ACT:
		act_step(w);
		break;
	case FR_WS_IDLE:
	case FR_WS_DONE:
	default:
		return true;
	}
	return w->state == FR_WS_DONE;
}

unsigned fr_writer_progress(const struct fr_writer *w)
{
	uint64_t done;

	switch (w->state) {
	case FR_WS_SELECT_ERASE:
	case FR_WS_SELECT_PROGRAM:
		done = 0;
		break;
	case FR_WS_SLOT:
		done = w->pos;
		break;
	case FR_WS_LOG_ERASE:
	case FR_WS_LOGO_ERASE:
	case FR_WS_SETTINGS_ERASE:
	case FR_WS_LOG_PENDING:
	case FR_WS_SELECT_LOG:
	case FR_WS_RECORD_ERASE:
	case FR_WS_RECORD_PROGRAM:
		done = w->n;
		break;
	case FR_WS_READBACK:
		done = (uint64_t)w->n + w->pos;
		break;
	case FR_WS_SELECTOR_ERASE:
	case FR_WS_SELECTOR_PROGRAM:
	case FR_WS_TRIAL_VERIFIED:
	case FR_WS_TRIAL_INDEX:
		done = 2u * (uint64_t)w->n;
		break;
	case FR_WS_ACT:
		/* by operations; the CRC pass by its bytes */
		done = (uint64_t)w->ip * w->n;
		if (w->ops[w->ip] == FR_OP_CRC)
			done += w->pos;
		return w->n == 0u || w->n_ops == 0u ? 0u :
		       (unsigned)(done * 999u / ((uint64_t)w->n_ops * w->n));
	case FR_WS_DONE:
		return w->result == FR_WR_OK ? 1000u : 0u;
	default:
		return 0;
	}
	return w->n == 0u ? 0u : (unsigned)(done * 999u / (2u * (uint64_t)w->n));
}
