/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery's screens (S-15.151). The main one: each slot's state and
 * version, which runs and which is selected, how to write an image and the
 * way to the slot menu, then a transfer's progress and the result, and why
 * recovery runs when no key asked for it. The slot menu and the confirm
 * screen (fr_recui.c), and the countdown before a host's write. Every line
 * says what to do where there is something to do, and every key's digit is
 * a key badge. Wording and layout are free; the lines carry fr_draw.h's
 * control bytes.
 */
#include "fr_start.h"

static void put(char *line, size_t *at, const char *t)
{
	while (*t != '\0' && *at < FR_LINE_MAX)
		line[(*at)++] = *t++;
	line[*at] = '\0';
}

static size_t text_len(const char *t)
{
	size_t n = 0;

	while (t[n] != '\0')
		n++;
	return n;
}

/* The parts of a slot line, each whole or not at all. */
static void part(char *line, size_t *at, const char *a, const char *b, const char *c)
{
	if (*at + text_len(a) + text_len(b) + text_len(c) > FR_LINE_MAX)
		return;
	put(line, at, a);
	put(line, at, b);
	put(line, at, c);
}

/*
 * "Slot 1 [WB] v0.9.36 [checked] [running] [boot slot]" (S-15's
 * numbering): the slot's name and its version in the title bar's font;
 * the kind (S-02.106) in a filled chip; the state, running and the boot
 * slot (the selected one) in outlined chips. A slot under a write shows
 * that in place of its kind, version and state, which the write changes.
 */
static void slot_line(struct fr_screen *scr, unsigned row, const struct fr_rec_status *st,
		      unsigned k, bool writing)
{
	const struct fr_slot_info *in = &st->slot[k];
	char line[FR_LINE_MAX + 1];
	const uint16_t *ver = NULL;
	const char *kind = NULL;
	size_t at = 0;

	line[0] = '\0';
	put(line, &at, k == 0u ? FR_C_TITLE "Slot 1" : FR_C_TITLE "Slot 2");
	if (!writing) {
		switch (in->kind) {
		case FR_KIND_WB:
			kind = "WB";
			ver = in->desc.bl_version;
			break;
		case FR_KIND_TE_BUILD:
			kind = "TE";
			ver = in->te_version;
			break;
		case FR_KIND_TE:
			kind = "TE";
			break;
		case FR_KIND_UNKNOWN:
			kind = "??";
			break;
		default:
			break;
		}
	}
	if (kind != NULL)
		part(line, &at, " " FR_C_CHIP, kind, FR_C_END);
	if (ver != NULL) {
		char v[FR_VERSION_TEXT_MAX + 1];

		v[fr_text_version(v, ver)] = '\0';
		part(line, &at, " ", v, "");
	}
	put(line, &at, FR_C_BODY);
	if (writing)
		part(line, &at, " " FR_C_RING FR_A_WARN, "being written", FR_C_END);
	else if (in->good)
		part(line, &at, " " FR_C_RING FR_A_OK, "checked", FR_C_END);
	else if (in->tried)
		part(line, &at, " " FR_C_RING FR_A_WARN, in->started ? "tried, started" : "tried",
		     FR_C_END);
	else
		part(line, &at, " " FR_C_RING FR_A_ERROR, "check failed", FR_C_END);
	if (st->running.established && st->running.slot == k)
		part(line, &at, " " FR_C_RING FR_A_WARN, "running", FR_C_END);
	if (st->selector_ok && fr_selected_slot(st->sel.index) == k)
		part(line, &at, " " FR_C_RING FR_A_TEXT, "boot slot", FR_C_END);
	fr_screen_line(scr, row, FR_INK_TEXT, line);
}

