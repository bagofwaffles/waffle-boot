/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The check suite's eMMC parts (waffle/board/testos_check.h): the guard on the
 * eMMC operations, the switch-back that makes the unit's OS active again
 * when it is intact in the other boot partition, the read of both boot
 * partitions, and the temporary file. Pure: S-03's operations come in as
 * struct s03_emmc_ops, the file layer as struct tchk_file_ops, so the
 * host's unit tests run all of it against fakes. Nothing is compiled
 * without OPXY_TESTOS_CHECK.
 */
#include "waffle/board/testos_check.h"

#if defined(OPXY_TESTOS_CHECK)

#include <string.h>

#include "waffle/s09_bridge.h"

#define FLUSH_INDEX 32u		/* EXT_CSD[32] FLUSH_CACHE (S-03.068) */

/* --- the guard --- */

static void breathe(const struct tchk_guard *g)
{
	if (g->breathe != NULL)
		g->breathe(g->breathe_ctx);
}

static int g_select(void *ctx, unsigned part)
{
	struct tchk_guard *g = ctx;
	int rc;

	breathe(g);
	if (part > S03_PART_BP2) {
		g->refused++;
		return -1;
	}
	rc = g->in->select(g->in->ctx, part);
	/* after a failed select the partition is not known (S-03.005) */
	g->sel = part;
	g->sel_known = rc == 0;
	return rc;
}

static int g_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	struct tchk_guard *g = ctx;

	breathe(g);
	return g->in->read(g->in->ctx, sector, count, buf);
}

/* A boot partition's header sector alone: S-03.058's write, nothing else. */
static int g_write(void *ctx, uint32_t sector, uint32_t count, const uint8_t *buf)
{
	struct tchk_guard *g = ctx;

	breathe(g);
	if (!g->sel_known || (g->sel != S03_PART_BP1 && g->sel != S03_PART_BP2) ||
	    sector != 0u || count != 1u) {
		g->refused++;
		return -1;
	}
	return g->in->write(g->in->ctx, sector, count, buf);
}

static int g_erase(void *ctx, uint32_t first_sector)
{
	struct tchk_guard *g = ctx;

	(void)first_sector;
	g->refused++;
	return -1;
}

static int g_switch(void *ctx, uint8_t index, uint8_t value)
{
	struct tchk_guard *g = ctx;

	breathe(g);
	if (index != FLUSH_INDEX || value != 1u) {
		g->refused++;
		return -1;
	}
	return g->in->switch_byte(g->in->ctx, index, value);
}

void tchk_guard_init(struct tchk_guard *g, const struct s03_emmc_ops *in)
{
	g->in = in;
	g->sel = S03_PART_USER;
	g->sel_known = false;
	g->refused = 0;
	g->breathe = NULL;
	g->breathe_ctx = NULL;
	g->ops.ctx = g;
	g->ops.select = g_select;
	g->ops.read = g_read;
	g->ops.write = g_write;
	g->ops.erase = g_erase;
	g->ops.switch_byte = g_switch;
}

/* --- S-03.010's attempts --- */

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

static int do_write(const struct s03_env *env, const uint8_t *buf)
{
	unsigned i;

	for (i = 0; i < S03_RETRIES; i++)
		if (env->ops->write(env->ops->ctx, 0u, 1u, buf) == 0)
			return 0;
	return -1;
}

static int do_flush(const struct s03_env *env)
{
	unsigned i;

	for (i = 0; i < S03_RETRIES; i++)
		if (env->ops->switch_byte(env->ops->ctx, FLUSH_INDEX, 1u) == 0)
			return 0;
	return -1;
}

/* A partition's sector 0 into hdr: select, read. */
static int read_header(const struct s03_env *env, unsigned part, uint8_t *hdr)
{
	return do_select(env, part) == 0 && do_read(env, 0u, 1u, hdr) == 0 ? 0 : -1;
}

/* --- the switch-back --- */

static bool all_of(const uint8_t *p, uint8_t v)
{
	unsigned i;

	for (i = 0; i < S03_SECTOR; i++)
		if (p[i] != v)
			return false;
	return true;
}

