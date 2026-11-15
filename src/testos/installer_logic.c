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
#include "waffle/s02_flash.h"
#include "waffle/s05_handoff.h"
#include "waffle/s15_confirm.h"
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
			    bool image_ok, bool selected_ok)
{
	bool failed = false, untested = false, guided = false;
	unsigned i;

	for (i = 0; i < TCHK_COUNT; i++) {
		const uint8_t state = t->r[i].state;

		if (i == TCHK_OS_PARTITION || state == TCHK_PASS)
			continue;
		if (tchk_tests[i].kind == TCHK_KIND_GUIDED)
			guided = true;
		else if (state == TCHK_FAIL)
			failed = true;
		else
			untested = true;
	}
	if (sw->result == TCHK_SW_NO_SLAVE)
		return INST_LOCK_EMMC;
	if (failed)
		return INST_LOCK_FAILED;
	if (untested)
		return INST_LOCK_NOT_TESTED;
	if (guided)
		return INST_LOCK_GUIDED;
	if (!image_ok)
		return INST_LOCK_IMAGE;
	if (!selected_ok)
		return INST_LOCK_SELECTED;
	/*
	 * SPEC-Q: S-03.058 lets an OS write erase the other partition's group
	 * 0 after deactivating it; waffle-boot's does not. Whether the OS
	 * write of the bootloader the unit came with leaves the old OS's
	 * image readable is not stated. The install is held to an OS this
	 * image can make active again; a unit whose OS is gone stays locked.
	 */
	if (sw->result != TCHK_SW_READY)
		return INST_LOCK_NO_OS;
	return INST_LOCK_NONE;
}

const char *inst_lock_words(enum inst_lock l)
{
	static const char *const w[] = {
		[INST_LOCK_NONE] = "Every check passed.",
		[INST_LOCK_FAILED] = "A check failed.",
		[INST_LOCK_NOT_TESTED] = "A check could not run.",
		[INST_LOCK_IMAGE] = "The image it carries does not pass its check.",
		[INST_LOCK_SELECTED] = "The unit's bootloader slot is not settled.",
		[INST_LOCK_NO_OS] = "The unit's OS was not found, or fails its check.",
		[INST_LOCK_GUIDED] = "The key check (com and M4) was ended with 8.",
		[INST_LOCK_EMMC] = "The eMMC could not be reached.",
	};

	return (unsigned)l < sizeof w / sizeof w[0] ? w[l] : "";
}

