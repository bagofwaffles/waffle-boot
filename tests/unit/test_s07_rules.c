/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07 rules: S-07.031, S-07.032 (keys.json), S-07.040-S-07.042 (leds.json,
 * charge_led.tsv), S-07.050, S-07.051, S-07.053, S-07.055 (power.json),
 * S-07.060, S-07.061 (pmic.json).
 */
#include "waffle/s07_rules.h"

#include "s07_test.h"
#include "vectors/s07_vectors.h"

/* S-07.031: bitmaps through the module, events in increasing key order. */
static void test_bitmaps(void)
{
	for (size_t i = 0; i < s07v_bitmaps_count; i++) {
		const struct s07v_bitmaps *v = &s07v_bitmaps[i];
		const struct s07v_key_ev *e = &s07v_key_events[v->first_event];
		size_t k = 0, at = 0;

		t_case = "keys.json bitmaps";
		t_rig_init(S06_MS(10000));
		for (size_t b = 0; b < v->n; b++) {
			size_t before = rig.nkev;

			/* S-07.079: shorter ones ignored, longer ones read to 9 */
			t_rig_from_psoc(rig.sim.now, 0x41, 0x42, v->maps + at, v->lens[b]);
			at += v->lens[b];
			CHECK_EQ(s07_poll(&rig.psoc), S07_OK);
			CHECK_EQ(rig.nkev - before, v->counts[b]);
			for (size_t j = before; j < rig.nkev && k < 1000; j++, k++) {
				CHECK_EQ(rig.kev[j][0], e[k].key);
				CHECK_EQ(rig.kev[j][1], e[k].down);
			}
		}
		CHECK_EQ(rig.nitems, 0);	/* nothing sent in answer */
	}
}

/* S-07.032, S-16.008 */
static void test_status(void)
{
	for (size_t i = 0; i < s07v_status_count; i++) {
		const struct s07v_status *v = &s07v_status[i];
		struct s07_key_status ks;

		t_case = "keys.json status";
		s07_key_status_decode(v->none ? 0 : v->p, v->len, &ks);
		CHECK_EQ(ks.replied, !v->none);
		CHECK_EQ(ks.hold, v->hold);
		CHECK_EQ(ks.debug, v->debug);
		CHECK_EQ(ks.precomp, v->precomp);
	}
	/* S-07.033: the chord from the same reply; Power + 1 and Power + 2
	 * are none (S-02.080) */
	for (size_t i = 0; i < s07v_chords_count; i++) {
		const struct s07v_chord *v = &s07v_chords[i];

		t_case = v->name;
		CHECK_EQ(s07_chord_digit(v->none ? 0 : v->p, v->len),
			 (unsigned)(v->chord < 0 ? 0 : v->chord));
	}
	/* through the link: no reply within 100 ms counts as held */
	t_case = "status-none";
	t_rig_init(S06_MS(10000));
	{
		struct s07_key_status ks;

		CHECK_EQ(s07_key_status(&rig.psoc, &ks), S07_OK);
		CHECK(ks.hold && !ks.debug && !ks.replied);
		CHECK_EQ(rig.nitems, 1);
		CHECK_EQ(rig.item[0].id, 0x41);
		CHECK(s06_src(rig.item[0].route) >= 8);
		CHECK(rig.sim.now >= S06_MS(100));
	}
}

/* S-07.041 */
static void test_leds(void)
{
	for (size_t i = 0; i < s07v_leds_count; i++) {
		const struct s07v_leds *v = &s07v_leds[i];
		size_t at = 0;

		t_case = "leds.json";
		t_rig_init(S06_MS(10000));
		for (size_t o = 0; o < v->ops_len; o += 5) {
			const uint8_t *op = v->ops + o;

			if (op[0] == 0)
				s07_led_bar(&rig.psoc, op[1]);
			else if (op[0] == 1)
				s07_led_key(&rig.psoc, op[1], op[2], op[3]);
			else
				s07_led_rgb(&rig.psoc, op + 1, op[4] != 0);
		}
		CHECK_EQ(rig.nitems, v->n_sent);
		for (size_t j = 0; j < rig.nitems && at < v->sent_len; j++) {
			CHECK_EQ(rig.item[j].id, 0x40);
			CHECK_EQ(rig.item[j].route, 0x14);
			CHECK_MEM(rig.item[j].d, rig.item[j].len, v->sent + at + 1,
				  v->sent[at]);
			at += 1u + v->sent[at];
		}
	}
}