/* S-03.042's header checks, in its order, with B. */
static bool header_ok(const struct s03_header *h, uint32_t boot_size)
{
	if (h->length == 0u || h->length > S03_OS_MAX)
		return false;
	if ((uint64_t)S03_SECTOR * ((uint64_t)s03_sectors(h->length) + 1u) > boot_size)
		return false;
	return h->format <= 3u;
}

enum tchk_look tchk_header_look(const uint8_t raw[S03_SECTOR], uint32_t boot_size)
{
	struct s03_header h;

	if (all_of(raw, 0x00u))
		return TCHK_LOOK_ZERO;
	if (all_of(raw, 0xFFu))
		return TCHK_LOOK_FF;
	s03_header_decode(raw, &h);
	if (!header_ok(&h, boot_size))
		return TCHK_LOOK_OTHER;
	return s03_header_active(&h) ? TCHK_LOOK_ACTIVE : TCHK_LOOK_INACTIVE;
}

unsigned tchk_loader_takes(const uint8_t bp1[S03_SECTOR], const uint8_t bp2[S03_SECTOR])
{
	/* S-03.040, S-03.022: BP1 if active, else BP2 if active, no fallback */
	if (bp1[S03_ACTIVE_OFF] != 0u)
		return S03_PART_BP1;
	if (bp2[S03_ACTIVE_OFF] != 0u)
		return S03_PART_BP2;
	return 0u;
}

/* S-03.044: the CRC of sectors 1..N of the selected partition's image. */
static int image_crc(const struct s03_env *env, uint32_t length, uint32_t *crc)
{
	const uint32_t n = s03_sectors(length);
	uint32_t room = env->scratch_len / S03_SECTOR, done = 0, left = length;

	if (env->max_xfer != 0u && room > env->max_xfer)
		room = env->max_xfer;
	if (room == 0u)
		return -1;
	*crc = 0u;
	while (done < n) {
		const uint32_t cnt = n - done < room ? n - done : room;
		const uint32_t bytes = cnt * S03_SECTOR < left ? cnt * S03_SECTOR : left;

		if (do_read(env, 1u + done, cnt, env->scratch) != 0)
			return -1;
		*crc = env->crc32(*crc, env->scratch, bytes);
		left -= bytes;
		done += cnt;
	}
	return 0;
}

/*
 * S-03.058's method: the sector as read, byte 0x0C alone changed, written,
 * flushed (S-03.068), read back whole and compared. 0 when it holds.
 */
static int set_active(const struct s03_env *env, unsigned part, const uint8_t *as_read,
		      uint8_t active, uint8_t *want, uint8_t *back)
{
	memcpy(want, as_read, S03_SECTOR);
	want[S03_ACTIVE_OFF] = active;
	if (do_select(env, part) != 0 || do_write(env, want) != 0 || do_flush(env) != 0 ||
	    do_read(env, 0u, 1u, back) != 0)
		return -1;
	return memcmp(want, back, S03_SECTOR) == 0 ? 0 : -1;
}

/* The OS's header as read again: as first read, byte 0x0C set. */
static bool same_but_active(const uint8_t *now, const uint8_t *was)
{
	unsigned i;

	for (i = 0; i < S03_SECTOR; i++)
		if (i != S03_ACTIVE_OFF && now[i] != was[i])
			return false;
	return now[S03_ACTIVE_OFF] != 0u;
}

/* Steps 1 to 3: both headers, where the image came from, whether the unit's
 * OS is there and passes its check. Reads only. */
