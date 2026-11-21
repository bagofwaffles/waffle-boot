/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-08 reading rules against tests/oracle/S-08-nordic/vectors: S-08.010-S-08.013
 * (greeting.json), S-08.020-S-08.022 (queries.json), S-08.031 step 4
 * (greeting.json accept), S-08.040 (dfu_replies.json), S-08.003, S-08.030,
 * S-08.032 (messages.json), S-08.043-S-08.046 (transfers.json); and S-08.004
 * forwarding before and after the link greeting.
 */
#include "waffle/s07_psoc.h"

#include "s08_test.h"
#include "vectors/s08_vectors.h"

/* S-08.010-S-08.013 through the module: answer, revision, one per greeting */
static void test_greeting(void)
{
	for (size_t i = 0; i < s08v_greet_count; i++) {
		const struct s08v_greet *v = &s08v_greet[i];
		uint8_t raw[256];
		size_t n;

		t_case = i < S08V_SEQUENCE_FIRST ? "greeting.json rows" :
						   "greeting.json sequence";
		t_nrig_init(v->state, S06_MS(1000));
		t_port_inject_frame(&nrig.port, 0, 0x31, 9, 0x05, v->p, v->len);
		s08_poll(&nrig.nordic);
		n = t_nth_raw(nrig.port.tx, nrig.port.ntx, 0, raw);
		CHECK_EQ(n != 0, v->answered);
		if (n && v->answered) {
			CHECK_EQ(raw[0], 0x13);
			CHECK_EQ(raw[2], v->id);
			CHECK_EQ(raw[3], 1);
			CHECK_EQ(raw[4], v->byte);
			CHECK_EQ(t_nth_raw(nrig.port.tx, nrig.port.ntx, 1, raw), 0);
		}
		if (v->revision >= 0)
			CHECK_EQ(nrig.revision, v->revision);
		if (!v->answered || v->len < 2)
			CHECK_EQ(nrig.revision, -1);
	}
	/* S-08.013: the state in force when the greeting is handled */
	t_case = "greeting state changes";
	t_nrig_init(S08_STATE_IDLE, S06_MS(1000));
	{
		static const uint8_t bb[2] = { 0xBB, 0x02 };
		uint8_t raw[256];
		const int states[3] = { 0, 0xAB0D, 0 };
		const uint8_t want[3] = { 0x18, 0xEE, 0x18 };

		for (int k = 0; k < 3; k++) {
			/* held mode between them, so each starts a new run of
			 * greetings (S-08.014) */
			s08_idle(&nrig.nordic, S06_MS(100));
			s08_set_state(&nrig.nordic, states[k]);
			t_port_inject_frame(&nrig.port, nrig.sim.now, 0x31, 0, 0x05, bb, 2);
			s08_poll(&nrig.nordic);
			CHECK(t_nth_raw(nrig.port.tx, nrig.port.ntx, (size_t)k, raw) == 6);
			CHECK_EQ(raw[4], want[k]);
		}
	}
}

/* S-08.014: between an install run's steps the greetings that arrived are
 * answered, by the state in force (S-08.013), and nothing else is handled:
 * other messages keep their order for held mode's loop. */
static unsigned commons;

static void count_common(void *ctx, const struct s06_frame *f)
{
	(void)ctx;
	(void)f;
	commons++;
}

static void test_step_greetings(void)
{
	static const uint8_t bb[2] = { 0xBB, 0x02 };
	struct s06_frame f;
	uint8_t raw[256];

	t_case = "greetings between steps";
	t_nrig_init(S08_STATE_UPGRADING, S06_MS(1000));
	nrig.ops.common = count_common;
	commons = 0;
	t_port_inject_frame(&nrig.port, 0, 0x31, 0, 0x01, 0, 0);
	t_port_inject_frame(&nrig.port, 0, 0x31, 1, 0x05, bb, 2);
	t_port_inject_frame(&nrig.port, 0, 0x31, 2, 0x02, 0, 0);
	s08_answer_greetings(&nrig.nordic);
	CHECK(t_nth_raw(nrig.port.tx, nrig.port.ntx, 0, raw) == 6);
	CHECK_EQ(raw[2], 0x0D);
	CHECK_EQ(raw[4], 0xEE);			/* enter DFU: a run is on */
	CHECK_EQ(t_nth_raw(nrig.port.tx, nrig.port.ntx, 1, raw), 0);
	CHECK_EQ(nrig.revision, 2);
	CHECK_EQ(commons, 0u);
	CHECK(s06_link_take(&nrig.link, &f) && f.id == 0x01);
	CHECK(s06_link_take(&nrig.link, &f) && f.id == 0x02);
	CHECK(!s06_link_take(&nrig.link, &f));
	/* nothing waiting: nothing sent */
	s08_answer_greetings(&nrig.nordic);
	CHECK_EQ(t_nth_raw(nrig.port.tx, nrig.port.ntx, 1, raw), 0);
}

