/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-02 formats, the slot check, the conforming first stage and the selector
 * writes of S-02.050.
 */
#include "waffle/s02_flash.h"

#include "waffle/bytes.h"

/* S-02.011 */
unsigned s02_selected_slot(uint8_t index)
{
	return index == 0x00u ? 0u : 1u;
}

/* S-02.043, S-16.030: the same rule, inverted. */
unsigned s02_other_slot(uint8_t index)
{
	return index == 0x00u ? 1u : 0u;
}

/* S-02.012 */
bool s02_verified(uint8_t verified)
{
	return verified == 0x01u;
}

uint32_t s02_slot_base(unsigned slot)
{
	return slot == 0u ? S02_SLOT0_OFF : S02_SLOT1_OFF;
}

uint32_t s02_record_off(unsigned slot)
{
	return slot == 0u ? S02_RECORD0_OFF : S02_RECORD1_OFF;
}

/* S-02.013 */
void s02_record_decode(const uint8_t raw[S02_RECORD_SIZE], struct s02_record *rec)
{
	rec->crc32 = wb_get_le32(raw);
	rec->length = wb_get_le32(raw + 4);
	rec->tag = wb_get_le32(raw + 8);
}

void s02_record_encode(const struct s02_record *rec, uint8_t raw[S02_RECORD_SIZE])
{
	wb_put_le32(raw, rec->crc32);
	wb_put_le32(raw + 4, rec->length);
	wb_put_le32(raw + 8, rec->tag);
}

/* S-02.014 */
bool s02_tag_ok(uint32_t tag)
{
	return tag == S02_RECORD_TAG;
}

/* S-02.016 items 1 and 2; S-02.031 rejects these without a read. */
bool s02_record_plausible(const struct s02_record *rec)
{
	return s02_tag_ok(rec->tag) && rec->length >= 1u &&
	       rec->length <= S02_SLOT_CAPACITY;
}

int s02_read_selector(const struct s02_env *env, struct s02_selector *sel)
{
	uint8_t raw[2];

	if (env->flash->read(env->flash->ctx, S02_SELECTOR_OFF, raw, 2u) != 0)
		return S02_ERR_IO;
	sel->index = raw[0];
	sel->verified = raw[1];
	return S02_OK;
}

int s02_read_record(const struct s02_env *env, unsigned slot, struct s02_record *rec)
{
	uint8_t raw[S02_RECORD_SIZE];

	if (env->flash->read(env->flash->ctx, s02_record_off(slot), raw,
			     S02_RECORD_SIZE) != 0)
		return S02_ERR_IO;
	s02_record_decode(raw, rec);
	return S02_OK;
}

int s02_flash_crc(const struct s02_env *env, uint32_t off, uint32_t len, uint32_t *crc)
{
	uint32_t c = 0;

	while (len > 0u) {
		uint32_t chunk = len < env->scratch_len ? len : env->scratch_len;

		if (env->flash->read(env->flash->ctx, off, env->scratch, chunk) != 0)
			return S02_ERR_IO;
		c = env->crc32(c, env->scratch, chunk);
		off += chunk;
		len -= chunk;
	}
	*crc = c;
	return S02_OK;
}

/* S-02.016, judged as S-02.031 does. */
int s02_check_slot(const struct s02_env *env, unsigned slot, bool *good)
{
	struct s02_record rec;
	uint32_t crc;
	int rc;

	*good = false;
	rc = s02_read_record(env, slot, &rec);
	if (rc != S02_OK)
		return rc;
	if (!s02_record_plausible(&rec))
		return S02_OK;
	rc = s02_flash_crc(env, s02_slot_base(slot), rec.length, &crc);
	if (rc != S02_OK)
		return rc;
	*good = crc == rec.crc32;
	return S02_OK;
}

/* Bytes compared and bytes equal, and whether the stream reached its
 * FINAL block. */
struct score {
	uint32_t compared, equal;
	bool complete;
};

static bool score_full(const struct score *s)
{
	return s->compared != 0u && s->equal == s->compared;
}

/*
 * How a slot's stream is walked. s02_running_slot() reads every byte from
 * flash, once, and counts to the stream's end. The walks of
 * s02_running_image() read flash only where memory can be compared, make a
 * failed read once more, take the stream from memory when it is there, and
 * stop at the first chunk that differs; with `only`, they compare just the
 * chunks that hold its bytes. Every walk cuts a block into the same chunks,
 * so all of them compare the same bytes. Every read of a walk is the
 * flash's usual one: nothing checks a walk's bytes, so none comes from an
 * image's faster read, which only a CRC makes safe.
 */
