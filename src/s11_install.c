/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/s11_package.h"
#include "waffle/s11_run.h"

static void refused(struct s11_install_result *out, enum s11_reason r)
{
	out->reason = r;
	out->outcome.kind = S11_OUTCOME_REFUSED;
	out->outcome.reason = r;
	out->outcome.step = 0;
	out->outcome.result = s11_reason_result(r);
}

/* S-11.020: a refused package starts no run; S-11.055, S-11.090,
 * S-11.091, S-11.096: forcing, the option, the confirmations and the
 * return to the factory bootloader are fixed before step 1 and hold for the whole run. */
void s11_install(const struct s11_install_ops *ops,
		 const struct s11_check_in *in, struct s17_copro *copro,
		 struct s11_install_result *out)
{
	struct s11_check_out checked;
	struct s11_staged_sizes sizes;
	struct s11_staged st;
	struct s11_step_env env;
	struct s11_run_cfg cfg;
	enum s11_reason r;

	out->te_signed = out->forced = out->stock = false;
	out->run.end = S11_RUN_FAILED;
	out->run.step = 0xFF;
	out->run.result = S11_OK;
	out->run.versions_read = out->run.nordic_in_dfu = false;
	r = s11_check_package(ops->pkg, in, &checked);	/* checks 1-15 */
	out->te_signed = checked.te_signed;
	out->forced = checked.forced;
	if (r != S11_REASON_NONE) {
		refused(out, r);
		return;
	}
	r = s11_check_zip(ops->zip, &sizes);		/* checks 16-17 */
	if (r != S11_REASON_NONE) {
		refused(out, r);
		return;
	}
	out->reason = S11_REASON_NONE;
	s11_staged_scan(ops->fs, &st);
	/* S-11.096: asked before the run's first write */
	env.confirmed = s11_confirm(&st, in->option, ops->confirm,
				    ops->confirm_ctx);
	env.off = ops->off;
	out->stock = s11_return_to_stock(in->option, checked.te_signed,
					 (env.confirmed &
					  S11_STEP_BIT(S11_STEP_BOOTLOADER)) != 0u);
	env.staged = &st;
	env.option = in->option;
	env.forced = checked.forced;
	env.nordic_in_dfu = false;
	env.copro = copro;
	cfg.content_only = s11_staged_content_only(&st);	/* S-11.056 */
	cfg.nordic = s11_run_nordic(&st, env.off);
	cfg.has_version = st.has_fw_version;			/* S-11.038 */
	cfg.version = st.fw_version;
	s11_run_steps(ops->run, ops->steps, &env, &cfg, &out->run);
	s11_run_outcome(&out->run, &out->outcome);
}