/* S-08.014: one answer per greeting run */
static int drops_logged;

static void count_drops(void *ctx, int ev, const uint8_t *d, size_t n, uint32_t a)
{
	(void)ctx;
	(void)d;
	(void)n;
	(void)a;
	if (ev == S08_LOG_GREETING_DROPPED)
		drops_logged++;
}

/* The ids and first payload bytes of what the module sent the Nordic. */
static size_t sent_list(uint8_t ids[], uint8_t first[], size_t max)
{
	uint8_t raw[256];
	size_t k = 0;

	while (k < max && t_nth_raw(nrig.port.tx, nrig.port.ntx, k, raw) >= 5) {
		ids[k] = raw[2];
		first[k] = raw[3] ? raw[4] : 0;
		k++;
	}
	return k;
}

/* Answers 0x09 with mode:bootloader until mode_until_us, then
 * mode:application, 1 ms after each query. */
static uint32_t mode_until_us;

static void mode_peer(void *ctx, struct t_port *p, const uint8_t *d, size_t n)
{
	static const char boot[] = "mode:bootloader", app[] = "mode:application";
	uint8_t raw[256];
	const uint32_t t = p->sim->now;
	const bool in_dfu = !s06_time_reached(t, mode_until_us);

	(void)ctx;
	if (t_nth_raw(d, n, 0, raw) < 5 || raw[2] != 0x09)
		return;
	t_port_inject_frame(p, t + 1000u, s06_swap_route(raw[0]), 0, 0x0A,
			    (const uint8_t *)(in_dfu ? boot : app),
			    in_dfu ? sizeof boot - 1u : sizeof app - 1u);
}

/* The longest time between two of the clock's idles while on. */
static uint32_t tick_last, tick_gap;
static bool tick_on;

static void gap_tick(void *ctx)
{
	const uint32_t t = ((struct t_sim *)ctx)->now;

	if (tick_on && t - tick_last > tick_gap)
		tick_gap = t - tick_last;
	tick_last = t;
}

/* S-08.014: an answer at a step's start, then a long step that does not
 * read the link, with a greeting 50 ms into it: the link is read through
 * the 20 ms after the answer, every 1 ms, before the step goes on, so the
 * greeting is judged after the window and answered at the next step's
 * start. */
static void test_step_window(void)
{
	static const uint8_t bb[2] = { 0xBB, 0x02 };
	uint8_t ids[64] = { 0 }, first[64] = { 0 };
	uint32_t step;

	t_case = "greeting during a long step";
	t_nrig_init(S08_STATE_UPGRADING, S06_MS(10000));
	nrig.ops.log = count_drops;
	drops_logged = 0;
	nrig.sim.tick = gap_tick;
	nrig.sim.tick_ctx = &nrig.sim;
	tick_on = true;
	tick_gap = 0;
	tick_last = 0;
	t_port_inject_frame(&nrig.port, 0, 0x31, 0, 0x05, bb, 2);
	s08_answer_greetings(&nrig.nordic);		/* step 1 starts */
	tick_on = false;
	CHECK_EQ(sent_list(ids, first, 64), 1);
	CHECK_EQ(nrig.nordic.answered_at, 0);
	CHECK(nrig.sim.now > S06_MS(20));
	CHECK(nrig.link.read_at > S06_MS(20));
	CHECK(tick_gap <= S06_MS(2));
	step = nrig.sim.now;				/* step 1's own work */
	t_port_inject_frame(&nrig.port, step + S06_MS(50), 0x31, 1, 0x05, bb, 2);
	nrig.sim.now = step + S06_MS(3000);
	s08_answer_greetings(&nrig.nordic);		/* step 2 starts */
	CHECK_EQ(sent_list(ids, first, 64), 2);
	CHECK(ids[1] == 0x0D && first[1] == 0xEE);
	CHECK_EQ(drops_logged, 0);
	nrig.sim.tick = 0;
}