static void look(const struct s03_env *env, struct tchk_switch *out, uint8_t hdr[2][S03_SECTOR])
{
	const uint8_t *other_raw, *mine_raw;
	struct s03_header oh;
	uint32_t crc;

	/* 1: both headers */
	if (env->boot_size == 0u || read_header(env, S03_PART_BP1, hdr[0]) != 0 ||
	    read_header(env, S03_PART_BP2, hdr[1]) != 0) {
		out->result = TCHK_SW_IO;
		return;
	}
	out->headers_read = true;
	s03_header_decode(hdr[0], &out->h[0]);
	s03_header_decode(hdr[1], &out->h[1]);
	memcpy(out->raw, hdr, sizeof out->raw);

	/* 2: the image came from the partition the loader takes (S-03.040) */
	out->mine = tchk_loader_takes(hdr[0], hdr[1]);
	if (out->mine == 0u) {
		out->result = TCHK_SW_NOT_ACTIVE;
		return;
	}
	out->other = out->mine == S03_PART_BP1 ? S03_PART_BP2 : S03_PART_BP1;
	other_raw = hdr[out->other - 1u];
	mine_raw = hdr[out->mine - 1u];
	/* A partition the loader takes but whose header S-03.042 refuses (an
	 * erased 0xFF one) cannot be where this image came from: nothing is
	 * known well enough to write. */
	if (tchk_header_look(mine_raw, env->boot_size) != TCHK_LOOK_ACTIVE) {
		out->mine = 0u;
		out->other = 0u;
		out->result = TCHK_SW_NOT_ACTIVE;
		return;
	}

	/* 3: is the unit's OS there? */
	out->look = (uint8_t)tchk_header_look(other_raw, env->boot_size);
	if (out->look != TCHK_LOOK_ACTIVE && out->look != TCHK_LOOK_INACTIVE) {
		out->result = TCHK_SW_NO_OTHER_OS;
		return;
	}
	s03_header_decode(other_raw, &oh);
	if (s03_header_mode(&oh) == S03_MODE_ENCRYPTED) {
		/*
		 * SPEC-Q: S-03.048, S-05.029: the stored region of a header
		 * marked encrypted is ciphertext, and its crc is the
		 * plaintext's. Whether an image stored encrypted can be
		 * told intact without the unit's key is not stated. Read
		 * conservatively as present and unverified: it is made active,
		 * and the screen says it was not checked.
		 */
		out->unverified = true;
	} else {
		if (do_select(env, out->other) != 0 || image_crc(env, oh.length, &crc) != 0) {
			out->result = TCHK_SW_IO;
			return;
		}
		out->other_crc_checked = true;
		out->other_crc_ok = crc == oh.crc;
		/* S-03.045: activating a bad image leaves nothing that loads */
		if (!out->other_crc_ok) {
			out->result = TCHK_SW_OTHER_BAD;
			return;
		}
	}
	out->result = TCHK_SW_READY;
}

/* Step 4, on a look that found the OS: its header active, then this
 * image's inactive. */
static void write_back(const struct s03_env *env, struct tchk_switch *out)
{
	_Alignas(4) uint8_t want[S03_SECTOR];
	_Alignas(4) uint8_t back[S03_SECTOR];
	const uint8_t *other_raw = out->raw[out->other - 1u];
	const uint8_t *mine_raw = out->raw[out->mine - 1u];

	/* 4a: the OS's header active first: after it alone the loader takes
	 * the OS (other = BP1) or still this image (other = BP2), never nothing */
	if (other_raw[S03_ACTIVE_OFF] == 0u) {
		if (set_active(env, out->other, other_raw, 0x01u, want, back) != 0) {
			/* the sector back as it was read */
			if (do_select(env, out->other) != 0 || do_write(env, other_raw) != 0 ||
			    do_flush(env) != 0)
				out->undo_failed = true;
			out->result = TCHK_SW_WRITE_FAILED;
			return;
		}
		out->wrote_a = true;
	}
	/* 4b: this image's header inactive: then one partition is active */
	if (set_active(env, out->mine, mine_raw, 0x00u, want, back) != 0) {
		out->b_failed = true;
		out->result = out->other == S03_PART_BP1 ? TCHK_SW_RESTORED : TCHK_SW_WRITE_FAILED;
		return;
	}
	out->wrote_b = true;
	out->result = TCHK_SW_RESTORED;
}

