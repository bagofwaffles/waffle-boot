/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The installer's pure parts (waffle/board/installer.h): the lock, the
 * hold, the carried image's check, the USB protocol and the screens. In the
 * installer variant alone; the host tests include this file.
 */
#include "waffle/board/installer.h"

#if defined(OPXY_TESTOS_INSTALLER)

#include <string.h>

#include "front/fr_handoff.h"
#include "waffle/bytes.h"
#include "waffle/device.h"
#include "waffle/s02_flash.h"
#include "waffle/s05_handoff.h"
#include "waffle/s15_keytest.h"
#include "waffle/s15_lines.h"

/* --- the OS partition, the trial --- */

enum inst_os inst_os_state(const struct tchk_switch *sw)
{
	switch (sw->result) {
	case TCHK_SW_READY:
		return sw->unverified ? INST_OS_ENCRYPTED : INST_OS_CHECKED;
	case TCHK_SW_RESTORED:
		return INST_OS_ACTIVE;
	case TCHK_SW_WRITE_FAILED:
		return INST_OS_WRITE_FAILED;
	case TCHK_SW_NO_OTHER_OS:
		return INST_OS_NONE;
	case TCHK_SW_OTHER_BAD:
		return INST_OS_BAD;
	case TCHK_SW_IO:
	case TCHK_SW_NO_SLAVE:
	case TCHK_SW_NOT_ACTIVE:
		return INST_OS_UNREAD;
	default:
		return INST_OS_NOT_LOOKED;
	}
}

enum inst_trial inst_trial_of(uint8_t st)
{
	if ((st & OPXY_INSTALL_SLOT_TRIAL) == 0u)
		return INST_TRIAL_NONE;
	if ((st & OPXY_INSTALL_SLOT_PENDING) != 0u)
		return INST_TRIAL_WAITING;
	if ((st & OPXY_INSTALL_SLOT_REACHED) != 0u)
		return INST_TRIAL_REACHED;
	if ((st & OPXY_INSTALL_SLOT_STARTED) != 0u)
		return INST_TRIAL_NOT_REACHED;
	if ((st & (OPXY_INSTALL_SLOT_SPENT | OPXY_INSTALL_SLOT_ODD)) != 0u)
		return INST_TRIAL_SPENT;
	return INST_TRIAL_NONE;
}

/* --- the lock --- */

enum inst_lock inst_lock_of(const struct tchk_table *t, const struct tchk_switch *sw,
			    const struct inst_lock_in *in)
{
	bool failed = false, untested = false;
	unsigned i;

	/* the device first: nothing else is judged on an unknown one; an
	 * installer installs only on a table match (S-02.108, S-18.004) */
	if (in->how != (uint8_t)WB_DEV_BY_TABLE)
		return INST_LOCK_DEVICE;
	if (!in->device_mine)
		return INST_LOCK_OTHER_DEVICE;
	for (i = 0; i < TCHK_COUNT; i++) {
		const uint8_t state = t->r[i].state;

		if (i == TCHK_OS_PARTITION || state == TCHK_PASS || state == TCHK_NOT_ON_DEVICE)
			continue;
		if (state == TCHK_FAIL && tchk_tests[i].kind != TCHK_KIND_GUIDED)
			failed = true;
		else
			untested = true;
	}
	if (sw->result == TCHK_SW_NO_SLAVE)
		return INST_LOCK_EMMC;
	if (failed)
		return INST_LOCK_FAILED;
	/* a guided test not passed counts as one that could not run: the key
	 * check ends only passed, or with 8 held, which offers no install */
	if (untested)
		return INST_LOCK_NOT_TESTED;
	if (!in->image_ok)
		return INST_LOCK_IMAGE;
	if (!in->selected_ok)
		return INST_LOCK_SELECTED;
	if (!in->keeps_factory)
		return INST_LOCK_FACTORY;
	/* last, so that a survey shows whether an install would be offered */
	if (in->survey)
		return INST_LOCK_SURVEY;
	/* S-16.027: after the OS write that started this image the other
	 * partition may hold no OS at all; the install never waits on one */
	return INST_LOCK_NONE;
}

bool inst_keeps_factory(const struct wb_device *dev, unsigned target, uint8_t streams)
{
	const unsigned bits = streams;
	const bool waffle_t = (bits >> (target & 1u) & 1u) != 0u;
	const bool waffle_other = (bits >> ((target & 1u) ^ 1u) & 1u) != 0u;

	if (dev == NULL || dev->id != WB_DEV_OP1F)
		return true;
	return target <= 1u && (waffle_t || !waffle_other);
}

const char *inst_lock_words(enum inst_lock l)
{
	static const char *const w[] = {
		[INST_LOCK_NONE] = "Checks passed",
		[INST_LOCK_FAILED] = "A check failed",
		[INST_LOCK_NOT_TESTED] = "A check could not run",
		[INST_LOCK_IMAGE] = "Carried image fails its check",
		[INST_LOCK_SELECTED] = "Bootloader slot not settled",
		[INST_LOCK_RESERVED] = "",
		[INST_LOCK_RESERVED_6] = "",
		[INST_LOCK_EMMC] = "eMMC not reachable",
		[INST_LOCK_DEVICE] = "Device unknown (first stage)",
		[INST_LOCK_OTHER_DEVICE] = "Built for another device",
		[INST_LOCK_FACTORY] = "Would overwrite the unit's own bootloader",
		[INST_LOCK_SURVEY] = "Survey build: writes nothing",
	};

	return (unsigned)l < sizeof w / sizeof w[0] ? w[l] : "";
}

const char *inst_rc_words(uint8_t rc)
{
	switch (rc) {
	case 0:
		return "Done.";
	case S02_ERR_IO:
		return "Flash read, erase or program failed";
	case S02_ERR_SIZE:
		return "Image too big for the slot";
	case S02_ERR_VERIFY:
		return "Read-back does not match";
	case S02_ERR_START:
		return "Bootloader slot not settled";
	case S02_ERR_NOT_GOOD:
		return "Written slot fails its check";
	case S02_ERR_STREAM:
		return "Not an image the unit can start";
	case OPXY_INSTALL_REFUSED_CRUMBS:
		return "Breadcrumb builds are never kept";
	case OPXY_INSTALL_REFUSED_UNSTARTED:
		return "Trial has not run yet";
	case OPXY_INSTALL_REFUSED_IMAGE:
		return "Carried image fails its check";
	case OPXY_INSTALL_REFUSED_UNREACHED:
		return "Trial did not reach the boot menu";
	case OPXY_INSTALL_REFUSED_HELD:
		return "Restart before another write";
	case OPXY_INSTALL_REFUSED_FACTORY:
		return "Would overwrite the unit's own bootloader";
	case OPXY_INSTALL_REFUSED_SURVEY:
		return "Survey build: writes nothing";
	default:
		return "Cause unknown";
	}
}