static void test_one_answer(void)
{
	static const uint8_t bb[2] = { 0xBB, 0x02 }, aa[2] = { 0xAA, 0x02 },
			     zero[2] = { 0x00, 0x02 };
	uint8_t ids[64] = { 0 }, first[64] = { 0 };
	size_t k;

	t_case = "greeting_due";
	CHECK(s08_greeting_due(false, 0, 0));
	CHECK(!s08_greeting_due(true, 1000, 1000));
	CHECK(!s08_greeting_due(true, 1000, 500));		/* before the answer */
	CHECK(!s08_greeting_due(true, 1000, 1000 + S06_MS(20)));
	CHECK(s08_greeting_due(true, 1000, 1001 + S06_MS(20)));
	CHECK(s08_greeting_due(true, 0xFFFFF000u, 0x00010000u));	/* wrap */
	CHECK(!s08_greeting_due(true, 0x00001000u, 0xFFFFF000u));

	/* two greetings delivered before either is handled: one answer */
	t_case = "two queued greetings";
	t_nrig_init(S08_STATE_UPGRADING, S06_MS(1000));
	nrig.ops.log = count_drops;
	drops_logged = 0;
	t_port_inject_frame(&nrig.port, 0, 0x31, 0, 0x05, bb, 2);
	t_port_inject_frame(&nrig.port, 0, 0x31, 1, 0x05, bb, 2);
	(void)s08_poll(&nrig.nordic);
	k = sent_list(ids, first, 64);
	CHECK_EQ(k, 1);
	CHECK_EQ(first[0], 0xEE);
	CHECK_EQ(drops_logged, 1);
	/* the same between an install run's steps */
	t_case = "two queued greetings between steps";
	s08_idle(&nrig.nordic, S06_MS(100));
	t_port_inject_frame(&nrig.port, nrig.sim.now, 0x31, 2, 0x05, bb, 2);
	t_port_inject_frame(&nrig.port, nrig.sim.now, 0x31, 3, 0x05, bb, 2);
	s08_answer_greetings(&nrig.nordic);
	CHECK_EQ(sent_list(ids, first, 64), 2);
	CHECK_EQ(drops_logged, 2);

	/* in held mode: 10 ms after an answer dropped, and a dropped one
	 * is no answer: 30 ms after the answer it is answered again */
	t_case = "greetings 10 and 30 ms after an answer";
	t_nrig_init(S08_STATE_IDLE, S06_MS(1000));
	nrig.ops.log = count_drops;
	drops_logged = 0;
	t_port_inject_frame(&nrig.port, S06_MS(10), 0x31, 0, 0x05, bb, 2);
	t_port_inject_frame(&nrig.port, S06_MS(20), 0x31, 1, 0x05, bb, 2);
	t_port_inject_frame(&nrig.port, S06_MS(40), 0x31, 2, 0x05, bb, 2);
	t_port_inject_frame(&nrig.port, S06_MS(45), 0x31, 3, 0x05, bb, 2);
	s08_idle(&nrig.nordic, S06_MS(100));
	CHECK_EQ(sent_list(ids, first, 64), 2);
	CHECK_EQ(drops_logged, 2);
	CHECK(nrig.nordic.answered_at >= S06_MS(40) &&
	      nrig.nordic.answered_at <= S06_MS(41));

	/* an answer long past is forgotten, so the clock's wrap never makes it
	 * recent again */
	t_case = "answer forgotten";
	CHECK(!nrig.nordic.answered);
	nrig.sim.now = S06_MS(40) + 0x90000000u;
	t_port_inject_frame(&nrig.port, nrig.sim.now, 0x31, 4, 0x05, bb, 2);
	(void)s08_poll(&nrig.nordic);
	CHECK_EQ(sent_list(ids, first, 64), 3);

	/* the frames already delivered are handled before the command */
	t_case = "greeting handled before enter";
	t_nrig_init(S08_STATE_UPGRADING, S06_MS(50000));
	t_port_inject_frame(&nrig.port, 0, 0x31, 0, 0x05, zero, 2);
	(void)s08_enter_dfu(&nrig.nordic);
	CHECK(sent_list(ids, first, 64) >= 2);
	CHECK(ids[0] == 0x06 && first[0] == 0x40);
	CHECK(ids[1] == 0x0D && first[1] == 0xEE);
	CHECK_EQ(nrig.revision, 2);

	t_case = "greeting handled before exit";
	t_nrig_init(S08_STATE_IDLE, S06_MS(50000));
	t_port_inject_frame(&nrig.port, 0, 0x31, 0, 0x05, zero, 2);
	(void)s08_exit_dfu(&nrig.nordic);
	CHECK(sent_list(ids, first, 64) >= 2);
	CHECK(ids[0] == 0x06 && first[0] == 0x40);
	CHECK(ids[1] == 0x0D && first[1] == 0x18);

	/* S-08.033: a greeting 5 ms after the answer sent before the command is
	 * dropped and does not end phase 1; the exit succeeds there */
	t_case = "dropped greeting in the exit exchange";
	t_nrig_init(S08_STATE_IDLE, S06_MS(50000));
	nrig.ops.log = count_drops;
	drops_logged = 0;
	nrig.port.on_tx = mode_peer;
	mode_until_us = S06_MS(50);
	t_port_inject_frame(&nrig.port, 0, 0x31, 0, 0x05, aa, 2);
	t_port_inject_frame(&nrig.port, S06_MS(5), 0x31, 1, 0x05, aa, 2);
	CHECK(s08_exit_dfu(&nrig.nordic));
	k = sent_list(ids, first, 64);
	CHECK(k >= 3);
	CHECK(ids[0] == 0x06 && first[0] == 0x40);
	CHECK(ids[1] == 0x0D && first[1] == 0x18);
	for (size_t i = 2; i < k; i++)
		CHECK_EQ(ids[i], 0x09);
	CHECK_EQ(drops_logged, 1);
	CHECK(!nrig.nordic.connected);
	/* phase 1's queries ran to the first "not in DFU", 1-2 ms apart */
	CHECK(k - 2 > 15);
}

