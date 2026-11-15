/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Recovery's protocol (see fr_proto.h). Portable: the port, the flash and
 * the staging buffer come from the caller. */
#include "fr_proto.h"

_Static_assert(FR_INFO_LEN <= 256u, "INFO fits the reply buffer");
_Static_assert(FR_HELLO_LEN <= 256u, "HELLO fits the reply buffer");

#define FR_PROTO_PASSES 8u

/* The writer's plan for a BEGIN with these flags (fr_plan_decide()), over
 * what st holds of the flash: every fact given, the selector where it
 * was read. */
static void plan_of(const struct fr_rec_status *st, uint8_t flags, struct fr_plan *p)
{
	const unsigned a = st->running.slot < 2u ? st->running.slot : 0u;
	const struct fr_slot_info *b = &st->slot[1u - a];
	const struct fr_facts x = {
		.run = &st->running,
		.home = &st->home,
		.flags = flags,
		.known = (uint8_t)(FR_KNOW_A | FR_KNOW_B | FR_KNOW_LOG |
				   (st->selector_ok ? FR_KNOW_SEL : 0u)),
		.sel = st->sel,
		.a = &st->slot[a],
		.b_good = b->good,
		.b_tried = b->tried,
		.b_started = b->started,
		.force = (flags & FR_BEGIN_F_FORCE) != 0u,
	};

	(void)fr_plan_decide(&x, p, NULL);
}

bool fr_rec_rescue(const struct fr_rec_status *st)
{
	struct fr_plan p;

	plan_of(st, 0, &p);
	return p.rescue;
}

void fr_rec_target(struct fr_rec_status *st)
{
	struct fr_plan p;

	plan_of(st, 0, &p);
	st->target = p.target;
	st->refusal = p.refusal;
	st->trial = p.trial_ok;
	plan_of(st, FR_BEGIN_F_RUNNING, &p);
	st->run_way = p.way;
	st->run_refusal = p.refusal;
	plan_of(st, FR_BEGIN_F_SELECT, &p);
	st->select_target = p.target;
	st->select_refusal = p.refusal;
}

void fr_rec_act(const struct fr_rec_status *st, uint8_t act, bool force, struct fr_act_plan *plan)
{
	const unsigned a = st->running.slot < 2u ? st->running.slot : 0u;
	const struct fr_act_facts x = {
		.run = &st->running,
		.home = &st->home,
		.sel_ok = st->selector_ok && !st->read_error,
		.sel = st->sel,
		.a = &st->slot[a],
		.b = &st->slot[1u - a],
		.act = act,
		.force = force,
	};

	fr_act_decide(&x, plan);
}

/* The survey, with idle called before each slot's passes and after them. */
static void survey(struct fr_rec_status *st, const struct fr_flash *f, void (*idle)(void *),
		   void *ctx)
{
	st->read_error = false;
	st->selector_ok = fr_read_selector(f, &st->sel) == FR_FLASH_OK;
	if (!st->selector_ok)
		st->read_error = true;
	for (unsigned k = 0; k < 2u; k++) {
		if (idle != NULL)
			idle(ctx);
		if (fr_slot_info(f, k, &st->slot[k]) != FR_FLASH_OK) {
			st->slot[k].good = false;
			st->read_error = true;
		}
	}
	if (idle != NULL)
		idle(ctx);
	fr_rec_target(st);
}

void fr_rec_survey(struct fr_rec_status *st, const struct fr_flash *f)
{
	survey(st, f, NULL, NULL);
}

unsigned fr_rec_reach(const struct fr_rec_status *st)
{
	bool good[2];

	/* what st holds may not be the flash's: nothing is claimed */
	if (!st->selector_ok || st->read_error)
		return FR_REACH_NONE;
	good[0] = st->slot[0].good;
	good[1] = st->slot[1].good;
	return fr_reach(&st->sel, good, &st->slot[fr_selected_slot(st->sel.index)].rec);
}

/* A write is under way, or accepted and counted down to. */
static bool busy(const struct fr_proto *p)
{
	return p->phase == FR_PH_WRITING || p->phase == FR_PH_COUNTDOWN;
}

void fr_proto_tick(struct fr_proto *p)
{
	if (!busy(p) && p->st->read_error) {
		survey(p->st, p->flash, p->idle, p->idle_ctx);
		p->changes++;
	}
}

