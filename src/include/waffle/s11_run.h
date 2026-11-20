/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S11_RUN_H
#define WAFFLE_S11_RUN_H

/*
 * S-11 sections 5, 7 and 8: the run (its start, steps, progress record and
 * end), the step results, an install a reset interrupted, and the protected
 * steps.
 * The metadata record (S-04), the progress record's file, the Nordic and
 * PSoC exchanges (S-07, S-08) and the image writers (S-02, S-03) come in
 * through ops.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s11_common.h"
#include "waffle/s11_staging.h"
#include "waffle/s11_version.h"
#include "waffle/s17_revisions.h"

#define S11_STEP_CONTENT 0u
#define S11_STEP_OS 1u
#define S11_STEP_BOOTLOADER 2u
#define S11_STEP_BLE 3u
#define S11_STEP_BLE_BOOTLOADER 4u
#define S11_STEP_KEYBOARD 5u
#define S11_STEP_RESERVED 6u	/* does nothing: Not found */
#define S11_STEP_DONE 7u		/* every step is done (S-11.057) */

#define S11_STATE_UPGRADING 0xAB0Du	/* the metadata state (S-04.045) */
#define S11_RECORD_PATH "upgrade/progress.log"

/* The progress record's two bytes (S-11.062): the step to run next, 0-7,
 * then a count that is always 0 here. The record is written for S-11.094
 * and never read. */
void s11_record_bytes(uint8_t step, uint8_t out[2]);

/* After this result the run goes on (S-11.059). */
bool s11_result_continues(enum s11_result r);

/* ---- the run (S-11.057-S-11.065) ---- */

struct s11_run_ops {
	/* S-04.045: M1 (state 0xAB0D) and M4 (state 0). */
	bool (*meta_state)(void *ctx, uint16_t state);
	/* S-04.045 M3: os_version set to `v` and state 0 in one write. */
	bool (*meta_complete)(void *ctx, const struct s11_version *v);
	/* Replaces upgrade/progress.log with {step, 0} (S-11.062). */
	bool (*record_write)(void *ctx, uint8_t step);
	/* S-08: asks the Nordic to enter DFU; false when that fails. */
	bool (*nordic_enter_dfu)(void *ctx);
	/* S-11.061 step 3: the Nordic's versions (S-17.021 step 2);
	 * returns whether the Nordic reported its bootloader mode (DFU). */
	bool (*nordic_read_versions)(void *ctx);
	/* S-11.064 step 3: takes the Nordic out of DFU if it is in it. */
	void (*nordic_exit_dfu)(void *ctx);
	/* One step's result (S-11.066-S-11.071). */
	enum s11_result (*step)(void *ctx, unsigned step);
	void *ctx;
};

struct s11_run_cfg {
	bool content_only;		/* S-11.056, from the staging area */
	bool nordic;			/* a Nordic image is present (S-11.061) */
	bool has_version;		/* S-11.038: firmware/firmware.version */
	struct s11_version version;
};

enum s11_run_end {
	S11_RUN_DONE,
	S11_RUN_NOTHING_TO_DO,
	S11_RUN_FAILED,			/* the state is 0 again (S-11.064) */
};

struct s11_run_result {
	enum s11_run_end end;
	uint8_t step;			/* FAILED: the step that stopped the run, */
	enum s11_result result;		/* and its result; 0xFF before any step */
	/* The start-of-run version read was made: S-17.021 step 1's
	 * end-of-run exchange follows only then. */
	bool versions_read;
	bool nordic_in_dfu;		/* what that read reported */
};

enum s11_run_end s11_run(const struct s11_run_ops *ops,
			 const struct s11_run_cfg *cfg,
			 struct s11_run_result *res);

/* The outcome to report (S-11.060, S-11.051) of a run that ended. */
void s11_run_outcome(const struct s11_run_result *res, struct s11_outcome *o);

/* S-11.080: a boot that found the state 0xAB0D. A reset fell during a run:
 * the state goes back to 0 (M4) and nothing else is done; no step, no
 * record access, no Nordic exchange. The caller stays in held mode and says
 * that the install did not finish. False when the write failed: the next
 * boot then finds the state again. */
bool s11_interrupted(const struct s11_run_ops *ops);

struct s11_step_ops;

/* ---- the steps (S-11.066-S-11.071, S-11.090) ---- */