/* S-08.022 through the module: a query with no slot leaves a log event
 * with the reply as data, or with none when nothing came back. */
static int slot_ev, slot_len;
static uint32_t slot_a;

static void slot_log(void *ctx, int ev, const uint8_t *d, size_t n, uint32_t a)
{
	(void)ctx;
	(void)d;
	if (ev != S08_LOG_SLOT_UNREAD)
		return;
	slot_ev++;
	slot_len = (int)n;
	slot_a = a;
}

static void test_slot_unread(void)
{
	t_case = "slot answer not read";
	t_nrig_init(S08_STATE_UPGRADING, S06_MS(1000));
	nrig.ops.log = slot_log;
	slot_ev = 0;
	CHECK_EQ(s08_boot_slot_query(&nrig.nordic), -1);	/* no reply */
	CHECK(slot_ev == 1 && slot_len == 0 && slot_a == 0u);
	CHECK(!nrig.nordic.replied);
}

static void test_accept(void)
{
	t_case = "greeting.json accept";
	for (size_t i = 0; i < s08v_accept_count; i++) {
		struct s06_frame f;

		f.id = s08v_accept[i].id;
		f.len = (uint8_t)s08v_accept[i].len;
		memcpy(f.payload, s08v_accept[i].p, f.len);
		CHECK_EQ(s08_counts_as_greeting(&f), s08v_accept[i].counts);
	}
}

static void test_queries(void)
{
	t_case = "queries.json mode";
	for (size_t i = 0; i < s08v_mode_count; i++) {
		const struct s08v_mode *v = &s08v_mode[i];

		CHECK_EQ(s08_mode_answer(v->none ? 0 : v->p, v->len), v->answer);
	}
	t_case = "queries.json boot_slot";
	for (size_t i = 0; i < s08v_slot_count; i++) {
		const struct s08v_slot *v = &s08v_slot[i];

		long got = s08_boot_slot(v->none ? 0 : v->p, v->len);

		if (v->slot == -2)	/* "other": any slot but 0 and 1, or none */
			CHECK(got != 0 && got != 1);
		else
			CHECK_EQ(got, v->slot);
	}
}

static void test_dfu_replies(void)
{
	t_case = "dfu_replies.json";
	for (size_t i = 0; i < s08v_dfu_count; i++) {
		const struct s08v_dfu *v = &s08v_dfu[i];

		CHECK_EQ(s08_classify_dfu(v->cmd, v->id, v->p, v->len), v->cls);
		/* the error text: bytes 1.. of a failing 0x0E reply */
		CHECK_EQ(v->has_text, v->cls == S08_DFU_FAIL && v->id == 0x0E &&
					      v->len > 1);
		if (v->has_text)
			CHECK_MEM(v->p + 1, v->len - 1, v->text, v->text_len);
	}
}

/* S-08.003, S-08.030, S-08.032: the frames at seq 0 */
static void test_messages(void)
{
	for (size_t i = 0; i < s08v_msgs_count; i++) {
		const struct s08v_msg *v = &s08v_msgs[i];

		t_case = v->name;
		t_nrig_init(0, S06_MS(1000));
		if (!strcmp(v->name, "enter") || !strcmp(v->name, "exit")) {
			uint8_t b = !strcmp(v->name, "enter") ? 0xEE : 0x18;

			/* the command's first frame: the exchange starts with it */
			nrig.sim.cap = S06_MS(50000);
			if (b == 0xEE)
				(void)s08_enter_dfu(&nrig.nordic);
			else
				(void)s08_exit_dfu(&nrig.nordic);
		} else if (!strcmp(v->name, "accept")) {
			t_port_inject_frame(&nrig.port, 0, 0x31, 0, 0x05, 0, 0);
			s08_poll(&nrig.nordic);
		} else if (!strcmp(v->name, "info_request_slot0")) {
			uint8_t buf[8];

			(void)s08_info(&nrig.nordic, buf, sizeof buf);
		} else {
			nrig.sim.cap = S06_MS(10000);
			(void)s08_link_greeting(&nrig.nordic);
		}
		CHECK(nrig.port.ntx >= v->len);
		CHECK_MEM(nrig.port.tx, v->len, v->wire, v->len);
	}
}