/* S-02.084: no start would reach a good slot, by a write of this recovery. */
static bool damaged(const struct fr_rec_status *st)
{
	return fr_rec_rescue(st) && fr_rec_reach(st) == FR_REACH_NONE;
}

bool fr_rec_held(const struct fr_rec_status *st)
{
	return damaged(st);
}

bool fr_proto_hold(const struct fr_proto *p)
{
	return fr_proto_in_place(p) || damaged(p->st);
}

bool fr_proto_in_place(const struct fr_proto *p)
{
	return p->phase == FR_PH_WRITING && p->w.way == FR_WAY_IN_PLACE;
}

void fr_proto_init(struct fr_proto *p, const struct fr_bytes *io,
		   struct fr_rec_status *st, const struct fr_flash *flash,
		   uint8_t *staging)
{
	fr_fill(p, 0, sizeof *p);
	p->io = io;
	p->st = st;
	p->flash = flash;
	p->staging = staging;
	p->phase = FR_PH_IDLE;
	p->w.target = FR_NO_SLOT;
	fr_rec_target(st);
}

/* Hands the waiting reply to the port, as much as it takes. */
static void flush(struct fr_proto *p)
{
	/* bounded: each pass takes at least one byte, or stops */
	while (p->tx_at < p->tx_n) {
		size_t k = p->io->write(p->io->ctx, p->tx + p->tx_at, p->tx_n - p->tx_at);

		if (k == 0)
			return;
		p->tx_at += k;
	}
	p->tx_n = p->tx_at = 0;
}

static uint8_t *reply_begin(struct fr_proto *p, uint8_t type, uint8_t seq)
{
	p->tx[0] = 'W';
	p->tx[1] = 'R';
	p->tx[2] = (uint8_t)(type | FR_T_REPLY);
	p->tx[3] = seq;
	return p->tx + FR_PROTO_HEAD;
}

static void reply_end(struct fr_proto *p, size_t len)
{
	fr_put16(p->tx + 4, (uint16_t)len);
	fr_put32(p->tx + FR_PROTO_HEAD + len,
		 fr_crc32(0, p->tx + 2, 4u + len));
	p->tx_n = FR_PROTO_HEAD + len + 4u;
	p->tx_at = 0;
	flush(p);
}

static void status_reply(struct fr_proto *p, uint8_t type, uint8_t seq, uint8_t status)
{
	reply_begin(p, type, seq)[0] = status;
	reply_end(p, 1);
}

static void put_version(uint8_t *o, const uint16_t v[4])
{
	unsigned i;

	for (i = 0; i < 4u; i++)
		fr_put16(o + 2u * i, v[i]);
}

static uint8_t flags(const struct fr_rec_status *st)
{
	const unsigned a = st->running.slot < 2u ? st->running.slot : 0u;
	const bool away = st->running.established && st->selector_ok &&
			  fr_selected_slot(st->sel.index) != a;

	return (uint8_t)((st->running.established ? FR_INFO_F_ESTABLISHED : 0u) |
			 (st->selector_ok ? FR_INFO_F_SELECTOR : 0u) |
			 (st->own_desc ? FR_INFO_F_OWN_DESC : 0u) |
			 (st->home.moved && away ? FR_INFO_F_MOVED : 0u) |
			 (st->home.spoiled && !st->slot[a].good ? FR_INFO_F_SPOILED : 0u) |
			 (damaged(st) ? FR_INFO_F_HELD : 0u) |
			 (st->read_error ? FR_INFO_F_READ_ERROR : 0u) |
			 (fr_rec_rescue(st) && st->slot[a].good ? FR_INFO_F_UNSETTLED : 0u));
}

static void hello(struct fr_proto *p, uint8_t seq)
{
	const struct fr_rec_status *st = p->st;
	uint8_t *o = reply_begin(p, FR_T_HELLO, seq);

	fr_fill(o, 0, FR_HELLO_LEN);
	o[FR_HELLO_O_PROTO] = FR_PROTO_VERSION;
	fr_put16(o + FR_HELLO_O_FRONT, FR_FRONT_VERSION);
	fr_put16(o + FR_HELLO_O_MAX_DATA, FR_PROTO_MAX_DATA);
	o[FR_HELLO_O_WHY] = st->why;
	o[FR_HELLO_O_FLAGS] = flags(st);
	if (st->own_desc) {
		put_version(o + FR_HELLO_O_VERSION, st->desc.bl_version);
		fr_copy(o + FR_HELLO_O_PIN, st->desc.pin, FR_SHA256_LEN);
	}
	reply_end(p, FR_HELLO_LEN);
}

