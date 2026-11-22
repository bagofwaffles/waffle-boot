/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-03 eMMC: the eMMC checks and features, the OS header, the load rule,
 * the OS writer and the user-area map.
 */
#include "waffle/s03_emmc.h"

#include "waffle/bytes.h"

/* EXT_CSD bytes (JESD84-B51) of S-03.009 and S-03.011. */
#define EXT_CSD_FLUSH_CACHE      32u
#define EXT_CSD_CACHE_CTRL       33u
#define EXT_CSD_POWER_OFF_NOTIF  34u
#define EXT_CSD_BKOPS_EN         163u
#define EXT_CSD_PART_CONFIG      179u

static bool same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return false;
	return true;
}

/* --- S-03.010: every eMMC operation is retried a bounded number of times. --- */

/* The sector buffers here start on a 32-byte boundary: the bf70x eMMC
 * driver takes no other (S-13b.054, bf70x_msi.h). */

static int do_select(const struct s03_env *env, unsigned part)
{
	unsigned i;

	for (i = 0; i < S03_RETRIES; i++)
		if (env->ops->select(env->ops->ctx, part) == 0)
			return 0;
	return -1;
}

static int do_read(const struct s03_env *env, uint32_t sector, uint32_t count, uint8_t *buf)
{
	unsigned i;

	for (i = 0; i < S03_RETRIES; i++)
		if (env->ops->read(env->ops->ctx, sector, count, buf) == 0)
			return 0;
	return -1;
}

/* S-03.056: a write counts as done only when the eMMC says so (S-16.091). */
static int do_write(const struct s03_env *env, uint32_t sector, uint32_t count, const uint8_t *buf)
{
	unsigned i;

	for (i = 0; i < S03_RETRIES; i++)
		if (env->ops->write(env->ops->ctx, sector, count, buf) == 0)
			return 0;
	return -1;
}

static int do_erase(const struct s03_env *env, uint32_t group)
{
	unsigned i;

	for (i = 0; i < S03_RETRIES; i++)
		if (env->ops->erase(env->ops->ctx, group * S03_GROUP_SECTORS) == 0)
			return 0;
	return -1;
}

static int do_switch(const struct s03_env *env, uint8_t index, uint8_t value)
{
	unsigned i;

	for (i = 0; i < S03_RETRIES; i++)
		if (env->ops->switch_byte(env->ops->ctx, index, value) == 0)
			return 0;
	return -1;
}

static uint32_t xfer_limit(const struct s03_env *env, uint32_t left)
{
	return (env->max_xfer != 0u && left > env->max_xfer) ? env->max_xfer : left;
}

/* --- The eMMC --- */

/* S-03.001, S-03.002, S-16.070 */
bool s03_emmc_supported(const struct wb_device *dev, uint32_t sec_count,
			uint32_t erase_group_sectors, uint32_t opt_write)
{
	const bool count_ok = dev->mem.emmc_sectors_at_least ? sec_count >= dev->mem.emmc_sectors
							      : sec_count == dev->mem.emmc_sectors;

	return count_ok && erase_group_sectors == S03_GROUP_SECTORS &&
	       opt_write != 0u && 8u % opt_write == 0u;
}

/* S-03.009, S-16.077 */
int s03_emmc_features_on(const struct s03_env *env)
{
	if (do_switch(env, EXT_CSD_POWER_OFF_NOTIF, 0x01u) != 0 ||
	    do_switch(env, EXT_CSD_CACHE_CTRL, 0x01u) != 0 ||
	    do_switch(env, EXT_CSD_BKOPS_EN, 0x02u) != 0)
		return -1;
	return 0;
}

/*
 * S-03.011.  A step that fails once S-03.010's retries are spent does not end
 * the sequence: every later step is made, in order, so the power-off
 * notification is always the last command.  The failure is returned.
 */
int s03_emmc_shutdown(const struct s03_env *env, int (*unmount)(void *ctx), void *unmount_ctx)
{
	int rc = 0;

	if (do_switch(env, EXT_CSD_FLUSH_CACHE, 0x01u) != 0)
		rc = -1;
	if (do_switch(env, EXT_CSD_CACHE_CTRL, 0x00u) != 0)
		rc = -1;
	if (unmount != NULL && unmount(unmount_ctx) != 0)
		rc = -1;
	if (do_switch(env, EXT_CSD_BKOPS_EN, 0x00u) != 0)
		rc = -1;
	if (do_switch(env, EXT_CSD_POWER_OFF_NOTIF, 0x03u) != 0)	/* POWER_OFF_LONG */
		rc = -1;
	return rc;
}