/* --- the hold --- */

static bool bit(const uint8_t *m, unsigned k)
{
	return ((unsigned)m[k >> 3] >> (k & 7u) & 1u) != 0u;
}

static void set_bit(uint8_t *m, unsigned k, bool v)
{
	if (v)
		m[k >> 3] = (uint8_t)(m[k >> 3] | 1u << (k & 7u));
	else
		m[k >> 3] = (uint8_t)(m[k >> 3] & ~(1u << (k & 7u)));
}

static const struct wb_device *hold_dev(const struct inst_hold *h)
{
	return h->dev != NULL ? h->dev : &wb_device_opxy;
}

/* S-07.032: device.key.precomp is not acted on (none on the OP-1F). */
static bool precomp(const struct inst_hold *h, unsigned key)
{
	return hold_dev(h)->keys.precomp != WB_KEY_NONE && key == hold_dev(h)->keys.precomp;
}

static bool alone(const struct inst_hold *h, unsigned key)
{
	for (unsigned k = 0; k < S15_NKEYS; k++)
		if (k != key && !precomp(h, k) && bit(h->down, k))
			return false;
	return true;
}

void inst_hold_offer(struct inst_hold *h, uint16_t mask)
{
	h->mask = mask;
	h->holding = false;
	h->key = 0;
	h->digit = 0;
	/* keys down now act only after an up */
	memcpy(h->stale, h->down, sizeof h->stale);
}

void inst_hold_key(struct inst_hold *h, unsigned key, unsigned digit, bool down, uint32_t now_ms)
{
	if (key >= S15_NKEYS)
		return;
	set_bit(h->down, key, down);
	if (bit(h->stale, key)) {
		if (!down)
			set_bit(h->stale, key, false);
		return;
	}
	if (!down) {
		if (h->holding && key == h->key)
			h->holding = false;	/* an early release */
		return;
	}
	if (h->holding) {
		if (!precomp(h, key))
			h->holding = false;	/* another key down */
		return;
	}
	/* a knob click starts no hold */
	if (digit != 0u && digit < 16u && ((unsigned)h->mask >> digit & 1u) != 0u &&
	    !s15_knob_click(hold_dev(h), key) && alone(h, key)) {
		h->holding = true;
		h->key = key;
		h->digit = digit;
		h->t0 = now_ms;
	}
}

unsigned inst_hold_tick(struct inst_hold *h, uint32_t now_ms)
{
	if (!h->holding || now_ms - h->t0 < S15_HOLD_MS)
		return 0;
	h->holding = false;
	/* its release does nothing more */
	set_bit(h->stale, h->key, true);
	h->mask = 0;
	return h->digit;
}

unsigned inst_hold_fill(const struct inst_hold *h, uint32_t now_ms)
{
	const uint32_t held = now_ms - h->t0;

	if (!h->holding)
		return 0;
	return held >= S15_HOLD_MS ? 1000u : (unsigned)(held * 1000u / S15_HOLD_MS);
}

/* --- what a completed hold starts --- */

uint16_t inst_holds_for(enum inst_phase p, bool os_ok)
{
	const uint16_t back = os_ok ? 1u << INST_DIGIT_LEAVE : 0u;

	if (p == INST_COMMIT_READY)
		return (uint16_t)(1u << INST_DIGIT_WRITE | 1u << INST_DIGIT_RETRIAL | back);
	if (p == INST_READY || p == INST_FAILED || p == INST_TRIAL_FAILED)
		return (uint16_t)(1u << INST_DIGIT_WRITE | back);
	if (p == INST_GUIDED || p == INST_LOCKED || p == INST_DONE || p == INST_SURVEYED)
		return back;
	return 0;
}

enum inst_act inst_act_of(enum inst_phase p, unsigned digit)
{
	if (digit == INST_DIGIT_LEAVE)
		return INST_ACT_LEAVE;
	if (digit == INST_DIGIT_WRITE) {
		if (p == INST_READY || p == INST_TRIAL_FAILED)
			return INST_ACT_TRIAL;
		if (p == INST_COMMIT_READY)
			return INST_ACT_COMMIT;
		if (p == INST_FAILED)
			return INST_ACT_RETRY;
	}
	if (digit == INST_DIGIT_RETRIAL && p == INST_COMMIT_READY)
		return INST_ACT_TRIAL;
	return INST_ACT_NONE;
}

/* --- the carried image --- */

static bool meets(uint32_t a, uint32_t n, uint32_t lo, uint32_t hi)
{
	return n != 0u && a < hi && lo < (uint64_t)a + n;
}

/* The front's windows (src/front/front.ld; tools/mkldr.py FR_WINDOWS). */
static const uint32_t front_windows[3][2] = {
	{ FR_L1_CODE_START, FR_L1_CODE_END },
	{ FR_L1_DATA_START, FR_L1_DATA_END },
	{ FR_L2_START, FR_L2_END },
};

/* The front's part of the stream, as tools/wbrecover.py's front_scan()
 * and the front's own scan read it: blocks to FINAL; a block that meets
 * the shared block must be the descriptor; one that meets a front window
 * must be data inside it, and counts toward the front's CRC and pin. */