void tchk_switch_look(const struct s03_env *env, struct tchk_switch *out)
{
	_Alignas(4) uint8_t hdr[2][S03_SECTOR];

	memset(out, 0, sizeof(*out));
	memset(hdr, 0, sizeof hdr);
	look(env, out, hdr);
	if (out->headers_read)
		out->takes = tchk_loader_takes(hdr[0], hdr[1]);
	/* S-03.007: the user area last, always; file requests need it */
	if (do_select(env, S03_PART_USER) != 0)
		out->select0_failed = true;
}

void tchk_switch_write(const struct s03_env *env, struct tchk_switch *out)
{
	_Alignas(4) uint8_t now[2][S03_SECTOR];

	if (out->result != TCHK_SW_READY)
		return;
	write_back(env, out);
	/* 5: what the loader takes now, read again; RESTORED only if the OS */
	if (read_header(env, S03_PART_BP1, now[0]) != 0 ||
	    read_header(env, S03_PART_BP2, now[1]) != 0) {
		out->result = TCHK_SW_IO;
	} else {
		out->takes = tchk_loader_takes(now[0], now[1]);
		if (out->takes == out->other && out->other != 0u &&
		    same_but_active(now[out->other - 1u], out->raw[out->other - 1u]))
			out->result = TCHK_SW_RESTORED;
		else if (out->result == TCHK_SW_RESTORED)
			out->result = TCHK_SW_WRITE_FAILED;
	}
	/* S-03.007: the user area last, always; file requests need it */
	if (do_select(env, S03_PART_USER) != 0)
		out->select0_failed = true;
}

void tchk_switch_back(const struct s03_env *env, struct tchk_switch *out)
{
	tchk_switch_look(env, out);
	tchk_switch_write(env, out);
}

/* --- emmc-boot --- */

void tchk_emmc_boot(const struct s03_env *env, const struct tchk_switch *sw,
		    uint64_t (*now_us)(void), struct tchk_boot *out)
{
	uint32_t room = env->scratch_len / S03_SECTOR;
	const uint64_t t0 = now_us != NULL ? now_us() : 0u;
	unsigned p;

	memset(out, 0, sizeof(*out));
	out->run = true;
	out->unchecked = TCHK_UNCHECKED_NO_IMAGE;
	if (env->max_xfer != 0u && room > env->max_xfer)
		room = env->max_xfer;
	for (p = 0; p < 2u && room != 0u && env->boot_size >= S03_SECTOR; p++) {
		const unsigned part = p + 1u;
		const uint32_t total = env->boot_size / S03_SECTOR;
		/* the suite's own image, when plaintext and its header sound
		 * (S-03.044); the other's CRC was the switch-back's */
		const struct s03_header *h = &sw->h[p];
		const bool mine = sw->headers_read && part == sw->mine && header_ok(h, env->boot_size);
		const bool check = mine && s03_header_mode(h) == S03_MODE_PLAIN;
		uint32_t s = 0, left = check ? h->length : 0u, crc = 0u;
		bool broken = false;	/* a read inside the image failed */

		if (mine && !check)
			out->unchecked = TCHK_UNCHECKED_ENCRYPTED;
		if (do_select(env, part) != 0) {
			out->errors[p] = total;
			out->first_bad[p] = 0u;
			if (check)
				out->unchecked = TCHK_UNCHECKED_READ;
			continue;
		}
		while (s < total) {
			const uint32_t cnt = total - s < room ? total - s : room;

			if (do_read(env, s, cnt, env->scratch) != 0) {
				if (out->errors[p] == 0u)
					out->first_bad[p] = s;
				out->errors[p] += cnt;
				/* the CRC needs every sector of the image; a
				 * failure past its end leaves it whole */
				if (left != 0u)
					broken = true;
				s += cnt;
				continue;
			}
			out->sectors[p] += cnt;
			out->bytes += (uint64_t)cnt * S03_SECTOR;
			if (check && left != 0u && !broken) {
				/* sector 0 is the header; the image starts at 1 */
				const uint32_t skip = s == 0u ? S03_SECTOR : 0u;
				const uint32_t avail = cnt * S03_SECTOR - skip;
				const uint32_t take = avail < left ? avail : left;

				crc = env->crc32(crc, env->scratch + skip, take);
				left -= take;
			}
			s += cnt;
		}
		if (check && !broken && left == 0u) {
			out->mine_checked = true;
			out->mine_ok = crc == h->crc;
			out->unchecked = TCHK_UNCHECKED_NONE;
		} else if (check) {
			out->unchecked = TCHK_UNCHECKED_READ;
		}
	}
	if (do_select(env, S03_PART_USER) != 0)
		out->select0_failed = true;
	out->us = now_us != NULL ? now_us() - t0 : 0u;
}