struct s11_step_ops {
	/* Step 0 (S-11.066, S-11.075-S-11.079). */
	enum s11_result (*content)(void *ctx);
	/* S-03 section 5, always the plaintext form, a return to the factory bootloader
	 * included (S-11.092). */
	bool (*os_write)(void *ctx);
	/* S-04.045 M2 */
	void (*meta_sw_version)(void *ctx, const struct s11_version *v);
	/* S-02.042-S-02.053, S-02.090, S-02.091 */
	bool (*bootloader_write)(void *ctx);
	/* The result of the os_write, bootloader_write, nordic_send or
	 * psoc_program that just returned false, when the image was refused
	 * before any byte of it was written or sent: S11_BAD_FILE when its
	 * check failed (S-11.095) or S-02.091 refused it (S-11.068),
	 * S11_INTERNAL when a read of it failed
	 * (S-11.095); for bootloader_write alone, S11_SKIPPED when
	 * S-02.090 refused its start in S-11.068 item 5's case (already
	 * written); any other value, or NULL, means the write itself
	 * failed and the step reports its own failure result (Internal
	 * error for the OS step, S-11.067; Failure otherwise). */
	enum s11_result (*write_failure)(void *ctx);
	/* S-08's DFU transfer of a staged image */
	bool (*nordic_send)(void *ctx, enum s11_component c);
	/* S-08: the Nordic bootloader slot to write, 0 or 1; -1 for any
	 * other answer or none (S-16.124). */
	int (*nordic_boot_slot)(void *ctx);
	/* S-07's bootloader-host protocol */
	bool (*psoc_program)(void *ctx);
	void *ctx;
};

/* A step's bit in s11_step_env's off and confirmed. */
#define S11_STEP_BIT(step) ((uint8_t)(1u << (step)))
/* The steps S-11.073 lets a build switch off: the Nordic and PSoC
 * steps. */
#define S11_STEPS_SWITCHABLE (S11_STEP_BIT(S11_STEP_BLE) | \
			      S11_STEP_BIT(S11_STEP_BLE_BOOTLOADER) | \
			      S11_STEP_BIT(S11_STEP_KEYBOARD))

struct s11_step_env {
	const struct s11_staged *staged;
	bool option;			/* S-11.090: as when the checks began */
	bool forced;			/* S-11.055 */
	bool nordic_in_dfu;		/* reported at the run's start */
	struct s17_copro *copro;	/* the installed versions (S-17.034) */
	/* S-11.073: the switched-off steps, a bit each; only those of
	 * S11_STEPS_SWITCHABLE count. */
	uint8_t off;
	/* S-11.096: the protected steps confirmed on the unit, a bit each. */
	uint8_t confirmed;
};

enum s11_result s11_step_result(unsigned step, const struct s11_step_env *env,
				const struct s11_step_ops *ops);

/* Why a step whose component is present reports Skipped (S-11.058), as
 * decided before the step runs. */
enum s11_skip s11_step_skip(unsigned step, const struct s11_step_env *env);

/* Why a step that reported r reports Skipped: S11_SKIP_NONE unless r is
 * S11_SKIPPED; the bootloader step with the option on and confirmed was
 * skipped in S-11.068 item 5's case. */
enum s11_skip s11_result_skip(unsigned step, const struct s11_step_env *env,
			      enum s11_result r);

/* S-11.061 item 2: a Nordic image is present whose step is switched on. */
bool s11_run_nordic(const struct s11_staged *st, uint8_t off);

/*
 * S-11.096: the protected component the package carries, the bootloader,
 * asked for on the unit; the bit of the step confirmed. With the option
 * off, or ask NULL, nothing is asked and none is confirmed.
 */
uint8_t s11_confirm(const struct s11_staged *st, bool option,
		    bool (*ask)(void *ctx, enum s11_component c), void *ctx);

/* s11_run with each step's result from s11_step_result(); the run's
 * Nordic read fills env->nordic_in_dfu. */
enum s11_run_end s11_run_steps(const struct s11_run_ops *ops,
			       const struct s11_step_ops *steps,
			       struct s11_step_env *env,
			       const struct s11_run_cfg *cfg,
			       struct s11_run_result *res);

/* ---- an install from end to end (S-11.020, S-11.055, S-11.090, S-11.091) */

struct s11_package_ops;
struct s11_check_in;

struct s11_install_ops {
	const struct s11_package_ops *pkg;	/* checks 1-15 */
	const struct s11_zip_ops *zip;		/* checks 16-18 */
	const struct s11_fs_ops *fs;		/* the staging area */
	const struct s11_run_ops *run;
	const struct s11_step_ops *steps;
	/* S-11.096: the user's answer on the unit for one protected
	 * component; NULL: none is confirmed. */
	bool (*confirm)(void *ctx, enum s11_component c);
	void *confirm_ctx;
	uint8_t off;				/* S-11.073 */
};

struct s11_install_result {
	enum s11_reason reason;		/* NONE: the package was accepted */
	bool te_signed, forced;
	bool stock;			/* a return to the factory bootloader (S-11.091) */
	struct s11_run_result run;
	struct s11_outcome outcome;	/* for S-10c and S-15 (S-11.051) */
};

/*
 * One package: checks 1-15, then 16-18 (a refusal at any of them starts
 * nothing after it), then the confirmations of S-11.096, then the run. The option in `in` is the one when the
 * checks began, and holds for the whole run (S-11.090).
 */
void s11_install(const struct s11_install_ops *ops,
		 const struct s11_check_in *in, struct s17_copro *copro,
		 struct s11_install_result *out);

#endif /* WAFFLE_S11_RUN_H */
