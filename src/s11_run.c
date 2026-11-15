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
		step++;
		(void)ops->record_write(ops->ctx, step);
	}

	/* S-11.064: no run leaves the state set; only a reset during one
	 * does (S-11.065, S-11.080). */
	if (step == S11_STEP_DONE) {
		if (!cfg->content_only) {
			/* SPEC-Q: S-11.073 records the package version
			 * though a run skipped components; their own reported
			 * versions are kept. */
			if (cfg->has_version)
				(void)ops->meta_complete(ops->ctx, &cfg->version);
			else	/* S-11.038: the state only */
				(void)ops->meta_state(ops->ctx, 0);
		}
		(void)ops->record_write(ops->ctx, 0);
		res->end = nothing ? S11_RUN_NOTHING_TO_DO : S11_RUN_DONE;
	} else {
		if (!cfg->content_only)
			(void)ops->meta_state(ops->ctx, 0);
		res->end = S11_RUN_FAILED;
	}
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
			      enum s11_result own)
{
	enum s11_result r;

	if (ops->write_failure == NULL)
		return own;
	r = ops->write_failure(ops->ctx);
	return r == S11_BAD_FILE || r == S11_INTERNAL ? r : own;
}

/* S-11.073: a switched-off step makes no version test, no check and no
 * exchange with its device. */
static bool off(const struct s11_step_env *env, unsigned step)
{
	return (env->off & S11_STEPS_SWITCHABLE & S11_STEP_BIT(step)) != 0u;
}

/* S-11.068, S-11.072, S-11.090: no version test, so a return to the factory
 * bootloader writes any version (S-11.093) */
static enum s11_result protected_step(const struct s11_step_env *env,
				      unsigned step, enum s11_component c,
				      bool (*write)(void *ctx),
				      const struct s11_step_ops *ops)
{
	if (!s11_staged_present(env->staged, c))
		return S11_NOT_FOUND;
	if (!env->option)
		return S11_SKIPPED;	/* no SPI flash access (S-02.057) */
	if ((env->confirmed & S11_STEP_BIT(step)) == 0u)
		return S11_SKIPPED;	/* S-11.096: as with the option off */
	if (write(ops->ctx))
		return S11_OK;
	return failed(ops, S11_FAILURE);
}

enum s11_result s11_step_result(unsigned step, const struct s11_step_env *env,
				const struct s11_step_ops *ops)
{
	const struct s11_staged *st = env->staged;
	const struct s11_version *v;
	int slot;

	switch (step) {
	case S11_STEP_CONTENT:			/* S-11.066 */
		return ops->content(ops->ctx);
	case S11_STEP_OS:			/* S-11.067: no version test */
		if (!s11_staged_present(st, S11_C_OS))
			return S11_NOT_FOUND;
		if (!ops->os_write(ops->ctx))
			return failed(ops, S11_INTERNAL);
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
		if (!env->nordic_in_dfu)
			return S11_FAILURE;
		if (!ops->nordic_send(ops->ctx, S11_C_BLE))
			return failed(ops, S11_FAILURE);
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
		slot = ops->nordic_boot_slot(ops->ctx);	/* S-16.124 */
		if (slot != 0 && slot != 1)
			return S11_FAILURE;
		if (!s11_staged_present(st, slot ? S11_C_NORDIC_BL1 :
						   S11_C_NORDIC_BL0))
			return S11_NOT_FOUND;
		if (!ops->nordic_send(ops->ctx, slot ? S11_C_NORDIC_BL1 :
							S11_C_NORDIC_BL0))
			return failed(ops, S11_FAILURE); /* S-16.032: version unchanged */
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
		return ops->psoc_program(ops->ctx) ? S11_OK :
						     failed(ops, S11_FAILURE);
	case S11_STEP_SPIBOOT:			/* S-11.072 */
		return protected_step(env, step, S11_C_SPIBOOT,
				      ops->spiboot_write, ops);
	default:
		return S11_INTERNAL;
	}
}

/* The component a step reports Not found without; the Nordic bootloader
 * step has two (S-11.070). */
static bool carried(unsigned step, const struct s11_staged *st)
{
	switch (step) {
	case S11_STEP_BOOTLOADER:
		return s11_staged_present(st, S11_C_BOOTLOADER);
	case S11_STEP_BLE:
		return s11_staged_present(st, S11_C_BLE);
	case S11_STEP_BLE_BOOTLOADER:
		return s11_staged_present(st, S11_C_NORDIC_BL0) ||
		       s11_staged_present(st, S11_C_NORDIC_BL1);
	case S11_STEP_KEYBOARD:
		return s11_staged_present(st, S11_C_BACKPLANE);
	case S11_STEP_SPIBOOT:
		return s11_staged_present(st, S11_C_SPIBOOT);
	default:
		return false;
	}
}

enum s11_skip s11_step_skip(unsigned step, const struct s11_step_env *env)
{
	if (!carried(step, env->staged))
		return S11_SKIP_NONE;
	if (step == S11_STEP_BOOTLOADER || step == S11_STEP_SPIBOOT) {
		if (!env->option)
			return S11_SKIP_OPTION_OFF;
		if ((env->confirmed & S11_STEP_BIT(step)) == 0u)
			return S11_SKIP_NOT_CONFIRMED;
		return S11_SKIP_NONE;
	}
	return off(env, step) ? S11_SKIP_SWITCHED_OFF : S11_SKIP_NONE;
}

bool s11_run_nordic(const struct s11_staged *st, uint8_t off_steps)
{
	const uint8_t on = (uint8_t)~(off_steps & S11_STEPS_SWITCHABLE);

	return ((on & S11_STEP_BIT(S11_STEP_BLE)) != 0u &&
		carried(S11_STEP_BLE, st)) ||
	       ((on & S11_STEP_BIT(S11_STEP_BLE_BOOTLOADER)) != 0u &&
		carried(S11_STEP_BLE_BOOTLOADER, st));
}

uint8_t s11_confirm(const struct s11_staged *st, bool option,
		    bool (*ask)(void *ctx, enum s11_component c), void *ctx)
{
	uint8_t confirmed = 0;

	if (!option || ask == NULL)
		return 0;
	if (s11_staged_present(st, S11_C_BOOTLOADER) &&
	    ask(ctx, S11_C_BOOTLOADER))
		confirmed |= S11_STEP_BIT(S11_STEP_BOOTLOADER);
	if (s11_staged_present(st, S11_C_SPIBOOT) && ask(ctx, S11_C_SPIBOOT))
		confirmed |= S11_STEP_BIT(S11_STEP_SPIBOOT);
	return confirmed;
}
