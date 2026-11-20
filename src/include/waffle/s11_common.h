/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S11_COMMON_H
#define WAFFLE_S11_COMMON_H

/*
 * S-11 values shared with the host protocol (S-10c): the product-word rule,
 * the refusal reasons, the step result codes and a package's outcome.
 */

#include <stdbool.h>
#include <stdint.h>

/* S-11.007: bits 14-23 equal 0x21 and bits 10-13 are 0. */
bool s11_product_ok(uint32_t word);

/* Refusal reasons in S-11.020's check order. */
enum s11_reason {
	S11_REASON_NONE,
	S11_REASON_SHORT,
	S11_REASON_PREAMBLE,
	S11_REASON_TYPE,
	S11_REASON_MAGIC,
	S11_REASON_PRODUCT,
	S11_REASON_DEVICE,
	S11_REASON_CRC,
	S11_REASON_FORMAT,
	S11_REASON_SKU,
	S11_REASON_LENGTH,
	S11_REASON_HASH,
	S11_REASON_KEY,
	S11_REASON_NOKEY,
	S11_REASON_NAME,
	S11_REASON_ZIPLEN,
	S11_REASON_ZIP,
	S11_REASON_MEMBER,
	S11_REASON_SPACE,
	S11_REASON_SPACEQUERY,
	S11_REASON_SIZE,
	S11_REASON_STREAM,		/* check 18 (S-11.035) */
};

/* The reason's name as S-11.020 writes it ("" for S11_REASON_NONE). */
const char *s11_reason_name(enum s11_reason r);

/* Step result codes (S-11.058); 5 and 7 are reserved, and no step
 * reports them. */
enum s11_result {
	S11_OK = 0,
	S11_FAILURE = 1,
	S11_NOT_FOUND = 2,
	S11_BAD_FILE = 3,
	S11_INTERNAL = 4,
	S11_ALREADY = 6,
	S11_SKIPPED = 8,
};

/* The result code that goes with a refusal reason (S-11.020). */
enum s11_result s11_reason_result(enum s11_reason r);

/* What became of a package or a run (S-11.051, S-11.060). */
enum s11_outcome_kind {
	S11_OUTCOME_DONE,
	S11_OUTCOME_NOTHING_TO_DO,
	S11_OUTCOME_REFUSED,		/* `reason` */
	S11_OUTCOME_FAILED,		/* `step` stopped the run with `result` */
};

struct s11_outcome {
	enum s11_outcome_kind kind;
	enum s11_reason reason;
	uint8_t step;
	enum s11_result result;
};

/* Why a step whose component is present reports Skipped (S-11.058). */
enum s11_skip {
	S11_SKIP_NONE,			/* it does not */
	S11_SKIP_OPTION_OFF,		/* a protected step, the option off (S-11.090) */
	S11_SKIP_NOT_SELECTED,		/* a step not selected on the unit (S-11.096) */
	S11_SKIP_SWITCHED_OFF,		/* a switched-off step (S-11.073) */
	S11_SKIP_ALREADY_WRITTEN	/* the bootloader step, S-11.068 item 5 */
};

#endif /* WAFFLE_S11_COMMON_H */
