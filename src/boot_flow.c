/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The boot flow's order (see waffle/boot.h). */
#include "waffle/boot.h"

#include "waffle/s04_records.h"

/* The master's state for a failed load's held mode (one boot at a time). */
static struct boot_held master_held;

void boot_master(const struct boot_master_ops *ops)
{
	struct s05_decision d;
	struct s05_master m;
	uint32_t state = 0;
	bool nonzero = false, present;
	uint16_t reason;
	enum s05_keys keys;

	ops->bringup(ops->ctx);
	/* S-13a.021: before the decision; a failure halts. */
	if (!ops->ddr_check(ops->ctx)) {
		ops->ddr_fatal(ops->ctx);
		return;
	}
	/* S-05.001: the record after S-02.065's restore. */
	ops->metadata(ops->ctx, &state, &nonzero);
	reason = ops->reboot_reason(ops->ctx);
	/* S-07.023 steps 1-6a: the keys are the decision's last input. They
	 * come before the slave's stream (S-09.001, S-02.054, S-16.233). */
	keys = ops->psoc_start(ops->ctx);
	/* S-01.001 step 5, S-09.001: the slave before any eMMC access. With
	 * one CPU there is no slave to be absent: the eMMC is its own. */
	if (wb_has(ops->dev, WB_F_SECOND_CPU)) {
		present = ops->slave_start(ops->ctx);
	} else {
		if (ops->storage_start != NULL)
			ops->storage_start(ops->ctx);
		present = true;
	}
	s05_boot_decision(reason, state, keys, present, &d);

	master_held.reason = reason;
	master_held.load_attempted = false;
	master_held.cause = S05_FAIL_NONE;
	master_held.start = (struct boot_start){ 0 };
	master_held.slave_absent = !present;
	/* S-11.080; the test OS (waffle/testos.h) leaves the state alone. */
	master_held.interrupted = OPXY_TESTOS_OR(false, d.interrupted);

	/* S-09.008: with the slave absent there is no load to attempt. The
	 * test OS (waffle/testos.h) is held mode whatever the decision. */
	if (OPXY_TESTOS_OR(false, d.boot_os) && present) {
		/* S-15.175: the setting, read only at a start; with it on, held
		 * mode with no load attempted and no boot prompt. Not on the
		 * one start after Continue to the OS (S-04.013) or after Start
		 * /os.ldr once (S-04.014). */
		if (reason != S04_REASON_OS_ONCE && reason != S04_REASON_OS_FILE &&
		    ops->start_in_menu != NULL && ops->start_in_menu(ops->ctx)) {
			ops->held(ops->ctx, &master_held);
			return;
		}
#if OPXY_BOOT_PROMPT
		/* S-05.025: a build with the boot prompt asks here, after the
		 * slave's init session and before S-05.003's steps; digit 2
		 * is held mode with no load attempted and no cause. */
		if (ops->boot_prompt != NULL &&
		    ops->boot_prompt(ops->ctx) == BOOT_PROMPT_HELD) {
			ops->held(ops->ctx, &master_held);
			return;
		}
#endif
		m.staging = ops->staging;
		m.config_addr = ops->config_addr;
		m.sw_version_nonzero = nonzero;
		m.file = reason == S04_REASON_OS_FILE;	/* S-05.035 */
		m.dev = ops->dev;
		s05_master_handoff(ops->handoff, &m);
		return;
	}
	ops->held(ops->ctx, &master_held);
}

void boot_master_load_failed(const struct boot_master_ops *ops,
			     enum s05_fail cause, const struct boot_start *start)
{
	master_held.load_attempted = true;
	master_held.cause = cause;
	if (start != NULL)
		master_held.start = *start;
	ops->held(ops->ctx, &master_held);
}

void boot_held_enter(const struct boot_held_ops *ops, const struct boot_held *h)
{
	/* S-08.001: the greeting comes before any other Nordic exchange,
	 * S-17's included. */
	ops->nordic_greeting(ops->ctx);
	/* S-07.023 step 8. */
	ops->psoc_info(ops->ctx);
	ops->psoc_held_state(ops->ctx);
	ops->service(ops->ctx, h);
}

void boot_slave(const struct boot_slave_ops *ops)
{
	ops->bringup(ops->ctx);
	if (!ops->ddr_check(ops->ctx)) {
		ops->ddr_fatal(ops->ctx);
		return;
	}
	/*
	 * S-05.020: serve until the release; after a reply of 1 serve on
	 * (S-05.021, S-09.052). After a reply of 0 the hand-off's own steps
	 * run on the image os_status staged.
	 */
	for (;;) {
		if (ops->serve_until_release(ops->ctx) == 0u &&
		    s05_slave_release(ops->handoff, ops->staging,
				      ops->config_addr))
			return;
	}
}