bool inst_image_check(const uint8_t *img, uint32_t len, uint32_t crc, const uint8_t pin[32],
		      inst_crc_fn crc32, const struct inst_sha_ops *sha, struct inst_image *out)
{
	uint32_t off = 0, fcrc = 0, descs = 0, desc_at = 0, desc_n = 0;
	uint8_t calc[32];
	bool bad = false, final = false;

	memset(out, 0, sizeof(*out));
	out->crc_ok = crc32(0u, img, len) == crc;
	sha->init(sha->ctx);
	while (off < len && !final) {
		struct s05_block b;
		uint32_t w;

		if (s05_ldr_next(img, len, &off, &b) != 0)
			break;
		final = (b.flags & S05_FLAG_FINAL) != 0u;
		if (!s05_block_has_destination(&b))
			continue;
		const bool data = (b.flags & S05_FLAG_FILL) == 0u;
		const uint8_t *payload = img + b.offset + S05_LDR_HEADER;

		if (meets(b.target, b.count, FR_SHARED_START, FR_SHARED_END)) {
			if (data && b.target == FR_DESC_ADDR &&
			    (uint64_t)b.target + b.count <= FR_SHARED_END) {
				descs++;
				desc_at = b.offset + S05_LDR_HEADER;
				desc_n = b.count;
			} else {
				bad = true;
			}
			continue;
		}
		for (w = 0; w < 3u; w++) {
			uint8_t head[8];

			if (!meets(b.target, b.count, front_windows[w][0], front_windows[w][1]))
				continue;
			if (!data || b.target < front_windows[w][0] ||
			    (uint64_t)b.target + b.count > front_windows[w][1]) {
				bad = true;
				break;
			}
			fcrc = crc32(fcrc, payload, b.count);
			wb_put_le32(head, b.target);
			wb_put_le32(head + 4, b.count);
			sha->update(sha->ctx, head, sizeof head);
			sha->update(sha->ctx, payload, b.count);
			break;
		}
	}
	sha->final(sha->ctx, calc);
	out->stream_ok = final;
	if (descs == 1u && desc_n >= FR_DESC_HEAD + 4u) {
		const uint8_t *d = img + desc_at;
		const uint32_t size = wb_get_le16(d + FR_DESC_O_SIZE);
		const uint32_t nreg = wb_get_le32(d + FR_DESC_O_NREGIONS);

		out->desc_ok = wb_get_le32(d + FR_DESC_O_MAGIC) == FR_DESC_MAGIC &&
			       wb_get_le16(d + FR_DESC_O_VERSION) == FR_DESC_VERSION &&
			       nreg <= FR_DESC_REGIONS && size == FR_DESC_HEAD + 8u * nreg + 4u &&
			       size <= desc_n &&
			       crc32(0u, d, size - 4u) == wb_get_le32(d + size - 4u);
		if (out->desc_ok) {
			for (unsigned i = 0; i < 4u; i++)
				out->version[i] = wb_get_le16(d + FR_DESC_O_BL_VERSION + 2u * i);
			memcpy(out->pin, d + FR_DESC_O_FRONT_PIN, 32);
			out->front_crc_ok = !bad && fcrc == wb_get_le32(d + FR_DESC_O_FRONT_CRC);
			out->pin_ok = !bad && memcmp(calc, out->pin, 32) == 0 &&
				      memcmp(calc, pin, 32) == 0;
		}
	}
	return out->crc_ok && out->stream_ok && out->desc_ok && out->front_crc_ok && out->pin_ok;
}

/* --- the protocol --- */

void inst_proto_init(struct inst_proto *p, inst_crc_fn crc32)
{
	memset(p, 0, sizeof(*p));
	p->crc32 = crc32;
}

size_t inst_frame(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *pl, size_t n,
		  inst_crc_fn crc32)
{
	if (n > INST_FRAME_MAX_PAYLOAD)
		n = INST_FRAME_MAX_PAYLOAD;
	out[0] = 0x57u;
	out[1] = 0x52u;
	out[2] = type;
	out[3] = seq;
	wb_put_le16(out + 4, (uint16_t)n);
	if (n != 0u && pl != out + INST_FRAME_HEAD)
		memmove(out + INST_FRAME_HEAD, pl, n);
	wb_put_le32(out + INST_FRAME_HEAD + n, crc32(0u, out + 2, 4u + n));
	return INST_FRAME_HEAD + n + 4u;
}

static size_t put_str(uint8_t *p, size_t at, size_t cap, const char *s)
{
	const size_t n = s != NULL ? strlen(s) : 0u;

	if (at + n + 1u > cap)
		return at;
	if (n != 0u)
		memcpy(p + at, s, n);
	p[at + n] = 0u;
	return at + n + 1u;
}

/* The reply's payload into pl; its length. */
static size_t answer(uint8_t type, const uint8_t *rq, size_t rn, const struct inst_view *v,
		     uint8_t *pl)
{
	const struct inst_state *s = v->state;
	unsigned i;

	switch (type) {
	case INST_T_HELLO: {
		const struct inst_hello *h = v->hello;

		memset(pl, 0, INST_HELLO_LEN);
		pl[0] = 0x57u;
		pl[1] = 0x42u;
		pl[2] = 0x49u;
		pl[3] = 0x4Eu;
		pl[4] = INST_PROTO_VERSION;
		pl[5] = s->phase;
		pl[6] = s->flags;
		pl[7] = (uint8_t)TCHK_COUNT;
		for (i = 0; i < 4u; i++)
			wb_put_le16(pl + 8 + 2u * i, h->version[i]);
		wb_put_le32(pl + 16, h->len);
		wb_put_le32(pl + 20, h->crc);
		memcpy(pl + 24, h->pin, 32);
		pl[INST_HELLO_O_DEVICE] = h->device;
		pl[INST_HELLO_O_DEVICE + 1u] = h->how;
		pl[INST_HELLO_O_DEVICE + 2u] = h->image_device;
		pl[INST_HELLO_O_DEVICE + 3u] = h->survey;
		return INST_HELLO_LEN;
	}
	case INST_T_SURVEY:
		if (v->survey == NULL) {
			pl[0] = INST_ST_UNKNOWN;
			return 1;
		}
		return inst_survey_bytes(v->survey, pl);
	case INST_T_STATE:
		memset(pl, 0, INST_STATE_HEAD + TCHK_COUNT);
		pl[0] = s->phase;
		pl[1] = s->flags;
		pl[2] = s->verdict;
		pl[3] = s->under_way;
		pl[4] = s->lock;
		pl[5] = s->sel_index;
		pl[6] = s->sel_verified;
		pl[7] = s->slot_state[0];
		pl[8] = s->slot_state[1];
		pl[9] = s->target;
		pl[10] = s->last_rc;
		pl[11] = s->os;
		wb_put_le16(pl + 12, s->hold_fill);
		pl[14] = s->hold_digit;
		pl[15] = s->trial;
		pl[16] = s->next_digit;
		pl[17] = s->streams;
		for (i = 0; i < TCHK_COUNT; i++)
			pl[INST_STATE_HEAD + i] = v->table->r[i].state;
		return INST_STATE_HEAD + TCHK_COUNT;
	case INST_T_TEST: {
		const unsigned id = rn >= 1u ? rq[0] : TCHK_COUNT;
		const struct tchk_result *r;
		size_t at = 4;

		if (id >= TCHK_COUNT) {
			pl[0] = INST_ST_NO_TEST;
			return 1;
		}
		r = &v->table->r[id];
		pl[0] = INST_ST_OK;
		pl[1] = (uint8_t)id;
		pl[2] = r->state;
		pl[3] = tchk_tests[id].kind;
		at = put_str(pl, at, INST_FRAME_MAX_PAYLOAD, tchk_tests[id].id);
		at = put_str(pl, at, INST_FRAME_MAX_PAYLOAD, tchk_tests[id].name);
		at = put_str(pl, at, INST_FRAME_MAX_PAYLOAD, r->detail.s);
		at = put_str(pl, at, INST_FRAME_MAX_PAYLOAD, r->more.s);
		return at;
	}
	case INST_T_REPORT: {
		const uint32_t off = rn >= 4u ? wb_get_le32(rq) : 0u;
		const size_t left = off < v->report_len ? v->report_len - off : 0u;
		const size_t k = left < INST_REPORT_CHUNK ? left : INST_REPORT_CHUNK;

		pl[0] = INST_ST_OK;
		wb_put_le32(pl + 1, (uint32_t)v->report_len);
		if (k != 0u)
			memcpy(pl + 5, v->report + off, k);
		return 5u + k;
	}
	default:
		pl[0] = INST_ST_UNKNOWN;
		return 1;
	}
}