/* A refusal, as what to do next. after: this recovery has made a write. */
static const char *refusal(unsigned rc, bool after)
{
	switch (rc) {
	case FR_WR_NOT_RUNNING:
		return "No write: the unit cannot tell which slot is running";
	case FR_WR_NOT_SELECTED:
	case FR_WR_UNVERIFIED:
		return after ? "Restart before another write" :
			       "No write yet: restart the unit once first";
	case FR_WR_SELECTED_BAD:
		return "No write: the boot slot fails its check; change it first";
	case FR_WR_SIZE:
		return "Failed: the image is too large for a slot";
	case FR_WR_IMAGE:
		return "Failed: this is not an image the unit can start";
	case FR_WR_HASH:
		return "Failed: the image arrived damaged; send it again";
	case FR_WR_FLASH:
		return "Failed: the flash did not take it; write again";
	case FR_WR_READBACK:
		return "Failed: the slot did not read back right; write again";
	case FR_WR_SELECTOR:
		return "Failed: the selector write failed; write again";
	case FR_WR_FRONT:
		return "Failed: another front; write this image as a trial";
	case FR_WR_SELECT_FIRST:
		return "Failed: the selector write failed; no slot was written";
	case FR_WR_TRY_OTHER:
		return "No write: write the other slot first";
	case FR_WR_CHANGED:
		return "Failed: the unit's state changed; send the image again";
	case FR_WR_READ:
		return "No write: the flash could not be read; ask again";
	case FR_WR_ONLY_GOOD:
		return "No write: send the image the checked slot holds";
	case FR_WR_TRIAL_STATE:
		return "Failed: no trial or keep now; restart the unit first";
	case FR_WR_NOT_TRIED:
		return "Failed: no slot holds a tried image to keep";
	case FR_WR_NOT_STARTED:
		return "Failed: the tried slot has not started; restart first";
	case FR_WR_ALREADY:
		return "Nothing to do: that slot is the boot slot already";
	case FR_WR_TARGET_BAD:
		return "Failed: that slot failed its check; write it first";
	case FR_WR_STOPPED:
		return "Stopped: the computer left; nothing was written";
	case FR_WR_BUSY:
		return "Busy: wait for the write to end";
	default:
		return "No write";
	}
}

/* text, then the slot's shown number, then tail. */
static void slot_text(char *line, const char *text, unsigned k, const char *tail)
{
	size_t at = 0;

	line[0] = '\0';
	put(line, &at, text);
	put(line, &at, k == 0u ? "1" : "2");
	put(line, &at, tail);
}

/* This recovery began a write of the running slot, and the slot is bad. */
static bool damaged(const struct fr_rec_status *st)
{
	return st->home.spoiled && st->running.slot < 2u && !st->slot[st->running.slot].good;
}

/* S-02.093: a trial start that fails is left by a power cycle, which
 * boots slot run again. */
static void off_on_row(struct fr_screen *scr, unsigned row, unsigned run)
{
	char line[FR_LINE_MAX + 1];

	slot_text(line, "If it does not start, switch off and on: slot ", run, " boots");
	fr_screen_line(scr, row, FR_INK_WARN, line);
}

/* "v1.0.0", or "an image" without a version. */
static void put_version(char *line, size_t *at, const uint16_t *version)
{
	char v[FR_VERSION_TEXT_MAX + 1];

	if (version == NULL) {
		put(line, at, "an image");
		return;
	}
	v[fr_text_version(v, version)] = '\0';
	put(line, at, v);
}

/* The warning of a try of, or a switch to, a slot of this tier: two rows. */
static void tier_rows(struct fr_screen *scr, unsigned row, unsigned tier, bool sw, unsigned run)
{
	switch (tier) {
	case FR_TIER_SAME:
		if (sw)
			fr_screen_line(scr, row, FR_INK_DIM, "A trial first costs one restart, and is safer");
		break;
	case FR_TIER_FRONT:
		fr_screen_line(scr, row, FR_INK_WARN,
			       "It has another front: recovery may not open from it");
		break;
	case FR_TIER_FOREIGN:
		fr_screen_line(scr, row, FR_INK_ERROR,
			       "It is not a waffle-boot image: no recovery from there");
		break;
	case FR_TIER_BAD:
		fr_screen_line(scr, row, FR_INK_ERROR, sw ?
			       "Its check failed: nothing will ever check it again" :
			       "Its check failed: it may not start");
		break;
	default:
		break;
	}
	if (!sw)
		off_on_row(scr, row + 1u, run);
	else if (tier == FR_TIER_BAD)
		fr_screen_line(scr, row + 1u, FR_INK_ERROR,
			       "There may be no way back without a flash programmer");
	else if (tier == FR_TIER_FRONT || tier == FR_TIER_FOREIGN)
		fr_screen_line(scr, row + 1u, FR_INK_WARN, "Try it once first: " FR_C_BADGE "8" " then " FR_C_BADGE "1");
}