/* --- emmc-file --- */

static uint8_t pattern(uint32_t off)
{
	return (uint8_t)(off ^ (off >> 8) ^ (off >> 16) ^ 0x5Au);
}

static bool gone(const struct tchk_file_ops *ops)
{
	return ops->exists(ops->ctx, TCHK_FILE_PATH) == 0;
}

void tchk_file_test(const struct tchk_file_ops *ops, uint8_t *buf, struct tchk_file *out)
{
	uint32_t off, i;
	int32_t fd;
	const int32_t there = ops->exists(ops->ctx, TCHK_FILE_PATH);

	memset(out, 0, sizeof(*out));
	if (there < 0) {
		out->failed = TCHK_FILE_LEFTOVER;
		return;
	}
	/* one left by an earlier run, cut off before its unlink */
	if (there > 0) {
		out->leftover = true;
		if (ops->unlink(ops->ctx, TCHK_FILE_PATH) < 0 || !gone(ops)) {
			out->failed = TCHK_FILE_LEFTOVER;
			return;
		}
	}
	fd = ops->open(ops->ctx, TCHK_FILE_PATH, S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);
	if (fd < 0) {
		out->failed = TCHK_FILE_CREATE;
		return;
	}
	for (off = 0; off < TCHK_FILE_BYTES && out->failed == TCHK_FILE_OK; off += TCHK_FILE_PIECE) {
		for (i = 0; i < TCHK_FILE_PIECE; i++)
			buf[i] = pattern(off + i);
		if (ops->write(ops->ctx, fd, buf, TCHK_FILE_PIECE) != (int32_t)TCHK_FILE_PIECE)
			out->failed = TCHK_FILE_WRITE;
		else
			out->written += TCHK_FILE_PIECE;
	}
	if (ops->close(ops->ctx, fd) < 0 && out->failed == TCHK_FILE_OK)
		out->failed = TCHK_FILE_CLOSE;
	if (out->failed == TCHK_FILE_OK) {
		fd = ops->open(ops->ctx, TCHK_FILE_PATH, S09_O_RDONLY);
		if (fd < 0) {
			out->failed = TCHK_FILE_OPEN;
		} else {
			for (off = 0; off < TCHK_FILE_BYTES && out->failed == TCHK_FILE_OK;
			     off += TCHK_FILE_PIECE) {
				if (ops->read(ops->ctx, fd, buf, TCHK_FILE_PIECE) != (int32_t)TCHK_FILE_PIECE) {
					out->failed = TCHK_FILE_READ;
					break;
				}
				for (i = 0; i < TCHK_FILE_PIECE; i++)
					if (buf[i] != pattern(off + i))
						break;
				if (i != TCHK_FILE_PIECE)
					out->failed = TCHK_FILE_COMPARE;
				else
					out->compared += TCHK_FILE_PIECE;
			}
			(void)ops->close(ops->ctx, fd);
		}
	}
	/* removed whatever happened above */
	if (ops->unlink(ops->ctx, TCHK_FILE_PATH) < 0) {
		if (out->failed == TCHK_FILE_OK)
			out->failed = TCHK_FILE_UNLINK;
		return;
	}
	if (!gone(ops) && out->failed == TCHK_FILE_OK)
		out->failed = TCHK_FILE_GONE;
}

#else

/* ISO C wants a declaration in every translation unit. */
typedef int tchk_emmc_not_built;

#endif /* OPXY_TESTOS_CHECK */
