/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/s11_run.h"

#include "waffle/s02_flash.h"

/* S-11.062, S-11.094 */
void s11_record_bytes(uint8_t step, uint8_t out[2])
{
	out[0] = step;
	out[1] = 0;		/* S-11.062: always 0 */
}

/* S-11.059 */
bool s11_result_continues(enum s11_result r)
{
	switch (r) {
	case S11_OK:
	case S11_NOT_FOUND:
	case S11_ALREADY:
	case S11_SKIPPED:
		return true;
	case S11_FAILURE:
	case S11_BAD_FILE:
	case S11_INTERNAL:
	default:
		return false;
	}
}

/* S-11.060: results that did nothing */
static bool quiet(enum s11_result r)
{
	return r == S11_NOT_FOUND || r == S11_ALREADY || r == S11_SKIPPED;
}

/*
 * S-11.064: a metadata write or a progress-record write that fails changes
 * nothing else: the run goes on, and ends, as if it had succeeded.
 */
static enum s11_run_end run(const struct s11_run_ops *ops,
			    const struct s11_step_ops *steps,
			    struct s11_step_env *env,
			    const struct s11_run_cfg *cfg,
			    struct s11_run_result *res)
{
	uint8_t step = 0;
	bool nothing = true;
	bool os_installed = false;

	res->step = 0xFF;
	res->result = S11_OK;
	res->versions_read = false;
	res->nordic_in_dfu = false;
	if (!cfg->content_only)
		(void)ops->meta_state(ops->ctx, S11_STATE_UPGRADING); /* S-11.061 1 */
	/* S-11.061 step 2: no step and no progress record if this fails, and
	 * of S-11.064 only the state: no in-DFU query, no exit. */
	if (cfg->nordic && !ops->nordic_enter_dfu(ops->ctx)) {
		if (!cfg->content_only)
			(void)ops->meta_state(ops->ctx, 0);
		res->end = S11_RUN_FAILED;
		return res->end;
	}
	res->nordic_in_dfu = ops->nordic_read_versions(ops->ctx); /* 061 3 */
	res->versions_read = true;
	if (env != NULL)
		env->nordic_in_dfu = res->nordic_in_dfu;
	(void)ops->record_write(ops->ctx, step);		/* S-11.061 4 */

	/* S-11.057, S-11.059, S-11.063 */
	while (step < S11_STEP_DONE) {
		/* s11_run_steps() gives both, s11_run() neither */
		enum s11_result r = steps != NULL && env != NULL ?
				    s11_step_result(step, env, steps) :
				    ops->step(ops->ctx, step);

		if (!s11_result_continues(r)) {
			res->step = step;		/* the record stays */
			res->result = r;
			break;
		}
		nothing = nothing && quiet(r);
		os_installed = os_installed || (step == S11_STEP_OS && r == S11_OK);
		step++;
		(void)ops->record_write(ops->ctx, step);
	}

	/* S-11.064: no run leaves the state set; only a reset during one
	 * does (S-11.065, S-11.080). */
	if (step == S11_STEP_DONE) {
		if (!cfg->content_only) {
			/* S-11.064: the package version only with the OS
			 * this run installed; S-11.038: without a version
			 * file, the state only.
			 * SPEC-Q: the package version is recorded with the
			 * OS though the run skipped other components. */
			if (cfg->has_version && os_installed)
				(void)ops->meta_complete(ops->ctx, &cfg->version);
			else
				(void)ops->meta_state(ops->ctx, 0);
		}
		(void)ops->record_write(ops->ctx, 0);
		res->end = nothing ? S11_RUN_NOTHING_TO_DO : S11_RUN_DONE;
	} else {
		if (!cfg->content_only)
			(void)ops->meta_state(ops->ctx, 0);
		res->end = S11_RUN_FAILED;
	}
	/* S-11.064 item 3; no exit after a failed application transfer
	 * (S-08.048) */
	ops->nordic_exit_dfu(ops->ctx);
	return res->end;
}

enum s11_run_end s11_run(const struct s11_run_ops *ops,
			 const struct s11_run_cfg *cfg,
			 struct s11_run_result *res)
{
	return run(ops, NULL, NULL, cfg, res);
}