/* Bytes up to the next "WR" dropped, from the front of rx. */
static void resync(struct inst_proto *p, size_t from)
{
	size_t i;

	for (i = from; i < p->rx_n; i++)
		if (p->rx[i] == 0x57u && (i + 1u == p->rx_n || p->rx[i + 1u] == 0x52u))
			break;
	p->dropped += (uint32_t)i;
	memmove(p->rx, p->rx + i, p->rx_n - i);
	p->rx_n -= i;
}

size_t inst_proto_feed(struct inst_proto *p, const uint8_t *b, size_t n,
		       const struct inst_view *v)
{
	size_t taken = 0;

	while (taken < n && p->tx_off == p->tx_n) {
		size_t len;

		p->rx[p->rx_n++] = b[taken++];
		if (p->rx_n == 1u && p->rx[0] != 0x57u) {
			resync(p, 1u);
			continue;
		}
		if (p->rx_n == 2u && p->rx[1] != 0x52u) {
			resync(p, 1u);
			continue;
		}
		if (p->rx_n < INST_FRAME_HEAD)
			continue;
		len = wb_get_le16(p->rx + 4);
		if (len > INST_FRAME_MAX_PAYLOAD) {
			resync(p, 1u);
			continue;
		}
		if (p->rx_n < INST_FRAME_HEAD + len + 4u)
			continue;
		if (p->crc32(0u, p->rx + 2, 4u + len) != wb_get_le32(p->rx + INST_FRAME_HEAD + len)) {
			resync(p, 1u);
			continue;
		}
		{
			uint8_t rq[INST_FRAME_MAX_PAYLOAD];
			const uint8_t type = p->rx[2], seq = p->rx[3];
			size_t k;

			memcpy(rq, p->rx + INST_FRAME_HEAD, len);
			p->rx_n = 0;
			k = answer(type, rq, len, v, p->tx + INST_FRAME_HEAD);
			p->tx_n = inst_frame(p->tx, (uint8_t)(type | 0x80u), seq, p->tx + INST_FRAME_HEAD,
					     k, p->crc32);
			p->tx_off = 0;
		}
	}
	return taken;
}

const uint8_t *inst_proto_out(const struct inst_proto *p, size_t *n)
{
	*n = p->tx_n - p->tx_off;
	return p->tx + p->tx_off;
}

void inst_proto_sent(struct inst_proto *p, size_t n)
{
	p->tx_off += n;
	if (p->tx_off >= p->tx_n)
		p->tx_off = p->tx_n = 0;
}

/* --- the screens --- */

struct lines {
	struct tchk_line *l;
	unsigned n;
};

static void add(struct lines *o, enum s15_style st, const char *a, const char *b)
{
	struct tchk_line *l;
	size_t k = 0, len;

	if (o->n >= S15_ROWS)
		return;
	l = &o->l[o->n++];
	l->style = (uint8_t)st;
	len = strlen(a);
	if (len > sizeof l->s - 1u)
		len = sizeof l->s - 1u;
	memcpy(l->s, a, len);
	k = len;
	if (b != NULL) {
		len = strlen(b);
		if (k + len > sizeof l->s - 1u)
			len = sizeof l->s - 1u - k;
		memcpy(l->s + k, b, len);
		k += len;
	}
	l->s[k] = '\0';
}

static const char *slot_name(uint8_t t)
{
	return t == 0u ? "Slot 1" : t == 1u ? "Slot 2" : "the other slot";
}

/* An item, its label perhaps with a hint after a tab; armed while its digit
 * is held, its label alone then (the bar on the last row says the rest). */
static void item(struct lines *o, const struct inst_screen *s, unsigned digit, const char *label)
{
	const char d[3] = { (char)('0' + digit), ' ', '\0' };
	char row[S15_LINE];
	const size_t len = strcspn(label, "\t");

	if (s->hold_digit != digit) {
		add(o, S15_ST_ITEM, d, label);
		return;
	}
	memcpy(row, d, 2);
	memcpy(row + 2, label, len);
	row[2 + len] = '\0';
	add(o, S15_ST_ARMED, row, NULL);
}

/* Under the items a hold chooses whose label does not name it, while
 * none is held. */
static void hold_line(struct lines *o, const struct inst_screen *s)
{
	if (s->hold_digit == 0u)
		add(o, S15_ST_DIM, s15_hold_line, NULL);
}

/* An item whose label names its hold (wiki/Screen-Style-Guide.md): no
 * hint under it, and its label whole when armed. */
static void held_item(struct lines *o, const struct inst_screen *s, unsigned digit,
		      const char *label)
{
	char row[S15_LINE];
	const size_t h = strlen(s15_hold_named);
	const size_t n = strlen(label) < sizeof row - 1u - h ? strlen(label) : sizeof row - 1u - h;

	memcpy(row, label, n);
	memcpy(row + n, s15_hold_named, h + 1u);
	item(o, s, digit, row);
}