static void info(struct fr_proto *p, uint8_t seq)
{
	const struct fr_rec_status *st = p->st;
	uint8_t *o = reply_begin(p, FR_T_INFO, seq);
	unsigned k;

	fr_fill(o, 0, FR_INFO_LEN);
	o[FR_INFO_O_WHY] = st->why;
	o[FR_INFO_O_FLAGS] = flags(st);
	o[FR_INFO_O_RUNNING] = st->running.slot;
	o[FR_INFO_O_SEL_INDEX] = st->sel.index;
	o[FR_INFO_O_SEL_VERIFIED] = st->sel.verified;
	o[FR_INFO_O_TARGET] = st->target;
	o[FR_INFO_O_REFUSAL] = st->refusal;
	o[FR_INFO_O_PHASE] = p->phase;
	fr_put32(o + FR_INFO_O_LEN, p->len);
	fr_put32(o + FR_INFO_O_GOT, p->got);
	if (p->phase == FR_PH_COUNTDOWN)
		o[FR_INFO_O_COUNT] = p->count_left;
	o[FR_INFO_O_ACT] = p->w.act;
	o[FR_INFO_O_REFUSED] = p->refused;
	o[FR_INFO_O_KEEP] = FR_NO_SLOT;
	for (k = 0; k < 4u; k++) {
		const uint8_t a = (uint8_t)(FR_ACT_TRY + k);
		struct fr_act_plan ap;

		fr_rec_act(st, a, true, &ap);
		o[FR_INFO_O_FORCED + k] = ap.status;
		if (a == FR_ACT_TRY)
			o[FR_INFO_O_TIER] = ap.tier;
		if (a == FR_ACT_KEEP && ap.status == FR_WR_OK)
			o[FR_INFO_O_KEEP] = ap.target;
		fr_rec_act(st, a, false, &ap);
		o[FR_INFO_O_ACTS + k] = ap.status;
	}
	if (p->phase == FR_PH_WRITING || p->phase == FR_PH_DONE)
		fr_put16(o + FR_INFO_O_PROGRESS,
			 (uint16_t)(p->phase == FR_PH_DONE ? 1000u : fr_writer_progress(&p->w)));
	o[FR_INFO_O_RESULT] = p->result;
	o[FR_INFO_O_IMAGE_RC] = p->image_rc;
	o[FR_INFO_O_WRITTEN] = p->phase == FR_PH_DONE ? p->w.target : (uint8_t)FR_NO_SLOT;
	o[FR_INFO_O_RUN_WAY] = st->run_way;
	o[FR_INFO_O_RUN_REFUSAL] = st->run_refusal;
	o[FR_INFO_O_WAY] = p->w.way;
	o[FR_INFO_O_BATT_LEN] = st->batt_len;
	fr_copy(o + FR_INFO_O_BATT, st->batt, st->batt_len);
	if (st->own_desc) {
		put_version(o + FR_INFO_O_VERSION, st->desc.bl_version);
		fr_copy(o + FR_INFO_O_PIN, st->desc.pin, FR_SHA256_LEN);
	}
	if (p->phase == FR_PH_DONE)
		fr_copy(o + FR_INFO_O_READBACK, p->w.readback_sha, FR_SHA256_LEN);
	for (k = 0; k < 2u; k++) {
		const struct fr_slot_info *s = &st->slot[k];
		uint8_t *q = o + FR_INFO_O_SLOT(k);

		q[FR_INFO_S_FLAGS] = (uint8_t)((s->good ? FR_SLOT_F_GOOD : 0u) |
					       (s->has_desc ? FR_SLOT_F_HAS_DESC : 0u) |
					       (s->has_front ? FR_SLOT_F_HAS_FRONT : 0u) |
					       (s->tried ? FR_SLOT_F_TRIED : 0u) |
					       (s->started ? FR_SLOT_F_STARTED : 0u));
		fr_put32(q + FR_INFO_S_LENGTH, s->rec.length);
		fr_put32(q + FR_INFO_S_CRC, s->rec.crc32);
		if (s->has_desc) {
			put_version(q + FR_INFO_S_VERSION, s->desc.bl_version);
			fr_put32(q + FR_INFO_S_FRONT_CRC, s->desc.front_crc);
			fr_copy(q + FR_INFO_S_PIN, s->desc.pin, FR_SHA256_LEN);
		}
	}
	reply_end(p, FR_INFO_LEN);
}

