/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11 runs and the boot after a reset during one. Checks S-11.038,
 * S-11.055, S-11.056, S-11.057, S-11.058, S-11.059, S-11.060, S-11.061,
 * S-11.062, S-11.063, S-11.064, S-11.065, S-11.067, S-11.068, S-11.069,
 * S-11.070, S-11.071, S-11.073, S-11.080, S-11.090, S-11.091,
 * S-11.093, S-11.094, S-11.095, S-11.096 (what the selection offers, its
 * answers, and a step's use of it). Power cuts are made by the fakes, which longjmp out of the
 * run after a given number of writes.
 */
#include <setjmp.h>
#include <string.h>

#include "waffle/flash/layout.h"
#include "waffle/install/package.h"
#include "waffle/install/run.h"

#include "s11_check.h"
#include "s11_ref.h"
#include "vectors/s11_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define B(r) (s11v_blob + (r).off)

/* ---- runs.tsv: the engine against scripted step results ---- */

struct log {
	char text[2048];
	size_t n;
	const struct s11v_run *v;
	unsigned next;
};

static struct log L;

static void say(const char *s)
{
	size_t k = strlen(s);

	if (L.n != 0 && L.n < sizeof(L.text) - 1)
		L.text[L.n++] = ';';
	if (L.n + k < sizeof(L.text)) {
		memcpy(L.text + L.n, s, k);
		L.n += k;
	}
	L.text[L.n] = '\0';
}

static bool l_meta_state(void *ctx, uint16_t state)
{
	(void)ctx;
	say(state == S11_STATE_UPGRADING ? "meta state=AB0D" : "meta state=0");
	return true;
}

static bool l_meta_complete(void *ctx, const struct s11_version *v)
{
	(void)ctx;
	(void)v;
	say("meta os_version=package,state=0");
	return true;
}

static bool l_record(void *ctx, uint8_t step)
{
	char s[32];
	uint8_t rec[2];

	(void)ctx;
	s11_record_bytes(step, rec);		/* S-11.062: the count is 0 */
	snprintf(s, sizeof(s), "progress %u %u", rec[0], rec[1]);
	say(s);
	return true;
}

static bool l_enter(void *ctx)
{
	(void)ctx;
	say("nordic enter_dfu");
	return L.v->nordic_dfu_ok != 0;
}

static bool l_versions(void *ctx)
{
	(void)ctx;
	say("nordic read_versions");
	return true;
}

static void l_exit(void *ctx)
{
	(void)ctx;
	say("nordic exit_dfu_if_in_dfu");
}

static enum s11_result l_step(void *ctx, unsigned step)
{
	(void)ctx;
	CHECK(L.next < L.v->nresults && step == L.next,
	      "%s: step %u ran", L.v->name, step);
	return L.next < L.v->nresults ? (enum s11_result)L.v->results[L.next++] :
					S11_INTERNAL;
}

static const struct s11_run_ops log_ops = {
	l_meta_state, l_meta_complete, l_record, l_enter, l_versions, l_exit,
	l_step, NULL,
};

static const char *end_name(enum s11_run_end e)
{
	switch (e) {
	case S11_RUN_DONE: return "done";
	case S11_RUN_NOTHING_TO_DO: return "nothing to do";
	case S11_RUN_FAILED: return "failed";
	default: return "?";
	}
}

static void test_runs(void)
{
	for (size_t i = 0; i < N(s11v_runs); i++) {
		const struct s11v_run *v = &s11v_runs[i];
		struct s11_run_cfg cfg = {
			v->content_only != 0, v->dfu_entry != 0,
			v->version_file != 0, { 1, 2, 0, 0 },
		};
		struct s11_run_result res;
		struct s11_staged st;
		struct s11_step_env env;
		const char *outcome;

		/* S-11.061 item 2, S-11.073, S-11.096: the DFU entry from the
		 * package's Nordic images, the switched-off steps and the
		 * selection */
		memset(&st, 0, sizeof(st));
		if (v->nordic) {
			st.size[S11_C_BLE] = st.size[S11_C_NORDIC_BL0] = 1u;
			st.size[S11_C_NORDIC_BL1] = 1u;
		}
		memset(&env, 0, sizeof(env));
		env.staged = &st;
		env.off = (uint8_t)v->steps_off;
		env.selected = (uint8_t)v->selected;
		CHECK(s11_run_nordic(&env) == (v->dfu_entry != 0),
		      "%s: the DFU entry", v->name);
		memset(&L, 0, sizeof(L));
		L.v = v;
		outcome = end_name(s11_run(&log_ops, &cfg, &res));
		CHECK(strcmp(outcome, v->outcome) == 0, "%s: %s, want %s", v->name,
		      outcome, v->outcome);
		CHECK(strcmp(L.text, v->writes) == 0, "%s: writes\n    got  %s\n    want %s",
		      v->name, L.text, v->writes);
	}
}

/* ---- interrupted.tsv, stock.tsv ---- */

/* S-11.080: a boot that found the state 0xAB0D makes one metadata write,
 * state 0, and nothing else: no step, no record, no Nordic exchange,
 * whatever the keys and the slave. A boot with the state 0 makes none. */
static void test_interrupted(void)
{
	for (size_t i = 0; i < N(s11v_interrupted); i++) {
		const struct s11v_interrupted *v = &s11v_interrupted[i];

		memset(&L, 0, sizeof(L));
		if (v->state == S11_STATE_UPGRADING)
			CHECK(s11_interrupted(&log_ops), "the write's result");
		CHECK(strcmp(L.text, v->writes) == 0, "interrupted row %zu: '%s', want '%s'",
		      i, L.text, v->writes);
		CHECK((v->state == S11_STATE_UPGRADING) == (v->notice != 0) &&
		      v->steps == 0 && v->nordic_dfu == 0 && v->record_access == 0,
		      "interrupted row %zu: the vector", i);
	}
	for (size_t i = 0; i < N(s11v_stock); i++) {
		const struct s11v_stock *v = &s11v_stock[i];

		CHECK(s11_return_to_stock(v->option, v->valid, v->waffle,
					  v->selected) == (v->stock != 0),
		      "stock row %zu", i);
		CHECK(!v->selected || (v->option && v->bootloader),
		      "stock row %zu: selected only when it can be (S-11.096)", i);
		CHECK(!v->waffle || v->bootloader,
		      "stock row %zu: only a present image is waffle-boot's", i);
	}
}

/* ---- steps.tsv: the step results ---- */

struct step_fake {
	const struct s11v_step *v;
	unsigned writes, sends, slots, contents;
	int sent_slot;
};

static struct step_fake S;

/* Step 0 as S-11.066 has it for the row's staging area. */
static enum s11_result s_content(void *ctx)
{
	(void)ctx;
	S.contents++;
	if (!S.v->content_dir)
		return S11_NOT_FOUND;
	if (S.v->move_fails)
		return S11_FAILURE;
	return S.v->files > 0 ? S11_OK : S11_NOT_FOUND;
}

/* S-11.095: an image whose check or read fails is neither written nor
 * sent; s_failure says which. */
static bool s_write(void *ctx)
{
	(void)ctx;
	if (S.v->staged != 0)
		return false;
	if (S.v->start != 0)	/* S-02.090: refused, nothing written */
		return false;
	S.writes++;
	return S.v->write_ok != 0;
}

/* S-11.068 item 5: the writer's refused start, told apart as the board's
 * writer tells it */
static enum s11_result s_failure(void *ctx)
{
	(void)ctx;
	if (S.v->staged != 0)
		return S.v->staged == 1 ? S11_BAD_FILE : S11_INTERNAL;
	return S.v->start == 1 && S.v->written_here && S.v->slot_matches ?
	       S11_SKIPPED : S11_FAILURE;
}

static void s_sw(void *ctx, const struct s11_version *v)
{
	(void)ctx;
	(void)v;
}

/* S-11.069, S-11.070: the image's check, then the step's own DFU-mode
 * queries (in_dfu): not in DFU fails with no DFU request sent. */
static bool s_send(void *ctx, enum s11_component c)
{
	(void)ctx;
	if (S.v->staged != 0 || !S.v->in_dfu)
		return false;
	S.sends++;
	S.sent_slot = c == S11_C_NORDIC_BL1;
	return S.v->write_ok != 0;
}

static int s_slot(void *ctx)
{
	(void)ctx;
	S.slots++;
	return S.v->slot == 255 ? -1 : S.v->slot;
}

static const struct s11_step_ops step_ops = {
	s_content, s_write, s_sw, s_write, s_failure, s_send, s_slot, s_write,
	NULL,
};

static void test_steps(void)
{
	static const enum s11_component comp[7] = {
		S11_COMPONENTS, S11_C_OS, S11_C_BOOTLOADER, S11_C_BLE,
		S11_C_NORDIC_BL0, S11_C_BACKPLANE, S11_COMPONENTS,
	};
	const struct s11_version pkg = { 1, 2, 0, 0 };

	for (size_t i = 0; i < N(s11v_steps); i++) {
		const struct s11v_step *v = &s11v_steps[i];
		struct s11_staged st;
		struct s17_copro copro;
		struct s11_step_env env;
		struct s11_version installed = pkg;
		enum s11_result r;

		memset(&st, 0, sizeof(st));
		memset(&S, 0, sizeof(S));
		S.v = v;
		st.content_dir = v->content_dir != 0;	/* its files: s_content */
		for (unsigned c = 0; c < S11_COMPONENTS; c++)
			st.version[c] = pkg;
		if (v->step == S11_STEP_BLE_BOOTLOADER) {
			st.size[S11_C_NORDIC_BL0] = v->present0 ? 1u : 0u;
			st.size[S11_C_NORDIC_BL1] = v->present1 ? 1u : 0u;
		} else if (v->step != S11_STEP_RESERVED &&
			   v->step != S11_STEP_CONTENT) {
			st.size[comp[v->step]] = v->present ? 1u : 0u;
		}
		/* rel: the package against what is installed */
		installed.minor = (uint16_t)(installed.minor - v->rel);
		s17_copro_init(&copro);
		copro.nordic_app = copro.nordic_bl = copro.psoc = installed;
		env.staged = &st;
		env.option = v->option != 0;	/* dev mode has no field: no effect */
		/* S-11.090: the signature state decides no step; each row
		 * under a state of its own */
		env.sig = (enum s11_sig)(i % 3u);
		env.forced = v->forced != 0;
		env.nordic_in_dfu = true;	/* the run-start answer decides nothing */
		env.copro = &copro;
		/* S-11.073: only this row's step is switched on or off */
		env.off = v->switched_on ? 0u : S11_STEP_BIT(v->step);
		/* S-11.096: only this row's step is selected or not */
		env.selected = v->selected ? S11_STEP_BIT(v->step) : 0u;
		r = s11_step_result(v->step, &env, &step_ops);
		CHECK(r == (enum s11_result)v->result,
		      "step row %zu (step %u): %d, want %u", i, v->step, (int)r,
		      v->result);
		if (v->step == S11_STEP_BOOTLOADER &&
		    (!v->option || !v->selected))
			CHECK(S.writes == 0, "row %zu: no flash access", i);
		if (!v->selected)	/* S-11.096: nothing written, sent or moved */
			CHECK(S.writes == 0 && S.sends == 0 && S.slots == 0 &&
			      (S.contents == 0 || !v->content_dir),
			      "row %zu: a step not selected reaches nothing", i);
		if (v->step == S11_STEP_RESERVED)	/* S-11.057: nothing */
			CHECK(S.writes == 0 && S.sends == 0 && S.slots == 0,
			      "row %zu: the reserved step reaches nothing", i);
		if (!v->switched_on)	/* S-11.073: nothing sent, no slot asked */
			CHECK(S.writes == 0 && S.sends == 0 && S.slots == 0,
			      "row %zu: a switched-off step reaches nothing", i);
		{
			/* S-11.058: the reason a Skipped is reported with */
			enum s11_skip want = S11_SKIP_NONE;

			if (r == S11_SKIPPED)
				want = !v->switched_on ? S11_SKIP_SWITCHED_OFF :
				       v->step == S11_STEP_BOOTLOADER &&
				       !v->option ? S11_SKIP_OPTION_OFF :
				       !v->selected ? S11_SKIP_NOT_SELECTED :
						       S11_SKIP_ALREADY_WRITTEN;
			CHECK(s11_result_skip(v->step, &env, r) == want,
			      "row %zu: skip reason %d, want %d", i,
			      (int)s11_result_skip(v->step, &env, r), (int)want);
		}
		if (v->staged != 0)	/* S-11.095: nothing written or sent */
			CHECK(S.writes == 0 && S.sends == 0,
			      "row %zu: a refused image reaches nothing", i);
		if (r == S11_OK && v->step == S11_STEP_BLE_BOOTLOADER)
			CHECK(S.sent_slot == v->slot, "row %zu: the slot named", i);
		if (v->step == S11_STEP_BLE_BOOTLOADER && r == S11_FAILURE &&
		    S.sends != 0)
			CHECK(s11_version_cmp(&copro.nordic_bl, &installed) == 0,
			      "row %zu: S-16.032, version unchanged", i);
	}
}

/*
 * S-11.068: a write S-02.091 refuses reports Bad file, any other failure
 * Failure (S-02.090's refusal included).
 */
static enum s11_result bad_file(void *ctx)
{
	(void)ctx;
	return S11_BAD_FILE;
}

static enum s11_result read_error(void *ctx)
{
	(void)ctx;
	return S11_INTERNAL;
}

static enum s11_result written(void *ctx)
{
	(void)ctx;
	return S11_SKIPPED;
}

static void test_protected_results(void)
{
	static const struct s11v_step fail = { 0 };
	struct s11_step_ops ops = step_ops;
	struct s11_staged st;
	struct s11_step_env env;
	struct s17_copro copro;

	memset(&st, 0, sizeof(st));
	memset(&S, 0, sizeof(S));
	S.v = &fail;			/* every write fails */
	st.size[S11_C_BOOTLOADER] = 1u;
	s17_copro_init(&copro);
	memset(&env, 0, sizeof(env));
	env.staged = &st;
	env.option = true;
	env.sig = S11_SIG_VALID;
	env.selected = 0x3Fu;	/* every step selected */
	env.copro = &copro;	/* env.off 0: every step switched on */
	CHECK(s11_step_result(S11_STEP_BOOTLOADER, &env, &ops) == S11_FAILURE,
	      "no write_failure: Failure");
	ops.write_failure = bad_file;
	CHECK(s11_step_result(S11_STEP_BOOTLOADER, &env, &ops) == S11_BAD_FILE,
	      "S-02.091: Bad file");
	/* S-11.068 item 5: the bootloader step alone takes Skipped from its
	 * writer, with the reason already written */
	ops.write_failure = written;
	CHECK(s11_step_result(S11_STEP_BOOTLOADER, &env, &ops) == S11_SKIPPED &&
	      s11_result_skip(S11_STEP_BOOTLOADER, &env, S11_SKIPPED) ==
		      S11_SKIP_ALREADY_WRITTEN,
	      "already written: Skipped");
	CHECK(s11_result_skip(S11_STEP_BOOTLOADER, &env, S11_OK) ==
		      S11_SKIP_NONE &&
	      s11_result_skip(S11_STEP_BOOTLOADER, &env, S11_FAILURE) ==
		      S11_SKIP_NONE, "no reason unless Skipped");
	ops.write_failure = bad_file;
	CHECK(s11_step_result(S11_STEP_RESERVED, &env, &ops) == S11_NOT_FOUND,
	      "the reserved step: Not found");

	/* Every step that writes or sends a staged image: Bad file when
	 * write_failure says its check failed, Internal error when a read of
	 * it failed (S-11.095), else the step's own result (S-11.067,
	 * S-11.069-S-11.071). */
	st.size[S11_C_OS] = st.size[S11_C_BLE] = 1u;
	st.size[S11_C_NORDIC_BL0] = st.size[S11_C_BACKPLANE] = 1u;
	env.forced = true;			/* no version test */
	env.nordic_in_dfu = true;
	ops.write_failure = NULL;
	CHECK(s11_step_result(S11_STEP_OS, &env, &ops) == S11_INTERNAL,
	      "OS write fails: Internal error");
	CHECK(s11_step_result(S11_STEP_BLE, &env, &ops) == S11_FAILURE,
	      "Nordic transfer fails: Failure");
	CHECK(s11_step_result(S11_STEP_BLE_BOOTLOADER, &env, &ops) == S11_FAILURE,
	      "Nordic bootloader transfer fails: Failure");
	CHECK(s11_step_result(S11_STEP_KEYBOARD, &env, &ops) == S11_FAILURE,
	      "PSoC programming fails: Failure");
	ops.write_failure = bad_file;
	CHECK(s11_step_result(S11_STEP_OS, &env, &ops) == S11_BAD_FILE,
	      "OS image refused: Bad file");
	CHECK(s11_step_result(S11_STEP_BLE, &env, &ops) == S11_BAD_FILE,
	      "Nordic image refused: Bad file");
	CHECK(s11_step_result(S11_STEP_BLE_BOOTLOADER, &env, &ops) == S11_BAD_FILE,
	      "Nordic bootloader image refused: Bad file");
	CHECK(s11_step_result(S11_STEP_KEYBOARD, &env, &ops) == S11_BAD_FILE,
	      "PSoC image refused: Bad file");
	ops.write_failure = read_error;
	CHECK(s11_step_result(S11_STEP_OS, &env, &ops) == S11_INTERNAL,
	      "OS image unreadable: Internal error");
	CHECK(s11_step_result(S11_STEP_BOOTLOADER, &env, &ops) == S11_INTERNAL,
	      "bootloader image unreadable: Internal error");
	CHECK(s11_step_result(S11_STEP_BLE, &env, &ops) == S11_INTERNAL,
	      "Nordic image unreadable: Internal error");
	CHECK(s11_step_result(S11_STEP_KEYBOARD, &env, &ops) == S11_INTERNAL,
	      "PSoC image unreadable: Internal error");
}

/* ---- S-11.073: the switched-off steps; S-11.058: why a step skipped ---- */

static struct {
	unsigned os, bootloader, sends, slots, psoc, contents;
} C;

static enum s11_result c_content(void *ctx)
{
	(void)ctx;
	C.contents++;
	return S11_OK;
}

static bool c_os(void *ctx)
{
	(void)ctx;
	C.os++;
	return true;
}

static bool c_bootloader(void *ctx)
{
	(void)ctx;
	C.bootloader++;
	return true;
}

static bool c_send(void *ctx, enum s11_component c)
{
	(void)ctx;
	(void)c;
	C.sends++;
	return true;
}

static int c_slot(void *ctx)
{
	(void)ctx;
	C.slots++;
	return 0;
}

static bool c_psoc(void *ctx)
{
	(void)ctx;
	C.psoc++;
	return true;
}

static const struct s11_step_ops count_ops = {
	c_content, c_os, s_sw, c_bootloader, NULL, c_send, c_slot,
	c_psoc, NULL,
};

/* Every component staged, version 1.2.0, the installed ones older. */
static void all_staged(struct s11_staged *st, struct s17_copro *copro)
{
	memset(st, 0, sizeof(*st));
	for (unsigned c = 0; c < S11_COMPONENTS; c++) {
		st->size[c] = 1u;
		st->version[c] = (struct s11_version){ 1, 2, 0, 0 };
	}
	st->has_fw_version = true;
	st->fw_version = (struct s11_version){ 1, 2, 0, 0 };
	s17_copro_init(copro);
}

static void test_switched_off(void)
{
	static const unsigned switchable[] = {
		S11_STEP_BLE, S11_STEP_BLE_BOOTLOADER, S11_STEP_KEYBOARD,
	};
	struct s11_staged st;
	struct s17_copro copro;
	struct s11_step_env env;
	struct s11_run_cfg cfg;
	struct s11_run_result res;

	all_staged(&st, &copro);
	memset(&env, 0, sizeof(env));
	env.staged = &st;
	env.copro = &copro;
	env.nordic_in_dfu = true;
	env.forced = true;
	env.option = true;
	env.sig = S11_SIG_VALID;
	env.selected = 0x3Fu;

	/* each switched-off step alone: Skipped, none of its ops called */
	for (size_t i = 0; i < N(switchable); i++) {
		const unsigned step = switchable[i];

		memset(&C, 0, sizeof(C));
		env.off = S11_STEP_BIT(step);
		CHECK(s11_step_result(step, &env, &count_ops) == S11_SKIPPED,
		      "step %u switched off: Skipped", step);
		CHECK(C.sends == 0 && C.slots == 0 && C.psoc == 0,
		      "step %u switched off: no nordic_send, no nordic_boot_slot, "
		      "no psoc_program", step);
		CHECK(s11_step_skip(step, &env) == S11_SKIP_SWITCHED_OFF,
		      "step %u: the reason", step);
		env.off = 0;
		CHECK(s11_step_result(step, &env, &count_ops) == S11_OK &&
		      (C.sends != 0 || C.psoc != 0),
		      "step %u switched on: it runs", step);
		CHECK(s11_step_skip(step, &env) == S11_SKIP_NONE,
		      "step %u switched on: no reason", step);
	}
	/* only steps 3-5 can be switched off; step 6 is reserved */
	env.off = 0xFFu;
	env.option = true;
	memset(&C, 0, sizeof(C));
	CHECK(s11_step_result(S11_STEP_OS, &env, &count_ops) == S11_OK &&
	      s11_step_result(S11_STEP_BOOTLOADER, &env, &count_ops) == S11_OK &&
	      C.os == 1 && C.bootloader == 1,
	      "steps 1 and 2 cannot be switched off");
	CHECK(s11_step_result(S11_STEP_RESERVED, &env, &count_ops) ==
		      S11_NOT_FOUND &&
	      s11_step_skip(S11_STEP_RESERVED, &env) == S11_SKIP_NONE,
	      "step 6: Not found, never Skipped");
	/* absent component: Not found, as switched on (step 4: both images) */
	st.size[S11_C_BLE] = st.size[S11_C_BACKPLANE] = 0;
	st.size[S11_C_NORDIC_BL0] = st.size[S11_C_NORDIC_BL1] = 0;
	for (size_t i = 0; i < N(switchable); i++) {
		CHECK(s11_step_result(switchable[i], &env, &count_ops) ==
			      S11_NOT_FOUND &&
		      s11_step_skip(switchable[i], &env) == S11_SKIP_NONE,
		      "step %u off, absent: Not found", switchable[i]);
	}
	st.size[S11_C_NORDIC_BL1] = 1u;
	CHECK(s11_step_result(S11_STEP_BLE_BOOTLOADER, &env, &count_ops) ==
		      S11_SKIPPED,
	      "step 4 off, one image: Skipped");

	/* the protected steps' reasons (S-11.090, S-11.096) */
	all_staged(&st, &copro);
	st.content_dir = true;
	env.off = 0;
	env.option = false;
	env.selected = 0x3Fu;
	CHECK(s11_step_skip(S11_STEP_BOOTLOADER, &env) == S11_SKIP_OPTION_OFF,
	      "option off, though selected");
	memset(&C, 0, sizeof(C));
	CHECK(s11_step_result(S11_STEP_BOOTLOADER, &env, &count_ops) ==
		      S11_SKIPPED && C.bootloader == 0,
	      "option off: no write, though selected");
	env.option = true;
	env.selected = 0;
	CHECK(s11_step_skip(S11_STEP_BOOTLOADER, &env) == S11_SKIP_NOT_SELECTED,
	      "the bootloader not selected");
	memset(&C, 0, sizeof(C));
	CHECK(s11_step_result(S11_STEP_BOOTLOADER, &env, &count_ops) ==
		      S11_SKIPPED && C.bootloader == 0,
	      "the bootloader writes only when selected");
	/* S-11.090: a package invalid or unsigned writes the bootloader
	 * the same, selected with the option on */
	env.selected = 0x3Fu;
	for (unsigned k = 0; k < 2u; k++) {
		env.sig = k == 0u ? S11_SIG_NONE : S11_SIG_INVALID;
		memset(&C, 0, sizeof(C));
		CHECK(s11_step_result(S11_STEP_BOOTLOADER, &env, &count_ops) ==
			      S11_OK && C.bootloader == 1 &&
		      s11_step_skip(S11_STEP_BOOTLOADER, &env) ==
			      S11_SKIP_NONE &&
		      s11_step_pick(S11_STEP_BOOTLOADER, &env) ==
			      S11_PICK_OPEN &&
		      s11_step_result(S11_STEP_OS, &env, &count_ops) == S11_OK,
		      "state %d: the bootloader and the OS written",
		      (int)env.sig);
	}
	env.sig = S11_SIG_VALID;
	env.selected = 0;
	/* every other step not selected: Skipped, nothing reached */
	{
		static const unsigned each[] = {
			S11_STEP_CONTENT, S11_STEP_OS, S11_STEP_BLE,
			S11_STEP_BLE_BOOTLOADER, S11_STEP_KEYBOARD,
		};

		for (size_t i = 0; i < N(each); i++) {
			memset(&C, 0, sizeof(C));
			CHECK(s11_step_result(each[i], &env, &count_ops) ==
				      S11_SKIPPED &&
			      s11_step_skip(each[i], &env) == S11_SKIP_NOT_SELECTED &&
			      s11_result_skip(each[i], &env, S11_SKIPPED) ==
				      S11_SKIP_NOT_SELECTED,
			      "step %u not selected: Skipped, not selected", each[i]);
			CHECK(C.os == 0 && C.sends == 0 && C.slots == 0 &&
			      C.psoc == 0 && C.contents == 0,
			      "step %u not selected: nothing reached", each[i]);
			env.selected = S11_STEP_BIT(each[i]);
			CHECK(s11_step_skip(each[i], &env) == S11_SKIP_NONE &&
			      s11_step_result(each[i], &env, &count_ops) != S11_SKIPPED,
			      "step %u selected: it runs", each[i]);
			env.selected = 0;
		}
	}
	/* a Nordic or PSoC step already installed reports so, selected or not */
	env.forced = false;
	copro.nordic_app = copro.nordic_bl = copro.psoc = st.version[S11_C_BLE];
	CHECK(s11_step_result(S11_STEP_BLE, &env, &count_ops) == S11_ALREADY &&
	      s11_step_result(S11_STEP_KEYBOARD, &env, &count_ops) == S11_ALREADY &&
	      s11_step_skip(S11_STEP_BLE, &env) == S11_SKIP_NONE,
	      "Already installed comes before the selection");
	env.forced = true;

	/* S-11.061 item 2: the DFU entry only for a Nordic image whose step
	 * is switched on and selected */
	for (unsigned m = 0; m < 64u; m++) {
		const bool app = (m & 1u) != 0u, bl = (m & 2u) != 0u;
		const uint8_t off = (uint8_t)(((m & 4u) != 0u ?
					       S11_STEP_BIT(S11_STEP_BLE) : 0u) |
					      ((m & 8u) != 0u ?
					       S11_STEP_BIT(S11_STEP_BLE_BOOTLOADER) : 0u));
		const uint8_t sel = (uint8_t)(((m & 16u) != 0u ?
					       S11_STEP_BIT(S11_STEP_BLE) : 0u) |
					      ((m & 32u) != 0u ?
					       S11_STEP_BIT(S11_STEP_BLE_BOOTLOADER) : 0u));
		const bool want = (app && (m & 4u) == 0u && (m & 16u) != 0u) ||
				  (bl && (m & 8u) == 0u && (m & 32u) != 0u);
		struct s11_step_env e;

		memset(&st, 0, sizeof(st));
		memset(&e, 0, sizeof(e));
		st.size[S11_C_BLE] = app ? 1u : 0u;
		st.size[S11_C_NORDIC_BL1] = bl ? 1u : 0u;
		e.staged = &st;
		e.off = off;
		e.selected = sel;
		CHECK(s11_run_nordic(&e) == want, "DFU entry case %u", m);
		e.off = (uint8_t)(off | S11_STEP_BIT(S11_STEP_KEYBOARD));
		e.selected = (uint8_t)(sel | S11_STEP_BIT(S11_STEP_KEYBOARD) |
				       S11_STEP_BIT(S11_STEP_OS));
		CHECK(s11_run_nordic(&e) == want,
		      "DFU entry case %u: the other steps have no say", m);
	}

	/* this version's run of a package with every component: steps 3-5
	 * Skipped, no DFU entry, done */
	all_staged(&st, &copro);
	memset(&env, 0, sizeof(env));
	env.staged = &st;
	env.copro = &copro;
	env.off = S11_STEPS_SWITCHABLE;
	env.selected = 0x3Fu;
	memset(&cfg, 0, sizeof(cfg));
	cfg.nordic = s11_run_nordic(&env);
	cfg.has_version = true;
	cfg.version = st.fw_version;
	memset(&C, 0, sizeof(C));
	memset(&L, 0, sizeof(L));
	{
		static const struct s11v_run any = { "this_version", 0, 1, 0, 0,
						     0, 0, 1, { 0 }, 0, "", "" };

		L.v = &any;	/* l_enter's answer, never asked */
		CHECK(s11_run_steps(&log_ops, &count_ops, &env, &cfg, &res) ==
			      S11_RUN_DONE,
		      "this version: done");
	}
	CHECK(!cfg.nordic && strstr(L.text, "enter_dfu") == NULL &&
	      strstr(L.text, "read_versions") != NULL &&
	      strstr(L.text, "exit_dfu_if_in_dfu") != NULL,
	      "this version: no DFU entry, the version read and the exit query: %s",
	      L.text);
	CHECK(C.sends == 0 && C.slots == 0 && C.psoc == 0 && C.os == 1 &&
	      C.bootloader == 0,
	      "this version: only the OS written");
	CHECK(strstr(L.text, "meta os_version=package,state=0") != NULL,
	      "this version: the package version recorded with the OS (S-11.064)");

	/* S-11.064: the OS not selected: the state alone, no version */
	memset(&C, 0, sizeof(C));
	memset(&L, 0, sizeof(L));
	env.selected = (uint8_t)~S11_STEP_BIT(S11_STEP_OS);
	{
		static const struct s11v_run any = { "no_os", 0, 1, 0, 0,
						     0, 0, 1, { 0 }, 0, "", "" };

		L.v = &any;
		CHECK(s11_run_steps(&log_ops, &count_ops, &env, &cfg, &res) ==
			      S11_RUN_DONE && C.os == 0 && C.bootloader == 0,
		      "OS not selected: done, the OS not written");
	}
	CHECK(strstr(L.text, "os_version") == NULL &&
	      strstr(L.text, "meta state=0") != NULL,
	      "OS not selected: the state cleared, no version recorded: %s",
	      L.text);
}

/* ---- picks.tsv: what the selection offers (S-11.096) ---- */

static void test_picks(void)
{
	static const enum s11_component comp[7] = {
		S11_COMPONENTS, S11_C_OS, S11_C_BOOTLOADER, S11_C_BLE,
		S11_C_NORDIC_BL0, S11_C_BACKPLANE, S11_COMPONENTS,
	};
	const struct s11_version pkg = { 1, 2, 0, 0 };

	for (size_t i = 0; i < N(s11v_picks); i++) {
		const struct s11v_pick *v = &s11v_picks[i];
		struct s11_staged st;
		struct s17_copro copro;
		struct s11_step_env env;
		struct s11_version installed = pkg;
		enum s11_pick got;

		memset(&st, 0, sizeof(st));
		for (unsigned c = 0; c < S11_COMPONENTS; c++)
			st.version[c] = pkg;
		st.content_dir = v->content_dir != 0;
		if (v->step == S11_STEP_BLE_BOOTLOADER) {
			st.size[S11_C_NORDIC_BL0] = v->present0 ? 1u : 0u;
			st.size[S11_C_NORDIC_BL1] = v->present1 ? 1u : 0u;
		} else if (comp[v->step] != S11_COMPONENTS) {
			st.size[comp[v->step]] = v->present ? 1u : 0u;
		}
		installed.minor = (uint16_t)(installed.minor - v->rel);
		s17_copro_init(&copro);	/* 0.0.0: not known */
		if (v->known)
			copro.nordic_app = copro.nordic_bl = copro.psoc = installed;
		memset(&env, 0, sizeof(env));
		env.staged = &st;
		env.option = v->option != 0;
		env.sig = (enum s11_sig)(i % 3u);	/* decides no pick */
		env.forced = v->forced != 0;
		env.copro = &copro;
		env.off = v->switched_on ? 0u : S11_STEP_BIT(v->step);
		env.selected = 0xFFu;	/* the selection is not read */
		got = s11_step_pick(v->step, &env);
		CHECK(got == v->pick, "pick row %zu (step %u): %d, want %d", i,
		      v->step, (int)got, (int)v->pick);
		CHECK(((s11_pickable(&env) & S11_STEP_BIT(v->step)) != 0u) ==
			      (v->pick == S11_PICK_OPEN),
		      "pick row %zu: pickable", i);
	}
}

/* ---- the answers (S-11.096) ---- */

static struct {
	enum s11_answer answer;
	uint8_t give, offered;
	unsigned asked;
} A;

static enum s11_answer ask(void *ctx, const struct s11_step_env *env,
			   uint8_t *selected)
{
	(void)ctx;
	A.asked++;
	A.offered = s11_pickable(env);
	*selected = A.give;
	return A.answer;
}

static void test_select(void)
{
	struct s11_staged st;
	struct s17_copro copro;
	struct s11_step_env env;

	all_staged(&st, &copro);
	st.content_dir = true;
	memset(&env, 0, sizeof(env));
	env.staged = &st;
	env.copro = &copro;
	env.option = true;
	env.sig = S11_SIG_VALID;
	env.off = S11_STEPS_SWITCHABLE;
	CHECK(s11_pickable(&env) == (S11_STEP_BIT(S11_STEP_CONTENT) |
				     S11_STEP_BIT(S11_STEP_OS) |
				     S11_STEP_BIT(S11_STEP_BOOTLOADER)),
	      "this version: content, OS and the bootloader");
	CHECK(s11_unasked(&env) == (S11_STEP_BIT(S11_STEP_CONTENT) |
				    S11_STEP_BIT(S11_STEP_OS)),
	      "unasked: all but the bootloader");
	/* an answer: as far as the rows can be picked */
	memset(&A, 0, sizeof(A));
	A.answer = S11_ANSWER_INSTALL;
	A.give = 0xFFu;
	env.selected = 0;
	CHECK(s11_select(&env, ask, NULL) && A.asked == 1 &&
	      env.selected == s11_pickable(&env) && A.offered == env.selected,
	      "install: the steps given that can be picked");
	A.give = S11_STEP_BIT(S11_STEP_BOOTLOADER);
	CHECK(s11_select(&env, ask, NULL) &&
	      env.selected == S11_STEP_BIT(S11_STEP_BOOTLOADER),
	      "the bootloader alone");
	A.answer = S11_ANSWER_CANCEL;
	CHECK(!s11_select(&env, ask, NULL) && env.selected == 0,
	      "cancel: false, nothing selected");
	/* the keys cannot be read: the old run, the bootloader not selected */
	A.answer = S11_ANSWER_UNASKED;
	CHECK(s11_select(&env, ask, NULL) && env.selected == s11_unasked(&env),
	      "unasked: every step but the bootloader");
	CHECK(s11_select(&env, NULL, NULL) && env.selected == s11_unasked(&env),
	      "no ask: as unasked");
	/* the option off: the bootloader cannot be picked, whatever is given */
	env.option = false;
	A.answer = S11_ANSWER_INSTALL;
	A.give = 0xFFu;
	CHECK(s11_select(&env, ask, NULL) &&
	      (env.selected & S11_STEP_BIT(S11_STEP_BOOTLOADER)) == 0u,
	      "option off: the bootloader never selected");
	/* S-11.090: an invalid package, the option on: offered and
	 * selected as for any */
	env.option = true;
	env.sig = S11_SIG_INVALID;
	CHECK(s11_select(&env, ask, NULL) &&
	      (A.offered & S11_STEP_BIT(S11_STEP_BOOTLOADER)) != 0u &&
	      (env.selected & S11_STEP_BIT(S11_STEP_BOOTLOADER)) != 0u &&
	      (env.selected & S11_STEP_BIT(S11_STEP_OS)) != 0u,
	      "invalid: the bootloader offered and selected");
	/* S-15.197: an invalid package nobody was asked about: no run */
	A.answer = S11_ANSWER_UNASKED;
	env.selected = 0xFFu;
	CHECK(!s11_select(&env, ask, NULL) && env.selected == 0,
	      "invalid, unasked: false, nothing selected");
	env.selected = 0xFFu;
	CHECK(!s11_select(&env, NULL, NULL) && env.selected == 0,
	      "invalid, no ask: false, nothing selected");
}

/* ---- staged_check.json: S-11.095 ---- */

static void test_staged_check(void)
{
	static const enum s11_result results[3] = { S11_OK, S11_BAD_FILE,
						    S11_INTERNAL };

	for (size_t i = 0; i < N(s11v_staged_checks); i++) {
		const struct s11v_staged_check *v = &s11v_staged_checks[i];
		const struct s11_staged_read stated = { true, v->stated_len,
							v->stated_crc };
		const struct s11_staged_read read = {
			v->read.present != 0, (uint32_t)v->read.len,
			ref_crc32(0, B(v->read), v->read.len),
		};
		enum s11_staged_check c = s11_staged_check(&stated, &read);

		CHECK((int)c == v->expect, "%s: %d, want %d", v->name, (int)c,
		      v->expect);
		CHECK(s11_staged_result(c) == results[v->expect],
		      "%s: the step's result", v->name);
	}
}

/* ---- across boots (S-11.064, S-11.065, S-11.080) ---- */

struct dev {
	uint16_t state;
	struct s11_version os;
	bool rec_present;
	uint8_t rec[2];
	bool comp[S11_COMPONENTS];
	/* this run's faults */
	int cut;			/* events left before a reset; -1: none */
	bool dfu_ok, option;
	int fail_step;			/* -1: none */
	unsigned steps, records, nordic;	/* what a boot or a run did */
	jmp_buf jb;
};

static struct dev D;

static void event(void)
{
	if (D.cut == 0)
		longjmp(D.jb, 1);
	if (D.cut > 0)
		D.cut--;
}

static bool d_meta_state(void *ctx, uint16_t state)
{
	(void)ctx;
	event();
	D.state = state;
	return true;
}

static bool d_meta_complete(void *ctx, const struct s11_version *v)
{
	(void)ctx;
	event();
	D.os = *v;
	D.state = 0;
	return true;
}

static bool d_record(void *ctx, uint8_t step)
{
	(void)ctx;
	event();
	s11_record_bytes(step, D.rec);
	D.rec_present = true;
	D.records++;
	return true;
}

static bool d_enter(void *ctx)
{
	(void)ctx;
	event();
	D.nordic++;
	return D.dfu_ok;
}

static bool d_versions(void *ctx)
{
	(void)ctx;
	event();
	D.nordic++;
	return true;
}

static void d_exit(void *ctx)
{
	(void)ctx;
	event();
	D.nordic++;
}

static enum s11_result d_step(void *ctx, unsigned step)
{
	static const enum s11_component comp[7] = {
		S11_COMPONENTS, S11_C_OS, S11_C_BOOTLOADER, S11_C_BLE,
		S11_C_NORDIC_BL0, S11_C_BACKPLANE, S11_COMPONENTS,
	};

	(void)ctx;
	D.steps++;
	if ((int)step == D.fail_step)
		return S11_FAILURE;
	if (step == S11_STEP_CONTENT || step == S11_STEP_RESERVED)
		return S11_NOT_FOUND;
	if (!D.comp[comp[step]])
		return S11_NOT_FOUND;
	if (step == S11_STEP_BOOTLOADER && !D.option)
		return S11_SKIPPED;
	return S11_OK;
}

static const struct s11_run_ops dev_ops = {
	d_meta_state, d_meta_complete, d_record, d_enter, d_versions, d_exit,
	d_step, NULL,
};

static void dev_fresh(void)
{
	memset(&D, 0, sizeof(D));
	D.os = (struct s11_version){ 1, 0, 0, 0 };
	D.cut = -1;
	D.dfu_ok = true;
	D.fail_step = -1;
}

/* A run, after the package passed every check; the extraction removed the
 * record (S-11.031). Returns 0 when cut. */
static int dev_install(bool option, int cut, int fail_step)
{
	/* static: nothing automatic lives across the setjmp */
	static const struct s11_run_cfg cfg = { false, true, true,
						{ 1, 2, 0, 0 } };
	static struct s11_run_result res;

	D.rec_present = false;
	for (unsigned c = 0; c < S11_COMPONENTS; c++)
		D.comp[c] = true;
	D.option = option;
	D.cut = cut;
	D.fail_step = fail_step;
	if (setjmp(D.jb) != 0)
		return 0;
	s11_run(&dev_ops, &cfg, &res);
	return 1;
}

/* One boot: 'o' the OS; 'h' held mode with the notice, after the state
 * was cleared (S-11.080). */
static char dev_boot(void)
{
	if (D.state != S11_STATE_UPGRADING)
		return 'o';
	D.cut = -1;
	D.steps = D.records = D.nordic = 0;
	(void)s11_interrupted(&dev_ops);
	return 'h';
}

static void test_multiboot(void)
{
	/* S-11.065: a run cut after each of its events. A cut before M1 or
	 * after the run's last metadata write leaves the state 0 and the
	 * next boot is the OS; any other leaves 0xAB0D, and the next boot
	 * clears it with no step, no record write and no Nordic exchange,
	 * and the boot after that is the OS. */
	for (int k = 0; k <= 20; k++) {
		bool cut, set;

		dev_fresh();
		cut = dev_install(true, k, -1) == 0;
		set = D.state == S11_STATE_UPGRADING;
		CHECK(cut || !set, "cut %d: a run that ends leaves the state 0", k);
		if (set) {
			const uint8_t before[2] = { D.rec[0], D.rec[1] };

			CHECK(dev_boot() == 'h' && D.state == 0 &&
			      D.steps == 0 && D.records == 0 && D.nordic == 0 &&
			      D.rec[0] == before[0] && D.rec[1] == before[1],
			      "cut %d: cleared, nothing run", k);
		}
		CHECK(dev_boot() == 'o', "cut %d: then the OS", k);
	}

	/* S-11.064: a step that fails ends the run with the state 0; the
	 * record stays at that step, and the next boot is the OS. */
	for (int step = 0; step < (int)S11_STEP_DONE; step++) {
		dev_fresh();
		CHECK(dev_install(true, -1, step) == 1 && D.state == 0 &&
		      D.rec[0] == step && D.rec[1] == 0,
		      "step %d fails: state 0, record {%d, 0}", step, step);
		CHECK(D.os.major == 1 && D.os.minor == 0, "no version recorded");
		CHECK(dev_boot() == 'o', "then the OS");
	}

	/* S-11.061: a Nordic that refuses DFU: no step, no record, state 0 */
	dev_fresh();
	D.dfu_ok = false;
	{
		static const struct s11_run_cfg cfg = { false, true, true,
							{ 1, 2, 0, 0 } };
		struct s11_run_result res;

		CHECK(s11_run(&dev_ops, &cfg, &res) == S11_RUN_FAILED &&
		      D.state == 0 && D.steps == 0 && !D.rec_present &&
		      D.nordic == 1, "no DFU: failed before any step");
	}

	/* S-11.064: a completed run records the package version */
	dev_fresh();
	CHECK(dev_install(true, -1, -1) == 1 && D.state == 0 &&
	      D.os.major == 1 && D.os.minor == 2 && D.rec[0] == 0 &&
	      D.rec[1] == 0, "completed: os_version 1.2.0, record {0, 0}");
}

int main(void)
{
	test_runs();
	test_interrupted();
	test_steps();
	test_protected_results();
	test_switched_off();
	test_picks();
	test_select();
	test_staged_check();
	test_multiboot();
	for (size_t i = 0; i < N(s11v_records); i++) {	/* S-11.062 */
		uint8_t b[2];

		CHECK(s11v_records[i].count == 0u, "record %zu: count 0", i);
		s11_record_bytes((uint8_t)s11v_records[i].step, b);
		CHECK(((unsigned)b[0] << 8 | b[1]) == s11v_records[i].bytes,
		      "record bytes %zu", i);
	}
	return s11_check_report("test_s11_run");
}
