/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery's screens (S-15.151). The main one: each slot's state and
 * version, which runs and which is selected, how to write an image and the
 * way to the slot menu, then a transfer's progress and the result, and why
 * recovery runs when no key asked for it. The slot menu and the confirm
 * screen (fr_recui.c), and the countdown before a host's write. Every line
 * says what to do where there is something to do. Wording and layout are
 * free.
 */
#include "fr_start.h"

static void put(char *line, size_t *at, const char *t)
{
	while (*t != '\0' && *at < FR_COLS)
		line[(*at)++] = *t++;
	line[*at] = '\0';
}

/* "Slot 1  v1.0.0  good  running  selected" (S-15's numbering): the
 * version, or that the image is not waffle-boot's, then good, tried (and
 * started) or bad. The parts are two spaces apart, or one where the longest
 * version would not fit. */
static void slot_line(struct fr_screen *scr, unsigned row, const struct fr_rec_status *st,
		      unsigned k)
{
	const struct fr_slot_info *in = &st->slot[k];
	char line[FR_COLS + 1], v[FR_VERSION_TEXT_MAX + 1];
	const char *sep = "  ";
	size_t at = 0;

	v[0] = '\0';
	if (in->has_desc) {
		size_t n = fr_text_version(v, in->desc.bl_version);

		v[n] = '\0';
		if (n + 2u > FR_VERSION_TEXT_MAX)
			sep = " ";
	}
	line[0] = '\0';
	put(line, &at, k == 0u ? "Slot 1" : "Slot 2");
	put(line, &at, sep);
	if (in->has_desc) {
		put(line, &at, v);
		put(line, &at, sep);
	} else if (in->good || in->tried) {
		put(line, &at, "not waffle-boot");
		put(line, &at, sep);
	}
	put(line, &at, in->good ? "good" : !in->tried ? "bad" :
				 in->started ? "tried, started" : "tried");
	if (st->running.established && st->running.slot == k) {
		put(line, &at, sep);
		put(line, &at, "running");
	}
	if (st->selector_ok && fr_selected_slot(st->sel.index) == k) {
		put(line, &at, sep);
		put(line, &at, "selected");
	}
	fr_screen_line(scr, row, in->good ? FR_INK_TEXT : in->tried ? FR_INK_WARN : FR_INK_ERROR,
		       line);
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
		return "No write: choose the slot to start first (key 1)";
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
		return "No write: send the image the good slot holds";
	case FR_WR_TRIAL_STATE:
		return "Failed: no trial or keep now; restart the unit first";
	case FR_WR_NOT_TRIED:
		return "Failed: no slot holds a tried image to keep";
	case FR_WR_NOT_STARTED:
		return "Failed: the tried slot has not started; restart first";
	case FR_WR_ALREADY:
		return "Nothing to do: that slot starts on every start already";
	case FR_WR_TARGET_BAD:
		return "Failed: that slot is bad; write an image to it first";
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
	char line[FR_COLS + 1];

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
			       "Its record is bad: nothing will ever check this slot" :
			       "Its record is bad: it may not start");
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
		fr_screen_line(scr, row + 1u, FR_INK_WARN, "Try it once first (8, then 1)");
}

/* What a write or an action will do, in plain words, from row on, with its
 * warning: before it starts. version is the image's or the slot's, NULL
 * for none. */