/* What a write or an action will do, in plain words, from row on, with its
 * warning: before it starts. version is the image's or the slot's, NULL
 * for none. */
static void what_rows(struct fr_screen *scr, unsigned row, const uint16_t *version, bool forced,
		      unsigned way, unsigned t, unsigned tier, unsigned run)
{
	char line[FR_LINE_MAX + 1];
	size_t at = 0;

	line[0] = '\0';
	switch (way) {
	case FR_WAY_IN_PLACE:
		slot_text(line, "Slot ", t, ", the running slot, will be written in place");
		fr_screen_line(scr, row, FR_INK_WARN, line);
		at = 0;
		line[0] = '\0';
		put(line, &at, "with ");
		put_version(line, &at, version);
		put(line, &at, ": no slot to fall back on");
		fr_screen_line(scr, row + 1u, FR_INK_ERROR, line);
		break;
	case FR_WAY_PLAIN:
	case FR_WAY_SELECT_FIRST:
	case FR_WAY_TRIAL:
		put(line, &at, t == 0u ? "Slot 1" : "Slot 2");
		put(line, &at, " will be erased and written with ");
		put_version(line, &at, version);
		fr_screen_line(scr, row, FR_INK_WARN, line);
		if (way == FR_WAY_TRIAL) {
			fr_screen_line(scr, row + 1u, FR_INK_TEXT,
				       "and started once, as a trial, at the next start");
			off_on_row(scr, row + 2u, run);
		} else if (t == run) {
			if (way == FR_WAY_SELECT_FIRST)
				slot_text(line, "It is running; slot ", 1u - t,
					  " becomes the boot slot first");
			else
				slot_text(line, "It is the running slot; slot ", 1u - t,
					  " stays the boot slot");
			fr_screen_line(scr, row + 1u, FR_INK_TEXT, line);
		} else {
			fr_screen_line(scr, row + 1u, FR_INK_TEXT, "and is the boot slot");
		}
		if (forced && way != FR_WAY_TRIAL)
			fr_screen_line(scr, row + 2u, FR_INK_ERROR,
				       "Forced: the unit's own refusals are off");
		break;
	case FR_WAY_TRY:
		put(line, &at, t == 0u ? "Slot 1 (" : "Slot 2 (");
		put_version(line, &at, version);
		put(line, &at, ") will start once, as a trial");
		fr_screen_line(scr, row, FR_INK_HEAD, line);
		tier_rows(scr, row + 1u, tier, false, run);
		break;
	case FR_WAY_SWITCH:
		put(line, &at, t == 0u ? "Slot 1 (" : "Slot 2 (");
		put_version(line, &at, version);
		put(line, &at, ") will be the boot slot");
		fr_screen_line(scr, row, FR_INK_HEAD, line);
		tier_rows(scr, row + 1u, tier, true, run);
		break;
	case FR_WAY_SELECT:
	case FR_WAY_KEEP:
		slot_text(line, "Slot ", t, ", which was tried, will be kept:");
		fr_screen_line(scr, row, FR_INK_HEAD, line);
		fr_screen_line(scr, row + 1u, FR_INK_TEXT, "it will be the boot slot");
		break;
	case FR_WAY_PUT_BACK:
		slot_text(line, "Slot ", t, "'s record will be put back:");
		fr_screen_line(scr, row, FR_INK_HEAD, line);
		fr_screen_line(scr, row + 1u, FR_INK_TEXT,
			       "it reads checked again; the unit starts as it does now");
		break;
	default:
		break;
	}
}

/* A bar on row, filled pm of 1000. */
static void set_bar(struct fr_screen *scr, unsigned row, unsigned pm)
{
	scr->bar_line = (int8_t)row;
	scr->bar_col = 0;
	scr->bar_cols = 40;
	scr->bar_pm = (uint16_t)(pm > 1000u ? 1000u : pm);
}