uint32_t s03_power_off_limit_ms(uint8_t power_off_long_time)
{
	return 10u * (power_off_long_time != 0u ? power_off_long_time : 0xFFu);
}

/*
 * S-03.013. The notification is the last command: a retry follows only a
 * command that failed, not a wait that ran out.
 */
int s03_emmc_shutdown_1f(const struct s03_env *env, int (*unmount)(void *ctx), void *unmount_ctx,
			 s03_switch_poll_fn poll, uint8_t power_off_long_time)
{
	const uint32_t limit = s03_power_off_limit_ms(power_off_long_time);
	int rc = 0, r = -1;
	unsigned i;

	if (do_switch(env, EXT_CSD_FLUSH_CACHE, 0x01u) != 0)
		rc = -1;
	if (do_switch(env, EXT_CSD_CACHE_CTRL, 0x00u) != 0)
		rc = -1;
	if (unmount != NULL && unmount(unmount_ctx) != 0)
		rc = -1;
	for (i = 0; i < S03_RETRIES && poll != NULL; i++) {
		r = poll(env->ops->ctx, EXT_CSD_POWER_OFF_NOTIF, 0x03u, limit);
		if (r >= 0)
			break;
	}
	if (r != 0)
		rc = -1;
	return rc;
}

/* S-03.012 */
bool s03_select_value(uint8_t cur, unsigned part, uint8_t *value)
{
	if ((unsigned)(cur & 0x07u) == part)
		return false;
	*value = (uint8_t)((cur & 0xF8u) | (part & 0x07u));
	return true;
}

int s03_select_keep(struct s03_select_keep *k, unsigned part)
{
	uint8_t v;

	if (part > S03_PART_BP2)
		return -1;
	if (!k->known) {
		if (k->read_part_config(k->ctx, &k->cur) != 0)
			return -1;
		k->known = true;
	}
	if (!s03_select_value(k->cur, part, &v))
		return 0;		/* selected without a command */
	if (k->switch_byte(k->ctx, EXT_CSD_PART_CONFIG, v) != 0) {
		k->known = false;	/* what the eMMC holds now is not known */
		return -1;
	}
	k->cur = v;
	return 0;
}

/* --- The header --- */

/* S-03.021 */
void s03_header_decode(const uint8_t raw[S03_SECTOR], struct s03_header *h)
{
	h->format = wb_get_le32(raw);
	h->length = wb_get_le32(raw + 4);
	h->crc = wb_get_le32(raw + 8);
	h->active = raw[S03_ACTIVE_OFF];
	h->encrypted = raw[S03_ENCRYPTED_OFF];
}

/* S-03.053, S-03.026, S-03.033 */
void s03_header_encode(uint32_t length, uint32_t crc, uint8_t active, uint8_t raw[S03_SECTOR])
{
	uint32_t i;

	for (i = 0; i < S03_SECTOR; i++)
		raw[i] = 0;
	wb_put_le32(raw, S03_FORMAT_WRITTEN);
	wb_put_le32(raw + 4, length);
	wb_put_le32(raw + 8, crc);
	raw[S03_ACTIVE_OFF] = active;
	raw[S03_ENCRYPTED_OFF] = 0;
}

/* S-03.022 */
bool s03_header_active(const struct s03_header *h)
{
	return h->active != 0u;
}

/* S-03.023, S-03.024, S-16.139 */
enum s03_mode s03_header_mode(const struct s03_header *h)
{
	if (h->format > 3u)
		return S03_MODE_REFUSED;
	if (h->format >= 2u && h->encrypted != 0u)
		return S03_MODE_ENCRYPTED;
	return S03_MODE_PLAIN;
}

/* S-03.027 */
uint32_t s03_sectors(uint32_t length)
{
	return length / S03_SECTOR + (length % S03_SECTOR != 0u ? 1u : 0u);
}

/* --- The load --- */

/* S-03.042, in its order, with B from the eMMC. */
static enum s03_load_result header_check(const struct s03_header *h, uint32_t boot_size)
{
	uint64_t need;

