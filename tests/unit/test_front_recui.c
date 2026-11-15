/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery's keys (src/front/fr_recui.c, S-02.084): one reading at a time,
 * as fr_keys_one() gives it. No key acts until a reading shows every key
 * up, after entry and after every change of screen; a reading that did not
 * come, or shows two keys, arms nothing and disarms. Key 1 opens the slot
 * menu; there 1 to 4 choose an action, each only where it is offered, and
 * 8 goes back; on the confirm screen key 1 read down alone in every reading
 * for 5 s starts the action, and anything else empties the bar.
 */
#include <string.h>

#include "support/check.h"

#include "front/fr_proto.h"
#include "front/fr_psoc.h"
#include "front/fr_start.h"

#define MS 1000u
#define GAP (50u * MS)		/* FR_KEYS_GAP_US */

static struct fr_rec_status rst;
static struct fr_ui ui;
static uint64_t now;

/* Slot 0 runs and is selected {00 01}; slot 1 holds a waffle-boot image
 * with the same front, tried, and its trial start marked: every action is
 * offered. */
static void base(void)
{
	memset(&rst, 0, sizeof rst);
	rst.why = FR_WHY_KEYS;
	rst.running.established = true;
	rst.running.slot = 0;
	rst.selector_ok = true;
	rst.sel.index = 0x00;
	rst.sel.verified = 0x01;
	for (unsigned k = 0; k < 2u; k++) {
		rst.slot[k].good = true;
		rst.slot[k].has_desc = rst.slot[k].has_front = rst.slot[k].bootable = true;
		rst.slot[k].rec.tag = FR_TAG;
		rst.slot[k].rec.length = 0x4000u;
		rst.slot[k].has_log = true;
		rst.slot[k].log0 = 0xFFu;
	}
	rst.slot[1].good = false;
	rst.slot[1].tried = true;
	rst.slot[1].started = true;
	rst.slot[1].log0 = FR_LOG0_MARKED;
	rst.own_desc = true;
	fr_rec_target(&rst);
	fr_ui_reset(&ui);
	now = 1000000u;
}

/* One reading, GAP after the last; true when an action was confirmed. */
static bool rd(unsigned key, uint8_t *act)
{
	uint8_t a = FR_ACT_NONE;
	bool go;

	now += GAP;
	go = fr_ui_key(&ui, &rst, key, now, &a);
	if (act != NULL)
		*act = a;
	CHECK(go == (a != FR_ACT_NONE));
	return go;
}

/* n readings of one key; true when any confirmed an action. */
static bool rd_n(unsigned key, unsigned n, uint8_t *act)
{
	bool any = false;

	while (n--)
		any |= rd(key, act);
	return any;
}

/* To the confirm screen of act from the main screen, each key released. */
static void to_confirm(uint8_t act)
{
	static const uint8_t keys[5] = { 0, FR_KEY_1, FR_KEY_2, FR_KEY_3, FR_KEY_4 };

	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEY_1, NULL));
	CHECK(ui.screen == FR_UI_SLOTS);
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(keys[act], NULL));
	CHECK(ui.screen == FR_UI_CONFIRM && ui.act == act);
	CHECK(!rd(FR_KEYS_NONE, NULL));
}

/* Recovery entered with key 4 still held: nothing acts until a reading
 * shows no key; unread and two-key readings never arm. */
static void test_gate_entry(void)
{
	static const unsigned noarm[] = { FR_KEY_4, FR_KEYS_UNREAD, FR_KEYS_MANY, FR_KEY_1,
					  FR_KEY_8, 0x00u, 0x3Fu };

	base();
	CHECK(ui.screen == FR_UI_MAIN && !ui.armed);
	/* key 4 held, then key 1 with no release between: nothing */
	CHECK(!rd_n(FR_KEY_4, 40, NULL));
	CHECK(!rd_n(FR_KEY_1, 10, NULL));
	CHECK(ui.screen == FR_UI_MAIN && !ui.armed);
	/* readings that are not "no key", in every order: never armed */
	for (unsigned i = 0; i < ARRAY_LEN(noarm); i++)
		for (unsigned j = 0; j < ARRAY_LEN(noarm); j++) {
			CHECK(!rd(noarm[i], NULL));
			CHECK(!rd(noarm[j], NULL));
			CHECK(!rd(FR_KEY_1, NULL));
			CHECK(ui.screen == FR_UI_MAIN && !ui.armed);
		}
	/* a release arms; an unread or two-key reading after it disarms */
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(ui.armed);
	CHECK(!rd(FR_KEYS_UNREAD, NULL));
	CHECK(!ui.armed);
	CHECK(!rd(FR_KEY_1, NULL));
	CHECK(ui.screen == FR_UI_MAIN);
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEYS_MANY, NULL));
	CHECK(!rd(FR_KEY_1, NULL));
	CHECK(ui.screen == FR_UI_MAIN);
	/* released, then key 1: the slot menu */
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEY_1, NULL));
	CHECK(ui.screen == FR_UI_SLOTS && !ui.armed);
	/* on the main screen no other key does anything */
	for (unsigned k = 0; k < 64u; k++) {
		if (k == FR_KEY_1)
			continue;
		fr_ui_reset(&ui);
		CHECK(!rd(FR_KEYS_NONE, NULL));
		CHECK(!rd(k, NULL));
		CHECK(ui.screen == FR_UI_MAIN);
	}
}