/* 8, where an OS was found to make active again: what leaving does. */
static void return_item(struct lines *o, const struct inst_screen *s, bool os_ok)
{
	if (os_ok)
		held_item(o, s, INST_DIGIT_LEAVE, "Return to the old OS");
}

/* The hold's bar on the last row, while a key is held. */
static void bar(struct lines *o, const struct inst_screen *s)
{
	struct s15_buf b;

	if (s->hold_digit == 0u)
		return;
	s15_hold_bar(&b, s->hold_fill / 10u, s->hold_left_s);
	while (o->n < S15_ROWS - 1u)
		add(o, S15_ST_TEXT, "", NULL);
	add(o, S15_ST_STEP_RUN, b.s, NULL);
}

/* Making an OS found in the other partition active is quiet; only a
 * header write that failed is shown. */
static void os_line(struct lines *o, uint8_t os)
{
	if (os == INST_OS_WRITE_FAILED)
		add(o, S15_ST_ERROR, "eMMC boot partition write failed", NULL);
}

/* Small formatters for the screens and the report, into a fixed buffer. */
struct fmt {
	char s[S15_LINE];
	size_t n;
};

static void f_str(struct fmt *f, const char *s)
{
	while (*s != '\0' && f->n + 1u < sizeof f->s)
		f->s[f->n++] = *s++;
	f->s[f->n] = '\0';
}

static void f_dec(struct fmt *f, uint32_t v)
{
	char b[11];
	unsigned i = sizeof b;

	b[--i] = '\0';
	do {
		b[--i] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u);
	f_str(f, b + i);
}

static void f_hex(struct fmt *f, uint32_t v, unsigned digits)
{
	static const char hx[] = "0123456789abcdef";
	char b[9];
	unsigned i;

	digits = digits > 8u ? 8u : digits;
	for (i = 0; i < digits; i++)
		b[i] = hx[v >> (4u * (digits - 1u - i)) & 0xFu];
	b[digits] = '\0';
	f_str(f, b);
}

/* S-18.003's fingerprint as S-18.004 shows it: n in decimal, c in hex. */
static void f_fingerprint(struct fmt *f, uint32_t n, uint32_t c)
{
	f_str(f, "n ");
	f_dec(f, n);
	f_str(f, ", c ");
	f_hex(f, c, 8u);
}

static const char *device_name(uint8_t id)
{
	return id == WB_DEV_OP1F ? "OP-1F" : "OP-XY";
}

/* The key check, on the OP-XY by the keys' names, elsewhere by role. */
static void key_check(struct lines *o, const struct inst_screen *s, bool os_ok)
{
	const bool usb = (s->keys_seen & 1u) != 0u, rec = (s->keys_seen & 2u) != 0u;

	if (s->device == WB_DEV_OPXY) {
		add(o, S15_ST_HEAD, "Press com, then M4 to continue", NULL);
		add(o, S15_ST_TEXT, "", NULL);
		add(o, usb ? S15_ST_STEP_OK : S15_ST_STEP_RUN, "com", usb ? "\tpressed" : "\twaiting");
		add(o, rec ? S15_ST_STEP_OK : S15_ST_STEP_RUN, "M4 (under display)",
		    rec ? "\tpressed" : "\twaiting");
		add(o, S15_ST_TEXT, "", NULL);
	} else {
		struct fmt f = { "", 0 };

		add(o, S15_ST_HEAD, "Press com, then the recovery key to continue", NULL);
		add(o, S15_ST_TEXT, "com: boot menu (hold at power-on)", NULL);
		add(o, S15_ST_TEXT, "Recovery key: recovery menu (with Power)", NULL);
		add(o, usb ? S15_ST_STEP_OK : S15_ST_STEP_RUN, "com", usb ? "\tpressed" : "\twaiting");
		add(o, rec ? S15_ST_STEP_OK : S15_ST_STEP_RUN, "Recovery key",
		    rec ? "\tpressed" : "\twaiting");
		if (s->last_key != 0xFFu) {
			f_str(&f, "Last key: 0x");
			f_hex(&f, s->last_key, 2u);
		}
		add(o, S15_ST_DIM, f.s, NULL);
	}
	return_item(o, s, os_ok);
}

/* A slot as the survey found it, in a few words. */
static void slot_words(struct fmt *f, const struct inst_slot_survey *sl)
{
	if ((sl->bits & INST_SV_RECORD) == 0u) {
		f_str(f, "not read");
		return;
	}
	f_str(f, (sl->bits & INST_SV_GOOD) != 0u ? "good" : "not good");
	f_str(f, (sl->bits & INST_SV_WAFFLE) != 0u ? ", waffle-boot" : ", not waffle-boot");
}

/* The survey's screen: what it found, and what an install would do. */
static void surveyed(struct lines *o, const struct inst_screen *s)
{
	const struct inst_survey *sv = s->survey;
	struct fmt f = { "", 0 };
	unsigned k;

	add(o, S15_ST_HEAD, "Survey: SPI flash untouched", NULL);
	if (sv == NULL)
		return;
	f_str(&f, sv->how == WB_DEV_BY_TABLE ? device_name(sv->device) : "not identified");
	f_str(&f, " (");
	f_fingerprint(&f, sv->fs_n, sv->fs_c);
	f_str(&f, ")");
	add(o, S15_ST_KV, "Device\t", f.s);
	f.n = 0;
	f.s[0] = '\0';
	if (sv->sel_read) {
		f_hex(&f, sv->sel_index, 2u);
		f_str(&f, " ");
		f_hex(&f, sv->sel_verified, 2u);
		f_str(&f, ": ");
		f_str(&f, s02_selected_slot(sv->sel_index) == 0u ? "Slot 1" : "Slot 2");
		f_str(&f, s02_verified(sv->sel_verified) ? ", verified" : ", not verified");
	} else {
		f_str(&f, "not read");
	}
	add(o, S15_ST_KV, "Selector\t", f.s);
	for (k = 0; k < 2u; k++) {
		f.n = 0;
		f.s[0] = '\0';
		slot_words(&f, &sv->slot[k]);
		add(o, S15_ST_KV, k == 0u ? "Slot 1\t" : "Slot 2\t", f.s);
	}
	f.n = 0;
	f.s[0] = '\0';
	if (sv->ext_read) {
		f_str(&f, "PART_CONFIG ");
		f_hex(&f, sv->part_config, 2u);
		f_str(&f, ", boot ");
		f_dec(&f, sv->boot_kib);
		f_str(&f, " KiB");
	} else {
		f_str(&f, "not read");
	}
	add(o, S15_ST_KV, "eMMC\t", f.s);
	if (s->lock == INST_LOCK_SURVEY)
		add(o, S15_ST_OK, "Install possible", NULL);
	else
		add(o, S15_ST_WARN, inst_lock_words((enum inst_lock)s->lock), NULL);
	if (s->os == INST_OS_ACTIVE)
		add(o, S15_ST_TEXT, "OS active again: switch off and on.", NULL);
	else if (s->os == INST_OS_WRITE_FAILED)
		os_line(o, s->os);
	else
		add(o, S15_ST_TEXT, "No OS made active: the survey runs again", NULL);
}

