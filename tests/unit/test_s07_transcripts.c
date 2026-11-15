/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07 timed transcripts (tests/oracle/S-07-psoc/transcripts): the master's frames
 * and raw writes are compared, in order, with the transcript's (log lines
 * left out, kind compared rather than slot, as timed_check does), and every
 * timing check is applied to the observed times. The PSoC's side replays
 * the transcript's PSoC events, each released relative to the master item
 * before it, replies addressed to the slot the implementation chose.
 *
 * The driver below calls the S-07 functions in the order S-07.023 gives,
 * standing in for the bootloader's phases (S-05 boot decision, held mode,
 * S-15 actions) the way s07_model does.
 *
 * Requirements: S-07.001-S-07.004 (S-07.003: ids the PSoC accepts), S-07.011, S-07.012, S-07.020-S-07.025,
 * S-07.031, S-07.032, S-07.040-S-07.045, S-07.051-S-07.058, S-07.061,
 * S-05.025 and S-15.180 (step 6b, the boot prompt's transcripts),
 * S-07.062, S-07.065-S-07.067, S-07.070-S-07.072, S-07.075-S-07.078,
 * S-07.080-S-07.086.
 */
#include "waffle/s07_program.h"
#include "waffle/s15_prompt.h"

#include "s07_test.h"
#include "vectors/s07_vectors.h"
#include "vectors/s07_transcripts.h"

static const uint8_t synth_unlock[6] = { 0x0A, 0x1B, 0x2C, 0x3D, 0x4E, 0x5F };

static const struct s07t *cur;
static uint32_t p_time[4096];
static size_t next_action;
static int halted;

/* Release the PSoC events anchored to master item k (or, k = -1, the
 * unanchored ones at the start). */
static void release(int k, uint32_t base)
{
	for (size_t i = 0; i < cur->pn; i++) {
		const struct s07t_p *e = &s07t_p[cur->p0 + i];
		uint32_t at;

		if (e->anchor != k)
			continue;
		at = base + (uint32_t)(e->delta_us < 0 ? 0 : e->delta_us);
		p_time[i] = at;
		if (e->raw) {
			t_port_inject(&rig.port, at, e->d, e->len);
		} else {
			uint8_t route = e->route;

			if (e->req >= 0 && (size_t)e->req < rig.nitems)
				route = (uint8_t)((route & 0xF0u) |
						  s06_src(rig.item[e->req].route));
			t_rig_from_psoc(at, route, e->id, e->d, e->len);
		}
	}
}

static void on_item(const struct t_rig *r, const struct t_item *it)
{
	release((int)(r->nitems - 1), it->t);
}

static int actions_due(int only_usb)
{
	int rc = S07_OK;

	while (!rc && next_action < cur->an) {
		const struct s07t_action *a = &s07t_actions[cur->a0 + next_action];
		int usb = strncmp(a->what, "usb_", 4) == 0;

		if (rig.sim.now < S06_MS(a->t_ms) || (only_usb && !usb))
			break;
		next_action++;
		if (!strcmp(a->what, "usb_config")) {
			s07_usb_configured(&rig.psoc, a->ma);
		} else if (!strcmp(a->what, "usb_reset")) {
			s07_usb_reset(&rig.psoc);
		} else if (!strcmp(a->what, "volcal")) {
			s07_volcal(&rig.psoc);
		} else if (!strcmp(a->what, "soft_reboot")) {
			s07_reset_request(&rig.psoc);
			s07_shutdown_reports(&rig.psoc);
			halted = 1;
		} else if (!strcmp(a->what, "key_test_start")) {
			s07_sleep(&rig.psoc, S06_MS(100));
			s07_key_test_start(&rig.psoc);
		} else if (!strcmp(a->what, "key_test_stop")) {
			s07_sleep(&rig.psoc, S06_MS(100));
			s07_key_test_stop(&rig.psoc);
		} else if (!strcmp(a->what, "continue_os")) {
			/* S-05.033 with the slave present: a key test ends
			 * with its LED messages (S-07.045) as the restarting
			 * view is drawn; the view's hold, nothing handled;
			 * then W11's shutdown reports (S-07.072) */
			if (rig.psoc.key_test)
				s07_key_test_stop(&rig.psoc);
			s07_sleep(&rig.psoc, S06_MS(750));
			s07_shutdown_reports(&rig.psoc);
			halted = 1;
		} else if (!strcmp(a->what, "backplane")) {
			bool ok;

			rc = s07_program(&rig.psoc, (const char *)cur->cyacd,
					 cur->cyacd_len, synth_unlock, &ok);
		} else {
			CHECK_FAIL("unknown action");
		}
	}
	return rc;
}

/* During the battery wait's rounds: USB events only (as the model). */
static void service(void *ctx)
{
	static int busy;

	(void)ctx;
	if (busy)
		return;
	busy = 1;
	(void)actions_due(1);
	busy = 0;
}

/*
 * S-05.025, S-07.023 step 6b: a build with the boot prompt waits for digit 1
 * or 2 (S-15.180) and meanwhile handles the PSoC's messages (S-07.076),
 * sending nothing else. The bitmaps waiting when the prompt opens are
 * taken into account first; the keys down then act after an up and a down.
 * Returns the choice, S15_PROMPT_WAIT when none came by end_ms.
 */
static struct s15_prompt *prompt_shown;

static void prompt_key(void *ctx, uint8_t key, bool down)
{
	t_rig_key(ctx, key, down);
	if (prompt_shown != NULL)
		s15_prompt_key(prompt_shown, key, down);
}

static enum s15_prompt_choice prompt_wait(int *rc)
{
	struct s15_prompt p;
	uint32_t end = cur->end_ms >= 0 ? S06_MS(cur->end_ms) :
					  rig.sim.now + S06_MS(200);

	rig.ops.key_event = prompt_key;
	*rc = s07_poll(&rig.psoc);
	if (*rc)
		return S15_PROMPT_WAIT;
	s15_prompt_open(&p, rig.psoc.keys);
	prompt_shown = &p;
	while (s15_prompt_choice(&p) == S15_PROMPT_WAIT && rig.sim.now < end) {
		*rc = s07_poll(&rig.psoc);
		if (*rc)
			break;
		s07_sleep(&rig.psoc, S06_MS(1));
	}
	prompt_shown = NULL;
	return s15_prompt_choice(&p);
}

static void drive(void)
{
	struct s07_key_status ks;
	uint8_t info[256];
	uint32_t end;
	int st;

	if (s07_startup(&rig.psoc, &ks))
		return;
	if (cur->boot_allowed && !ks.hold) {
		bool held = false;

		if (cur->boot_prompt) {
			int rc;
			enum s15_prompt_choice c = prompt_wait(&rc);

			if (rc)
				return;
			CHECK(c != S15_PROMPT_WAIT);
			if (c == S15_PROMPT_WAIT)
				return;
			held = c == S15_PROMPT_HELD;	/* step 8 */
		}
		if (!held) {
			if (s07_handover(&rig.psoc))
				return;
			if (cur->os_loads) {	/* S-07.071: then only log lines */
				s07_sleep(&rig.psoc, S06_MS(cur->tail_ms));
				return;
			}
		}
	}
	(void)s07_info_request(&rig.psoc, info, sizeof info, &st);
	s07_report(&rig.psoc, S07_REPORT_HELD);
	end = cur->end_ms >= 0 ? S06_MS(cur->end_ms) : rig.sim.now + S06_MS(200);
	while (rig.sim.now < end && !halted) {
		if (actions_due(0) || halted)
			return;
		if (s07_service_pass(&rig.psoc))
			return;
		s07_sleep(&rig.psoc, S06_MS(10));
	}
	if (!halted)
		(void)s07_poll(&rig.psoc);
}

static int kind_of(const struct t_item *it)
{
	if (it->raw)
		return 2;
	return s06_src(it->route) >= 8 && s06_dst(it->route) == 4;
}

/* S-07.002: in table 1 with an allowed payload, or the pong */
static int in_catalog(const struct t_item *it)
{
	static const char hex[] = "0123456789abcdef";

	if (it->id == 0x02 && kind_of(it) == 0)
		return 1;
	for (size_t i = 0; i < s07v_catalog_count; i++) {
		const struct s07v_cat *c = &s07v_catalog[i];
		size_t pl = strlen(c->pattern);
		int k = kind_of(it), ok = 1;

		if (c->id != it->id || !(k ? c->request : c->plain) ||
		    pl != 2u * it->len)
			continue;
		for (size_t j = 0; j < pl && ok; j++) {
			char want = c->pattern[j];
			char got = hex[(it->d[j / 2] >> (j % 2 ? 0 : 4)) & 0xF];

			ok = want == '.' || want == got;
		}
		if (ok)
			return 1;
	}
	return 0;
}

static void compare(void)
{
	size_t n = rig.nitems < cur->mn ? rig.nitems : cur->mn;

	CHECK_EQ(rig.nitems, cur->mn);
	for (size_t i = 0; i < n; i++) {
		const struct s07t_m *e = &s07t_m[cur->m0 + i];
		const struct t_item *it = &rig.item[i];

		CHECK_EQ(kind_of(it), e->kind);
		if (e->kind != 2)
			CHECK_EQ(it->id, e->id);
		CHECK_MEM(it->d, it->len, e->d, e->len);
		if (kind_of(it) != e->kind || (e->kind != 2 && it->id != e->id) ||
		    it->len != e->len || memcmp(it->d, e->d, e->len)) {
			fprintf(stderr, "  first difference at master item %zu (t %u us)\n",
				i, it->t);
			return;
		}
	}
	for (size_t i = 0; i < rig.nitems; i++) {
		const struct t_item *it = &rig.item[i];

		if (it->raw)
			continue;
		CHECK(in_catalog(it));
		/* S-07.003: an id the PSoC responds to, or 0x06 */
		CHECK(it->id == 0x06 || memchr(s07v_psoc_accepts, it->id,
					       sizeof s07v_psoc_accepts) != 0);
		/* S-06.012 on this link: 0x14, or 8-11 to 4, or the pong */
		CHECK(it->route == 0x14 || (s06_src(it->route) >= 8 &&
					    s06_dst(it->route) == 4) ||
		      it->id == 0x02);
	}
	for (size_t i = 0; i < cur->cn; i++) {
		const struct s07t_check *c = &s07t_checks[cur->c0 + i];
		uint32_t a = c->from_p ? p_time[c->from] : rig.item[c->from].t;
		uint32_t b = c->to_p ? p_time[c->to] : rig.item[c->to].t;
		int64_t d = (int64_t)b - (int64_t)a;

		if ((!c->from_p && (size_t)c->from >= rig.nitems) ||
		    (!c->to_p && (size_t)c->to >= rig.nitems)) {
			CHECK_FAIL("check refers to an item not observed");
			continue;
		}
		if (d < c->min_us - 1 || d > c->max_us + 1) {
			t_failures++;
			fprintf(stderr, "[%s] timing check %zu: %lld us not in [%d, %d]\n",
				cur->name, i, (long long)d, c->min_us, c->max_us);
		}
	}
}

/* S-07.084: the failed-run transcripts are among those replayed */
static void test_present(void)
{
	static const char *const names[] = {
		"backplane_run_fail", "backplane_run_retry",
		"backplane_run_retry_valid",
	};

	for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
		size_t i = 0;

		while (i < s07t_count && strcmp(s07t[i].name, names[k]))
			i++;
		t_case = names[k];
		CHECK(i < s07t_count);
	}
}