/* S-07.042 */
static void test_charge_led(void)
{
	for (size_t i = 0; i < s07v_charge_count; i++) {
		const struct s07v_charge *v = &s07v_charge[i];
		uint8_t rgb[3];
		bool forced;

		t_case = "charge_led.tsv";
		s07_charge_led(v->state, v->charge, rgb, &forced);
		CHECK_EQ(rgb[0], v->r);
		CHECK_EQ(rgb[1], v->g);
		CHECK_EQ(rgb[2], v->b);
		CHECK_EQ(forced, v->forced);
	}
}

/* S-07.050, S-07.051, S-07.053, S-07.055 */
static void test_power(void)
{
	t_case = "power.json states";
	for (size_t i = 0; i < s07v_states_count; i++) {
		unsigned s = s07v_states[i].state;
		int kind = s == S07_POWER_ON ? 0 : s == S07_POWER_OFF ? 1 :
			   s == S07_POWER_CHARGE ? 2 : 3;

		CHECK_EQ(kind, s07v_states[i].kind);
	}
	t_case = "power.json transitions";
	for (size_t i = 0; i < s07v_trans_count; i++)
		CHECK_EQ(s07_power_off_trigger(s07v_trans[i].prev,
					       s07v_trans[i].state),
			 s07v_trans[i].off);
	t_case = "power.json battery";
	for (size_t i = 0; i < s07v_batt_count; i++) {
		const struct s07v_batt *v = &s07v_batt[i];
		struct s07_battery b;

		s07_battery_decode(v->p, v->len, &b);
		CHECK_EQ(b.present, v->present);
		CHECK_EQ(b.charging, v->charging);
		CHECK_EQ(b.charge, v->charge);
		CHECK_EQ(b.temp, v->temp);
		CHECK_EQ(b.ma, v->ma);
		CHECK_EQ(b.mv, v->mv);
		CHECK_EQ(s07_battery_usable(&b), v->usable);
	}
	t_case = "power.json vbus";
	for (size_t i = 0; i < s07v_vbus_count; i++) {
		const struct s07v_vbus *v = &s07v_vbus[i];
		int t;

		CHECK_EQ(s07_vbus_update(v->applied, v->target, v->st, v->n, &t),
			 v->after);
		CHECK_EQ(t, v->send_t);
	}
}

/* S-07.060, S-07.061: the frames for each read value */
static int pmic_read;

static void pmic_answer(const struct t_rig *r, const struct t_item *it)
{
	(void)r;
	if (it->id == 0x73 && pmic_read >= 0) {
		const uint8_t p[3] = { 0x03, 0x10, (uint8_t)pmic_read };

		t_rig_from_psoc(it->t + 1000, s06_swap_route(it->route), 0x73, p, 3);
	} else if (it->id == 0x01) {
		t_rig_from_psoc(it->t + 1000, s06_swap_route(it->route), 0x02, 0, 0);
	}
}

static void test_pmic(void)
{
	for (size_t i = 0; i < s07v_pmic_count; i++) {
		const struct s07v_pmic *v = &s07v_pmic[i];
		size_t at = 0;

		t_case = "pmic.json";
		t_rig_init(S06_MS(10000));
		pmic_read = v->read;
		rig.on_item = pmic_answer;
		CHECK_EQ(s07_pmic(&rig.psoc), S07_OK);
		CHECK_EQ(rig.nitems, v->n);
		for (size_t j = 0; j < rig.nitems && at < v->len; j++) {
			const uint8_t *f = v->frames + at;

			CHECK_EQ(rig.item[j].id, f[0]);
			CHECK_EQ(s06_src(rig.item[j].route) >= 8, f[1]);
			CHECK_MEM(rig.item[j].d, rig.item[j].len, f + 3, f[2]);
			at += 3u + f[2];
		}
	}
}

int main(void)
{
	test_bitmaps();
	test_status();
	test_leds();
	test_charge_led();
	test_power();
	test_pmic();
	return t_finish("test_s07_rules");
}