/* After each change of screen a release is needed again: a key held
 * through it acts on nothing, however long. */
static void test_gate_screens(void)
{
	base();
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEY_1, NULL));
	CHECK(ui.screen == FR_UI_SLOTS);
	/* key 1 still held on the menu: no try */
	CHECK(!rd_n(FR_KEY_1, 200, NULL));
	CHECK(ui.screen == FR_UI_SLOTS);
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEY_1, NULL));
	CHECK(ui.screen == FR_UI_CONFIRM && ui.act == FR_ACT_TRY);
	/* key 1 held through the change: no hold begins, 10 s of it */
	CHECK(!rd_n(FR_KEY_1, 200, NULL));
	CHECK(ui.screen == FR_UI_CONFIRM && !ui.holding && ui.hold_pct == 0u);
	/* key 8 back to the menu, held: no further back */
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEY_8, NULL));
	CHECK(ui.screen == FR_UI_SLOTS);
	CHECK(!rd_n(FR_KEY_8, 5, NULL));
	CHECK(ui.screen == FR_UI_SLOTS);
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEY_8, NULL));
	CHECK(ui.screen == FR_UI_MAIN);
	/* an action not offered: nothing, and a release needed again */
	base();
	rst.slot[1].started = false;		/* no keep */
	CHECK(!fr_ui_offers(&rst, FR_ACT_KEEP) && fr_ui_offers(&rst, FR_ACT_PUT_BACK));
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEY_1, NULL));
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEY_3, NULL));
	CHECK(ui.screen == FR_UI_SLOTS && !ui.armed);
	CHECK(!rd(FR_KEY_4, NULL));
	CHECK(ui.screen == FR_UI_SLOTS);
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEY_4, NULL));
	CHECK(ui.screen == FR_UI_CONFIRM && ui.act == FR_ACT_PUT_BACK);
}

/* The hold: 5 s of key-1 readings start the action, key 1 for each of the
 * four; the bar follows; then the main screen. */
static void test_hold(void)
{
	for (uint8_t act = FR_ACT_TRY; act <= FR_ACT_PUT_BACK; act++) {
		uint8_t got = FR_ACT_NONE;
		uint64_t from;

		base();
		CHECK(fr_ui_offers(&rst, act));
		to_confirm(act);
		CHECK(!rd(FR_KEY_1, NULL));
		from = now;
		CHECK(ui.holding && ui.hold_pct == 0u);
		/* every reading short of 5 s: the bar, no action */
		while (now + GAP < from + FR_HOLD_US) {
			CHECK(!rd(FR_KEY_1, &got));
			CHECK(ui.hold_pct == (now - from) * 100u / FR_HOLD_US);
		}
		CHECK(ui.hold_pct == 99u && ui.screen == FR_UI_CONFIRM);
		CHECK(rd(FR_KEY_1, &got));
		CHECK(now - from == FR_HOLD_US && got == act);
		CHECK(ui.screen == FR_UI_MAIN && !ui.holding && ui.hold_pct == 0u && !ui.armed);
		/* key 1 still down after it: nothing more */
		CHECK(!rd_n(FR_KEY_1, 200, &got));
		CHECK(ui.screen == FR_UI_MAIN);
	}
	/* the readings' gap does not matter: one reading 5 s on */
	base();
	to_confirm(FR_ACT_SWITCH);
	CHECK(!rd(FR_KEY_1, NULL));
	{
		uint8_t got = FR_ACT_NONE;

		CHECK(!fr_ui_key(&ui, &rst, FR_KEY_1, now + FR_HOLD_US - 1u, &got));
		CHECK(fr_ui_key(&ui, &rst, FR_KEY_1, now + FR_HOLD_US, &got) && got == FR_ACT_SWITCH);
	}
	/* the hold key is key 1 for every action: 2, 3 and 4 on the confirm
	 * screen hold nothing */
	for (uint8_t act = FR_ACT_TRY; act <= FR_ACT_PUT_BACK; act++) {
		static const unsigned others[] = { FR_KEY_2, FR_KEY_3, FR_KEY_4, 0x00u };

		for (unsigned i = 0; i < ARRAY_LEN(others); i++) {
			base();
			to_confirm(act);
			CHECK(!rd(others[i], NULL));
			CHECK(!rd_n(others[i], 120, NULL));
			CHECK(!ui.holding && ui.screen == FR_UI_CONFIRM);
		}
	}
}

