/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery (S-02.084): no slave, no eMMC, no settings. One USB serial port
 * and the protocol of S-02.093; a write goes to the slot the selector does
 * not name, or on the host's request to the running slot, with the running
 * slot established as the selected one; and the actions on the image a
 * slot holds, from the host or from the keys (recovery/ui.c). The screen is
 * held mode's frame in red; the PSoC's power messages are served, so the
 * switch still turns the unit off, but while a power-off would leave a
 * unit that does not start: during an in-place write, and after one that
 * failed, until a good slot is selected again. The power-off's answer is
 * held for that time.
 */
#include "front/start/start.h"

static uint64_t now(const struct fr_start *s)
{
	return s->hw->clock->now_us(s->hw->clock->ctx);
}

/* What the slots, the selector and the running image say now. */
static void survey(struct fr_start *s)
{
	const struct fr_start_hw *hw = s->hw;
	struct fr_rec_status *st = &s->rec;
	unsigned slot = 0;
	bool est = false;

	if (fr_running_slot(hw->flash, hw->mem, &slot, &est) != FR_FLASH_OK)
		est = false;
	st->running.established = est;
	st->running.slot = (uint8_t)slot;
	st->own_desc = s->desc_ok;
	if (s->desc_ok)
		st->desc = s->desc;
	fr_rec_survey(st, hw->flash);
}

/* The PSoC's last battery report, for INFO. */
static void battery_copy(struct fr_start *s)
{
	struct fr_rec_status *st = &s->rec;

	st->batt_len = (uint8_t)(s->psoc.batt_len < FR_BATT_MAX ? s->psoc.batt_len : FR_BATT_MAX);
	fr_copy(st->batt, s->psoc.batt, st->batt_len);
}

/* S-02.084: asked once before an in-place write is accepted. A power-off
 * that comes meanwhile is the loop's to see. */
static void battery_ask(void *ctx)
{
	struct fr_start *s = ctx;

	/* INFO carries this request's report, or none when no reply came. */
	s->psoc.batt_len = 0;
	(void)fr_psoc_battery(&s->psoc);
	battery_copy(s);
}

/* Between the passes of a survey: USB and the PSoC's link, read only. A
 * power-off that arrives is the loop's to see. */
static void survey_idle(void *ctx)
{
	struct fr_start *s = ctx;

	s->hw->usb_poll();
	(void)fr_psoc_poll(&s->psoc);
	if (s->panel_up)
		(void)fr_panel_busy(&s->panel);	/* S-13c.017: a stalled send */
}

/*
 * The screen as it is now, painted into the frame buffer only while no
 * frame goes out (S-13c.024): at once after the frame going out when
 * forced (a change of phase), else not this time when one is going out.
 * Only the rows that changed are painted again, and a screen that did not
 * change is not sent.
 */
static void draw(struct fr_start *s, bool usb, bool force)
{
	if (s->panel_up) {
		if (force)
			fr_panel_idle(&s->panel);
		else if (fr_panel_busy(&s->panel))
			return;
	}
	fr_rec_screen(&s->screen, &s->rec, &s->proto, usb, &s->ui);
	s->drawn_at = now(s);
	s->drawn_changes = s->proto.changes;
	if (!s->panel_up || (s->drawn_ok && fr_same(&s->drawn, &s->screen, sizeof s->screen)))
		return;
	fr_paint_rows(s->hw->frame, &s->screen, s->drawn_ok ? &s->drawn : NULL);
	fr_copy(&s->drawn, &s->screen, sizeof s->screen);
	s->drawn_ok = true;
	fr_panel_frame(&s->panel, s->hw->frame);
}

/*
 * S-02.084: the keys, read while no transfer, countdown or write is under
 * way: one key-status request (S-07.032) a reading. No reply is no key. A
 * confirmed action starts at once, forced, with no countdown.
 */
static int keys(struct fr_start *s)
{
	const uint8_t phase = s->proto.phase;
	const struct fr_ui was = s->ui;
	unsigned key = FR_KEYS_UNREAD;
	uint8_t act = FR_ACT_NONE;

	if (phase == FR_PH_RECEIVING || phase == FR_PH_COUNTDOWN || phase == FR_PH_WRITING) {
		if (was.screen != FR_UI_MAIN || was.armed) {
			fr_ui_reset(&s->ui);
			s->proto.changes++;
		}
		return FR_PSOC_OK;
	}
	if (now(s) - s->keys_at < s->keys_gap)
		return FR_PSOC_OK;
	if (fr_psoc_key_status(&s->psoc) == FR_PSOC_OFF)
		return FR_PSOC_OFF;
	s->keys_at = now(s);
	s->keys_gap = s->psoc.keys_replied ? FR_KEYS_GAP_US : FR_KEYS_MISS_US;
	if (s->psoc.keys_replied)
		key = fr_keys_one(s->dev, s->psoc.keys, s->psoc.keys_len);
	if (fr_ui_key(&s->ui, &s->rec, key, s->keys_at, &act))
		s->ui.refusal = fr_proto_local(&s->proto, act);
	if (s->ui.screen != was.screen || s->ui.hold_pct != was.hold_pct ||
	    s->ui.refusal != was.refusal)
		s->proto.changes++;
	return FR_PSOC_OK;
}