static void what_rows(struct fr_screen *scr, unsigned row, const uint16_t *version, bool forced,
		      unsigned way, unsigned t, unsigned tier, unsigned run)
{
	char line[FR_COLS + 1];
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
			slot_text(line, "It is the running slot; slot ", 1u - t,
				  way == FR_WAY_SELECT_FIRST ? " is selected first" : " stays selected");
			fr_screen_line(scr, row + 1u, FR_INK_TEXT, line);
		} else {
			fr_screen_line(scr, row + 1u, FR_INK_TEXT, "and selected: the next start boots it");
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
		put(line, &at, t == 0u ? "The unit will start slot 1 (" : "The unit will start slot 2 (");
		put_version(line, &at, version);
		put(line, &at, ") from now on");
		fr_screen_line(scr, row, FR_INK_HEAD, line);
		tier_rows(scr, row + 1u, tier, true, run);
		break;
	case FR_WAY_SELECT:
	case FR_WAY_KEEP:
		slot_text(line, "Slot ", t, ", which was tried, will be kept:");
		fr_screen_line(scr, row, FR_INK_HEAD, line);
		fr_screen_line(scr, row + 1u, FR_INK_TEXT, "the unit will start it from now on");
		break;
	case FR_WAY_PUT_BACK:
		slot_text(line, "Slot ", t, "'s record will be put back:");
		fr_screen_line(scr, row, FR_INK_HEAD, line);
		fr_screen_line(scr, row + 1u, FR_INK_TEXT,
			       "it reads good again; the unit starts as it does now");
		break;
	default:
		break;
	}
}

static void set_bar(struct fr_screen *scr, unsigned row, unsigned pct)
{
	scr->bar_line = (int8_t)row;
	scr->bar_col = 0;
	scr->bar_cols = 40;
	scr->bar_pct = (uint8_t)(pct > 100u ? 100u : pct);
}

/* The slot menu (rows 2 to 8): what can be done with the slot that is not
 * running, or with the running one in its trial start. */