/* Anything but key 1 alone during the hold empties the bar and starts
 * nothing: released, unread, two keys (key 1 with a step key among them,
 * S-07.033), another key, a step key; key 8 goes back. */
static void test_hold_broken(void)
{
	static const unsigned breaks[] = { FR_KEYS_NONE, FR_KEYS_UNREAD, FR_KEYS_MANY, FR_KEY_2,
					   FR_KEY_4, FR_KEY_8, 0x00u, 0x40u, 0x47u };

	for (unsigned i = 0; i < ARRAY_LEN(breaks); i++) {
		uint8_t got = FR_ACT_NONE;

		base();
		to_confirm(FR_ACT_TRY);
		CHECK(!rd(FR_KEY_1, NULL));
		CHECK(!rd_n(FR_KEY_1, 80, &got));	/* 4 s */
		CHECK(ui.holding && ui.hold_pct == 80u);
		CHECK(!rd(breaks[i], &got));
		CHECK(!ui.holding && ui.hold_pct == 0u && got == FR_ACT_NONE);
		/* key 1 again: a new hold only after a release, and from zero */
		CHECK(!rd_n(FR_KEY_1, 40, &got));
		CHECK(!ui.holding || ui.hold_pct < 100u);
		if (breaks[i] == FR_KEYS_NONE) {
			/* released: armed, key 1 starts the hold afresh */
			CHECK(ui.holding && ui.hold_pct == 39u);
		} else {
			CHECK(!ui.holding);
		}
		CHECK(ui.screen == FR_UI_CONFIRM);
	}
	/* key 8: back to the menu, with a release first */
	base();
	to_confirm(FR_ACT_SWITCH);
	CHECK(!rd(FR_KEY_8, NULL));
	CHECK(ui.screen == FR_UI_SLOTS);
	/* and from the hold: the break, then 8 after a release */
	base();
	to_confirm(FR_ACT_SWITCH);
	CHECK(!rd_n(FR_KEY_1, 30, NULL));
	CHECK(!rd(FR_KEY_8, NULL));
	CHECK(ui.screen == FR_UI_CONFIRM && !ui.holding);
	CHECK(!rd(FR_KEYS_NONE, NULL));
	CHECK(!rd(FR_KEY_8, NULL));
	CHECK(ui.screen == FR_UI_SLOTS);
}

/* The menu offers an action where its forced plan would start, and a keep
 * only of a slot that started or of the running slot. */
static void test_offers(void)
{
	struct fr_act_plan p;

	base();
	for (uint8_t act = FR_ACT_TRY; act <= FR_ACT_PUT_BACK; act++)
		CHECK(fr_ui_offers(&rst, act));
	rst.slot[1].started = false;
	fr_rec_act(&rst, FR_ACT_KEEP, true, &p);
	CHECK(p.status == FR_WR_OK && !fr_ui_offers(&rst, FR_ACT_KEEP));
	/* a trial start: slot 1 tried and runs, slot 0 trusted */
	base();
	rst.running.slot = 1;
	rst.slot[1].started = false;
	rst.slot[1].log0 = FR_LOG0_PENDING;
	fr_rec_target(&rst);
	CHECK(fr_ui_offers(&rst, FR_ACT_KEEP));
	CHECK(!fr_ui_offers(&rst, FR_ACT_TRY) && !fr_ui_offers(&rst, FR_ACT_SWITCH));
	CHECK(!fr_ui_offers(&rst, FR_ACT_PUT_BACK));
	/* the running slot not known: nothing */
	base();
	rst.running.established = false;
	for (uint8_t act = FR_ACT_TRY; act <= FR_ACT_PUT_BACK; act++)
		CHECK(!fr_ui_offers(&rst, act));
	/* every offer is the forced plan's */
	base();
	for (unsigned v = 0; v < 64u; v++) {
		rst.slot[1].good = (v & 1u) != 0u;
		rst.slot[1].tried = !rst.slot[1].good && (v & 2u) != 0u;
		rst.slot[1].started = rst.slot[1].tried && (v & 4u) != 0u;
		rst.slot[1].bootable = (v & 8u) != 0u;
		rst.sel.index = (v & 16u) ? 0x01u : 0x00u;
		rst.home.moved = (v & 32u) != 0u;
		fr_rec_target(&rst);
		for (uint8_t act = FR_ACT_TRY; act <= FR_ACT_PUT_BACK; act++) {
			fr_rec_act(&rst, act, true, &p);
			CHECK(fr_ui_offers(&rst, act) ==
			      (p.status == FR_WR_OK &&
			       (act != FR_ACT_KEEP || p.target == 0u || rst.slot[1].started)));
		}
	}
}

int main(void)
{
	RUN(test_gate_entry);
	RUN(test_gate_screens);
	RUN(test_hold);
	RUN(test_hold_broken);
	RUN(test_offers);
	return check_summary("test_front_recui");
}