/* Why no install is offered, and that nothing changed: one error line, or
 * two where the width needs them. */
static void locked_words(struct lines *o, const char *why)
{
	static const char tail[] = ", no changes made.";
	char row[S15_LINE];
	const size_t n = strlen(why);

	if (3u + n + sizeof tail - 1u <= S15_COLS) {
		add(o, S15_ST_ERROR, why, tail);
		return;
	}
	memcpy(row, why, n);
	row[n] = ',';
	row[n + 1u] = '\0';
	add(o, S15_ST_ERROR, row, NULL);
	add(o, S15_ST_ERROR, "No changes made.", NULL);
}

/* The commit's question with the version: one line, or broken before
 * "waffle-boot" where the width needs two. */
static void commit_question(struct lines *o, const struct inst_screen *s)
{
	static const char question[] = "Trial booted, do you wish to install";
	struct fmt v = { "", 0 }, f = { "", 0 };

	f_str(&v, "waffle-boot v");
	f_str(&v, s->version != NULL ? s->version : "");
	f_str(&v, "?");
	f_str(&f, question);
	f_str(&f, " ");
	f_str(&f, v.s);
	if (f.n <= S15_COLS) {
		add(o, S15_ST_HEAD, f.s, NULL);
		return;
	}
	add(o, S15_ST_HEAD, question, NULL);
	add(o, S15_ST_HEAD, v.s, NULL);
}