	if (h->length == 0u)
		return S03_LOAD_ZERO_LENGTH;
	if (h->length > S03_OS_MAX)
		return S03_LOAD_TOO_LONG;
	need = (uint64_t)S03_SECTOR * ((uint64_t)s03_sectors(h->length) + 1u);
	if (need > boot_size)			/* S-16.136 */
		return S03_LOAD_PAST_PARTITION;
	switch (s03_header_mode(h)) {
	case S03_MODE_REFUSED:
		return S03_LOAD_FORMAT;
	case S03_MODE_ENCRYPTED:		/* S-03.048 */
		return S03_LOAD_ENCRYPTED;
	case S03_MODE_PLAIN:
		break;
	default:
		return S03_LOAD_FORMAT;
	}
	return S03_LOAD_OK;
}

/* A failed request of the load, kept in out (S-03.046, S-16.119). */
static enum s03_load_result load_io(struct s03_load *out, enum s03_load_step step,
				    unsigned part)
{
	out->step = step;
	out->step_part = part;
	return S03_LOAD_IO;
}

/* A partition's select and its header's read (S-03.040, S-03.041). */
static enum s03_load_result read_header(const struct s03_env *env, unsigned part,
					uint8_t *hdr, struct s03_load *out)
{
	if (do_select(env, part) != 0)
		return load_io(out, S03_STEP_SELECT, part);
	if (do_read(env, 0, 1, hdr) != 0)
		return load_io(out, S03_STEP_HEADER, part);
	s03_header_decode(hdr, &out->header);
	return S03_LOAD_OK;
}

static enum s03_load_result load_body(const struct s03_env *env, uint8_t *staging,
				      uint32_t staging_cap, struct s03_load *out)
{
	_Alignas(32) uint8_t hdr[S03_SECTOR];
	const struct s03_header *h = &out->header;
	enum s03_load_result r;
	uint32_t n, done;

	/* S-03.040, S-03.041: BP1 first; BP2 only behind an inactive BP1. */
	r = read_header(env, S03_PART_BP1, hdr, out);
	if (r != S03_LOAD_OK)
		return r;
	if (s03_header_active(h)) {
		out->part = S03_PART_BP1;
	} else {
		r = read_header(env, S03_PART_BP2, hdr, out);
		if (r != S03_LOAD_OK)
			return r;
		if (!s03_header_active(h))
			return S03_LOAD_NO_ACTIVE;
		out->part = S03_PART_BP2;
	}
	/* S-03.042: no image read when the header fails; S-03.045: no fallback. */
	r = header_check(h, env->boot_size);
	if (r != S03_LOAD_OK)
		return r;
	/* S-03.043: sectors 1 to N into the staging area. */
	n = s03_sectors(h->length);
	if ((uint64_t)n * S03_SECTOR > staging_cap)
		return S03_LOAD_TOO_LONG;
	for (done = 0; done < n;) {
		uint32_t cnt = xfer_limit(env, n - done);

		if (do_read(env, 1u + done, cnt, staging + (size_t)done * S03_SECTOR) != 0) {
			out->sector = 1u + done;
			out->count = cnt;
			return load_io(out, S03_STEP_IMAGE, out->part);
		}
		done += cnt;
	}
	/* S-03.044 */
	out->crc = env->crc32_fast != NULL ? env->crc32_fast(0u, staging, h->length) : ~h->crc;
	if (out->crc != h->crc)
		out->crc = env->crc32(0u, staging, h->length);
	if (out->crc != h->crc)
		return S03_LOAD_CRC;
	out->length = h->length;
	return S03_LOAD_OK;
}

/* S-03.040 to S-03.048, S-03.007 */
void s03_load_os(const struct s03_env *env, uint8_t *staging, uint32_t staging_cap,
		 struct s03_load *out)
{
	out->part = 0;
	out->length = 0;
	out->select0_failed = false;
	out->step = S03_STEP_NONE;
	out->step_part = 0;
	out->sector = 0;
	out->count = 0;
	out->header = (struct s03_header){ 0 };
	out->crc = 0;
	out->result = load_body(env, staging, staging_cap, out);
	/* S-03.007, S-03.046: always end on the user area. A failure before
	 * it stays the one the load reports. */
	if (do_select(env, S03_PART_USER) != 0) {
		out->select0_failed = true;
		if (out->result != S03_LOAD_IO)
			(void)load_io(out, S03_STEP_USER, S03_PART_USER);
		out->result = S03_LOAD_IO;
	}
	if (out->result != S03_LOAD_OK)
		out->length = 0;
}

/* --- The writer --- */

/* S-03.051, S-16.137 */
bool s03_image_acceptable(uint32_t length, uint32_t boot_size)
{
	if (length + 512u < 10000u || length > S03_OS_MAX)
		return false;
	return (uint64_t)S03_SECTOR * ((uint64_t)s03_sectors(length) + 1u) <= boot_size;
}