struct walk {
	const struct s02_env *env;
	const struct s02_mem_ops *mem;
	uint32_t base;			/* the slot's flash offset */
	const uint8_t *buf;		/* its stream in memory, or NULL */
	bool lazy;			/* memory first, one retry, stop on a difference */
	const struct s02_probe *only;
};

static int read_plain(const struct s02_env *env, uint32_t off, uint8_t *buf, uint32_t len,
		      bool retry)
{
	if (env->flash->read(env->flash->ctx, off, buf, len) == 0)
		return S02_OK;
	if (retry && env->flash->read(env->flash->ctx, off, buf, len) == 0)
		return S02_OK;
	return S02_ERR_IO;
}

static void count_equal(const uint8_t *a, const uint8_t *b, uint32_t k, struct score *score)
{
	for (uint32_t i = 0; i < k; i++)
		score->equal += a[i] == b[i] ? 1u : 0u;
	score->compared += k;
}

/* Compare count payload bytes at stream offset src with memory at addr,
 * where mem reads it: count them into *score. */
static int compare_payload(const struct walk *w, uint32_t src, uint32_t addr, uint32_t count,
			   struct score *score)
{
	const struct s02_env *env = w->env;
	uint32_t half = env->scratch_len / 2u, done = 0;
	uint8_t *fb = env->scratch, *mb = env->scratch + half;

	if ((uint64_t)addr + count > 0x100000000ull)
		return S02_OK;			/* wraps: not a load we can compare */
	for (; done < count && (!w->lazy || score->equal == score->compared); done += half) {
		uint32_t k = count - done < half ? count - done : half;
		const uint8_t *sb = fb;

		if (!w->lazy) {
			if (read_plain(env, w->base + src + done, fb, k, false) != S02_OK)
				return S02_ERR_IO;
			if (w->mem->read(w->mem->ctx, addr + done, mb, k))
				count_equal(fb, mb, k, score);
			continue;
		}
		if (w->only != NULL &&
		    ((uint64_t)addr + done + k <= w->only->addr ||
		     (uint64_t)addr + done >= (uint64_t)w->only->addr + w->only->len))
			continue;
		if (!w->mem->read(w->mem->ctx, addr + done, mb, k))
			continue;
		if (w->buf != NULL)
			sb = w->buf + src + done;
		else if (read_plain(env, w->base + src + done, fb, k, true) != S02_OK)
			return S02_ERR_IO;
		count_equal(sb, mb, k, score);
	}
	return S02_OK;
}

/* S-05's LDR block header (S-05 Terms), read here so that S-02 stands alone. */
#define LDR_HEADER 16u
#define LDR_SIGNATURE 0xADu
#define LDR_FINAL 0x8000u
#define LDR_IGNORE 0x1000u
#define LDR_FILL 0x0100u

/* Signature 0xAD and a zero XOR over the 16 bytes (s05_ldr_header_ok). */
static bool ldr_header_ok(const uint8_t h[LDR_HEADER])
{
	uint8_t x = 0;

	for (unsigned i = 0; i < LDR_HEADER; i++)
		x = (uint8_t)(x ^ h[i]);
	return h[3] == LDR_SIGNATURE && x == 0u;
}

/* A slot's stream of len bytes against the memory it loads into. */
static int walk_stream(const struct walk *w, uint32_t len, struct score *score)
{
	uint8_t hb[LDR_HEADER];
	uint32_t off = 0;
	int rc;

	score->compared = score->equal = 0;
	score->complete = false;
	while ((uint64_t)off + LDR_HEADER <= len) {
		const uint8_t *h = hb;
		uint32_t flags, target, count;

		if (w->buf != NULL)
			h = w->buf + off;
		else if (read_plain(w->env, w->base + off, hb, LDR_HEADER, w->lazy) != S02_OK)
			return S02_ERR_IO;
		if (!ldr_header_ok(h))
			break;			/* the stream ends here */
		flags = wb_get_le32(h) & 0xFFF0u;
		target = wb_get_le32(h + 4);
		count = wb_get_le32(h + 8);
		off += LDR_HEADER;
		if (!(flags & LDR_FILL)) {
			if ((uint64_t)off + count > len)
				break;
			/* a FILL block's memory is the program's own by now */
			if (count != 0u && !(flags & LDR_IGNORE)) {
				rc = compare_payload(w, off, target, count, score);
				if (rc != S02_OK)
					return rc;
				if (w->lazy && score->equal != score->compared)
					break;	/* it differs: not this slot */
			}
			off += count;
		}
		if (flags & LDR_FINAL) {
			score->complete = true;
			break;
		}
	}
	return S02_OK;
}