/* S-02.093: a transfer ends with no write. Once this recovery has made a
 * write, the phase, status, slot written, read-back and way stay that
 * write's, and the refusal is in the request's reply alone; before, the
 * phase is failed with the refusal. */
static void no_write(struct fr_proto *p, uint8_t refusal, uint8_t image_rc)
{
	if (!p->made) {
		p->result = refusal;
		p->image_rc = image_rc;
		p->phase = FR_PH_FAILED;
		return;
	}
	p->refused = refusal;
	p->phase = p->made_phase;
	p->result = p->made_result;
	p->image_rc = FR_IMG_OK;
	p->flags = p->made_flags;
	p->w.target = p->made_target;
	p->w.way = p->made_way;
	p->w.act = p->made_act;
	p->w.result = p->made_result;
	p->w.state = FR_WS_DONE;
	fr_copy(p->w.readback_sha, p->made_sha, FR_SHA256_LEN);
}

static uint8_t begin(struct fr_proto *p, const uint8_t *pl, size_t n)
{
	const struct fr_rec_status *st = p->st;
	struct fr_plan plan;
	uint8_t want = 0, mode;
	uint32_t len;

	if (busy(p))
		return FR_WR_BUSY;
	/* S-02.093: the flags byte is optional, with at most one of its
	 * three low bits set, bit 7 or not, and the others 0. */
	if (n == 4u + FR_SHA256_LEN + 1u)
		want = pl[4u + FR_SHA256_LEN];
	else if (n != 4u + FR_SHA256_LEN)
		return FR_ST_BAD_REQUEST;
	mode = want & (uint8_t)~FR_BEGIN_F_FORCE;
	if ((mode & (uint8_t)~FR_BEGIN_F_ALL) != 0u || (mode & (mode - 1u)) != 0u)
		return FR_ST_BAD_REQUEST;
	len = fr_le32(pl);
	/* The transfer before, accepted or not, is dropped. */
	p->len = p->got = 0;
	p->flags = want;
	p->image_rc = FR_IMG_OK;
	p->changes++;
	plan_of(st, want, &plan);
	if (plan.refusal != FR_WR_OK) {
		no_write(p, plan.refusal, FR_IMG_OK);
		return plan.refusal;
	}
	if (len == 0 || len > FR_LOG_IMAGE_MAX) {	/* S-02.084 */
		no_write(p, FR_WR_SIZE, FR_IMG_SIZE);
		return FR_WR_SIZE;
	}
	/* S-02.084: the battery asked once, for INFO, before a BEGIN for an
	 * in-place write is accepted. */
	if (plan.way == FR_WAY_IN_PLACE && p->battery != NULL)
		p->battery(p->battery_ctx);
	p->len = len;
	fr_copy(p->sha, pl + 4, FR_SHA256_LEN);
	/* What END will do, or refuse: a trial where one may go; whether the
	 * image's front differs is known at END. */
	p->plan_way = (want & FR_BEGIN_F_TRIAL) && plan.trial_ok ? (uint8_t)FR_WAY_TRIAL : plan.way;
	p->plan_target = plan.target;
	p->result = FR_WR_OK;
	p->phase = FR_PH_RECEIVING;
	return FR_WR_OK;
}

static void data(struct fr_proto *p, uint8_t seq, const uint8_t *pl, size_t n)
{
	uint8_t st = FR_WR_OK;
	uint8_t *o;

	if (p->phase != FR_PH_RECEIVING) {
		st = busy(p) ? (uint8_t)FR_WR_BUSY : (uint8_t)FR_ST_STATE;
	} else if (n <= 4u || n - 4u > FR_PROTO_MAX_DATA) {
		st = FR_ST_BAD_REQUEST;	/* 1 to 1024 image bytes (S-02.093) */
	} else {
		uint32_t off = fr_le32(pl), k = (uint32_t)(n - 4u);

		if (off == p->got) {
			if (k > p->len - p->got) {
				st = FR_ST_LENGTH;
			} else {
				fr_copy(p->staging + off, pl + 4, k);
				p->got += k;
				p->changes++;
			}
		} else if (!(off < p->got && k <= p->got - off &&
			     fr_same(p->staging + off, pl + 4, k))) {
			/* not a repeat of a chunk already taken */
			st = FR_ST_OFFSET;
		}
	}
	o = reply_begin(p, FR_T_DATA, seq);
	o[0] = st;
	fr_put32(o + 1, p->got);
	reply_end(p, FR_DATA_REPLY_LEN);
}