static void test_transfers(void)
{
	static uint8_t img[1000];
	uint8_t out[64];

	for (size_t i = 0; i < sizeof img; i++)
		img[i] = (uint8_t)(i * 7 + 3);
	t_case = "crc16 check";
	CHECK_EQ(t_crc16_xmodem((const uint8_t *)"123456789", 9), 0x31C3);
	t_case = "transfers.json prepare";
	for (size_t i = 0; i < s08v_prepare_count; i++) {
		const struct s08v_prepare *v = &s08v_prepare[i];

		s08_prepare_payload(v->version, v->type, v->product, out);
		CHECK_MEM(out, 14, v->p, v->len);
	}
	t_case = "transfers.json header";
	for (size_t i = 0; i < s08v_header_count; i++) {
		const struct s08v_header *v = &s08v_header[i];

		s08_te_header(img, v->image_len, v->version, v->product,
			      t_crc16_xmodem, out);
		CHECK_MEM(out, 64, v->h, v->len);
		CHECK_EQ(t_crc16_xmodem(img, v->image_len), v->crc);
	}
	t_case = "transfers.json product_word";
	for (size_t i = 0; i < s08v_pw_count; i++)
		CHECK_EQ(s08_product_word(wb_device_opxy.ident.product_base, s08v_pw[i].sku,
					  s08v_pw[i].read_ok),
			 s08v_pw[i].word);

	/* S-08.045: the sku is read once per power-on; one set later in the
	 * power-on is used from the next (SC-08-TRANSFER's two power-ons) */
	t_case = "transfers.json product_word_power_ons";
	{
		static struct s08_nordic nd;
		static struct s06_node node;	/* no routes to switch */
		static const uint16_t ver[4] = { 1, 1, 0, 0 };
		uint8_t pp[14];

		for (size_t i = 0; i < s08v_pw_on_count; i++) {
			const struct s08v_pw_on *v = &s08v_pw_on[i];

			s08_init(&nd, &node, 0, NULL, 0);	/* a power-on */
			CHECK_EQ(s08_product(&nd), 0u);
			s08_set_sku(&nd, &wb_device_opxy, v->boot_sku, true);
			if (v->set_sku >= 0)		/* set metadata, held mode */
				s08_set_sku(&nd, &wb_device_opxy, (uint32_t)v->set_sku, true);
			CHECK_EQ(s08_product(&nd), v->word);
			s08_prepare_payload(ver, 0xB0, s08_product(&nd), pp);
			CHECK_MEM(pp, sizeof pp, v->prepare, v->len);
		}
		s08_init(&nd, &node, 0, NULL, 0);
		s08_set_sku(&nd, &wb_device_opxy, 7, false);	/* unread: 0 for the power-on */
		s08_set_sku(&nd, &wb_device_opxy, 7, true);
		CHECK_EQ(s08_product(&nd), 0u);
	}

	/* SC-08-TRANSFER-1F: transfers-1f.json, device.product_base 0x00008000 */
	CHECK_EQ(wb_device_op1f.ident.product_base, S08V1F_PRODUCT_BASE);
	t_case = "transfers-1f.json prepare";
	for (size_t i = 0; i < s08v1f_prepare_count; i++) {
		const struct s08v_prepare *v = &s08v1f_prepare[i];

		s08_prepare_payload(v->version, v->type, v->product, out);
		CHECK_MEM(out, 14, v->p, v->len);
	}
	t_case = "transfers-1f.json header";
	for (size_t i = 0; i < s08v1f_header_count; i++) {
		const struct s08v_header *v = &s08v1f_header[i];

		s08_te_header(img, v->image_len, v->version, v->product,
			      t_crc16_xmodem, out);
		CHECK_MEM(out, 64, v->h, v->len);
	}
	t_case = "transfers-1f.json product_word";
	for (size_t i = 0; i < s08v1f_pw_count; i++)
		CHECK_EQ(s08_product_word(wb_device_op1f.ident.product_base, s08v1f_pw[i].sku,
					  s08v1f_pw[i].read_ok),
			 s08v1f_pw[i].word);
	t_case = "transfers-1f.json product_word_power_ons";
	{
		static struct s08_nordic nd;
		static struct s06_node node;
		static const uint16_t ver[4] = { 1, 1, 0, 0 };
		uint8_t pp[14];

		for (size_t i = 0; i < s08v1f_pw_on_count; i++) {
			const struct s08v_pw_on *v = &s08v1f_pw_on[i];

			s08_init(&nd, &node, 0, NULL, 0);
			s08_set_sku(&nd, &wb_device_op1f, v->boot_sku, true);
			if (v->set_sku >= 0)
				s08_set_sku(&nd, &wb_device_op1f, (uint32_t)v->set_sku, true);
			CHECK_EQ(s08_product(&nd), v->word);
			s08_prepare_payload(ver, 0xB0, s08_product(&nd), pp);
			CHECK_MEM(pp, sizeof pp, v->prepare, v->len);
		}
	}
}