enum fr_next fr_recovery_run(struct fr_start *s)
{
	const struct fr_start_hw *hw = s->hw;
	uint64_t ticked_at;

	fr_fill(&s->rec, 0, sizeof s->rec);
	s->rec.why = s->why;
	s->rec.dev_id = fr_dev_or_default(s->dev)->id;
	if (hw->ident != NULL) {
		/* S-18.004: shown when the device was not identified */
		s->rec.dev_how = hw->ident->how;
		s->rec.fs_n = hw->ident->n;
		s->rec.fs_c = hw->ident->c;
	}
	survey(s);
	fr_proto_init(&s->proto, hw->serial, &s->rec, hw->flash, hw->staging);
	s->proto.battery = battery_ask;
	s->proto.battery_ctx = s;
	s->proto.idle = survey_idle;
	s->proto.idle_ctx = s;
	hw->usb_start(hw->clock);
	s->usb_was = false;
	s->ui.dev = s->dev;
	fr_ui_reset(&s->ui);
	s->keys_at = now(s);
	s->keys_gap = FR_KEYS_GAP_US;
	fr_proto_clock(&s->proto, now(s));
	draw(s, false, true);
	if (s->panel_up && !s->lit) {
		fr_psoc_report(&s->psoc, FR_REPORT_HELD);
		s->lit = true;
	}
	ticked_at = now(s);
	for (;;) {
		const uint8_t phase = s->proto.phase;
		const bool quiet = fr_proto_in_place(&s->proto);
		bool usb, hold;

		/* S-13c.017: a send that has stalled is stopped and sent again
		 * by the CPU, whether or not the screen changes */
		if (s->panel_up)
			(void)fr_panel_busy(&s->panel);
		hw->usb_poll();
		usb = hw->usb_configured();
		if (usb != s->usb_was) {
			/* S-07.055-S-07.057: the current the host granted. */
			fr_psoc_usb(&s->psoc, usb, usb ? hw->usb_ma() : 0u);
			/* S-02.093: nothing half received outlasts the host,
			 * and a countdown stops (S-02.084) */
			fr_proto_drop(&s->proto);
			s->usb_was = usb;
			s->proto.changes++;
		}
		/* S-02.084: during an in-place write the link is read and
		 * nothing else is done on it; the current limit waits. */
		if ((!quiet && fr_psoc_service(&s->psoc) == FR_PSOC_OFF) ||
		    fr_psoc_poll(&s->psoc) == FR_PSOC_OFF)
			return FR_NEXT_HALT;
		battery_copy(s);
		/* At a write's end the protocol reads the selector and the
		 * target slot again itself. */
		fr_proto_poll(&s->proto);
		if (keys(s) == FR_PSOC_OFF)
			return FR_NEXT_HALT;
		/* S-02.084: a countdown that has run out: the phase is writing
		 * from here, the hold and the screen follow below, and the
		 * first erase comes in the next pass */
		fr_proto_clock(&s->proto, now(s));
		/* S-02.084: the power-off's answer is held from an in-place
		 * write's acceptance, before its first erase, until a good
		 * slot is selected again: through a failure and its retries. */
		hold = fr_proto_hold(&s->proto);
		/* A read that failed keeps the hold; with no host to ask
		 * again, the flash is read again here now and then. */
		if (hold && s->rec.read_error && now(s) - ticked_at >= FR_RESURVEY_US) {
			fr_proto_tick(&s->proto);
			ticked_at = now(s);
			hold = fr_proto_hold(&s->proto);
		}
		if (hold != s->psoc.hold_off && fr_psoc_hold(&s->psoc, hold) == FR_PSOC_OFF)
			return FR_NEXT_HALT;
		if (s->proto.restart) {
			/* The answer out, then S-04.065's restart. */
			const uint64_t end = now(s) + 100000u;

			while (!hw->serial->idle(hw->serial->ctx) && now(s) < end)
				hw->usb_poll();
			fr_psoc_reset(&s->psoc);
			return FR_NEXT_HALT;
		}
		/* Not redrawn for progress during an in-place write: its
		 * screen goes once, before the first erase. */
		if (s->proto.changes != s->drawn_changes &&
		    (s->proto.phase != phase ||
		     (!fr_proto_in_place(&s->proto) &&
		      now(s) - s->drawn_at >= FR_DRAW_GAP_US)))
			draw(s, usb, s->proto.phase != phase);
	}
}