static uint32_t refused_log[3];

static void refusal_log(void *ctx, int event, uint32_t a, uint32_t b)
{
	(void)ctx;
	refused_log[0] = (uint32_t)event;
	refused_log[1] = a;
	refused_log[2] = b;
}

/* An image s07_program refuses: nothing reaches the PSoC (S-07.083,
 * s07_cyacd_check), the step fails, S-14 is told. */
static void test_refused(void)
{
	static const struct {
		const char *name, *text;
		int no_crc;
	} cases[] = {
		{ "refused: header", "2E12306900\r\n", 0 },
		{ "refused: type 2", "2E1230690002\r\n:0000050003AABBCCF0\r\n", 0 },
		{ "refused: line 2 malformed",
		  "2E1230690000\r\n:0000050003AABBCCF0\r\n:000006GG\r\n"
		  ":0000070003AABBCCEE\r\n", 0 },
		{ "refused: no rows", "2E1230690000\r\n", 0 },
		{ "refused: CRC type, no CRC",
		  "2E1230690001\r\n:0000050003AABBCCF0\r\n", 1 },
	};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		bool ok = true;

		t_case = cases[i].name;
		t_rig_init(S06_MS(10000));
		rig.ops.log = refusal_log;
		if (cases[i].no_crc)
			rig.ops.crc16 = 0;
		memset(refused_log, 0, sizeof refused_log);
		CHECK_EQ(s07_program(&rig.psoc, cases[i].text,
				     strlen(cases[i].text), synth_unlock, &ok),
			 S07_OK);
		CHECK(!ok);
		CHECK_EQ(rig.nitems, 0);
		CHECK_EQ(rig.sim.now, 0);
		CHECK_EQ(refused_log[0], S07_LOG_BACKPLANE);
		CHECK_EQ(refused_log[1], 0);
		CHECK_EQ(refused_log[2], S07_CY_WHY_FILE);
	}

	/* S-07.081: no PSoC unlock, nothing sent */
	t_case = "refused: no unlock";
	t_rig_init(S06_MS(10000));
	rig.ops.log = refusal_log;
	memset(refused_log, 0, sizeof refused_log);
	{
		static const char good[] = "2E1230690000\r\n:0000050003AABBCCF0\r\n";
		bool ok = true;

		CHECK_EQ(s07_program(&rig.psoc, good, sizeof good - 1, 0, &ok),
			 S07_OK);
		CHECK(!ok);
		CHECK_EQ(rig.nitems, 0);
		CHECK_EQ(refused_log[0], S07_LOG_BACKPLANE);
		CHECK_EQ(refused_log[2], S07_CY_WHY_UNLOCK);
	}

	/* S-07.051: after a power-off, nothing more */
	t_case = "refused: powered off";
	t_rig_init(S06_MS(10000));
	rig.psoc.off = true;
	{
		static const char good[] = "2E1230690000\r\n:0000050003AABBCCF0\r\n";
		bool ok = true;

		CHECK_EQ(s07_program(&rig.psoc, good, sizeof good - 1,
				     synth_unlock, &ok), S07_POWEROFF);
		CHECK(!ok);
		CHECK_EQ(rig.nitems, 0);
	}
}