enum s11_run_end s11_run_steps(const struct s11_run_ops *ops,
			       const struct s11_step_ops *steps,
			       struct s11_step_env *env,
			       const struct s11_run_cfg *cfg,
			       struct s11_run_result *res)
{
	return run(ops, steps, env, cfg, res);
}

/* S-11.060, S-11.051 */
void s11_run_outcome(const struct s11_run_result *res, struct s11_outcome *o)
{
	o->reason = S11_REASON_NONE;
	o->step = res->step;
	o->result = res->result;
	switch (res->end) {
	case S11_RUN_DONE:
		o->kind = S11_OUTCOME_DONE;
		break;
	case S11_RUN_NOTHING_TO_DO:
		o->kind = S11_OUTCOME_NOTHING_TO_DO;
		break;
	case S11_RUN_FAILED:
	default:
		o->kind = S11_OUTCOME_FAILED;
		break;
	}
}

/* S-11.080 */
bool s11_interrupted(const struct s11_run_ops *ops)
{
	return ops->meta_state(ops->ctx, 0);
}

/* ---- the steps ---- */

static bool same(const struct s11_version *a, const struct s11_version *b)
{
	return s11_version_cmp(a, b) == 0;
}

/* A step whose write, transfer or programming returned false: Bad file
 * when the image was refused (S-11.095's check failed, or S-02.091),
 * Internal error when a read of it failed (S-11.095), else the step's own
 * failure result. */
static enum s11_result failed(const struct s11_step_ops *ops,
			      enum s11_result own, bool may_skip)
{
	enum s11_result r;

	if (ops->write_failure == NULL)
		return own;
	r = ops->write_failure(ops->ctx);
	if (r == S11_SKIPPED && may_skip)
		return r;	/* S-11.068 item 5: already written */
	return r == S11_BAD_FILE || r == S11_INTERNAL ? r : own;
}

/* S-11.073: a switched-off step makes no version test, no check and no
 * exchange with its device. */
static bool off(const struct s11_step_env *env, unsigned step)
{
	return (env->off & S11_STEPS_SWITCHABLE & S11_STEP_BIT(step)) != 0u;
}

/* S-11.096: a step not selected does nothing more. */
static bool unselected(const struct s11_step_env *env, unsigned step)
{
	return (env->selected & S11_STEP_BIT(step)) == 0u;
}

/* S-11.068, S-11.090: no version test, so a return to the factory
 * bootloader writes any version (S-11.093) */
static enum s11_result protected_step(const struct s11_step_env *env,
				      unsigned step, enum s11_component c,
				      bool (*write)(void *ctx),
				      const struct s11_step_ops *ops)
{
	if (!s11_staged_present(env->staged, c))
		return S11_NOT_FOUND;
	if (off(env, step))
		return S11_SKIPPED;	/* S-11.073: no SPI flash access */
	if (!env->option)
		return S11_SKIPPED;	/* no SPI flash access (S-02.057) */
	if (unselected(env, step))
		return S11_SKIPPED;	/* S-11.096: as with the option off */
	if (write(ops->ctx))
		return S11_OK;
	return failed(ops, S11_FAILURE, step == S11_STEP_BOOTLOADER);
}