/* The slot menu (rows 2 to 8): what can be done with the slot that is not
 * running, or with the running one in its trial start. */
static void menu_rows(struct fr_screen *scr, const struct fr_rec_status *st,
		      const struct fr_ui *ui, unsigned run)
{
	const unsigned b = 1u - run;
	struct fr_act_plan keep, sw;
	char line[FR_LINE_MAX + 1];
	unsigned row = 3;

	if (!st->running.established) {
		fr_screen_line(scr, 3, FR_INK_WARN, refusal(FR_WR_NOT_RUNNING, false));
		fr_screen_line(scr, 8, FR_INK_DIM, FR_C_BADGE "8" "  Back");
		return;
	}
	fr_rec_act(st, FR_ACT_KEEP, true, &keep);
	fr_rec_act(st, FR_ACT_SWITCH, true, &sw);
	if (fr_ui_offers(st, FR_ACT_TRY)) {
		slot_text(line, FR_C_BADGE "1" "  Try slot ", b, " once (suggested)");
		fr_screen_line(scr, row++, FR_INK_TEXT, line);
	}
	if (fr_ui_offers(st, FR_ACT_SWITCH)) {
		slot_text(line, FR_C_BADGE "2" "  Make slot ", b, " the boot slot");
		fr_screen_line(scr, row++, FR_INK_TEXT, line);
	}
	if (fr_ui_offers(st, FR_ACT_KEEP)) {
		slot_text(line, FR_C_BADGE "3" "  Keep slot ", keep.target,
			  keep.target == run ? ", the running slot" : "");
		fr_screen_line(scr, row++, FR_INK_TEXT, line);
	}
	if (fr_ui_offers(st, FR_ACT_PUT_BACK)) {
		slot_text(line, FR_C_BADGE "4" "  Put slot ", b, "'s record back");
		fr_screen_line(scr, row++, FR_INK_TEXT, line);
	}
	if (ui->refusal != FR_WR_OK) {
		fr_screen_line(scr, 7, FR_INK_ERROR, refusal(ui->refusal, false));
	} else if (sw.status == FR_WR_ALREADY) {
		slot_text(line, "Slot ", b, " is the boot slot: it starts on every start");
		fr_screen_line(scr, 7, FR_INK_DIM, line);
	} else if (sw.status == FR_WR_IMAGE) {
		slot_text(line, "Slot ", b, " holds nothing the unit can start");
		fr_screen_line(scr, 7, FR_INK_DIM, line);
	} else if (st->selector_ok && fr_selected_slot(st->sel.index) == b) {
		slot_text(line, "Slot ", b, " starts once at the next start");
		fr_screen_line(scr, 7, FR_INK_DIM, line);
	} else if (row == 3u) {
		fr_screen_line(scr, 3, FR_INK_DIM, "Nothing to choose now");
	}
	fr_screen_line(scr, 8, FR_INK_DIM, FR_C_BADGE "8" "  Back");
}

/* The right status text: the running image's version and the front's,
 * "v1.0.0 F11", or the front's alone. */
static void status(struct fr_screen *scr, const struct fr_rec_status *st, bool usb)
{
	char line[FR_STATUS_MAX + 1], n[12];
	size_t at = 0;

	if (st->own_desc) {
		at = fr_text_version(line, st->desc.bl_version);
		line[at++] = ' ';
	}
	line[at++] = 'F';
	n[fr_text_dec(n, FR_FRONT_VERSION)] = '\0';
	for (const char *t = n; *t != '\0'; t++)
		line[at++] = *t;
	line[at] = '\0';
	fr_screen_status(scr, usb ? "USB" : "", line);
}