/* Replay the transcript cur on a fresh rig; prepare adds to the rig first. */
static void replay(void (*prepare)(void))
{
	t_case = cur->name;
	t_rig_init(S06_MS(120000));
	rig.ops.service = service;
	rig.on_item = on_item;
	if (prepare)
		prepare();
	next_action = 0;
	halted = 0;
	memset(p_time, 0, sizeof p_time);
	release(-1, 0);
	drive();
	compare();
}

/*
 * S-07.054, S-05.004: a charge wait is dark and has no USB function. The
 * early logo comes once the switch is on, before the subscription
 * (S-07.025) and so before the wait loop's USB mass storage, which comes
 * after the key status (S-07.023); a run that never leaves 0x16 shows
 * nothing.
 */
static unsigned logo_calls, msc_on_calls, msc_before_logo;
static unsigned logo_state, logo_started, msc_unstarted;

static void count_logo(void *ctx)
{
	(void)ctx;
	logo_calls++;
	logo_state = rig.psoc.state;
	if (rig.psoc.started)
		logo_started++;
}

static void count_msc(void *ctx, bool on)
{
	(void)ctx;
	if (on) {
		msc_on_calls++;
		if (logo_calls == 0)
			msc_before_logo++;
		if (!rig.psoc.started)
			msc_unstarted++;
	}
}