/* A write in progress: the eMMC, and what it reports and to whom. */
struct wrun {
	const struct s03_env *env;
	struct s03_write *out;
	const struct s03_write_watch *watch;
	unsigned part;		/* the partition selected last */
	bool mutated;		/* an erase or a write has been issued */
};

static void w_stage(struct wrun *r, enum s03_write_stage stage)
{
	r->out->stage = stage;
	if (r->watch != NULL && r->watch->stage != NULL)
		r->watch->stage(r->watch->ctx, stage, r->out);
}

static void w_progress(const struct wrun *r, uint32_t done, uint32_t total)
{
	if (r->watch != NULL && r->watch->progress != NULL)
		r->watch->progress(r->watch->ctx, done, total);
}

/* A failure, of which the first is the one reported. */
static int w_failed(struct wrun *r, enum s03_write_fail fail, uint32_t at)
{
	if (r->out->fail == S03_FAIL_NONE) {
		r->out->fail = fail;
		r->out->part = r->part;
		r->out->at = at;
	}
	return -1;
}

static int w_select(struct wrun *r, unsigned part)
{
	r->part = part;
	return do_select(r->env, part) == 0 ? 0 : w_failed(r, S03_FAIL_SELECT, 0);
}

static int w_read(struct wrun *r, uint32_t sector, uint32_t count, uint8_t *buf)
{
	return do_read(r->env, sector, count, buf) == 0 ? 0 : w_failed(r, S03_FAIL_READ, sector);
}

static int w_write(struct wrun *r, uint32_t sector, uint32_t count, const uint8_t *buf)
{
	r->mutated = true;
	return do_write(r->env, sector, count, buf) == 0 ? 0 : w_failed(r, S03_FAIL_WRITE, sector);
}

static int w_erase(struct wrun *r, uint32_t group)
{
	r->mutated = true;
	return do_erase(r->env, group) == 0 ? 0 : w_failed(r, S03_FAIL_ERASE, group);
}

/* S-03.068 */
static int w_flush(struct wrun *r)
{
	return do_switch(r->env, EXT_CSD_FLUSH_CACHE, 0x01u) == 0 ? 0 : w_failed(r, S03_FAIL_FLUSH, 0);
}

/* Write sectors 1..N of the stored region; the last partial sector is padded with 0. */
static int write_stored(struct wrun *r, const uint8_t *image, uint32_t length)
{
	uint32_t full = length / S03_SECTOR, n = s03_sectors(length), done = 0;

	while (done < full) {
		uint32_t cnt = xfer_limit(r->env, full - done);

		if (w_write(r, 1u + done, cnt, image + (size_t)done * S03_SECTOR) != 0)
			return -1;
		done += cnt;
		w_progress(r, done, n);
	}
	if (length % S03_SECTOR != 0u) {
		_Alignas(32) uint8_t last[S03_SECTOR];
		uint32_t i;

		for (i = 0; i < S03_SECTOR; i++)
			last[i] = i < length % S03_SECTOR ? image[(size_t)full * S03_SECTOR + i] : 0u;
		if (w_write(r, 1u + full, 1, last) != 0)
			return -1;
		w_progress(r, n, n);
	}
	return 0;
}

/* The offset of the first byte where a and b differ, n when none does. */
static uint32_t diff_at(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return i;
	return n;
}

/*
 * S-03.055 step 3: read sectors 1..N back and compare every one of the length
 * bytes.  The whole range is read even after a mismatch.
 */
static int verify_stored(struct wrun *r, const uint8_t *image, uint32_t length)
{
	const struct s03_env *env = r->env;
	uint32_t n = s03_sectors(length), per = env->scratch_len / S03_SECTOR, done = 0;
	int rc = 0;

	if (per == 0u)
		return -1;
	while (done < n) {
		uint32_t cnt = xfer_limit(env, n - done < per ? n - done : per);
		uint32_t off = done * S03_SECTOR;
		uint32_t len = cnt * S03_SECTOR, at;

		if (off + len > length)
			len = length - off;
		if (w_read(r, 1u + done, cnt, env->scratch) != 0)
			return -1;
		at = diff_at(env->scratch, image + off, len);
		if (at != len)					/* S-16.121: every sector */
			rc = w_failed(r, S03_FAIL_MISMATCH, 1u + done + at / S03_SECTOR);
		done += cnt;
		w_progress(r, done, n);
	}
	return rc;
}