void fr_rec_screen(struct fr_screen *scr, const struct fr_rec_status *st,
		   const struct fr_proto *p, bool usb, const struct fr_ui *ui)
{
	const unsigned run = st->running.slot < 2u ? st->running.slot : 0u;
	const unsigned phase = p != NULL ? p->phase : FR_PH_IDLE;
	const bool quiet = phase == FR_PH_IDLE || phase == FR_PH_DONE || phase == FR_PH_FAILED;
	const unsigned screen = ui != NULL && quiet ? ui->screen : FR_UI_MAIN;
	const bool after = p != NULL && p->made;
	/* the slot a write changes, while it runs */
	const unsigned under = phase == FR_PH_WRITING && p->w.act == FR_ACT_NONE ? p->w.target : 2u;
	char line[FR_LINE_MAX + 1], n[12];
	size_t at = 0;
	unsigned k, refused = FR_WR_OK;

	fr_screen_text(scr, screen == FR_UI_SLOTS ? "Recovery: slots" :
			    screen == FR_UI_CONFIRM ? "Recovery: confirm" : "Recovery");
	slot_line(scr, 0, st, 0, under == 0u);
	slot_line(scr, 1, st, 1, under == 1u);
	status(scr, st, usb);
	if (screen == FR_UI_SLOTS) {
		menu_rows(scr, st, ui, run);
		return;
	}
	if (screen == FR_UI_CONFIRM) {
		struct fr_act_plan plan;

		fr_rec_act(st, ui->act, true, &plan);
		what_rows(scr, 2, plan.target < 2u && st->slot[plan.target].has_desc ?
				  st->slot[plan.target].desc.bl_version : NULL,
			  false, plan.way, plan.target, plan.tier, run);
		fr_screen_line(scr, 6, FR_INK_HEAD, "Hold " FR_C_BADGE "1" " for 5 s to confirm");
		set_bar(scr, 7, ui->hold_pct * 10u);
		fr_screen_line(scr, 8, FR_INK_DIM, FR_C_BADGE "8" "  Back");
		return;
	}
	if (phase == FR_PH_COUNTDOWN) {
		/* S-02.084: what is about to be written, and how to stop it */
		what_rows(scr, 2, p->has_version ? p->version : NULL,
			  (p->flags & FR_BEGIN_F_FORCE) != 0u, p->w.way, p->w.target, p->tier, run);
		line[0] = '\0';
		put(line, &at, "Switch the unit off within ");
		n[fr_text_dec(n, p->count_left)] = '\0';
		put(line, &at, n);
		put(line, &at, " s to stop");
		fr_screen_line(scr, 6, FR_INK_HEAD, line);
		set_bar(scr, 7, 1000u - p->count_left * 1000u / (FR_COUNTDOWN_US / 1000000u));
		return;
	}
	if (quiet && phase != FR_PH_DONE) {
		/* how to write an image, or why none can be written */
		if (damaged(st) && st->target < 2u && st->target != run && st->slot[st->target].good)
			/* the only good slot is not replaced (S-02.084): row 8
			 * says what a start does */
			line[0] = '\0';
		else if (st->target == FR_NO_SLOT && !(st->select_target == run &&
						       st->select_refusal == FR_WR_OK)) {
			/* as a start's refusal: a good running slot with no front */
			fr_screen_line(scr, 3, FR_INK_WARN, st->refusal == FR_WR_FRONT ?
					  "No write: the running slot holds no waffle-boot front" :
					  refusal(st->refusal, after));
			refused = st->refusal;
		} else if (!usb)
			fr_screen_line(scr, 3, FR_INK_HEAD, "Connect to a computer to write an image");
		else if (st->target != FR_NO_SLOT) {
			slot_text(line, "Connected: a write from the computer replaces slot ",
				  st->target, "");
			fr_screen_line(scr, 3, FR_INK_HEAD, line);
		} else
			fr_screen_line(scr, 3, FR_INK_HEAD, "Connected: waiting for the computer");
	}
	/* a request refused since the last write: what to do first */
	if (phase == FR_PH_DONE && p->refused != FR_WR_OK) {
		fr_screen_line(scr, 4, FR_INK_WARN, refusal(p->refused, true));
		refused = p->refused;
	}
	/* the way to the slot menu; not optional where the boot slot fails
	 * its check and so no write is taken */
	if (quiet)
		fr_screen_line(scr, 5, FR_INK_TEXT, refused == FR_WR_SELECTED_BAD ?
				  FR_C_BADGE "1" "  Change the boot slot first" :
				  FR_C_BADGE "1" "  Change the boot slot (optional)");
	switch (phase) {
	case FR_PH_RECEIVING:
		/* the slot this transfer was begun for (S-02.093's flags) */
		k = p->plan_target < 2u ? p->plan_target : run;
		if (p->plan_way == FR_WAY_TRIAL)
			fr_screen_line(scr, 3, FR_INK_WARN,
				       "A trial was asked for: the image is started once");
		else if (p->plan_way == FR_WAY_SELECT)
			fr_screen_line(scr, 3, FR_INK_HEAD, k == 0u ?
				       "Slot 1, which was tried, is to be kept" :
				       "Slot 2, which was tried, is to be kept");
		line[0] = '\0';
		put(line, &at, "Receiving ");
		n[fr_text_dec(n, p->got / 1024u)] = '\0';
		put(line, &at, n);
		put(line, &at, " of ");
		n[fr_text_dec(n, p->len / 1024u)] = '\0';
		put(line, &at, n);
		put(line, &at, k == 0u ? " KiB for slot 1" : " KiB for slot 2");
		fr_screen_line(scr, 6, FR_INK_TEXT, line);
		set_bar(scr, 7, p->len != 0u ? (unsigned)((uint64_t)p->got * 1000u / p->len) : 0u);
		break;
	case FR_PH_WRITING:
		if (p->w.way == FR_WAY_IN_PLACE) {
			/* drawn once, before the first erase: no bar */
			slot_text(line, "Writing slot ", p->w.target, " in place: keep the power on");
			fr_screen_line(scr, 6, FR_INK_WARN, line);
			fr_screen_line(scr, 7, FR_INK_ERROR,
				       "A power cut now leaves no image that starts");
			break;
		}
		slot_text(line, p->w.act != FR_ACT_NONE ? "Working on slot " : "Writing slot ",
			  p->w.target, ": keep the power on");
		fr_screen_line(scr, 6, FR_INK_WARN, line);
		set_bar(scr, 7, fr_writer_progress(&p->w));
		break;
	case FR_PH_DONE:
		switch (p->w.way) {
		case FR_WAY_TRIAL:
			/* the next start boots it once (S-02.023) */
			slot_text(line, "Done: slot ", p->w.target, " is written for one trial start");
			break;
		case FR_WAY_TRY:
			slot_text(line, "Done: slot ", p->w.target, " starts once, as a trial");
			break;
		case FR_WAY_SWITCH:
			slot_text(line, "Done: slot ", p->w.target, " is the boot slot");
			break;
		case FR_WAY_SELECT:
		case FR_WAY_KEEP:
			slot_text(line, "Done: slot ", p->w.target, " is kept as the boot slot");
			break;
		case FR_WAY_PUT_BACK:
			slot_text(line, "Done: slot ", p->w.target, "'s record is back; it is checked");
			break;
		case FR_WAY_IN_PLACE:
			slot_text(line, "Done: slot ", p->w.target, " is written, and still the boot slot");
			break;
		default:
			slot_text(line, "Done: slot ", p->w.target, " is written and is the boot slot");
			break;
		}
		fr_screen_line(scr, 6, FR_INK_OK, line);
		/* with the running slot damaged, row 8 says what a start does */
		if (p->w.way != FR_WAY_PUT_BACK && !damaged(st))
			fr_screen_line(scr, 7, FR_INK_DIM, p->w.way == FR_WAY_TRIAL ||
					  p->w.way == FR_WAY_TRY ?
					  "Restart to start it once (host or power switch)" :
					  "Restart to start it (host or power switch)");
		if (p->w.way == FR_WAY_TRIAL || p->w.way == FR_WAY_TRY)
			off_on_row(scr, 8, run);
		break;
	case FR_PH_FAILED:
		fr_screen_line(scr, 6, FR_INK_ERROR,
			       p->result == FR_WR_TRIAL_STATE && (p->flags & FR_BEGIN_F_SELECT) ?
			       "Failed: no keep after a write here; restart first" :
			       refusal(p->result, after));
		if (fr_rec_rescue(st))
			break;		/* rows 7 and 8 say what a start does */
		if (p->result != FR_WR_SELECTOR) {
			fr_screen_line(scr, 7, FR_INK_DIM, p->result == FR_WR_SELECT_FIRST ?
					  "No slot was written" :
					  "The unit starts as it did before");
		} else if (st->slot[0].good && st->slot[1].good) {
			fr_screen_line(scr, 7, FR_INK_DIM,
				       "Both slots are checked: the unit boots either one");
		} else {
			/* the slot written read back checked, the other one
			 * failed its check: what the selector, as it reads
			 * now, leads to */
			switch (fr_rec_reach(st)) {
			case FR_REACH_NAMED:
				slot_text(line, "A restart boots slot ",
					  fr_selected_slot(st->sel.index), ", which is checked");
				fr_screen_line(scr, 7, FR_INK_DIM, line);
				break;
			case FR_REACH_FALLBACK:
				/* the fallback is to slot 1, shown as slot 2 */
				fr_screen_line(scr, 7, FR_INK_DIM,
					       "The 2nd restart boots slot 2, which is checked");
				break;
			default:
				fr_screen_line(scr, 7, FR_INK_ERROR,
					       "No checked slot would start: write again");
				break;
			}
		}
		if (p->result == FR_WR_FRONT)
			fr_screen_line(scr, 8, FR_INK_DIM, "A new front is taken only for a trial start");
		break;
	case FR_PH_IDLE:
	default:
		if (st->select_target == run && st->select_refusal == FR_WR_OK) {
			/* a trial start, in the tried slot's own recovery */
			slot_text(line, "Trial start of slot ", run,
				  ": to keep it, " FR_C_BADGE "1" " then " FR_C_BADGE "3");
			fr_screen_line(scr, 6, FR_INK_HEAD, line);
		}
		break;
	}
	/* A write of the running slot that did not end well: what a start
	 * reaches now, whatever is asked next. */
	if (phase != FR_PH_WRITING) {
		if (damaged(st)) {
			switch (fr_rec_reach(st)) {
			case FR_REACH_NAMED:
				slot_text(line, "Slot ", run,
					  " is damaged: a restart boots the other slot");
				fr_screen_line(scr, 8, FR_INK_WARN, line);
				break;
			case FR_REACH_FALLBACK:
				/* the first start boots the damaged slot once */
				slot_text(line, "Slot ", run,
					  " is damaged: the 2nd restart boots the other slot");
				fr_screen_line(scr, 8, FR_INK_WARN, line);
				break;
			default:
				slot_text(line, "Slot ", run,
					  " is damaged: write again before a power-off");
				fr_screen_line(scr, 8, FR_INK_ERROR, line);
				break;
			}
		} else if (fr_rec_rescue(st)) {
			/* written and read back checked, the selector not
			 * settled, the other slot failed */
			switch (fr_rec_reach(st)) {
			case FR_REACH_NAMED:
				slot_text(line, "Slot ", run, " is written: a restart boots it");
				fr_screen_line(scr, 8, FR_INK_WARN, line);
				break;
			case FR_REACH_FALLBACK:
				slot_text(line, "Slot ", run, " is written: the 2nd restart boots it");
				fr_screen_line(scr, 8, FR_INK_WARN, line);
				break;
			default:
				slot_text(line, "Slot ", run,
					  " is written, not yet the boot slot: write it again");
				fr_screen_line(scr, 8, FR_INK_ERROR, line);
				break;
			}
		}
		/* S-02.084: no start would reach a good slot, and no host
		 * may be there to say so */
		if (fr_rec_held(st))
			fr_screen_line(scr, 7, FR_INK_ERROR,
				       "Keep the power on until a write has finished");
	}
	/* why recovery runs when no key asked for it: on row 4, or on row 2
	 * where a refusal holds row 4 */
	k = refused != FR_WR_OK && phase == FR_PH_DONE ? 2u : 4u;
	if (st->why == FR_WHY_CHECK)
		fr_screen_line(scr, k, FR_INK_DIM, "This image failed its check");
	else if (st->why == FR_WHY_COUNT)
		fr_screen_line(scr, k, FR_INK_DIM, "Three starts in a row did not finish");
}