static void prepare_charge(void)
{
	logo_calls = msc_on_calls = msc_before_logo = 0;
	logo_state = logo_started = msc_unstarted = 0;
	rig.ops.early_logo = count_logo;
	rig.ops.usb_msc = count_msc;
}

static void replay_named(const char *name)
{
	for (size_t i = 0; i < s07t_count; i++)
		if (strcmp(s07t[i].name, name) == 0) {
			cur = &s07t[i];
			replay(prepare_charge);
			return;
		}
	CHECK_FAIL(name);
}

static void test_charge_wait_dark(void)
{
	replay_named("charge_wait_flat");	/* 0x16, then 0x10; flat battery */
	CHECK_EQ(logo_calls, 1);
	CHECK_EQ(logo_state, 0x10);
	CHECK_EQ(logo_started, 0);
	CHECK_EQ(msc_on_calls, 1);
	CHECK_EQ(msc_before_logo, 0);
	CHECK_EQ(msc_unstarted, 0);
	replay_named("battery_wait_flat");	/* the keys, then the loop */
	CHECK_EQ(logo_calls, 1);
	CHECK_EQ(logo_started, 0);
	CHECK_EQ(msc_on_calls, 1);
	CHECK_EQ(msc_unstarted, 0);
	replay_named("battery_wait_charge");	/* 0x16, then 0x10; usable */
	CHECK_EQ(logo_calls, 1);
	CHECK_EQ(logo_state, 0x10);
	CHECK_EQ(msc_on_calls, 0);
	replay_named("charge_wait_unplug");	/* 0x16, then 0x14 */
	CHECK_EQ(logo_calls, 0);
	CHECK_EQ(msc_on_calls, 0);
	replay_named("boot_normal");		/* the logo as before */
	CHECK_EQ(logo_calls, 1);
}