/* S-08.046 through the module: chunk payload lengths, contents in order */
static uint8_t chunk_lens[64][2];
static size_t nchunks;

static void dfu_peer(void *ctx, struct t_port *p, const uint8_t *d, size_t n)
{
	uint8_t raw[256];
	size_t k = t_nth_raw(d, n, 0, raw);

	(void)ctx;
	if (k < 5)
		return;
	if (raw[2] == 0x09) {
		static const char t[] = "mode:bootloader";

		t_port_inject_frame(p, p->sim->now + 100, s06_swap_route(raw[0]), 0,
				    0x0A, (const uint8_t *)t, sizeof t - 1);
	} else if (raw[2] == 0x0D) {
		if (raw[4] == 0x03 && nchunks < 64) {
			chunk_lens[nchunks][0] = raw[3];
			chunk_lens[nchunks][1] = raw[5];	/* first data byte */
			nchunks++;
		}
		static const uint8_t reply[1] = { 0x54 };

		t_port_inject_frame(p, p->sim->now + 100, s06_swap_route(raw[0]), 0,
				    0x0E, reply, 1);
	}
}

static void test_chunks(void)
{
	static uint8_t img[1000];
	static const uint16_t ver[4] = { 1, 0, 0, 0 };

	for (size_t i = 0; i < sizeof img; i++)
		img[i] = (uint8_t)(i * 7 + 3);
	t_case = "transfers.json chunks";
	for (size_t i = 0; i < s08v_chunks_count; i++) {
		const struct s08v_chunks *v = &s08v_chunks[i];

		t_nrig_init(S08_STATE_UPGRADING, S06_MS(60000));
		nrig.port.on_tx = dfu_peer;
		nchunks = 0;
		CHECK(s08_transfer(&nrig.nordic, img, v->image_len, ver, 0xB0,
				   0x00084000));
		/* the header chunk, then the image chunks */
		CHECK_EQ(nchunks, 1 + v->len / 2);
		CHECK_EQ(chunk_lens[0][0], 65);
		for (size_t c = 0; c + 1 < nchunks && c < v->len / 2; c++) {
			CHECK_EQ(chunk_lens[c + 1][0], s06_rd_be16(v->lens_be16 + 2 * c));
			CHECK_EQ(chunk_lens[c + 1][1], img[248 * c]);
		}
	}
}

/*
 * S-08.048: a Nordic in DFU that answers every DFU request T, except the
 * refuse_nth request (from 1) whose first byte is refuse_cmd: {0x84}.
 * Queries are answered mode:bootloader, or mode:application with not_dfu.
 */
static uint8_t refuse_cmd;
static int refuse_nth, refuse_seen;
static bool not_dfu;
static int left_out_logged;

static void no_app_peer(void *ctx, struct t_port *p, const uint8_t *d, size_t n)
{
	static const char boot[] = "mode:bootloader", app[] = "mode:application";
	uint8_t raw[256];
	size_t k = t_nth_raw(d, n, 0, raw);
	uint8_t reply = 0x54;

	(void)ctx;
	if (k < 5)
		return;
	if (raw[2] == 0x09) {
		const char *t = not_dfu ? app : boot;

		t_port_inject_frame(p, p->sim->now + 100, s06_swap_route(raw[0]), 0,
				    0x0A, (const uint8_t *)t, strlen(t));
		return;
	}
	if (raw[2] != 0x0D || s06_src(raw[0]) < 8)	/* a request */
		return;
	if (raw[4] == refuse_cmd && ++refuse_seen == refuse_nth)
		reply = 0x84;
	t_port_inject_frame(p, p->sim->now + 100, s06_swap_route(raw[0]), 0,
			    0x0E, &reply, 1);
}

static void count_left_out(void *ctx, int ev, const uint8_t *d, size_t n,
			   uint32_t a)
{
	(void)ctx;
	(void)d;
	(void)n;
	(void)a;
	if (ev == S08_LOG_EXIT_LEFT_OUT)
		left_out_logged++;
}

/* One transfer of a 600-byte image (the header chunk and 3 image chunks)
 * against no_app_peer; refuse_nth 0 refuses nothing. */
static bool no_app_transfer(uint8_t type, uint8_t cmd, int nth)
{
	static uint8_t img[600];
	static const uint16_t ver[4] = { 1, 1, 0, 0 };

	refuse_cmd = cmd;
	refuse_nth = nth;
	refuse_seen = 0;
	return s08_transfer(&nrig.nordic, img, sizeof img, ver, type, 0x00084000);
}

static void no_app_rig(void)
{
	t_nrig_init(S08_STATE_UPGRADING, S06_MS(400000));
	nrig.port.on_tx = no_app_peer;
	nrig.ops.log = count_left_out;
	not_dfu = false;
	left_out_logged = 0;
}