unsigned inst_screen_lines(const struct inst_screen *s, struct tchk_line out[S15_ROWS])
{
	struct lines o = { out, 0 };
	const bool os_ok = s->os == INST_OS_CHECKED || s->os == INST_OS_ENCRYPTED;

	switch ((enum inst_phase)s->phase) {
	case INST_STARTING:
	case INST_CHECKING:
		add(&o, S15_ST_HEAD, "Checking...", NULL);
		break;
	case INST_GUIDED:
		/* the one guided test: reading this shows the panel works */
		key_check(&o, s, os_ok);
		break;
	case INST_LOCKED:
		locked_words(&o, inst_lock_words((enum inst_lock)s->lock));
		if (s->lock == INST_LOCK_DEVICE) {
			struct fmt f = { "", 0 };

			f_fingerprint(&f, s->fs_n, s->fs_c);
			add(&o, S15_ST_KV, "First stage\t", f.s);
		}
		add(&o, S15_ST_TEXT, "See check.txt on USB disk for details.", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		if (os_ok)
			return_item(&o, s, os_ok);
		else
			add(&o, S15_ST_TEXT, "No OS: write one from the developer menu.", NULL);
		break;
	case INST_READY:
		add(&o, S15_ST_OK, "Every check passed.", NULL);
		add(&o, S15_ST_TEXT, "A trial boot will be written, it will only last for", NULL);
		add(&o, S15_ST_TEXT, "1 power cycle as a final test.", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		held_item(&o, s, 1u, "Write the trial");
		return_item(&o, s, os_ok);
		break;
	case INST_WRITING_TRIAL:
		add(&o, S15_ST_HEAD, "Writing...", NULL);
		add(&o, S15_ST_STEP_RUN, slot_name(s->target), "\tWriting");
		add(&o, S15_ST_WARN, "Keep the power on", NULL);
		break;
	case INST_TRIAL_WRITTEN:
		add(&o, S15_ST_HEAD, "Trial written to ", slot_name(s->target));
		add(&o, S15_ST_OK, "It starts once, at the next start.", NULL);
		/* S-02.095: the trial start's boot menu goes on to the OS */
		add(&o, S15_ST_TEXT, "Switch off and on: its boot menu shows for a moment,", NULL);
		add(&o, S15_ST_TEXT, "then the installer comes back by itself.", NULL);
		break;
	case INST_COMMIT_READY:
		commit_question(&o, s);
		add(&o, S15_ST_TEXT, "", NULL);
		held_item(&o, s, 1u, "Install waffle-boot");
		held_item(&o, s, INST_DIGIT_RETRIAL, "Trial boot again");
		return_item(&o, s, os_ok);
		break;
	case INST_COMMITTING:
		add(&o, S15_ST_HEAD, "Keeping...", NULL);
		add(&o, S15_ST_STEP_RUN, slot_name(s->target), "\tWriting");
		add(&o, S15_ST_WARN, "Keep the power on", NULL);
		break;
	case INST_DONE:
		add(&o, S15_ST_HEAD, "Done!", NULL);
		add(&o, S15_ST_OK, "waffle-boot installed", NULL);
		os_line(&o, s->os);
		add(&o, S15_ST_TEXT, "Switch off and on.", NULL);
		if (os_ok) {
			add(&o, S15_ST_TEXT, "", NULL);
			return_item(&o, s, os_ok);
		}
		break;
	case INST_FAILED:
		add(&o, S15_ST_HEAD, "Write failed", NULL);
		add(&o, S15_ST_ERROR, inst_rc_words(s->last_rc), NULL);
		add(&o, S15_ST_TEXT, s->trial == INST_TRIAL_REACHED ?
				     "Both slots start." :
				     "Your bootloader stays selected.", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		item(&o, s, 1u, "Try again");
		hold_line(&o, s);
		return_item(&o, s, os_ok);
		break;
	case INST_TRIAL_FAILED:
		add(&o, S15_ST_HEAD, "Trial did not reach the boot menu", NULL);
		add(&o, S15_ST_ERROR, s->trial == INST_TRIAL_SPENT ?
				      "The new bootloader did not run" :
				      "It started, but no boot menu showed", NULL);
		add(&o, S15_ST_TEXT, "Nothing kept: yours stays selected.", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		item(&o, s, 1u, "Write the trial again");
		hold_line(&o, s);
		return_item(&o, s, os_ok);
		break;
	case INST_LEFT:
		add(&o, S15_ST_HEAD, "Not installed", NULL);
		os_line(&o, s->os);
		add(&o, S15_ST_TEXT, "Switch off and on.", NULL);
		break;
	case INST_SURVEYED:
		surveyed(&o, s);
		break;
	default:
		break;
	}
	bar(&o, s);
	return o.n;
}

/* --- check.txt's install part --- */

struct rep {
	char *p;
	size_t n, cap;
};

static void r_str(struct rep *r, const char *s)
{
	while (*s != '\0' && r->n + 1u < r->cap)
		r->p[r->n++] = *s++;
}

static void r_dec(struct rep *r, uint32_t v)
{
	char b[11];
	unsigned i = sizeof b;

	b[--i] = '\0';
	do {
		b[--i] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u);
	r_str(r, b + i);
}

static void r_hex(struct rep *r, uint32_t v, unsigned digits)
{
	static const char hx[] = "0123456789abcdef";
	char b[9];
	unsigned i;

	digits = digits > 8u ? 8u : digits;
	for (i = 0; i < digits; i++)
		b[i] = hx[v >> (4u * (digits - 1u - i)) & 0xFu];
	b[digits] = '\0';
	r_str(r, b);
}

static const char *phase_word(uint8_t p)
{
	static const char *const w[] = {
		"starting", "checking", "key check", "locked", "ready", "writing the trial",
		"trial written", "commit ready", "committing", "done", "failed",
		"trial failed", "left", "surveyed",
	};

	return p < sizeof w / sizeof w[0] ? w[p] : "?";
}

static const char *trial_word(uint8_t t)
{
	static const char *const w[] = {
		"none", "written, not started", "reached the boot menu",
		"started, the boot menu not reached", "used up without a start of it",
	};

	return t < sizeof w / sizeof w[0] ? w[t] : "?";
}

static const char *os_word(uint8_t o)
{
	static const char *const w[] = {
		"not looked at", "an OS, checked", "an OS stored encrypted, not checked",
		"no OS header", "an image whose CRC differs", "not read", "made active",
		"a header write failed",
	};

	return o < sizeof w / sizeof w[0] ? w[o] : "?";
}

static void r_slot(struct rep *r, unsigned i, uint8_t st)
{
	r_str(r, i == 0u ? "slot 1: " : "slot 2: ");
	r_str(r, (st & OPXY_INSTALL_SLOT_GOOD) != 0u ? "good" : "not good");
	if ((st & OPXY_INSTALL_SLOT_COMMITTED) != 0u)
		r_str(r, ", holds this image");
	if ((st & OPXY_INSTALL_SLOT_TRIAL) != 0u)
		r_str(r, ", holds this image under the trial mark");
	r_str(r, "\n");
}

size_t inst_report_append(char *out, size_t len, size_t cap, const struct inst_hello *h,
			  const struct inst_state *st, const struct inst_image *img,
			  const uint8_t *eng)
{
	struct rep r = { out, len, cap };
	unsigned i;

	if (cap == 0u || len >= cap)
		return len;
	r_str(&r, "\ninstall\n");
	/* S-18: the device, and the fingerprint only of one not identified */
	r_str(&r, "device: ");
	if (h->how == (uint8_t)WB_DEV_BY_TABLE) {
		r_str(&r, device_name(h->device));
		r_str(&r, ", identified by its first stage");
	} else if (h->how == (uint8_t)WB_DEV_SECOND_CPU) {
		r_str(&r, "the second CPU");
	} else {
		struct fmt f = { "", 0 };

		f_fingerprint(&f, h->fs_n, h->fs_c);
		r_str(&r, "not identified, first stage ");
		r_str(&r, f.s);
	}
	r_str(&r, "\nimage built for: ");
	r_str(&r, device_name(h->image_device));
	r_str(&r, h->survey != 0u ? ", a survey build: no SPI-flash write\n" : "\n");
	r_str(&r, "image: waffle-boot v");
	for (i = 0; i < 3u; i++) {
		if (i != 0u)
			r_str(&r, ".");
		r_dec(&r, h->version[i]);
	}
	r_str(&r, ", ");
	r_dec(&r, h->len);
	r_str(&r, " bytes, crc32 ");
	r_hex(&r, h->crc, 8u);
	r_str(&r, "\nfront pin: ");
	for (i = 0; i < 32u; i++)
		r_hex(&r, h->pin[i], 2u);
	r_str(&r, "\nimage check: ");
	r_str(&r, !img->crc_ok ? "FAIL, crc32" : !img->stream_ok ? "FAIL, stream" :
		  !img->desc_ok ? "FAIL, descriptor" : !img->front_crc_ok ? "FAIL, front crc" :
		  !img->pin_ok ? "FAIL, front pin" : "pass");
	r_str(&r, "\nphase: ");
	r_str(&r, phase_word(st->phase));
	if (st->lock != INST_LOCK_NONE) {
		r_str(&r, "\nlocked: ");
		r_str(&r, inst_lock_words((enum inst_lock)st->lock));
	}
	if (eng == NULL) {
		/* the engine reads the flash once the checks end */
		r_str(&r, "\nselector and slots: not read yet\n");
	} else {
		r_str(&r, "\nselector: ");
		r_hex(&r, st->sel_index, 2u);
		r_str(&r, " ");
		r_hex(&r, st->sel_verified, 2u);
		r_str(&r, "\n");
		r_slot(&r, 0u, st->slot_state[0]);
		r_slot(&r, 1u, st->slot_state[1]);
	}
	if (eng != NULL && st->target <= 1u) {
		r_str(&r, "target: ");
		r_str(&r, slot_name(st->target));
		r_str(&r, "\n");
	}
	r_str(&r, "trial: ");
	r_str(&r, trial_word(st->trial));
	if (st->last_rc != 0xFFu) {
		r_str(&r, "\nlast write: ");
		r_str(&r, inst_rc_words(st->last_rc));
	}
	r_str(&r, "\nother boot partition: ");
	r_str(&r, os_word(st->os));
	r_str(&r, "\n");
	if (eng != NULL) {
		const uint8_t *log = eng + 9 + OPXY_INSTALL_CRUMBS;

		r_str(&r, "metadata mirror: ");
		r_str(&r, eng[8] == OPXY_INSTALL_MIRROR_SAME ? "same as the record" :
			  eng[8] == OPXY_INSTALL_MIRROR_DIFFERS ? "differs from the record" :
			  eng[8] == OPXY_INSTALL_MIRROR_INVALID ? "none (erased or not valid)" : "not read");
		r_str(&r, "\n");
		if (log[0] == 1u) {
			r_str(&r, "start log: ");
			r_dec(&r, (uint32_t)log[3] | (uint32_t)log[4] << 8);
			r_str(&r, " used, ");
			r_dec(&r, (uint32_t)log[5] | (uint32_t)log[6] << 8);
			r_str(&r, " unfinished at the end\n");
		}
		/* a breadcrumb build's marks, from byte 1 of the tried slot's
		 * last sector; byte 0 is its log's */
		bool crumbs = false;

		for (i = 1; i < OPXY_INSTALL_CRUMBS; i++)
			crumbs = crumbs || eng[9 + i] != 0xFFu;
		if (crumbs) {
			r_str(&r, "breadcrumbs:");
			for (i = 1; i < OPXY_INSTALL_CRUMBS; i++) {
				r_str(&r, " ");
				r_hex(&r, eng[9 + i], 2u);
			}
			r_str(&r, "\n");
		}
	}
	out[r.n] = '\0';
	return r.n;
}

/* --- the survey (make installer-survey) --- */

size_t inst_survey_bytes(const struct inst_survey *sv, uint8_t *out)
{
	unsigned k;

	memset(out, 0, INST_SURVEY_LEN);
	out[0] = INST_ST_OK;
	out[1] = sv->device;
	out[2] = sv->how;
	out[3] = sv->boot_mode;
	wb_put_le32(out + 4, sv->fs_n);
	wb_put_le32(out + 8, sv->fs_c);
	out[12] = sv->sel_read ? 1u : 0u;
	out[13] = sv->sel_index;
	out[14] = sv->sel_verified;
	out[15] = sv->pa2;
	wb_put_le32(out + 16, sv->sec_count);
	wb_put_le32(out + 20, sv->boot_kib);
	out[24] = sv->ext_read ? 1u : 0u;
	out[25] = sv->part_config;
	out[26] = sv->os;
	for (k = 0; k < 2u; k++) {
		uint8_t *o = out + 28u + 24u * k;
		const struct inst_slot_survey *sl = &sv->slot[k];

		o[0] = sl->bits;
		wb_put_le32(o + 4, sl->crc);
		wb_put_le32(o + 8, sl->length);
		wb_put_le32(o + 12, sl->tag);
		memcpy(o + 16, sl->log, INST_SURVEY_LOG);
	}
	memcpy(out + 76, sv->fault, sizeof(sv->fault));
	return INST_SURVEY_LEN;
}

size_t inst_survey_report(char *out, size_t len, size_t cap, const struct inst_survey *sv)
{
	struct rep r = { out, len, cap };
	struct fmt f = { "", 0 };
	unsigned k, i;

	if (cap == 0u || len >= cap)
		return len;
	r_str(&r, "\nsurvey (nothing was written to the SPI flash)\n");
	r_str(&r, "boot mode: ");
	r_dec(&r, sv->boot_mode);
	r_str(&r, "\nfirst stage: ");
	f_fingerprint(&f, sv->fs_n, sv->fs_c);
	r_str(&r, f.s);
	r_str(&r, sv->how == WB_DEV_BY_TABLE ? ", the table's " : ", in no row of the table");
	if (sv->how == WB_DEV_BY_TABLE)
		r_str(&r, device_name(sv->device));
	r_str(&r, "\nselector: ");
	if (sv->sel_read) {
		r_hex(&r, sv->sel_index, 2u);
		r_str(&r, " ");
		r_hex(&r, sv->sel_verified, 2u);
	} else {
		r_str(&r, "not read");
	}
	r_str(&r, "\n");
	for (k = 0; k < 2u; k++) {
		const struct inst_slot_survey *sl = &sv->slot[k];

		r_str(&r, k == 0u ? "slot 1 record: " : "slot 2 record: ");
		if ((sl->bits & INST_SV_RECORD) != 0u) {
			r_str(&r, "crc32 ");
			r_hex(&r, sl->crc, 8u);
			r_str(&r, ", length ");
			r_hex(&r, sl->length, 8u);
			r_str(&r, ", tag ");
			r_hex(&r, sl->tag, 8u);
			r_str(&r, (sl->bits & INST_SV_GOOD) != 0u ? "; verifies" : "; does not verify");
			r_str(&r, (sl->bits & INST_SV_WAFFLE) != 0u ? "; a waffle-boot stream" :
				  "; not a waffle-boot stream");
		} else {
			r_str(&r, "not read");
		}
		r_str(&r, k == 0u ? "\nslot 1 start log: " : "\nslot 2 start log: ");
		if ((sl->bits & INST_SV_LOG) != 0u) {
			for (i = 0; i < INST_SURVEY_LOG; i++) {
				r_str(&r, i != 0u ? " " : "");
				r_hex(&r, sl->log[i], 2u);
			}
		} else {
			r_str(&r, "not read");
		}
		r_str(&r, "\n");
	}
	r_str(&r, "emmc: ");
	if (sv->ext_read) {
		r_str(&r, "EXT_CSD[179] PART_CONFIG ");
		r_hex(&r, sv->part_config, 2u);
		r_str(&r, ", SEC_COUNT ");
		r_hex(&r, sv->sec_count, 8u);
		r_str(&r, ", boot partition ");
		r_dec(&r, sv->boot_kib);
		r_str(&r, " KiB");
	} else {
		r_str(&r, "EXT_CSD not read");
	}
	r_str(&r, "\nPA2: ");
	r_str(&r, sv->pa2 == 0u ? "low" : sv->pa2 == 1u ? "high" : "not read");
	r_str(&r, "\nOS partition: ");
	r_str(&r, os_word(sv->os));
	r_str(&r, "\n");
	out[r.n] = '\0';
	return r.n;
}

#else
typedef int installer_logic_not_built;	/* a translation unit is not empty */
#endif