enum s11_result s11_step_result(unsigned step, const struct s11_step_env *env,
				const struct s11_step_ops *ops)
{
	const struct s11_staged *st = env->staged;
	const struct s11_version *v;
	int slot;

	switch (step) {
	case S11_STEP_CONTENT:			/* S-11.066 */
		if (st->content_dir && unselected(env, step))
			return S11_SKIPPED;	/* S-11.096: nothing is moved */
		return ops->content(ops->ctx);
	case S11_STEP_OS:			/* S-11.067: no version test */
		if (!s11_staged_present(st, S11_C_OS))
			return S11_NOT_FOUND;
		if (unselected(env, step))
			return S11_SKIPPED;	/* S-11.096 */
		if (!ops->os_write(ops->ctx))
			return failed(ops, S11_INTERNAL, false);
		ops->meta_sw_version(ops->ctx, &st->version[S11_C_OS]);
		return S11_OK;
	case S11_STEP_BOOTLOADER:		/* S-11.068 */
		return protected_step(env, step, S11_C_BOOTLOADER,
				      ops->bootloader_write, ops);
	case S11_STEP_BLE:			/* S-11.069 */
		v = &st->version[S11_C_BLE];
		if (!s11_staged_present(st, S11_C_BLE))
			return S11_NOT_FOUND;
		if (off(env, step))
			return S11_SKIPPED;
		if (!env->forced && same(v, &env->copro->nordic_app))
			return S11_ALREADY;
		if (unselected(env, step))
			return S11_SKIPPED;	/* S-11.096: no exchange */
		/* S-11.069 items 4-6: the image's check, then the step's own
		 * DFU-mode queries (S-08.042), both inside the send; the
		 * run-start answer decides nothing here */
		if (!ops->nordic_send(ops->ctx, S11_C_BLE))
			return failed(ops, S11_FAILURE, false);
		env->copro->nordic_app = *v;
		return S11_OK;
	case S11_STEP_BLE_BOOTLOADER:		/* S-11.070 */
		v = &st->version[S11_C_NORDIC_BL0];	/* one version file */
		if (!s11_staged_present(st, S11_C_NORDIC_BL0) &&
		    !s11_staged_present(st, S11_C_NORDIC_BL1))
			return S11_NOT_FOUND;
		if (off(env, step))
			return S11_SKIPPED;
		if (!env->forced && same(v, &env->copro->nordic_bl))
			return S11_ALREADY;
		if (unselected(env, step))
			return S11_SKIPPED;	/* S-11.096: no slot query */
		slot = ops->nordic_boot_slot(ops->ctx);	/* S-16.124 */
		if (slot != 0 && slot != 1)
			return S11_FAILURE;
		if (!s11_staged_present(st, slot ? S11_C_NORDIC_BL1 :
						   S11_C_NORDIC_BL0))
			return S11_NOT_FOUND;
		if (!ops->nordic_send(ops->ctx, slot ? S11_C_NORDIC_BL1 :
							S11_C_NORDIC_BL0))
			return failed(ops, S11_FAILURE, false); /* S-16.032: version unchanged */
		env->copro->nordic_bl = *v;
		return S11_OK;
	case S11_STEP_KEYBOARD:			/* S-11.071 */
		v = &st->version[S11_C_BACKPLANE];
		if (!s11_staged_present(st, S11_C_BACKPLANE))
			return S11_NOT_FOUND;
		if (off(env, step))
			return S11_SKIPPED;
		if (!env->forced && same(v, &env->copro->psoc))
			return S11_ALREADY;
		if (unselected(env, step))
			return S11_SKIPPED;	/* S-11.096: no exchange */
		return ops->psoc_program(ops->ctx) ? S11_OK :
						     failed(ops, S11_FAILURE, false);
	case S11_STEP_RESERVED:		/* S-11.057: nothing is looked for */
		return S11_NOT_FOUND;
	default:
		return S11_INTERNAL;
	}
}

/* The component a step reports Not found without; the Nordic bootloader
 * step has two (S-11.070). */
static bool carried(unsigned step, const struct s11_staged *st)
{
	switch (step) {
	case S11_STEP_CONTENT:
		return st->content_dir;
	case S11_STEP_OS:
		return s11_staged_present(st, S11_C_OS);
	case S11_STEP_BOOTLOADER:
		return s11_staged_present(st, S11_C_BOOTLOADER);
	case S11_STEP_BLE:
		return s11_staged_present(st, S11_C_BLE);
	case S11_STEP_BLE_BOOTLOADER:
		return s11_staged_present(st, S11_C_NORDIC_BL0) ||
		       s11_staged_present(st, S11_C_NORDIC_BL1);
	case S11_STEP_KEYBOARD:
		return s11_staged_present(st, S11_C_BACKPLANE);
	default:
		return false;
	}
}

/* The installed version a Nordic or PSoC step compares with, and the
 * package's; NULL for the other steps, and with no versions known. */
static const struct s11_version *installed(unsigned step,
					   const struct s11_step_env *env,
					   const struct s11_version **pkg)
{
	const struct s11_staged *st = env->staged;

	*pkg = NULL;
	if (env->copro == NULL)
		return NULL;
	switch (step) {
	case S11_STEP_BLE:
		*pkg = &st->version[S11_C_BLE];
		return &env->copro->nordic_app;
	case S11_STEP_BLE_BOOTLOADER:
		*pkg = &st->version[S11_C_NORDIC_BL0];	/* one version file */
		return &env->copro->nordic_bl;
	case S11_STEP_KEYBOARD:
		*pkg = &st->version[S11_C_BACKPLANE];
		return &env->copro->psoc;
	default:
		*pkg = NULL;
		return NULL;
	}
}

