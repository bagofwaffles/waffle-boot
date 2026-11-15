/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07.080-S-07.085: the backplane step from an image in memory. The
 * exchange is s07_backplane's; this checks the image before it starts.
 */
#include "waffle/s07_program.h"

int s07_program(struct s07_psoc *p, const char *image, size_t len,
		const uint8_t unlock[S07_CY_UNLOCK_LEN], bool *ok)
{
	struct s07_cyacd f;
	uint32_t why = 0;

	*ok = false;
	if (p->off)
		return S07_POWEROFF;	/* S-07.051: nothing more */
	if (!unlock)
		why = S07_CY_WHY_UNLOCK;	/* S-07.081 */
	else if (!s07_cyacd_open(&f, image, len) || !s07_cyacd_check(&f) ||
		 (f.checksum_type == S07_CY_CRC && !p->ops->crc16))
		why = S07_CY_WHY_FILE;		/* S-07.083 */
	if (why) {
		if (p->ops->log)
			p->ops->log(p->ops->ctx, S07_LOG_BACKPLANE, 0, why);
		return S07_OK;
	}
	return s07_backplane(p, &f, unlock, ok);
}