/* S-02.054's rule over the two scores: the one slot whose stream matches
 * every byte compared; both whole, the one that reached its FINAL block. */
static unsigned running_of(const struct score score[2], unsigned none)
{
	bool full[2] = { score_full(&score[0]), score_full(&score[1]) };

	if (full[0] && full[1] && score[0].complete != score[1].complete)
		full[score[0].complete ? 1u : 0u] = false;
	if (full[0] != full[1])
		return full[0] ? 0u : 1u;
	return none;
}

/*
 * The slot later starts settle on, as the selector and the records read
 * now (S-02.020 to S-02.023): the slot the selector names when it is
 * verified, where the first stage starts it with no check, or when that
 * slot is good, where its check passes; otherwise the other slot, where a
 * failed check leaves the selector.
 */
static int settled_slot(const struct s02_env *env, unsigned *slot)
{
	struct s02_selector sel;
	bool good = true;
	int rc = s02_read_selector(env, &sel);

	if (rc == S02_OK && !s02_verified(sel.verified))
		rc = s02_check_slot(env, s02_selected_slot(sel.index), &good);
	if (rc != S02_OK)
		return rc;
	*slot = good ? s02_selected_slot(sel.index) : s02_other_slot(sel.index);
	return S02_OK;
}

int s02_running_slot(const struct s02_env *env, const struct s02_mem_ops *mem,
		     unsigned fallback, unsigned *slot)
{
	struct score score[2];

	*slot = fallback;
	for (unsigned k = 0; k < 2u; k++) {
		const struct walk w = { env, mem, s02_slot_base(k), NULL, false, NULL };
		struct s02_record rec;
		uint32_t len = S02_SLOT_CAPACITY;
		int rc = s02_read_record(env, k, &rec);

		if (rc != S02_OK)
			return rc;
		if (s02_record_plausible(&rec))
			len = rec.length;
		rc = walk_stream(&w, len, &score[k]);
		if (rc != S02_OK)
			return rc;
	}
	if (score_full(&score[0]) && score_full(&score[1]) && score[0].complete &&
	    score[1].complete)
		return settled_slot(env, slot);
	*slot = running_of(score, fallback);
	return S02_OK;
}

/* --- The running slot with its stream read once --- */

void s02_image_init(struct s02_image *im, uint8_t *buf, uint32_t cap, s02_read_fn fast,
		    void *fast_ctx)
{
	im->buf = buf;
	im->cap = cap;
	im->fast = fast;
	im->fast_ctx = fast_ctx;
	im->slot = 2u;
	im->len = 0;
	im->good = false;
	im->trial = false;
	im->by_fast = false;
	im->known = 0;
	im->probed = 0;
	im->ruled_out = 0;
}

/* A slot's record, read once for an image. */
static int image_record(const struct s02_env *env, unsigned slot, struct s02_image *im,
			struct s02_record *rec)
{
	if (!(im->known >> slot & 1u)) {
		uint8_t raw[S02_RECORD_SIZE];

		if (read_plain(env, s02_record_off(slot), raw, S02_RECORD_SIZE, true) != S02_OK)
			return S02_ERR_IO;
		s02_record_decode(raw, &im->recs[slot]);
		im->known |= 1u << slot;
	}
	*rec = im->recs[slot];
	return S02_OK;
}

int s02_image_read(const struct s02_env *env, unsigned slot, struct s02_image *im)
{
	struct s02_record rec;

	im->slot = 2u;
	im->len = 0;
	im->good = false;
	im->trial = false;
	im->by_fast = false;
	if (slot > 1u || image_record(env, slot, im, &rec) != S02_OK)
		return S02_ERR_IO;
	im->rec = rec;
	/* S-02.031: a record that fails the tag or the length is bad unread */
	if (s02_record_plausible(&rec) && rec.length <= im->cap) {
		if (im->fast != NULL &&
		    im->fast(im->fast_ctx, s02_slot_base(slot), im->buf, rec.length) == 0)
			im->by_fast = true;
		else if (read_plain(env, s02_slot_base(slot), im->buf, rec.length,
				    im->fast == NULL) != S02_OK)
			return S02_ERR_IO;
		im->len = rec.length;
	}
	im->slot = slot;
	return S02_OK;
}

/* The image's CRC: crc32_fast's when it matches the record or its
 * complement, else crc32's. */
static uint32_t image_crc(const struct s02_env *env, const struct s02_image *im)
{
	if (env->crc32_fast != NULL) {
		const uint32_t crc = env->crc32_fast(0, im->buf, im->len);

		if (crc == im->rec.crc32 || crc == ~im->rec.crc32)
			return crc;
	}
	return env->crc32(0, im->buf, im->len);
}