static void test_no_app(void)
{
	static const struct {
		const char *name;
		uint8_t cmd;
		int nth;
		enum s08_stage at;
	} after[] = {
		{ "prepare refused", S08_DFU_PREPARE, 1, S08_AT_PREPARE },
		{ "header refused", S08_DFU_CHUNK, 1, S08_AT_HEADER },
		{ "image chunk 2 refused", S08_DFU_CHUNK, 3, S08_AT_CHUNK },
		{ "perform refused", S08_DFU_PERFORM, 1, S08_AT_PERFORM },
	};
	uint8_t ids[64], first[64];
	size_t k;

	/* S-08.048: set by an application transfer failing at or after its
	 * prepare, whatever the stage; S-11.070's case is the same stop */
	for (size_t i = 0; i < sizeof after / sizeof after[0]; i++) {
		t_case = after[i].name;
		no_app_rig();
		CHECK(!nrig.nordic.no_app);
		CHECK(!no_app_transfer(S08_TYPE_APP, after[i].cmd, after[i].nth));
		CHECK_EQ(nrig.nordic.failure.at, after[i].at);
		CHECK(s08_after_prepare(&nrig.nordic.failure));
		CHECK(nrig.nordic.no_app);
		/* a bootloader image's failure leaves it as it was */
		no_app_rig();
		CHECK(!no_app_transfer(S08_TYPE_BOOTLOADER, after[i].cmd,
				       after[i].nth));
		CHECK(s08_after_prepare(&nrig.nordic.failure));
		CHECK(!nrig.nordic.no_app);
	}

	/* no prepare sent: the step's queries do not give "in DFU" */
	t_case = "not in DFU";
	no_app_rig();
	not_dfu = true;
	CHECK(!no_app_transfer(S08_TYPE_APP, 0, 0));
	CHECK_EQ(nrig.nordic.failure.at, S08_AT_QUERY);
	CHECK(!s08_after_prepare(&nrig.nordic.failure));
	CHECK(!nrig.nordic.no_app);
	{
		struct s08_failure f = { S08_AT_NONE, S08_REPLY_NONE, 0, 0 };

		CHECK(!s08_after_prepare(&f));
	}

	/* cleared by an application transfer that succeeds, not by a
	 * bootloader's */
	t_case = "cleared by success";
	no_app_rig();
	CHECK(!no_app_transfer(S08_TYPE_APP, S08_DFU_CHUNK, 2));
	CHECK(nrig.nordic.no_app);
	CHECK(no_app_transfer(S08_TYPE_BOOTLOADER, 0, 0));
	CHECK(nrig.nordic.no_app);
	CHECK(no_app_transfer(S08_TYPE_APP, 0, 0));
	CHECK(!nrig.nordic.no_app);
	CHECK(!no_app_transfer(S08_TYPE_APP, S08_DFU_PERFORM, 1));
	CHECK(nrig.nordic.no_app);

	/* while set: the exit sends nothing and is logged as left out */
	t_case = "exit left out";
	nrig.port.ntx = 0;
	CHECK(!s08_exit_dfu(&nrig.nordic));
	CHECK_EQ(nrig.port.ntx, 0);
	CHECK_EQ(left_out_logged, 1);
	/* the run's end: its query, and no exit */
	t_case = "run end left out";
	CHECK(!s08_run_end(&nrig.nordic));
	k = sent_list(ids, first, 64);
	CHECK(k == 1 && ids[0] == 0x09);
	CHECK_EQ(left_out_logged, 2);
	/* the enter is still sent */
	t_case = "enter still sent";
	nrig.port.ntx = 0;
	(void)s08_enter_dfu(&nrig.nordic);
	k = sent_list(ids, first, 64);
	CHECK(k >= 1 && ids[0] == 0x0D && first[0] == 0xEE);
	CHECK(nrig.nordic.no_app);

	/* a power-on starts without it */
	t_case = "init";
	s08_init(&nrig.nordic, &nrig.node, S06_LINK_NORDIC, &nrig.ops, 0);
	CHECK(!nrig.nordic.no_app);
	nrig.port.on_tx = NULL;
	nrig.port.ntx = 0;
	(void)s08_exit_dfu(&nrig.nordic);
	k = sent_list(ids, first, 64);
	CHECK(k >= 1 && ids[0] == 0x0D && first[0] == 0x18);
}