/* S-11.069-S-11.071: the step reports Already installed. */
static bool already(unsigned step, const struct s11_step_env *env)
{
	const struct s11_version *pkg;
	const struct s11_version *have = installed(step, env, &pkg);

	return have != NULL && !env->forced && same(pkg, have);
}

enum s11_skip s11_step_skip(unsigned step, const struct s11_step_env *env)
{
	if (!carried(step, env->staged))
		return S11_SKIP_NONE;
	if (off(env, step))
		return S11_SKIP_SWITCHED_OFF;
	if (step == S11_STEP_BOOTLOADER && !env->option)
		return S11_SKIP_OPTION_OFF;
	if (already(step, env))
		return S11_SKIP_NONE;
	return unselected(env, step) ? S11_SKIP_NOT_SELECTED : S11_SKIP_NONE;
}

enum s11_skip s11_result_skip(unsigned step, const struct s11_step_env *env,
			      enum s11_result r)
{
	const enum s11_skip why = s11_step_skip(step, env);

	if (r != S11_SKIPPED)
		return S11_SKIP_NONE;
	if (why == S11_SKIP_NONE && step == S11_STEP_BOOTLOADER)
		return S11_SKIP_ALREADY_WRITTEN;
	return why;
}

bool s11_run_nordic(const struct s11_step_env *env)
{
	static const unsigned nordic[] = { S11_STEP_BLE, S11_STEP_BLE_BOOTLOADER };

	for (size_t i = 0; i < sizeof(nordic) / sizeof(nordic[0]); i++) {
		if (carried(nordic[i], env->staged) && !off(env, nordic[i]) &&
		    !unselected(env, nordic[i]))
			return true;
	}
	return false;
}

/* ---- the selection (S-11.096) ---- */

static bool known(const struct s11_version *v)
{
	return v->major != 0u || v->minor != 0u || v->patch != 0u ||
	       v->build != 0u;
}

enum s11_pick s11_step_pick(unsigned step, const struct s11_step_env *env)
{
	const struct s11_version *pkg;
	const struct s11_version *have;

	if (step >= S11_STEP_RESERVED || !carried(step, env->staged))
		return S11_PICK_ABSENT;
	if (off(env, step))
		return S11_PICK_SWITCHED_OFF;
	if (step == S11_STEP_BOOTLOADER && !env->option)
		return S11_PICK_OPTION_OFF;
	have = installed(step, env, &pkg);
	if (have != NULL && known(have) && already(step, env))
		return S11_PICK_UP_TO_DATE;
	return S11_PICK_OPEN;
}

uint8_t s11_pickable(const struct s11_step_env *env)
{
	uint8_t bits = 0;

	for (unsigned step = 0; step < S11_STEP_RESERVED; step++) {
		if (s11_step_pick(step, env) == S11_PICK_OPEN)
			bits |= S11_STEP_BIT(step);
	}
	return bits;
}

uint8_t s11_unasked(const struct s11_step_env *env)
{
	return (uint8_t)(s11_pickable(env) &
			 ~S11_STEP_BIT(S11_STEP_BOOTLOADER));
}

bool s11_select(struct s11_step_env *env, s11_ask_fn ask, void *ctx)
{
	uint8_t chosen = 0;
	const enum s11_answer a = ask != NULL ? ask(ctx, env, &chosen) :
						S11_ANSWER_UNASKED;

	switch (a) {
	case S11_ANSWER_INSTALL:
		env->selected = (uint8_t)(chosen & s11_pickable(env));
		return true;
	case S11_ANSWER_CANCEL:
		env->selected = 0;
		return false;
	case S11_ANSWER_UNASKED:
	default:
		/* an invalid package runs only after S-15.197's prompt */
		if (env->sig == S11_SIG_INVALID) {
			env->selected = 0;
			return false;
		}
		env->selected = s11_unasked(env);
		return true;
	}
}