/* S-03.055 steps 4-5 or 6-7: write the header, read it back, compare all 512 bytes. */
static int header_step(struct wrun *r, const uint8_t hdr[S03_SECTOR])
{
	_Alignas(32) uint8_t back[S03_SECTOR];

	if (w_write(r, 0, 1, hdr) != 0)
		return -1;
	if (w_read(r, 0, 1, back) != 0)
		return -1;
	return same(back, hdr, S03_SECTOR) ? 0 : w_failed(r, S03_FAIL_MISMATCH, 0);
}

/*
 * S-03.055 step 1: the target's header reads inactive before its first
 * erase and before an image sector is written: a header that reads active
 * gets a zero sector, read back. Group 0, which holds the header, is never
 * erased, so what an erased group reads as plays no part: at every cut the
 * target reads inactive until the active header of step 6.
 */
static int header_inactive(struct wrun *r)
{
	_Alignas(32) uint8_t hdr[S03_SECTOR];
	uint32_t i;

	if (w_read(r, 0, 1, hdr) != 0)
		return -1;
	if (hdr[S03_ACTIVE_OFF] == 0u)
		return 0;
	for (i = 0; i < S03_SECTOR; i++)
		hdr[i] = 0u;
	if (w_write(r, 0, 1, hdr) != 0 || w_read(r, 0, 1, hdr) != 0)
		return -1;
	return hdr[S03_ACTIVE_OFF] == 0u ? 0 : w_failed(r, S03_FAIL_MISMATCH, 0);
}

/*
 * S-03.056: a failure in steps 1-3 after the first erase leaves the target's
 * header inactive as far as the eMMC allows; the first failure stays the one
 * reported.
 */
static enum s03_write_result failed_early(const struct wrun *r)
{
	_Alignas(32) uint8_t hdr[S03_SECTOR];

	if (r->mutated && (do_read(r->env, 0, 1, hdr) != 0 || hdr[S03_ACTIVE_OFF] != 0u)) {
		uint32_t i;

		for (i = 0; i < S03_SECTOR; i++)
			hdr[i] = 0u;
		(void)do_write(r->env, 0, 1, hdr);
	}
	return S03_WRITE_FAILED;
}

static enum s03_write_result write_body(struct wrun *r, const uint8_t *image, uint32_t length)
{
	const struct s03_env *env = r->env;
	struct s03_write *out = r->out;
	_Alignas(32) uint8_t hdr[S03_SECTOR];
	_Alignas(32) uint8_t inactive[S03_SECTOR];
	struct s03_header h1;
	uint32_t n, groups, g, crc;
	unsigned other;
	int rc;

	/* S-03.052: the target from BP1's header, read before any write. */
	w_stage(r, S03_WS_TARGET);
	if (w_select(r, S03_PART_BP1) != 0 || w_read(r, 0, 1, hdr) != 0)
		return S03_WRITE_FAILED;		/* S-03.061, S-16.134 */
	s03_header_decode(hdr, &h1);
	out->target = s03_header_active(&h1) ? S03_PART_BP2 : S03_PART_BP1;
	other = out->target == S03_PART_BP1 ? S03_PART_BP2 : S03_PART_BP1;
	if (out->target == S03_PART_BP2 && w_select(r, S03_PART_BP2) != 0)
		return S03_WRITE_FAILED;

	/* Step 1: the header inactive, then the target's groups
	 * 1..floor(N / 0x400) (S-03.054, S-03.004); group 0 is written over. */
	w_stage(r, S03_WS_ERASE);
	n = s03_sectors(length);
	groups = n / S03_GROUP_SECTORS + 1u;
	if (header_inactive(r) != 0)
		return failed_early(r);
	for (g = 1; g < groups; g++) {
		if (w_erase(r, g) != 0)
			return failed_early(r);		/* S-03.056 */
		w_progress(r, g, groups - 1u);
	}
	/* Steps 2 and 3. */
	w_stage(r, S03_WS_WRITE);
	if (write_stored(r, image, length) != 0)
		return failed_early(r);			/* S-03.056 */
	w_stage(r, S03_WS_VERIFY);
	if (verify_stored(r, image, length) != 0)
		return failed_early(r);			/* S-03.056, S-16.027 */