/*
 * The S-14 sink on the PSoC link, for the step's result line: the line goes
 * through the S-06 node, which sends nothing on a raw link and keeps nothing
 * for later (S-06.062, S-14.027). Lines that reach the port are counted here
 * and kept out of the master's items, as the transcripts leave log lines out.
 */
#define T_ID_LOG 0x03

static unsigned result_lines;
static uint32_t result_a, result_b, result_t;
static size_t result_after;

static void result_sink(void *ctx, int event, uint32_t a, uint32_t b)
{
	const uint8_t text[] = { (uint8_t)a, (uint8_t)(b >> 8), (uint8_t)b };

	(void)ctx;
	if (event == S07_LOG_BACKPLANE)
		(void)s06_send_log(&rig.node, S06_LINK_PSOC, T_ID_LOG, text,
				   sizeof text);
}

static void result_tx(void *ctx, struct t_port *p, const uint8_t *d, size_t n)
{
	uint8_t raw[256];

	if (d[n - 1] == 0x00 && t_nth_raw(d, n, 0, raw) >= 5 &&
	    raw[2] == T_ID_LOG) {
		result_lines++;
		result_a = raw[4];
		result_b = (uint32_t)raw[5] << 8 | raw[6];
		result_t = rig.sim.now;
		result_after = rig.nitems;
		return;
	}
	t_rig_tx(ctx, p, d, n);
}

static void result_prepare(void)
{
	rig.ops.log = result_sink;
	rig.port.on_tx = result_tx;
}

/*
 * S-14.022, S-14.027, L-RAW: however the step ends, its result line reaches
 * the link, once, after the last raw write of the runs and before the return
 * wait's exit packets (S-07.085: the result does not depend on the greeting).
 */
static void test_result_line(void)
{
	static const struct {
		const char *name;
		uint32_t ok, at;
	} cases[] = {
		{ "backplane_ok", 1, 0 },
		{ "backplane_run_retry", 1, 0 },
		{ "backplane_run_fail", 0, S07_CY_VERIFY_ROW },
		{ "backplane_no_greet", 1, 0 },
		{ "backplane_enter_fail", 0, 0 },	/* no run: at 0, why 0 */
	};

	for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		for (size_t i = 0; i < s07t_count; i++) {
			if (strcmp(s07t[i].name, cases[k].name))
				continue;
			cur = &s07t[i];
			result_lines = 0;
			replay(result_prepare);
			t_case = cases[k].name;
			CHECK_EQ(result_lines, 1);
			if (result_lines != 1)
				continue;
			CHECK_EQ(result_a, cases[k].ok);
			CHECK_EQ(result_b >> 8, cases[k].at);
			CHECK(((result_b & 0xFFu) != 0) == (cases[k].at != 0));
			/* what comes after the line is the return wait's: no
			 * raw write for its first second (S-07.085) */
			for (size_t j = result_after; j < rig.nitems; j++) {
				if (rig.item[j].raw)
					CHECK(rig.item[j].t >=
					      result_t + S06_MS(1000));
			}
		}
	}
}

int main(int argc, char **argv)
{
	if (argc == 1) {
		test_present();
		test_refused();
		test_result_line();
		test_charge_wait_dark();
	}
	for (size_t i = 0; i < s07t_count; i++) {
		cur = &s07t[i];
		if (argc > 1 && strcmp(argv[1], cur->name))
			continue;
		replay(0);
	}
	return t_finish("test_s07_transcripts");
}