/*
 * S-02.084: while a write of this recovery has left the running slot bad,
 * and after any read that failed, what st holds is read again from the
 * flash before a request is decided by it, so that no stale state keeps a
 * write refused or the power-off held. Not during a write.
 */
static void refresh(struct fr_proto *p)
{
	if (!busy(p) && (p->st->read_error || fr_rec_rescue(p->st))) {
		survey(p->st, p->flash, p->idle, p->idle_ctx);
		p->changes++;
	}
}

/* The write the writer took is the one BEGIN announced (S-02.093). A
 * trial announced for an image that carries the running front is not: that
 * image would be written and selected at once. */
static bool announced(const struct fr_proto *p)
{
	return p->w.target == p->plan_target && p->w.way == p->plan_way;
}

/*
 * S-02.084: a write or an action is accepted. One a host asked for waits
 * out the countdown before its first erase or program, but in the damaged
 * state, where the power-off is held already and the unit must stay on:
 * there, and for an action confirmed on the unit, it starts at once.
 */
static void accept(struct fr_proto *p, bool local)
{
	p->refused = FR_WR_OK;
	p->local = local;
	p->stepping = false;
	if (local || damaged(p->st)) {
		p->phase = FR_PH_WRITING;
		return;
	}
	p->phase = FR_PH_COUNTDOWN;
	p->count_end = p->now + FR_COUNTDOWN_US;
	p->count_left = (uint8_t)(FR_COUNTDOWN_US / 1000000u);
}

/* An action begun: what the phases report of it. */
static uint8_t act_begin(struct fr_proto *p, uint8_t act, bool force, bool local,
			 struct fr_act_plan *plan)
{
	struct fr_rec_status *st = p->st;
	const int rc = fr_act_begin(&p->w, p->flash, &st->running, &st->home, act, force, plan);

	p->changes++;
	if (rc != FR_WR_OK)
		return (uint8_t)rc;
	p->len = p->got = 0;
	p->flags = force ? FR_BEGIN_F_FORCE : 0u;
	p->plan_way = plan->way;
	p->plan_target = plan->target;
	p->tier = plan->tier;
	p->result = FR_WR_OK;
	p->image_rc = FR_IMG_OK;
	p->has_version = plan->target < 2u && st->slot[plan->target].has_desc;
	if (p->has_version)
		fr_copy(p->version, st->slot[plan->target].desc.bl_version, sizeof p->version);
	accept(p, local);
	return FR_WR_OK;
}

uint8_t fr_proto_local(struct fr_proto *p, uint8_t act)
{
	struct fr_act_plan plan;

	if (busy(p) || p->phase == FR_PH_RECEIVING)
		return FR_WR_BUSY;
	return act_begin(p, act, true, true, &plan);
}

/* S-02.093: ACT {enum fr_act, flags}; only bit 7 of the flags is defined. */
static void act(struct fr_proto *p, uint8_t seq, const uint8_t *pl, size_t n)
{
	struct fr_act_plan plan;
	uint8_t st;
	uint8_t *o;

	fr_fill(&plan, 0, sizeof plan);
	plan.target = FR_NO_SLOT;
	if (busy(p)) {
		st = FR_WR_BUSY;
	} else if (n != 2u || pl[0] < FR_ACT_TRY || pl[0] > FR_ACT_PUT_BACK ||
		   (pl[1] & (uint8_t)~FR_BEGIN_F_FORCE) != 0u) {
		st = FR_ST_BAD_REQUEST;
	} else {
		st = act_begin(p, pl[0], (pl[1] & FR_BEGIN_F_FORCE) != 0u, false, &plan);
		if (st != FR_WR_OK) {
			/* the state the refusal was read from is what holds now */
			survey(p->st, p->flash, p->idle, p->idle_ctx);
			no_write(p, st, FR_IMG_OK);
		}
	}
	o = reply_begin(p, FR_T_ACT, seq);
	o[0] = st;
	o[1] = st == FR_WR_OK ? plan.way : (uint8_t)FR_WAY_NONE;
	o[2] = st == FR_WR_OK ? plan.target : (uint8_t)FR_NO_SLOT;
	o[3] = plan.tier;
	reply_end(p, FR_ACT_REPLY_LEN);
}

