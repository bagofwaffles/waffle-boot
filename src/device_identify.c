/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-18.002 to S-18.004: the decision between the devices, by the boot mode
 * and the first stage's fingerprint.
 */
#include "waffle/device.h"

#include <stddef.h>

#include "waffle/bytes.h"
#include "waffle/wire_crc.h"

bool wb_first_stage_lookup(uint32_t n, uint32_t c, enum wb_dev_id *id)
{
	static const struct wb_device *const all[WB_DEV_COUNT] = {
		&wb_device_opxy, &wb_device_op1f,
	};
	unsigned i;

	for (i = 0; i < WB_DEV_COUNT; i++) {
		if (all[i]->first_stage_n == n && all[i]->first_stage_crc == c) {
			*id = all[i]->id;
			return true;
		}
	}
	return false;
}

/* The bytes are read in pieces of this many; each byte once. */
#define CHUNK 256u

/* One try of the read: 0 with *n and *c set (*c 0 when n is out of
 * range), or the failed read's result. */
static int fingerprint(wb_flash_read_fn read, void *ctx, uint32_t *n, uint32_t *c)
{
	uint8_t buf[CHUNK];
	uint32_t a, off, crc;
	int rc;

	*n = 0;
	*c = 0;
	rc = read(ctx, 0, buf, WB_FS_HEAD);
	if (rc != 0)
		return rc;
	a = wb_get_le32(buf + WB_FS_ARG_OFF);
	*n = a > 0xFFFFFFFFu - WB_FS_HEAD ? 0xFFFFFFFFu : a + WB_FS_HEAD;
	if (*n <= WB_FS_HEAD || *n > WB_FS_N_MAX)
		return 0;
	/* the header's bytes count once, from the read above; in software:
	 * portable, before any peripheral set-up, and in 256-byte pieces */
	crc = wire_crc32(0, buf, WB_FS_HEAD);
	for (off = WB_FS_HEAD; off < *n;) {
		const uint32_t k = *n - off < CHUNK ? *n - off : CHUNK;

		rc = read(ctx, off, buf, k);
		if (rc != 0)
			return rc;
		crc = wire_crc32(crc, buf, k);
		off += k;
	}
	*c = crc;
	return 0;
}

/* The device a whole read names (rc 0), or the default, not known. */
static struct wb_dev_decision named(int rc, uint32_t n, uint32_t c)
{
	struct wb_dev_decision d = { WB_DEV_DEFAULT, WB_DEV_BY_TABLE };

	if (rc != 0 || n <= WB_FS_HEAD || n > WB_FS_N_MAX || !wb_first_stage_lookup(n, c, &d.id)) {
		d.id = WB_DEV_DEFAULT;
		d.how = WB_DEV_UNKNOWN;
	}
	return d;
}

struct wb_dev_decision wb_first_stage_identify_n(wb_flash_read_fn read, void *ctx,
						 uint32_t *n, uint32_t *c, unsigned tries)
{
	struct wb_dev_decision d = { WB_DEV_DEFAULT, WB_DEV_UNKNOWN };

	for (unsigned t = 0; t < tries; t++) {
		const int rc = fingerprint(read, ctx, n, c);

		if (rc != 0)
			*c = 0;
		d = named(rc, *n, *c);
		/* S-18.003: the first row decides; no try after a full busy wait */
		if (d.how == WB_DEV_BY_TABLE || rc == WB_FS_READ_BUSY)
			break;
	}
	return d;
}

void wb_device_decide_role(unsigned boot_mode, wb_strap_fn pa2, void *pa2_ctx,
			   wb_flash_read_fn read, void *ctx, struct wb_dev_role *out)
{
	out->main = true;
	out->fallback = false;
	out->n = 0;
	out->c = 0;
	if (boot_mode != WB_BOOT_MODE_MAIN) {
		/* S-18.002: the fallback sample, the one before the flash read */
		if ((pa2(pa2_ctx) & WB_STRAP_PA2) == 0u) {
			/* the second CPU, no flash read */
			out->main = false;
			out->d.id = WB_DEV_OPXY;
			out->d.how = WB_DEV_SECOND_CPU;
			return;
		}
		out->fallback = true;
	}
	out->d = wb_first_stage_identify_n(read, ctx, &out->n, &out->c, WB_FS_TRIES);
}
