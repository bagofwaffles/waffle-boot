/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07.080-S-07.085 for S-11's backplane step: program the
 * PSoC's application from a .cyacd image held in memory, over the master's
 * S-06 link to the PSoC.
 */
#ifndef WAFFLE_S07_PROGRAM_H
#define WAFFLE_S07_PROGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s07_cypress.h"
#include "waffle/s07_psoc.h"

/*
 * Program the PSoC from image[0..len) (.cyacd text, format v0), entering its
 * bootloader with the PSoC unlock (Q-UNLOCK: the board's build-time value;
 * NULL: none). *ok is the step's result, the last run's (S-07.084); the
 * return wait of S-07.085 follows any run, and does not change it.
 *
 * Without the unlock (S-07.081), or with a bad image (S-07.083,
 * s07_cyacd_check; also one whose checksum type needs a CRC p->ops lacks),
 * the step fails before entry with nothing sent to the PSoC: *ok false, S-14
 * told S07_LOG_BACKPLANE with at 0 and why S07_CY_WHY_UNLOCK or
 * S07_CY_WHY_FILE.
 *
 * Returns S07_OK, or S07_POWEROFF as every waiting S-07 function does
 * (s07_psoc.h); *ok still holds the runs' result then.
 */
int s07_program(struct s07_psoc *p, const char *image, size_t len,
		const uint8_t unlock[S07_CY_UNLOCK_LEN], bool *ok);

#endif