static void end(struct fr_proto *p, uint8_t seq, size_t n)
{
	uint8_t st, img_rc = p->image_rc;
	uint8_t *o;

	if (busy(p)) {
		st = FR_WR_BUSY;
	} else if (p->phase != FR_PH_RECEIVING) {
		st = FR_ST_STATE;
	} else if (n != 0) {
		st = FR_ST_BAD_REQUEST;
	} else if (p->got != p->len) {
		st = FR_ST_LENGTH;
	} else {
		/* Every refusal, the start condition read again from the flash
		 * included, is the writer's (S-02.090, S-02.091). */
		int rc = fr_writer_begin(&p->w, p->flash, &p->st->running, &p->st->home,
					 p->staging, p->len, p->sha, p->flags);

		/* S-02.093: only the write BEGIN announced; no step is made
		 * yet, so nothing is written. */
		if (rc == FR_WR_OK && !announced(p)) {
			fr_writer_cancel(&p->w);
			survey(p->st, p->flash, p->idle, p->idle_ctx);
			rc = FR_WR_CHANGED;
		}
		st = (uint8_t)rc;
		img_rc = rc == FR_WR_IMAGE ? p->w.image_rc : (uint8_t)FR_IMG_OK;
		if (rc == FR_WR_OK) {
			p->result = st;
			p->image_rc = img_rc;
			p->tier = FR_TIER_NONE;
			p->has_version = p->w.has_version;
			fr_copy(p->version, p->w.version, sizeof p->version);
			accept(p, false);
		} else {
			no_write(p, st, img_rc);
		}
		p->stepping = false;
		p->changes++;
	}
	o = reply_begin(p, FR_T_END, seq);
	o[0] = st;
	o[1] = img_rc;
	reply_end(p, FR_END_REPLY_LEN);
}

static void handle(struct fr_proto *p, uint8_t type, uint8_t seq, const uint8_t *pl, size_t n)
{
	switch (type) {
	case FR_T_HELLO:
		hello(p, seq);
		break;
	case FR_T_INFO:
		refresh(p);
		info(p, seq);
		break;
	case FR_T_BEGIN: {
		uint8_t st;

		refresh(p);
		st = begin(p, pl, n);
		uint8_t *o = reply_begin(p, type, seq);

		o[0] = st;
		o[1] = st == FR_WR_OK ? p->plan_way : (uint8_t)FR_WAY_NONE;
		o[2] = st == FR_WR_OK ? p->plan_target : (uint8_t)FR_NO_SLOT;
		reply_end(p, FR_BEGIN_REPLY_LEN);
		break;
	}
	case FR_T_DATA:
		data(p, seq, pl, n);
		break;
	case FR_T_END:
		end(p, seq, n);
		break;
	case FR_T_ACT:
		refresh(p);
		act(p, seq, pl, n);
		break;
	case FR_T_RESTART:
		refresh(p);
		if (busy(p)) {
			status_reply(p, type, seq, FR_WR_BUSY);
		} else if (n > 1u || (n == 1u && (pl[0] & (uint8_t)~FR_BEGIN_F_FORCE) != 0u)) {
			status_reply(p, type, seq, FR_ST_BAD_REQUEST);
		} else if (damaged(p->st) && !(n == 1u && (pl[0] & FR_BEGIN_F_FORCE) != 0u)) {
			/* S-02.084: no restart would start a good slot. */
			status_reply(p, type, seq, FR_ST_DAMAGED);
		} else {
			status_reply(p, type, seq, FR_WR_OK);
			p->restart_due = true;
			p->changes++;
		}
		break;
	default:
		status_reply(p, type, seq, FR_ST_BAD_REQUEST);
		break;
	}
}

static void drop(struct fr_proto *p, size_t k)
{
	fr_copy(p->rx, p->rx + k, p->rx_n - k);
	p->rx_n -= k;
}

void fr_proto_drop(struct fr_proto *p)
{
	p->rx_n = 0;
	/* S-02.084: the host left during the countdown: nothing is written.
	 * A transfer it left half sent ends too, so that the keys act again. */
	if (p->phase == FR_PH_COUNTDOWN || p->phase == FR_PH_RECEIVING) {
		if (p->phase == FR_PH_COUNTDOWN)
			fr_writer_cancel(&p->w);
		no_write(p, FR_WR_STOPPED, FR_IMG_OK);
		p->changes++;
	}
}