int s02_image_check(const struct s02_env *env, struct s02_image *im)
{
	uint32_t crc;

	im->good = false;
	im->trial = false;
	if (im->slot > 1u || im->len == 0u)
		return S02_OK;
	crc = image_crc(env, im);
	if (crc != im->rec.crc32 && crc != ~im->rec.crc32 && im->by_fast) {
		/* judged by a read at the usual rate, as the first stage's */
		im->by_fast = false;
		if (read_plain(env, s02_slot_base(im->slot), im->buf, im->len, false) != S02_OK) {
			im->slot = 2u;
			im->len = 0;
			return S02_ERR_IO;
		}
		crc = image_crc(env, im);
	}
	im->good = crc == im->rec.crc32;
	/* S-02.016's trial mark: the complement, which no chance read gives */
	im->trial = crc == ~im->rec.crc32;
	return S02_OK;
}

/* Slot k against memory, lazily: from the image when it holds the slot's
 * stream as the flash's usual read gave it, or with its CRC found good or
 * the trial mark's. */
static int image_score(const struct s02_env *env, const struct s02_mem_ops *mem,
		       const struct s02_probe *only, struct s02_image *im, unsigned k,
		       struct score *score)
{
	struct walk w = { env, mem, s02_slot_base(k), NULL, true, only };
	struct s02_record rec;
	uint32_t len = S02_SLOT_CAPACITY;
	int rc = image_record(env, k, im, &rec);

	if (rc != S02_OK)
		return rc;
	if (s02_record_plausible(&rec))
		len = rec.length;
	if (im->slot == k && im->len == len && (im->good || im->trial || !im->by_fast))
		w.buf = im->buf;
	return walk_stream(&w, len, score);
}

static bool probed(const struct s02_probe *probe)
{
	return probe != NULL && probe->len != 0u;
}

/* Whether the probe's chunks rule slot k out, found once for an image. A
 * slot that cannot be read, or has none of those bytes, is not ruled out. */
static bool ruled_out(const struct s02_env *env, const struct s02_mem_ops *mem,
		      const struct s02_probe *probe, struct s02_image *im, unsigned k)
{
	const unsigned bit = 1u << k;
	struct score score;

	if (!probed(probe))
		return false;
	if (!(im->probed & bit)) {
		im->probed |= bit;
		if (image_score(env, mem, probe, im, k, &score) == S02_OK &&
		    score.equal != score.compared)
			im->ruled_out |= bit;
	}
	return (im->ruled_out & bit) != 0u;
}

unsigned s02_running_first(const struct s02_env *env, const struct s02_mem_ops *mem,
			   const struct s02_probe *probe, unsigned selected,
			   struct s02_image *im)
{
	const bool out0 = ruled_out(env, mem, probe, im, 0u);
	const bool out1 = ruled_out(env, mem, probe, im, 1u);

	if (out0 != out1)
		return out0 ? 1u : 0u;
	return selected;
}

/*
 * S-02.016 on slot k as s02_check_slot() gives it, from the image where it
 * can tell: a CRC found good, or the trial mark's, is the verdict on the
 * bytes it holds; bytes the usual read gave are judged here, with no read.
 * Otherwise the slot is read, a failed check once more.
 */
static int image_good(const struct s02_env *env, struct s02_image *im, unsigned k, bool *good)
{
	struct s02_record rec;
	int rc = image_record(env, k, im, &rec);

	*good = false;
	if (rc != S02_OK || !s02_record_plausible(&rec))
		return rc;
	if (im->slot == k && im->len == rec.length) {
		if (im->good || im->trial) {
			*good = im->good;
			return S02_OK;
		}
		if (!im->by_fast) {
			*good = env->crc32(0, im->buf, im->len) == rec.crc32;
			return S02_OK;
		}
	}
	rc = s02_check_slot(env, k, good);
	if (rc != S02_OK)
		rc = s02_check_slot(env, k, good);
	return rc;
}

/* settled_slot() for an image: the selector, read now, and the selected
 * slot's check only when the selector is not verified. */
static int image_settled(const struct s02_env *env, struct s02_image *im, unsigned *slot)
{
	uint8_t raw[2];
	bool good = true;
	unsigned sel;
	int rc = read_plain(env, S02_SELECTOR_OFF, raw, 2u, true);

	if (rc != S02_OK)
		return rc;
	sel = s02_selected_slot(raw[0]);
	if (!s02_verified(raw[1])) {
		rc = image_good(env, im, sel, &good);
		if (rc != S02_OK)
			return rc;
	}
	*slot = good ? sel : 1u - sel;
	return S02_OK;
}