static void menu_rows(struct fr_screen *scr, const struct fr_rec_status *st,
		      const struct fr_ui *ui, unsigned run)
{
	const unsigned b = 1u - run;
	struct fr_act_plan keep, sw;
	char line[FR_COLS + 1];
	unsigned row = 3;

	if (!st->running.established) {
		fr_screen_line(scr, 3, FR_INK_WARN, refusal(FR_WR_NOT_RUNNING, false));
		fr_screen_line(scr, 8, FR_INK_DIM, "8  Back");
		return;
	}
	fr_rec_act(st, FR_ACT_KEEP, true, &keep);
	fr_rec_act(st, FR_ACT_SWITCH, true, &sw);
	if (fr_ui_offers(st, FR_ACT_TRY)) {
		slot_text(line, "1  Try slot ", b, " once (suggested)");
		fr_screen_line(scr, row++, FR_INK_TEXT, line);
	}
	if (fr_ui_offers(st, FR_ACT_SWITCH)) {
		slot_text(line, "2  Switch to slot ", b, " for good");
		fr_screen_line(scr, row++, FR_INK_TEXT, line);
	}
	if (fr_ui_offers(st, FR_ACT_KEEP)) {
		slot_text(line, "3  Keep slot ", keep.target,
			  keep.target == run ? ", the running slot" : "");
		fr_screen_line(scr, row++, FR_INK_TEXT, line);
	}
	if (fr_ui_offers(st, FR_ACT_PUT_BACK)) {
		slot_text(line, "4  Put slot ", b, "'s record back");
		fr_screen_line(scr, row++, FR_INK_TEXT, line);
	}
	if (ui->refusal != FR_WR_OK) {
		fr_screen_line(scr, 7, FR_INK_ERROR, refusal(ui->refusal, false));
	} else if (sw.status == FR_WR_ALREADY) {
		slot_text(line, "Slot ", b, " is selected: it starts on every start");
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
	fr_screen_line(scr, 8, FR_INK_DIM, "8  Back");
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
	char line[FR_COLS + 1], n[12];
	size_t at = 0;
	unsigned k;

	fr_screen_text(scr, screen == FR_UI_SLOTS ? "Recovery: slots" :
			    screen == FR_UI_CONFIRM ? "Recovery: confirm" : "Recovery");
	slot_line(scr, 0, st, 0);
	slot_line(scr, 1, st, 1);
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
		fr_screen_line(scr, 6, FR_INK_HEAD, "Hold 1 for 5 s to confirm");
		set_bar(scr, 7, ui->hold_pct);
		fr_screen_line(scr, 8, FR_INK_DIM, "8  Back");
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
		set_bar(scr, 7, 100u - p->count_left * 100u / (FR_COUNTDOWN_US / 1000000u));
		return;
	}
	if (quiet && phase != FR_PH_DONE) {
		/* how to write an image, or why none can be written */
		if (damaged(st) && st->target < 2u && st->target != run && st->slot[st->target].good)
			/* the only good slot is not replaced (S-02.084): row 7
			 * says what a start does */
			line[0] = '\0';
		else if (st->target == FR_NO_SLOT && !(st->select_target == run &&
						       st->select_refusal == FR_WR_OK))
			/* as a start's refusal: a good running slot with no front */
			fr_screen_line(scr, 2, FR_INK_WARN, st->refusal == FR_WR_FRONT ?
					  "No write: the running slot holds no waffle-boot front" :
					  refusal(st->refusal, after));
		else if (!usb)
			fr_screen_line(scr, 2, FR_INK_HEAD, "Connect to a computer to write an image");
		else if (st->target != FR_NO_SLOT) {
			slot_text(line, "Connected: a write from the computer replaces slot ",
				  st->target, "");
			fr_screen_line(scr, 2, FR_INK_HEAD, line);
		} else
			fr_screen_line(scr, 2, FR_INK_HEAD, "Connected: waiting for the computer");
	}
	if (quiet)
		fr_screen_line(scr, 3, FR_INK_TEXT, "1  Choose the slot to start");
	/* a request refused since the last write: what to do first */
	if (phase == FR_PH_DONE && p->refused != FR_WR_OK)
		fr_screen_line(scr, 4, FR_INK_WARN, refusal(p->refused, true));
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
		fr_screen_line(scr, 5, FR_INK_TEXT, line);
		break;
	case FR_PH_WRITING:
		if (p->w.way == FR_WAY_IN_PLACE) {
			/* drawn once, before the first erase: no bar */
			slot_text(line, "Writing slot ", p->w.target, " in place: keep the power on");
			fr_screen_line(scr, 5, FR_INK_WARN, line);
			fr_screen_line(scr, 6, FR_INK_ERROR,
				       "A power cut now leaves no image that starts");
			break;
		}
		slot_text(line, p->w.act != FR_ACT_NONE ? "Working on slot " : "Writing slot ",
			  p->w.target, ": keep the power on");
		fr_screen_line(scr, 5, FR_INK_WARN, line);
		set_bar(scr, 6, fr_writer_progress(&p->w) / 10u);
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
			slot_text(line, "Done: the unit starts slot ", p->w.target, " from now on");
			break;
		case FR_WAY_SELECT:
		case FR_WAY_KEEP:
			slot_text(line, "Done: slot ", p->w.target, " is kept and selected");
			break;
		case FR_WAY_PUT_BACK:
			slot_text(line, "Done: slot ", p->w.target, "'s record is back; it is good");
			break;
		case FR_WAY_IN_PLACE:
			slot_text(line, "Done: slot ", p->w.target, " is written, and still selected");
			break;
		default:
			slot_text(line, "Done: slot ", p->w.target, " is written and selected");
			break;
		}
		fr_screen_line(scr, 5, FR_INK_OK, line);
		/* with the running slot damaged, row 7 says what a start does */
		if (p->w.way != FR_WAY_PUT_BACK && !damaged(st))
			fr_screen_line(scr, 6, FR_INK_DIM, p->w.way == FR_WAY_TRIAL ||
					  p->w.way == FR_WAY_TRY ?
					  "Restart to start it once (host or power switch)" :
					  "Restart to start it (host or power switch)");
		if (p->w.way == FR_WAY_TRIAL || p->w.way == FR_WAY_TRY)
			off_on_row(scr, 7, run);
		break;
	case FR_PH_FAILED:
		fr_screen_line(scr, 5, FR_INK_ERROR,
			       p->result == FR_WR_TRIAL_STATE && (p->flags & FR_BEGIN_F_SELECT) ?
			       "Failed: no keep after a write here; restart first" :
			       refusal(p->result, after));
		if (fr_rec_rescue(st))
			break;		/* rows 6 and 7 say what a start does */
		if (p->result != FR_WR_SELECTOR) {
			fr_screen_line(scr, 6, FR_INK_DIM, p->result == FR_WR_SELECT_FIRST ?
					  "No slot was written" :
					  "The unit starts as it did before");
		} else if (st->slot[0].good && st->slot[1].good) {
			fr_screen_line(scr, 6, FR_INK_DIM,
				       "Both slots read good: the unit boots either one");
		} else {
			/* the slot written read back good, the other one is
			 * bad: what the selector, as it reads now, leads to */
			switch (fr_rec_reach(st)) {
			case FR_REACH_NAMED:
				slot_text(line, "A restart boots slot ",
					  fr_selected_slot(st->sel.index), ", which reads good");
				fr_screen_line(scr, 6, FR_INK_DIM, line);
				break;
			case FR_REACH_FALLBACK:
				/* the fallback is to slot 1, shown as slot 2 */
				fr_screen_line(scr, 6, FR_INK_DIM,
					       "The 2nd restart boots slot 2, which reads good");
				break;
			default:
				fr_screen_line(scr, 6, FR_INK_ERROR,
					       "No good slot would start: write again");
				break;
			}
		}
		if (p->result == FR_WR_FRONT)
			fr_screen_line(scr, 7, FR_INK_DIM, "A new front is taken only for a trial start");
		break;
	case FR_PH_IDLE:
	default:
		if (st->select_target == run && st->select_refusal == FR_WR_OK) {
			/* a trial start, in the tried slot's own recovery */
			slot_text(line, "Trial start of slot ", run, ": to keep it, 1, then 3");
			fr_screen_line(scr, 5, FR_INK_HEAD, line);
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
				fr_screen_line(scr, 7, FR_INK_WARN, line);
				break;
			case FR_REACH_FALLBACK:
				/* the first start boots the damaged slot once */
				slot_text(line, "Slot ", run,
					  " is damaged: the 2nd restart boots the other slot");
				fr_screen_line(scr, 7, FR_INK_WARN, line);
				break;
			default:
				slot_text(line, "Slot ", run,
					  " is damaged: write again before a power-off");
				fr_screen_line(scr, 7, FR_INK_ERROR, line);
				break;
			}
		} else if (fr_rec_rescue(st)) {
			/* written and read back good, the selector not settled,
			 * the other slot bad */
			switch (fr_rec_reach(st)) {
			case FR_REACH_NAMED:
				slot_text(line, "Slot ", run, " is written: a restart boots it");
				fr_screen_line(scr, 7, FR_INK_WARN, line);
				break;
			case FR_REACH_FALLBACK:
				slot_text(line, "Slot ", run, " is written: the 2nd restart boots it");
				fr_screen_line(scr, 7, FR_INK_WARN, line);
				break;
			default:
				slot_text(line, "Slot ", run,
					  " is written, not yet selected: write it again");
				fr_screen_line(scr, 7, FR_INK_ERROR, line);
				break;
			}
		}
		/* S-02.084: no start would reach a good slot, and no host
		 * may be there to say so */
		if (fr_rec_held(st))
			fr_screen_line(scr, 6, FR_INK_ERROR,
				       "Keep the power on until a write has finished");
	}
	if (st->why == FR_WHY_CHECK)
		fr_screen_line(scr, 8, FR_INK_DIM, "This image failed its check");
	else if (st->why == FR_WHY_COUNT)
		fr_screen_line(scr, 8, FR_INK_DIM, "Three starts in a row did not finish");
}
