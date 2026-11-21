/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07 rules: S-07.031, S-07.032 (keys.json), S-07.040-S-07.042 (leds.json,
 * charge_led.tsv), S-07.050, S-07.051, S-07.053, S-07.055 (power.json),
 * S-07.060, S-07.061 (pmic.json); on the OP-1F S-07.031-.033 (the device's
 * key table), S-07.046-.049 (leds-1f.json, charge_led-1f.json), S-07.062
 * and S-07.079's 8-byte 0x42; S-18.020's trace rules no_pmic_ids and
 * no_key_led_ids (trace_rules-1f.json) over a whole session on each device.
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

/* S-07.031, S-07.032: the key-status reply is the known state the bitmaps
 * are told against (keys.json after_status), with no event of its own. */
static void test_after_status(void)
{
	for (size_t i = 0; i < s07v_after_status_count; i++) {
		const struct s07v_after *v = &s07v_after_status[i];
		const struct s07v_key_ev *e = &s07v_after_events[v->first_event];
		struct s07_key_status ks;
		size_t k = 0;

		t_case = "keys.json after_status";
		t_rig_init(S06_MS(10000));
		s07_key_status_given(&rig.psoc, v->none ? NULL : v->status, v->status_len, &ks);
		CHECK_EQ(rig.nkev, 0);
		for (size_t b = 0; b < v->n; b++) {
			size_t before = rig.nkev;

			t_rig_from_psoc(rig.sim.now, 0x41, 0x42, v->maps + 9u * b, 9u);
			CHECK_EQ(s07_poll(&rig.psoc), S07_OK);
			CHECK_EQ(rig.nkev - before, v->counts[b]);
			for (size_t j = before; j < rig.nkev && k < 1000; j++, k++) {
				CHECK_EQ(rig.kev[j][0], e[k].key);
				CHECK_EQ(rig.kev[j][1], e[k].down);
			}
		}
	}
	/* a reply replaces the state whole, a short one padded with 00 */
	t_case = "status-known";
	t_rig_init(S06_MS(10000));
	{
		static const uint8_t held[2] = { 0, 0x20 };
		struct s07_key_status ks;

		memset(rig.psoc.keys, 0xFF, sizeof rig.psoc.keys);
		s07_key_status_given(&rig.psoc, held, sizeof held, &ks);
		CHECK(ks.replied && ks.hold);	/* the white knob */
		CHECK_EQ(rig.psoc.keys[0], 0);
		CHECK_EQ(rig.psoc.keys[1], 0x20);
		CHECK_EQ(rig.psoc.keys[8], 0);
		CHECK_EQ(rig.nkev, 0);
	}
}