const char *inst_rc_words(uint8_t rc)
{
	switch (rc) {
	case 0:
		return "Done.";
	case S02_ERR_IO:
		return "A read, erase or program of the flash failed.";
	case S02_ERR_SIZE:
		return "The image does not fit the slot.";
	case S02_ERR_VERIFY:
		return "What was written does not read back the same.";
	case S02_ERR_START:
		return "The unit's bootloader slot is not settled.";
	case S02_ERR_NOT_GOOD:
		return "The slot written does not pass its check.";
	case S02_ERR_STREAM:
		return "The image is not one the unit can start.";
	case OPXY_INSTALL_REFUSED_CRUMBS:
		return "A breadcrumb build is never kept.";
	case OPXY_INSTALL_REFUSED_UNSTARTED:
		return "The trial has not started the new bootloader.";
	case OPXY_INSTALL_REFUSED_IMAGE:
		return "The image it carries does not pass its check.";
	case OPXY_INSTALL_REFUSED_UNREACHED:
		return "The trial did not reach the new boot menu.";
	default:
		return "The write failed.";
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

#define KEY_PRECOMP 0x17u	/* S-07.032: not acted on */

/* A knob click (0x0A - 0x0D) starts no hold. */
static bool knob_click(unsigned key)
{
	return key >= 0x0au && key <= 0x0du;
}

static bool alone(const struct inst_hold *h, unsigned key)
{
	for (unsigned k = 0; k < S15_NKEYS; k++)
		if (k != key && k != KEY_PRECOMP && bit(h->down, k))
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
		if (key != KEY_PRECOMP)
			h->holding = false;	/* another key down */
		return;
	}
	if (digit != 0u && digit < 16u && ((unsigned)h->mask >> digit & 1u) != 0u && !knob_click(key) &&
	    alone(h, key)) {
		h->holding = true;
		h->key = key;
		h->digit = digit;
		h->t0 = now_ms;
	}
}

unsigned inst_hold_tick(struct inst_hold *h, uint32_t now_ms)
{
	if (!h->holding || now_ms - h->t0 < S15_CONFIRM_HOLD_MS)
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
	return held >= S15_CONFIRM_HOLD_MS ? 1000u : (unsigned)(held * 1000u / S15_CONFIRM_HOLD_MS);
}

/* --- the carried image --- */

static bool meets(uint32_t a, uint32_t n, uint32_t lo, uint32_t hi)
{
	return n != 0u && a < hi && lo < (uint64_t)a + n;
}

/* The front's windows (src/front/front.ld; tools/mkldr.py FR_WINDOWS). */
static const uint32_t windows[3][2] = {
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

			if (!meets(b.target, b.count, windows[w][0], windows[w][1]))
				continue;
			if (!data || b.target < windows[w][0] ||
			    (uint64_t)b.target + b.count > windows[w][1]) {
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
		return INST_HELLO_LEN;
	}
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
	size_t k = 0, m;

	if (o->n >= S15_ROWS)
		return;
	l = &o->l[o->n++];
	l->style = (uint8_t)st;
	m = strlen(a);
	if (m > sizeof l->s - 1u)
		m = sizeof l->s - 1u;
	memcpy(l->s, a, m);
	k = m;
	if (b != NULL) {
		m = strlen(b);
		if (k + m > sizeof l->s - 1u)
			m = sizeof l->s - 1u - k;
		memcpy(l->s + k, b, m);
		k += m;
	}
	l->s[k] = '\0';
}

static const char *slot_name(uint8_t t)
{
	return t == 0u ? "Slot 1" : t == 1u ? "Slot 2" : "the other slot";
}

/* An item; armed while its digit is held, the hint then naming the hold. */
static void item(struct lines *o, const struct inst_screen *s, unsigned digit, const char *label)
{
	const char d[3] = { (char)('0' + digit), ' ', '\0' };
	char row[S15_LINE];

	if (s->hold_digit != digit) {
		add(o, S15_ST_ITEM, d, label);
		return;
	}
	{
		struct s15_buf hint;
		size_t n = 2;
		const size_t m = strlen(label);

		s15_hold_hint(&hint, digit);
		memcpy(row, d, 2);
		memcpy(row + n, label, m);
		n += m;
		row[n++] = '\t';
		memcpy(row + n, hint.s, hint.n);
		n += hint.n;
		row[n] = '\0';
	}
	add(o, S15_ST_ARMED, row, NULL);
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

static void os_line(struct lines *o, uint8_t os)
{
	if (os == INST_OS_ACTIVE)
		add(o, S15_ST_OK, "Your OS is active again.", NULL);
	else if (os == INST_OS_WRITE_FAILED)
		add(o, S15_ST_ERROR, "Your OS could not be made active again.", NULL);
}

unsigned inst_screen_lines(const struct inst_screen *s, struct tchk_line out[S15_ROWS])
{
	struct lines o = { out, 0 };
	const bool os_ok = s->os == INST_OS_CHECKED || s->os == INST_OS_ENCRYPTED;

	switch ((enum inst_phase)s->phase) {
	case INST_STARTING:
	case INST_CHECKING:
		add(&o, S15_ST_HEAD, "Checking the hardware", NULL);
		break;
	case INST_GUIDED:
		/* the one guided test: reading this shows the panel works */
		add(&o, S15_ST_HEAD, "Press com, then M4 (4 under the display), to go on", NULL);
		add(&o, S15_ST_TEXT, "These two presses are the key check.", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		add(&o, (s->keys_seen & 1u) ? S15_ST_STEP_OK : S15_ST_STEP_RUN, "com",
		    (s->keys_seen & 1u) ? "\tpressed" : "\twaiting");
		add(&o, (s->keys_seen & 2u) ? S15_ST_STEP_OK : S15_ST_STEP_RUN, "M4",
		    (s->keys_seen & 2u) ? "\tpressed" : "\twaiting");
		add(&o, S15_ST_TEXT, "", NULL);
		add(&o, S15_ST_ITEM, "8 End: the install is then not offered", NULL);
		break;
	case INST_LOCKED:
		add(&o, S15_ST_HEAD, "The install is not offered", NULL);
		add(&o, S15_ST_ERROR, inst_lock_words((enum inst_lock)s->lock), NULL);
		add(&o, S15_ST_TEXT, "Nothing was written to the SPI flash.", NULL);
		add(&o, S15_ST_TEXT, "Results: check.txt on the USB disk, or the page.", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		if (os_ok) {
			item(&o, s, 8u, "Make your OS active again");
		} else {
			add(&o, S15_ST_TEXT, "Write your OS file from the developer menu", NULL);
			add(&o, S15_ST_TEXT, "to start it again.", NULL);
		}
		break;
	case INST_READY:
		add(&o, S15_ST_HEAD, "Install waffle-boot v", s->version);
		add(&o, S15_ST_OK, "Every check passed.", NULL);
		add(&o, S15_ST_KV, "Writes\t", slot_name(s->target));
		add(&o, S15_ST_TEXT, "A trial first: the new bootloader starts once.", NULL);
		add(&o, S15_ST_TEXT, "Your bootloader stays selected until you keep it.", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		item(&o, s, 1u, "Write the trial");
		item(&o, s, 8u, "Leave, your OS active again");
		break;
	case INST_WRITING_TRIAL:
		add(&o, S15_ST_HEAD, "Writing the trial", NULL);
		add(&o, S15_ST_STEP_RUN, slot_name(s->target), "\tWriting");
		add(&o, S15_ST_WARN, "Do not switch off.", NULL);
		break;
	case INST_TRIAL_WRITTEN:
		add(&o, S15_ST_HEAD, "Trial written to ", slot_name(s->target));
		add(&o, S15_ST_OK, "The new bootloader starts once, at the next start.", NULL);
		add(&o, S15_ST_TEXT, "1. Switch off, then on: its boot menu shows.", NULL);
		add(&o, S15_ST_TEXT, "2. Switch off, then on again: this installer", NULL);
		add(&o, S15_ST_TEXT, "   comes back to keep it.", NULL);
		add(&o, S15_ST_DIM, "Until then your bootloader stays selected.", NULL);
		break;
	case INST_COMMIT_READY:
		add(&o, S15_ST_HEAD, "The trial reached the boot menu", NULL);
		add(&o, S15_ST_KV, "Keeps\t", slot_name(s->target));
		add(&o, S15_ST_TEXT, "Keep waffle-boot v", s->version);
		add(&o, S15_ST_TEXT, "as the unit's bootloader. Your OS is then made", NULL);
		add(&o, S15_ST_TEXT, "active again.", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		item(&o, s, 1u, "Keep the new bootloader");
		item(&o, s, 8u, "Leave, keep the old one");
		break;
	case INST_COMMITTING:
		add(&o, S15_ST_HEAD, "Keeping the new bootloader", NULL);
		add(&o, S15_ST_STEP_RUN, slot_name(s->target), "\tWriting");
		add(&o, S15_ST_WARN, "Do not switch off.", NULL);
		break;
	case INST_DONE:
		add(&o, S15_ST_HEAD, "Done!", NULL);
		add(&o, S15_ST_OK, "waffle-boot is the unit's bootloader.", NULL);
		os_line(&o, s->os);
		add(&o, S15_ST_TEXT, "Switch off, then on.", NULL);
		add(&o, S15_ST_DIM, "Recovery menu: hold Power + M4 while switching on.", NULL);
		if (os_ok) {
			add(&o, S15_ST_TEXT, "", NULL);
			item(&o, s, 8u, "Make your OS active again");
		}
		break;
	case INST_FAILED:
		add(&o, S15_ST_HEAD, "The write did not complete", NULL);
		add(&o, S15_ST_ERROR, inst_rc_words(s->last_rc), NULL);
		add(&o, S15_ST_TEXT, s->trial == INST_TRIAL_REACHED ?
				     "Both slots hold a bootloader that starts." :
				     "The unit keeps its bootloader selected.", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		item(&o, s, 1u, "Try again");
		if (os_ok)
			item(&o, s, 8u, "Leave, your OS active again");
		break;
	case INST_TRIAL_FAILED:
		add(&o, S15_ST_HEAD, "The trial did not reach the boot menu", NULL);
		add(&o, S15_ST_ERROR, s->trial == INST_TRIAL_SPENT ?
				      "The trial start did not run the new bootloader." :
				      "The new bootloader started; no boot menu showed.", NULL);
		add(&o, S15_ST_TEXT, "Nothing was kept: your bootloader stays selected.", NULL);
		add(&o, S15_ST_TEXT, "", NULL);
		item(&o, s, 1u, "Write the trial again");
		if (os_ok)
			item(&o, s, 8u, "Leave, your OS active again");
		break;
	case INST_LEFT:
		add(&o, S15_ST_HEAD, "Left without installing", NULL);
		os_line(&o, s->os);
		add(&o, S15_ST_TEXT, "Switch off, then on.", NULL);
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
		"trial failed", "left",
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
		"not looked at", "found, checked", "found, stored encrypted (not checked)",
		"not found", "fails its check", "could not be read", "active again",
		"could not be made active again",
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
	r_str(&r, "\nOS: ");
	r_str(&r, os_word(st->os));
	r_str(&r, "\n");
	if (eng != NULL) {
		const uint8_t *log = eng + 9 + OPXY_INSTALL_CRUMBS;

		r_str(&r, "metadata mirror: ");
		r_str(&r, eng[8] == OPXY_INSTALL_MIRROR_SAME ? "same as the record" :
			  eng[8] == OPXY_INSTALL_MIRROR_DIFFERS ? "differs from the record" :
			  eng[8] == OPXY_INSTALL_MIRROR_INVALID ? "not valid" : "not read");
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

#else
typedef int installer_logic_not_built;	/* a translation unit is not empty */
#endif