	/* Steps 4 to 7: the inactive copy, then the active header (S-03.067). */
	w_stage(r, S03_WS_HEADER);
	/* In software: the value written, with nothing known to check it by. */
	crc = env->crc32(0u, image, length);
	s03_header_encode(length, crc, 0x00u, inactive);
	s03_header_encode(length, crc, 0x01u, hdr);
	rc = header_step(r, inactive);
	if (rc == 0) {
		/* S-03.068: the image and the inactive copy, before the active byte. */
		if (w_flush(r) != 0)
			return S03_WRITE_FAILED;
		rc = header_step(r, hdr);
	}
	if (rc != 0) {
		/* S-03.057: make the target inactive again by the rewrite. */
		out->recovery_failed = do_write(env, 0, 1, inactive) != 0;
		return S03_WRITE_FAILED;
	}

	/* Step 8, S-03.058: rewrite the other header's active byte; no erase.
	 * S-03.068: the active header first; a failed flush is as S-03.062. */
	w_stage(r, S03_WS_DEACTIVATE);
	if (w_flush(r) != 0)
		return S03_WRITE_FAILED;
	if (w_select(r, other) != 0 || w_read(r, 0, 1, hdr) != 0)
		return S03_WRITE_FAILED;		/* S-03.062 */
	hdr[S03_ACTIVE_OFF] = 0x00u;
	if (w_write(r, 0, 1, hdr) != 0)
		return S03_WRITE_FAILED;
	w_stage(r, S03_WS_FLUSH);
	return w_flush(r) == 0 ? S03_WRITE_OK : S03_WRITE_FAILED;	/* S-03.060 */
}

void s03_write_os(const struct s03_env *env, const uint8_t *image, uint32_t length,
		  struct s03_write *out)
{
	s03_write_os_watched(env, image, length, out, NULL);
}

/*
 * S-03.050 to S-03.063.  The OS write takes no session option (S-03.066):
 * every install writes the plaintext form, a return to the factory bootloader included
 * (S-03.064); the result is S-03.060's.
 */
void s03_write_os_watched(const struct s03_env *env, const uint8_t *image, uint32_t length,
			  struct s03_write *out, const struct s03_write_watch *watch)
{
	struct wrun r = { env, out, watch, S03_PART_USER, false };

	out->target = 0;
	out->select0_failed = false;
	out->stage = S03_WS_ACCEPT;
	out->fail = S03_FAIL_NONE;
	out->part = 0;
	out->at = 0;
	out->recovery_failed = false;
	w_stage(&r, S03_WS_ACCEPT);
	/* S-03.051: refused before any eMMC operation. */
	if (!s03_image_acceptable(length, env->boot_size)) {
		out->result = S03_WRITE_REFUSED;
		out->fail = S03_FAIL_LENGTH;
		return;
	}
	out->result = write_body(&r, image, length);
	/* S-03.068: what a stopped write did is committed too; the result stands. */
	if (out->result == S03_WRITE_FAILED && r.mutated && out->fail != S03_FAIL_FLUSH)
		(void)do_switch(env, EXT_CSD_FLUSH_CACHE, 0x01u);
	/* Step 9, S-03.007; S-03.063: a failed select 0 alone changes nothing. */
	if (do_select(env, S03_PART_USER) != 0)
		out->select0_failed = true;
	if (out->result == S03_WRITE_OK)
		w_stage(&r, S03_WS_DONE);
}

/* --- The user area --- */

/* S-03.070 */
enum s03_region s03_user_region(uint32_t sector)
{
	if (sector <= S03_YAFFS2_LAST)
		return S03_REGION_YAFFS2;
	if (sector <= S03_UNUSED_LAST)
		return S03_REGION_UNUSED;
	if (sector <= S03_FAT_LAST)
		return S03_REGION_FAT;
	return S03_REGION_NONE;
}

/* S-03.072 */
enum s03_region s03_user_region_dev(const struct wb_device *dev, uint32_t sector)
{
	if (!wb_has(dev, WB_F_FAT_REGION) && sector >= S03_UNUSED_FIRST)
		return S03_REGION_NONE;
	return s03_user_region(sector);
}

bool s03_user_access_ok(const struct wb_device *dev, uint32_t first, uint32_t count)
{
	const uint32_t last = first + count - 1u;

	if (count == 0u || last < first)
		return false;
	if (last <= S03_YAFFS2_LAST)
		return true;
	return wb_has(dev, WB_F_FAT_REGION) &&
	       first >= S03_FACTORY_RESET_GROUP * S03_GROUP_SECTORS &&
	       last < (S03_FACTORY_RESET_GROUP + 1u) * S03_GROUP_SECTORS;
}

uint32_t s03_fat_volume_to_user(uint32_t s)
{
	return s < S03_FAT_FIRST ? s + S03_FAT_SECTORS : s;
}