/* S-07.032, S-16.008 */
static void test_status(void)
{
	for (size_t i = 0; i < s07v_status_count; i++) {
		const struct s07v_status *v = &s07v_status[i];
		struct s07_key_status ks;

		t_case = "keys.json status";
		s07_key_status_decode(&wb_device_opxy, v->none ? 0 : v->p, v->len, &ks);
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
		CHECK_EQ(s07_chord_digit(&wb_device_opxy, v->none ? 0 : v->p, v->len),
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

/* ---- the OP-1F ---- */

static void bm_set(uint8_t *b, unsigned k)
{
	b[k >> 3] = (uint8_t)(b[k >> 3] | 1u << (k & 7u));
}

/* S-07.032, S-07.033 by device.key.* (S-18.030) */
static void test_status_1f(void)
{
	const struct wb_device *d = &wb_device_op1f;
	struct s07_key_status ks;
	uint8_t b[10];

	t_case = "status-1f";
	memset(b, 0, sizeof b);
	s07_key_status_decode(d, b, 8, &ks);
	CHECK(ks.replied && !ks.hold && !ks.debug && !ks.precomp);
	bm_set(b, 0x39);				/* byte 7 bit 1 */
	s07_key_status_decode(d, b, 8, &ks);
	CHECK(ks.hold);
	memset(b, 0, sizeof b);
	bm_set(b, 0x2C);				/* byte 5 bit 4 */
	s07_key_status_decode(d, b, 8, &ks);
	CHECK(ks.hold);
	memset(b, 0, sizeof b);
	bm_set(b, 0x0A);
	s07_key_status_decode(d, b, 8, &ks);
	CHECK(!ks.debug);				/* one alone is not debug */
	bm_set(b, 0x0C);
	s07_key_status_decode(d, b, 8, &ks);
	CHECK(ks.debug && !ks.hold);
	memset(b, 0, sizeof b);
	bm_set(b, 0x0B);				/* the OP-XY's debug key */
	b[2] = 0x80;					/* and its precomp bit */
	s07_key_status_decode(d, b, 8, &ks);
	CHECK(!ks.debug && !ks.precomp);
	/* the OP-XY's bits are not the OP-1F's hold */
	memset(b, 0, sizeof b);
	b[3] = 0x01;
	b[1] = 0x20;
	s07_key_status_decode(d, b, 8, &ks);
	CHECK(!ks.hold);
	s07_key_status_decode(&wb_device_opxy, b, 8, &ks);
	CHECK(ks.hold);
	/* bits from 0x3A up and byte 8 are not read */
	memset(b, 0, sizeof b);
	b[7] = 0xFC;
	b[8] = 0xFF;
	s07_key_status_decode(d, b, 10, &ks);
	CHECK(!ks.hold && !ks.debug);
	s07_key_status_decode(d, 0, 0, &ks);
	CHECK(!ks.replied && ks.hold);

	/* S-07.033: device.key.recovery (0x09) alone; no precomp exception */
	t_case = "chord-1f";
	memset(b, 0, sizeof b);
	bm_set(b, 0x09);
	CHECK_EQ(s07_chord_digit(d, b, 8), 4u);
	CHECK_EQ(s07_chord_digit(d, b, 2), 4u);	/* a short reply is read */
	b[7] = 0xFC;					/* not keys */
	b[8] = 0xFF;					/* not read */
	CHECK_EQ(s07_chord_digit(d, b, 10), 4u);
	b[2] = 0x80;					/* key 0x17, a key here */
	CHECK_EQ(s07_chord_digit(d, b, 8), 0u);
	memset(b, 0, sizeof b);
	bm_set(b, 0x09);
	bm_set(b, 0x39);				/* a hold key with it */
	CHECK_EQ(s07_chord_digit(d, b, 8), 0u);
	CHECK_EQ(s07_chord_digit(d, 0, 0), 0u);
	memset(b, 0, sizeof b);
	CHECK_EQ(s07_chord_digit(d, b, 8), 0u);
}

/* S-07.031, S-07.079: 58 keys in 8 bytes; a shorter 0x42 ignored */
static void test_bitmaps_1f(void)
{
	uint8_t b[10];

	t_case = "bitmaps-1f";
	t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
	memset(b, 0, sizeof b);
	bm_set(b, 0x39);
	b[7] = (uint8_t)(b[7] | 0xFC);		/* 0x3A-0x3F: not keys */
	b[8] = 0xFF;				/* not read */
	t_rig_from_psoc(rig.sim.now, 0x41, 0x42, b, 7);	/* short: ignored */
	CHECK_EQ(s07_poll(&rig.psoc), S07_OK);
	CHECK_EQ(rig.nkev, 0u);
	t_rig_from_psoc(rig.sim.now, 0x41, 0x42, b, 10);
	CHECK_EQ(s07_poll(&rig.psoc), S07_OK);
	CHECK_EQ(rig.nkev, 1u);
	CHECK_EQ(rig.kev[0][0], 0x39u);
	CHECK_EQ(rig.kev[0][1], 1u);
	/* the key-status reply: 8 bytes kept, the rest cleared */
	{
		struct s07_key_status ks;

		s07_key_status_given(&rig.psoc, b, 10, &ks);
		CHECK(ks.hold);
		CHECK_EQ(rig.psoc.keys[7], 0x02u);
		CHECK_EQ(rig.psoc.keys[8], 0u);
	}
}

static void run_requests_1f(const struct s07v_leds *v)
{
	for (size_t o = 0; o < v->ops_len; o += 4) {
		const uint8_t *op = v->ops + o;

		if (op[0] == 0)
			s07_led_bar(&rig.psoc, op[1]);
		else if (op[0] == 1)
			s07_led_bar_clear(&rig.psoc);
		else
			s07_led_rgb(&rig.psoc, op + 1, false);
	}
}

static void check_sent(size_t from, const uint8_t *sent, size_t len, size_t n)
{
	size_t at = 0;

	CHECK_EQ(rig.nitems - from, n);
	for (size_t j = from; j < rig.nitems && at < len; j++) {
		CHECK_EQ(rig.item[j].id, 0x40);
		CHECK_MEM(rig.item[j].d, rig.item[j].len, sent + at + 1, sent[at]);
		at += 1u + sent[at];
	}
}

/* S-07.046, S-07.048: the forms, the change rule, the clear */
static void test_leds_1f(void)
{
	static const uint8_t forced_red[3] = { 0xC8, 0, 0 };

	for (size_t i = 0; i < s07v1f_bars_count; i++) {
		t_case = "leds-1f.json forms bar";
		t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
		rig.psoc.led_bar = -1;		/* so that n = 0 is sent too */
		s07_led_bar(&rig.psoc, s07v1f_bars[i].n);
		CHECK_EQ(rig.nitems, 1u);
		CHECK_MEM(rig.item[0].d, rig.item[0].len, s07v1f_bars[i].p, s07v1f_bars[i].len);
	}
	for (size_t i = 0; i < s07v1f_rgbs_count; i++) {
		t_case = "leds-1f.json forms rgb";
		t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
		rig.psoc.led_rgb[0] = 0x01;	/* so that 000000 is sent too */
		s07_led_rgb(&rig.psoc, s07v1f_rgbs[i].rgb, false);
		CHECK_EQ(rig.nitems, 1u);
		CHECK_MEM(rig.item[0].d, rig.item[0].len, s07v1f_rgbs[i].p, s07v1f_rgbs[i].len);
	}
	for (size_t i = 0; i < s07v1f_leds_count; i++) {
		t_case = "leds-1f.json change_only";
		t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
		run_requests_1f(&s07v1f_leds[i]);
		check_sent(0, s07v1f_leds[i].sent, s07v1f_leds[i].sent_len, s07v1f_leds[i].n_sent);
	}
	/* no forced message, no key LED, no {01}; a bar of more than 14 is 14 */
	t_case = "leds-1f no other form";
	t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
	s07_led_rgb(&rig.psoc, forced_red, true);
	s07_led_rgb(&rig.psoc, forced_red, true);
	CHECK_EQ(rig.nitems, 1u);
	s07_led_key(&rig.psoc, 3, 0x00, 0xFF);
	s07_led_all_off(&rig.psoc);
	CHECK_EQ(rig.nitems, 1u);
	s07_led_bar(&rig.psoc, 20);
	CHECK_EQ(rig.nitems, 2u);
	CHECK_EQ(rig.item[1].len, 15u);
	CHECK_MEM(rig.item[1].d, rig.item[1].len, s07v1f_bars[s07v1f_bars_count - 1u].p, 15);
	s07_led_bar_clear(&rig.psoc);
	CHECK_MEM(rig.item[2].d, rig.item[2].len, s07v1f_bar_clear, sizeof s07v1f_bar_clear);
	/* the first service-loop pass sends no 0x40 (S-07.043 is key-leds) */
	t_case = "leds-1f first pass";
	t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
	CHECK_EQ(s07_service_pass(&rig.psoc), S07_OK);
	CHECK_EQ(rig.nitems, 0u);
	/* the shutdown's reports: {00 00 02} alone (S-07.072) */
	s07_shutdown_reports(&rig.psoc);
	CHECK_EQ(rig.nitems, 1u);
	CHECK_EQ(rig.item[0].id, 0x51);
}

/* S-07.049 */
static void test_key_test_1f(void)
{
	for (size_t i = 0; i < s07v1f_kts_count; i++) {
		const struct s07v1f_kt *v = &s07v1f_kts[i];
		uint8_t bm[8];

		t_case = "leds-1f.json key_test";
		t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
		rig.psoc.state = v->state;
		rig.psoc.batt.present = true;
		rig.psoc.batt.charge = v->charge;
		memset(bm, 0, sizeof bm);
		s07_key_test_start(&rig.psoc);
		for (size_t e = 0; e + 1u < v->ev_len + 1u && e < v->ev_len; e += 2) {
			const unsigned k = v->ev[e];

			if (v->ev[e + 1])
				bm[k >> 3] = (uint8_t)(bm[k >> 3] | 1u << (k & 7u));
			else
				bm[k >> 3] = (uint8_t)(bm[k >> 3] & ~(1u << (k & 7u)));
			t_rig_from_psoc(rig.sim.now, 0x41, 0x42, bm, sizeof bm);
			CHECK_EQ(s07_poll(&rig.psoc), S07_OK);
		}
		s07_key_test_stop(&rig.psoc);
		check_sent(0, v->sent, v->sent_len, v->n_sent);
	}
}

/* S-07.047: steady colors through the module */
static void test_charge_led_1f(void)
{
	for (size_t i = 0; i < s07v1f_steady_count; i++) {
		const struct s07v1f_steady *v = &s07v1f_steady[i];

		t_case = "charge_led-1f.json steady";
		t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
		rig.psoc.led_rgb[0] = 0x01;	/* so that 000000 is sent too */
		rig.psoc.state = v->state;
		rig.psoc.batt.present = true;
		rig.psoc.batt.charge = v->charge;
		s07_update_charge_led(&rig.psoc);
		CHECK_EQ(rig.nitems, 1u);
		CHECK_MEM(rig.item[0].d, rig.item[0].len, v->p, v->len);
		s07_update_charge_led(&rig.psoc);
		CHECK_EQ(rig.nitems, 1u);	/* only changes, nothing forced */
	}
	/* the breathing: each level within the tolerance at each update */
	for (size_t i = 0; i < s07v1f_breathe_count; i++) {
		const struct s07v1f_breathe *v = &s07v1f_breathe[i];
		size_t n = v->t_len / 4u;

		t_case = "charge_led-1f.json breathing";
		t_rig_init_dev(S06_MS(100000), &wb_device_op1f);
		rig.psoc.state = S07_POWER_CHARGE;
		rig.psoc.batt.present = true;
		rig.psoc.batt.charge = v->charge;
		for (size_t r = 0; r < n; r++) {
			const uint32_t ms = (uint32_t)v->t_le32[4 * r] | (uint32_t)v->t_le32[4 * r + 1] << 8 |
					    (uint32_t)v->t_le32[4 * r + 2] << 16 |
					    (uint32_t)v->t_le32[4 * r + 3] << 24;
			size_t before = rig.nitems;

			rig.sim.now = S06_MS(ms);
			s07_update_charge_led(&rig.psoc);
			for (unsigned c = 0; c < 3u; c++) {
				const int want = v->levels[3 * r + c];
				const int got = rig.psoc.led_rgb[c];

				CHECK(got >= want - S07V1F_TOLERANCE && got <= want + S07V1F_TOLERANCE);
			}
			/* a message only on a change, in the 7-byte form */
			if (rig.nitems != before) {
				CHECK_EQ(rig.nitems, before + 1u);
				CHECK_EQ(rig.item[before].len, 7u);
				CHECK_EQ(rig.item[before].d[0], 0x06);
			}
		}
	}
	/* the rule's functions: the range, and no step after a gap */
	CHECK_EQ(s07_breathe_level(0xC8, 0x40000000u), 0xC8u);	/* sin = 1 */
	CHECK_EQ(s07_breathe_level(0xC8, 0xC0000000u), 0x32u);	/* sin = -1 */
	CHECK_EQ(s07_breathe_level(0xC8, 0), 125u);
	CHECK_EQ(s07_breathe_step(12345u, S07_BREATHE_GAP_US + 1u), 12345u);
	CHECK(s07_breathe_step(0, S07_BREATHE_GAP_US) != 0u);
	for (uint32_t i = 0, ph = 0; i < 256u; i++, ph += 0x01000000u) {
		const uint8_t l = s07_breathe_level(0xC8, ph);

		CHECK(l >= 0x32u && l <= 0xC8u);
	}
}

/* S-07.048: the bar clear after the battery wait's loop; none without it */
static void test_bar_clear_1f(void)
{
	static const uint8_t usable[7] = { 0x01, 50, 25, 0, 0, 0x0F, 0xA0 };

	t_case = "bar-clear-1f none";
	t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
	rig.psoc.state = S07_POWER_ON;
	s07_battery_decode(usable, sizeof usable, &rig.psoc.batt);
	CHECK_EQ(s07_battery_rest(&rig.psoc), S07_OK);
	for (size_t j = 0; j < rig.nitems; j++)
		CHECK(!(rig.item[j].id == 0x40 && rig.item[j].d[0] == 0x02));

	/* a flat battery: the loop runs until a usable report; then one clear,
	 * after the queries, before the charge LED */
	t_case = "bar-clear-1f after the loop";
	t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
	rig.psoc.state = S07_POWER_ON;
	t_rig_from_psoc(S06_MS(30), 0x41, 0x37, usable, sizeof usable);
	CHECK_EQ(s07_battery_rest(&rig.psoc), S07_OK);
	{
		size_t clear = rig.nitems, rgb = rig.nitems, bars = 0;

		for (size_t j = 0; j < rig.nitems; j++) {
			if (rig.item[j].id != 0x40)
				continue;
			if (rig.item[j].d[0] == 0x02) {
				bars++;
				clear = j;
				CHECK_MEM(rig.item[j].d, rig.item[j].len, s07v1f_bar_clear,
					  sizeof s07v1f_bar_clear);
			} else if (rig.item[j].d[0] == 0x06) {
				rgb = j;
			}
		}
		CHECK_EQ(bars, 1u);
		CHECK(clear < rig.nitems);
		/* the charge LED was set by the report in the loop; after the
		 * clear only an RGB message may follow, here none (no change) */
		(void)rgb;
		for (size_t j = clear + 1u; j < rig.nitems; j++)
			CHECK(rig.item[j].id != 0x40 || rig.item[j].d[0] == 0x06);
		CHECK(rig.psoc.led_rgb[1] == 0xC8u);
	}
}

/* S-07.062: no PMIC sequence and no ping on the OP-1F */
static void test_pmic_1f(void)
{
	t_case = "pmic-1f";
	t_rig_init_dev(S06_MS(10000), &wb_device_op1f);
	CHECK_EQ(s07_pmic(&rig.psoc), S07_OK);
	CHECK_EQ(rig.nitems, 0u);
	CHECK_EQ(s07_handover(&rig.psoc), S07_OK);
	CHECK_EQ(rig.nitems, 1u);
	CHECK_EQ(rig.item[0].id, 0x51);
	CHECK_EQ(rig.item[0].d[2], 0x32);
}

/* trace_rules-1f.json: no_pmic_ids's findings (pmic) or no_key_led_ids's in
 * the master's frames so far. */
static size_t rule_findings(bool pmic)
{
	size_t n = 0;

	for (size_t j = 0; j < rig.nitems; j++) {
		const struct t_item *it = &rig.item[j];

		if (it->raw)
			continue;
		if (pmic) {
			for (size_t k = 0; k < sizeof s07v_no_pmic_ids; k++)
				n += it->id == s07v_no_pmic_ids[k];
		} else if (it->id == S07V_LED_ID && it->len != 0u) {
			for (size_t k = 0; k < sizeof s07v_no_key_led_firsts; k++)
				n += it->d[0] == s07v_no_key_led_firsts[k];
		}
	}
	return n;
}

/* One session through every message the PSoC module sends: the greeting,
 * the battery wait's requests, the subscription and the key status, the
 * battery's rest, the first service passes, each LED request, a key test
 * with a press, the charge LED in each state, a ping answered, the USB and
 * calibration messages, the reports, the PMIC step and the hand-over. */
static void session(const struct wb_device *dev)
{
	static const uint8_t usable[7] = { 0x01, 50, 25, 0, 0, 0x0F, 0xA0 };
	static const uint8_t greet_ok[1] = { 0x40 }, ping[1] = { 0xAA };
	static const uint8_t red[3] = { 0xC8, 0, 0 };
	struct s07_key_status ks;
	uint8_t keys[9];
	bool greeted;

	t_rig_init_dev(S06_MS(100000), dev);
	t_rig_from_psoc(S06_MS(1), 0x41, 0x06, greet_ok, sizeof greet_ok);
	CHECK_EQ(s07_greet(&rig.psoc, &greeted), S07_OK);
	CHECK(greeted);
	s07_report(&rig.psoc, S07_REPORT_START);
	CHECK_EQ(s07_battery_wait(&rig.psoc), S07_OK);
	s07_subscribe(&rig.psoc);
	CHECK_EQ(s07_key_status(&rig.psoc, &ks), S07_OK);
	rig.psoc.state = S07_POWER_ON;
	s07_battery_decode(usable, sizeof usable, &rig.psoc.batt);
	CHECK_EQ(s07_battery_rest(&rig.psoc), S07_OK);
	CHECK_EQ(s07_service_pass(&rig.psoc), S07_OK);
	CHECK_EQ(s07_service_pass(&rig.psoc), S07_OK);
	s07_led_key(&rig.psoc, 3, 0x00, 0xFF);
	s07_led_all_off(&rig.psoc);
	s07_led_bar(&rig.psoc, 5);
	s07_led_bar_clear(&rig.psoc);
	s07_led_rgb(&rig.psoc, red, true);
	s07_key_test_start(&rig.psoc);
	memset(keys, 0, sizeof keys);
	keys[2] = 0x01;
	t_rig_from_psoc(rig.sim.now, 0x41, 0x42, keys, sizeof keys);
	CHECK_EQ(s07_poll(&rig.psoc), S07_OK);
	keys[2] = 0;
	t_rig_from_psoc(rig.sim.now, 0x41, 0x42, keys, sizeof keys);
	CHECK_EQ(s07_poll(&rig.psoc), S07_OK);
	s07_key_test_stop(&rig.psoc);
	rig.psoc.state = S07_POWER_CHARGE;
	s07_update_charge_led(&rig.psoc);
	rig.psoc.state = S07_POWER_ON;
	s07_update_charge_led(&rig.psoc);
	t_rig_from_psoc(rig.sim.now, 0x41, 0x01, ping, sizeof ping);
	CHECK_EQ(s07_poll(&rig.psoc), S07_OK);
	s07_volcal(&rig.psoc);
	s07_usb_configured(&rig.psoc, 500u);
	s07_usb_reset(&rig.psoc);
	s07_report(&rig.psoc, S07_REPORT_HELD);
	CHECK_EQ(s07_pmic(&rig.psoc), S07_OK);
	CHECK_EQ(s07_handover(&rig.psoc), S07_OK);
	s07_shutdown_reports(&rig.psoc);
}

/* S-18.020, scenario SC-18-FEATURES: the S-07 trace rules no_pmic_ids and
 * no_key_led_ids pass on the OP-1F's session; on the OP-XY's the same
 * session fails both (the negative control). */
static void test_trace_rules(void)
{
	size_t pong = 0;

	t_case = "trace rules OP-1F";
	session(&wb_device_op1f);
	CHECK(rig.nitems > 20u);
	CHECK_EQ(rule_findings(true), 0u);
	CHECK_EQ(rule_findings(false), 0u);
	for (size_t j = 0; j < rig.nitems; j++)
		pong += !rig.item[j].raw && rig.item[j].id == 0x02;
	CHECK_EQ(pong, 1u);			/* the PSoC's ping answered */
	t_case = "trace rules OP-XY, negative control";
	session(&wb_device_opxy);
	CHECK(rule_findings(true) != 0u);
	CHECK(rule_findings(false) != 0u);
}

int main(void)
{
	test_bitmaps();
	test_status();
	test_after_status();
	test_leds();
	test_charge_led();
	test_power();
	test_pmic();
	test_status_1f();
	test_bitmaps_1f();
	test_leds_1f();
	test_key_test_1f();
	test_charge_led_1f();
	test_bar_clear_1f();
	test_pmic_1f();
	test_trace_rules();
	return t_finish("test_s07_rules");
}