void fr_proto_clock(struct fr_proto *p, uint64_t now_us)
{
	p->now = now_us;
	if (p->phase != FR_PH_COUNTDOWN)
		return;
	if (now_us >= p->count_end) {
		p->count_left = 0;
		p->phase = FR_PH_WRITING;
		p->stepping = false;
		p->changes++;
	} else {
		const uint8_t left = (uint8_t)((p->count_end - now_us + 999999u) / 1000000u);

		if (left != p->count_left) {
			p->count_left = left;
			p->changes++;
		}
	}
}

/* The next whole request in rx, answered; false when none is complete.
 * Bad bytes are dropped up to the next "WR". */
static bool next_frame(struct fr_proto *p)
{
	/* bounded: each pass drops at least one byte or returns */
	for (;;) {
		size_t i, len;

		for (i = 0; i < p->rx_n && !(p->rx[i] == 'W' &&
					     (i + 1u == p->rx_n || p->rx[i + 1u] == 'R')); i++)
			;
		if (i)
			drop(p, i);
		if (p->rx_n < FR_PROTO_HEAD)
			return false;
		len = fr_le16(p->rx + 4);
		if (len > FR_PROTO_MAX) {
			drop(p, 1);
			continue;
		}
		if (p->rx_n < FR_PROTO_HEAD + len + 4u)
			return false;
		if (fr_crc32(0, p->rx + 2, 4u + len) != fr_le32(p->rx + FR_PROTO_HEAD + len)) {
			drop(p, 1);
			continue;
		}
		if (!(p->rx[2] & FR_T_REPLY))
			handle(p, p->rx[2], p->rx[3], p->rx + FR_PROTO_HEAD, len);
		drop(p, FR_PROTO_HEAD + len + 4u);
		return true;
	}
}

/* After a write ends: the selector and the target slot as the flash now
 * holds them (one CRC pass, at most FR_SLOT_CAP bytes), then the target.
 * The other slot was not written. */
static void reread(struct fr_proto *p)
{
	struct fr_rec_status *st = p->st;
	unsigned k = p->w.target;

	st->selector_ok = fr_read_selector(p->flash, &st->sel) == FR_FLASH_OK;
	if (!st->selector_ok)
		st->read_error = true;
	if (k < 2u && fr_slot_info(p->flash, k, &st->slot[k]) != FR_FLASH_OK) {
		st->slot[k].good = false;
		st->read_error = true;
	}
	fr_rec_target(st);
}

void fr_proto_poll(struct fr_proto *p)
{
	bool step;

	flush(p);
	/* S-02.093: a write's first step comes in a later call than the one
	 * that accepted it, so that the caller sees the phase first, and only
	 * once END's reply is with the port. */
	step = p->phase == FR_PH_WRITING && (p->stepping || p->tx_n == 0);
	if (p->tx_n == 0 && p->restart_due) {
		p->restart_due = false;
		p->restart = true;
	}
	/* One request at a time: none is read while a reply waits. At most
	 * FR_PROTO_PASSES reads a poll, so a busy port cannot hold the loop. */
	for (unsigned pass = 0; pass < FR_PROTO_PASSES && p->tx_n == 0 &&
	     !p->restart_due && !p->restart; pass++) {
		size_t k = 0;

		if (p->rx_n < sizeof p->rx)
			k = p->io->read(p->io->ctx, p->rx + p->rx_n, sizeof p->rx - p->rx_n);
		p->rx_n += k;
		if (!next_frame(p) && k == 0)
			break;
	}
	if (p->tx_n == 0 && p->restart_due) {
		p->restart_due = false;
		p->restart = true;
	}
	if (step) {
		p->stepping = true;
		if (fr_writer_step(&p->w)) {
			p->result = (uint8_t)p->w.result;
			p->image_rc = FR_IMG_OK;
			p->phase = p->w.result == FR_WR_OK ? (uint8_t)FR_PH_DONE : (uint8_t)FR_PH_FAILED;
			p->made = true;
			p->made_phase = p->phase;
			p->made_result = p->result;
			p->made_flags = p->flags;
			p->made_target = p->w.target;
			p->made_way = p->w.way;
			p->made_act = p->w.act;
			fr_copy(p->made_sha, p->w.readback_sha, FR_SHA256_LEN);
			reread(p);
		}
		p->changes++;
	}
}