int s02_running_image(const struct s02_env *env, const struct s02_mem_ops *mem,
		      const struct s02_probe *probe, struct s02_image *im, unsigned first,
		      unsigned *slot)
{
	struct score score[2];

	*slot = 2u;
	if (first > 1u)
		first = 0u;
	for (unsigned i = 0; i < 2u; i++) {
		const unsigned k = i == 0u ? first : 1u - first;
		int rc;

		/* the bytes most likely to differ first: another build is ruled
		 * out by them, and only a slot they match is compared whole */
		if (!(im->slot == k && im->len != 0u) && ruled_out(env, mem, probe, im, k)) {
			score[k].compared = 1u;
			score[k].equal = 0;
			score[k].complete = false;
			continue;
		}
		rc = image_score(env, mem, NULL, im, k, &score[k]);
		if (rc != S02_OK)
			return rc;
	}
	if (score_full(&score[0]) && score_full(&score[1]) && score[0].complete &&
	    score[1].complete)
		return image_settled(env, im, slot);
	*slot = running_of(score, 2u);
	return S02_OK;
}

/* One erase of the selector sector, then one 2-byte program (S-02.035). */
static int selector_store(const struct s02_env *env, struct s02_selector sel)
{
	uint8_t raw[2];

	raw[0] = sel.index;
	raw[1] = sel.verified;
	if (env->flash->erase(env->flash->ctx, S02_SELECTOR_OFF) != 0)
		return S02_ERR_IO;
	if (env->flash->program(env->flash->ctx, S02_SELECTOR_OFF, raw, 2u) != 0)
		return S02_ERR_IO;
	return S02_OK;
}

/*
 * S-02.030 to S-02.036.  On a failed read (S-02.036) a slot is still booted:
 * an unreadable selector is judged as the erased FF FF, a slot whose record
 * or contents cannot be read is bad, and the reset makes no selector write.
 */
void s02_first_stage(const struct s02_env *env, bool no_writes, struct s02_fs_outcome *out)
{
	struct s02_selector sel;
	unsigned selected, other;
	bool good = false, other_good = false;
	int rc;

	out->wrote = false;
	out->written.index = 0;
	out->written.verified = 0;
	out->checked = 0;
	out->selected_good = false;
	out->status = S02_OK;

	rc = s02_read_selector(env, &sel);
	if (rc != S02_OK) {
		/* No selector to go by: the erased value selects slot 1. */
		out->status = rc;
		sel.index = S02_ERASED;
		sel.verified = S02_ERASED;
	}
	selected = s02_selected_slot(sel.index);
	other = 1u - selected;
	out->boot_slot = selected;

	/* S-02.030: check the selected slot on every reset. */
	out->checked |= 1u << selected;
	rc = s02_check_slot(env, selected, &good);
	if (rc != S02_OK && out->status == S02_OK)
		out->status = rc;
	out->selected_good = good;

	if (good) {
		/* S-02.032 */
		if (!s02_verified(sel.verified)) {
			out->written.index = sel.index;
			out->written.verified = 0x01u;
			out->wrote = true;
		}
	} else {
		/* S-02.033: the other slot, in the same reset. */
		out->checked |= 1u << other;
		rc = s02_check_slot(env, other, &other_good);
		if (rc != S02_OK && out->status == S02_OK)
			out->status = rc;
		if (other_good) {
			out->boot_slot = other;
			out->written.index = (uint8_t)other;
			out->written.verified = 0x01u;
			out->wrote = true;
		} else if (!s02_verified(sel.verified)) {
			/* S-02.034: no flip; {index, 0x01} only when not yet 1. */
			out->written.index = sel.index;
			out->written.verified = 0x01u;
			out->wrote = true;
		}
	}

	if (no_writes || out->status != S02_OK)	/* S-02.082, S-02.036 */
		out->wrote = false;
	if (out->wrote) {
		/* S-02.035: complete the program before the ROM boot call. */
		rc = selector_store(env, out->written);
		if (rc != S02_OK && out->status == S02_OK)
			out->status = rc;
	}
}

/* S-02.050, S-02.053 (the caller's mechanism bounds how often). */
int s02_write_selector(const struct s02_env *env, unsigned slot, uint8_t verified)
{
	struct s02_selector sel;
	bool good;
	int rc;

	if (slot > 1u)
		return S02_ERR_NOT_GOOD;
	rc = s02_check_slot(env, slot, &good);
	if (rc != S02_OK)
		return rc;
	if (!good)
		return S02_ERR_NOT_GOOD;
	sel.index = (uint8_t)slot;
	sel.verified = verified;
	return selector_store(env, sel);
}
