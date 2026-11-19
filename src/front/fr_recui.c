/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery's keys (S-02.084, S-15.151): the main screen, the slot menu and
 * the confirm screen, driven by one key reading at a time (fr_keys_one()).
 * No key acts until a reading has shown every key up, after entry (which
 * follows the front's line under the logo, S-13c.025, so the chord's own
 * keys choose nothing) and after each change of screen; a reading that did not come, or shows more than
 * one key, is no key and ends a hold. An action starts only when key 1 was
 * read down, alone, in every reading for FR_HOLD_US on the confirm screen.
 */
#include "fr_start.h"

bool fr_ui_offers(const struct fr_rec_status *st, uint8_t act)
{
	const unsigned a = st->running.slot < 2u ? st->running.slot : 0u;
	struct fr_act_plan plan;

	fr_rec_act(st, act, true, &plan);
	if (plan.status != FR_WR_OK)
		return false;
	/* S-02.084: a keep is offered for a slot that started, or runs */
	return act != FR_ACT_KEEP || plan.target == a || st->slot[1u - a].started;
}

static void show(struct fr_ui *u, uint8_t screen)
{
	u->screen = screen;
	u->armed = false;
	u->holding = false;
	u->hold_pct = 0;
}

void fr_ui_reset(struct fr_ui *u)
{
	show(u, FR_UI_MAIN);
	u->act = FR_ACT_NONE;
	u->refusal = FR_WR_OK;
}

/* A key went down on a screen. */
static void press(struct fr_ui *u, const struct fr_rec_status *st, unsigned key, uint64_t now)
{
	static const uint8_t keys[4] = { FR_KEY_1, FR_KEY_2, FR_KEY_3, FR_KEY_4 };

	switch (u->screen) {
	case FR_UI_MAIN:
		if (key == FR_KEY_1) {
			u->refusal = FR_WR_OK;
			show(u, FR_UI_SLOTS);
		}
		break;
	case FR_UI_SLOTS:
		if (key == FR_KEY_8) {
			show(u, FR_UI_MAIN);
			break;
		}
		for (unsigned i = 0; i < 4u; i++) {
			const uint8_t act = (uint8_t)(FR_ACT_TRY + i);

			if (key == keys[i] && fr_ui_offers(st, act)) {
				u->act = act;
				u->refusal = FR_WR_OK;
				show(u, FR_UI_CONFIRM);
			}
		}
		break;
	case FR_UI_CONFIRM:
	default:
		if (key == FR_KEY_8) {
			show(u, FR_UI_SLOTS);
		} else if (key == FR_KEY_1) {
			u->holding = true;
			u->hold_from = now;
		}
		break;
	}
}

bool fr_ui_key(struct fr_ui *u, const struct fr_rec_status *st, unsigned key, uint64_t now,
	       uint8_t *act)
{
	if (u->holding) {
		if (key == FR_KEY_1 && u->screen == FR_UI_CONFIRM) {
			const uint64_t held = now - u->hold_from;

			if (held >= FR_HOLD_US) {
				*act = u->act;
				fr_ui_reset(u);
				return true;
			}
			u->hold_pct = (uint8_t)(held * 100u / FR_HOLD_US);
			return false;
		}
		/* released, unread or another key: the bar empties */
		u->holding = false;
		u->hold_pct = 0;
	}
	if (key == FR_KEYS_NONE) {
		u->armed = true;
		return false;
	}
	if (u->armed && key != FR_KEYS_UNREAD && key != FR_KEYS_MANY)
		press(u, st, key, now);
	/* the next key acts only after every key was seen up again */
	u->armed = false;
	return false;
}