/* S-08.004: nothing forwarded between CDC and Nordic before the greeting */
static void test_forwarding(void)
{
	uint8_t w[S06_MAX_WIRE];
	size_t n = s06_frame_encode(0x23, 4, 0x01, 0, 0, t_crc8, w);

	t_case = "forwarding";
	t_nrig_init(0, S06_MS(10000));
	t_port_inject(&nrig.cdc, 0, w, n);
	s06_node_poll(&nrig.node);
	CHECK_EQ(nrig.port.ntx, 0);
	/* the Nordic answers the third greeting request */
	CHECK_EQ(nrig.nordic.greet_tries, 0u);
	/* the tries made: every one of S-08.002's 1000 when none is answered */
	if (!s08_link_greeting(&nrig.nordic))
		CHECK_EQ(nrig.nordic.greet_tries, 1000u);
	else
		CHECK(nrig.nordic.greet_tries >= 1u && nrig.nordic.greet_tries < 1000u);
	t_port_inject(&nrig.cdc, nrig.sim.now, w, n);
	s06_node_poll(&nrig.node);
	CHECK(nrig.port.ntx >= n);
	CHECK_MEM(nrig.port.tx + nrig.port.ntx - n, n, w, n);
	n = s06_frame_encode(0x32, 4, 0x02, 0, 0, t_crc8, w);
	t_port_inject(&nrig.port, nrig.sim.now, w, n);
	s06_node_poll(&nrig.node);
	CHECK_MEM(nrig.cdc.tx, nrig.cdc.ntx, w, n);
	/* the link greeting is made once in a power-on */
	{
		size_t before = nrig.port.ntx;

		CHECK(!s08_link_greeting(&nrig.nordic));
		CHECK_EQ(nrig.port.ntx, before);
	}
}

/* S-08.004: the Nordic's power messages are handled as S-07's */
static struct s07_psoc psoc;

static int to_psoc(void *ctx, const struct s06_frame *f)
{
	(void)ctx;
	return s07_power_message(&psoc, f);
}

static void test_power_messages(void)
{
	static struct t_port pport;
	static struct s06_link plink;
	static struct s07_ops pops;
	static const uint8_t on[1] = { 0x10 }, off[1] = { 0x14 };
	static const uint8_t batt[7] = { 0x01, 100, 25, 0, 100, 0x10, 0x68 };
	uint8_t raw[256];

	t_case = "nordic power messages";
	t_nrig_init(0, S06_MS(10000));
	t_port_init(&pport, &nrig.sim);
	s06_link_init(&plink, &t_port_ops, &pport, S06_ADDR_MASTER, S06_ADDR_PSOC);
	nrig.node.link[S06_LINK_PSOC] = &plink;
	memset(&pops, 0, sizeof pops);
	s07_init(&psoc, &wb_device_opxy, &nrig.node, S06_LINK_PSOC, &pops);
	nrig.ops.power = to_psoc;

	/* 0x31 {10}: state on, charge 0: the charge LED goes red */
	t_port_inject_frame(&nrig.port, 0, 0x31, 0, 0x31, on, 1);
	CHECK_EQ(s08_poll(&nrig.nordic), 0);
	CHECK_EQ(t_nth_raw(pport.tx, pport.ntx, 0, raw), 9);
	CHECK_EQ(raw[2], 0x40);
	CHECK_EQ(raw[4], 0x03);
	CHECK_EQ(raw[5], 0xC8);
	/* 0x37: charge 100 %: green */
	t_port_inject_frame(&nrig.port, 0, 0x31, 1, 0x37, batt, 7);
	CHECK_EQ(s08_poll(&nrig.nordic), 0);
	CHECK_EQ(t_nth_raw(pport.tx, pport.ntx, 1, raw), 9);
	CHECK_EQ(raw[5], 0x00);
	CHECK_EQ(raw[6], 0xC8);
	/* 0x31 {14}: the shutdown reports, then 0x32 {14}, on the PSoC link */
	t_port_inject_frame(&nrig.port, 0, 0x31, 2, 0x31, off, 1);
	CHECK_EQ(s08_poll(&nrig.nordic), S07_POWEROFF);
	CHECK_EQ(t_nth_raw(pport.tx, pport.ntx, 2, raw), 8);
	CHECK_EQ(raw[2], 0x51);
	CHECK_EQ(raw[6], 0x02);
	CHECK_EQ(t_nth_raw(pport.tx, pport.ntx, 3, raw), 6);
	CHECK_EQ(raw[2], 0x40);
	CHECK_EQ(t_nth_raw(pport.tx, pport.ntx, 4, raw), 6);
	CHECK_EQ(raw[2], 0x32);
	CHECK_EQ(raw[4], 0x14);
	CHECK_EQ(t_nth_raw(pport.tx, pport.ntx, 5, raw), 0);
	/* nothing to the Nordic in answer */
	CHECK_EQ(nrig.port.ntx, 0);
}

int main(void)
{
	test_power_messages();
	test_greeting();
	test_step_greetings();
	test_one_answer();
	test_step_window();
	test_slot_unread();
	test_accept();
	test_queries();
	test_dfu_replies();
	test_messages();
	test_transfers();
	test_chunks();
	test_no_app();
	test_forwarding();
	return t_finish("test_s08_rules");
}
