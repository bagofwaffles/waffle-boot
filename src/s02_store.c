/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The settings sector, S-02.096 - S-02.099, S-02.105 (waffle/s02_store.h). */
#include "waffle/s02_store.h"

#include "waffle/bytes.h"
#include "waffle/wire_crc.h"

uint32_t s02_store_off(unsigned slot)
{
	return s02_slot_base(slot) + S02_STORE_SLOT_MAX;
}

void s02_store_defaults(struct s02_store *s)
{
	s->dev_mode = false;
	s->color = false;
	s->mtp_macos = true;
	s->start_menu = false;
}

bool s02_store_decode(const uint8_t *p, size_t n, uint32_t stamp, struct s02_store *out)
{
	uint32_t len;
	uint8_t f;

	s02_store_defaults(out);
	if (n < S02_STORE_HEADER || wb_get_le32(p) != S02_STORE_MAGIC ||
	    wb_get_le16(p + 4) != S02_STORE_VERSION)
		return false;
	len = wb_get_le16(p + 6);
	if (len < 1u || len > S02_STORE_PAYLOAD_MAX ||
	    n < S02_STORE_HEADER + len + S02_STORE_CRC)
		return false;
	/* software: a record of a few bytes */
	if (wire_crc32(0u, p, S02_STORE_HEADER + len) !=
	    wb_get_le32(p + S02_STORE_HEADER + len))
		return false;
	if (wb_get_le32(p + 8) != stamp)
		return false;
	/* payload version 1: byte 0's bits 0 to 3; the rest is ignored */
	f = p[S02_STORE_HEADER];
	out->dev_mode = (f & S02_STORE_FLAG_DEV) != 0u;
	out->color = (f & S02_STORE_FLAG_COLOR) != 0u;
	out->mtp_macos = (f & S02_STORE_FLAG_MTP_MACOS) != 0u;
	out->start_menu = (f & S02_STORE_FLAG_START_MENU) != 0u;
	return true;
}

size_t s02_store_encode(const struct s02_store *s, uint32_t stamp, uint8_t *buf, size_t cap)
{
	uint8_t f = 0u;

	if (cap < S02_STORE_WRITTEN)
		return 0u;
	if (s->dev_mode)
		f |= S02_STORE_FLAG_DEV;
	if (s->color)
		f |= S02_STORE_FLAG_COLOR;
	if (s->mtp_macos)
		f |= S02_STORE_FLAG_MTP_MACOS;
	if (s->start_menu)
		f |= S02_STORE_FLAG_START_MENU;
	wb_put_le32(buf, S02_STORE_MAGIC);
	wb_put_le16(buf + 4, S02_STORE_VERSION);
	wb_put_le16(buf + 6, 1u);
	wb_put_le32(buf + 8, stamp);
	buf[S02_STORE_HEADER] = f;
	wb_put_le32(buf + S02_STORE_HEADER + 1u,	/* software, as above */
		    wire_crc32(0u, buf, S02_STORE_HEADER + 1u));
	return S02_STORE_WRITTEN;
}

bool s02_store_record_ok(const struct s02_record *rec)
{
	return s02_tag_ok(rec->tag) && rec->length >= 1u &&
	       rec->length <= S02_STORE_SLOT_MAX;
}

static bool page_erased(const uint8_t *p)
{
	for (uint32_t i = 0; i < S02_PAGE_SIZE; i++)
		if (p[i] != S02_ERASED)
			return false;
	return true;
}

bool s02_store_pick(const uint8_t sector[S02_STORE_SIZE], uint32_t stamp, struct s02_store *out)
{
	s02_store_defaults(out);
	for (uint32_t i = S02_STORE_PAGES; i-- > 0u;) {
		if (s02_store_decode(sector + i * S02_PAGE_SIZE, S02_PAGE_SIZE, stamp, out))
			return true;
	}
	return false;
}

/* The used pages: *used of them, and whether they are the first ones. */
static bool layout(const uint8_t *sector, uint32_t *used)
{
	bool gap = false, ok = true;

	*used = 0u;
	for (uint32_t i = 0; i < S02_STORE_PAGES; i++) {
		if (page_erased(sector + i * S02_PAGE_SIZE)) {
			gap = true;
		} else {
			ok = ok && !gap;
			*used = i + 1u;
		}
	}
	return ok;
}

/* The running slot keeps settings: established, good, a record that
 * leaves the sector alone, and that record read again unchanged. Every
 * flash range is checked against the sector before it is used. */
static bool usable(const struct s02_env *env, const struct s02_store_slot *run)
{
	struct s02_record now;

	if (!run->established || !run->good || run->slot > 1u ||
	    !s02_store_record_ok(&run->rec) || env->scratch_len < S02_STORE_SIZE)
		return false;
	if (s02_read_record(env, run->slot, &now) != S02_OK)
		return false;
	return now.crc32 == run->rec.crc32 && now.length == run->rec.length &&
	       now.tag == run->rec.tag;
}

static bool in_store(const struct s02_store_slot *run, uint32_t off, uint32_t len)
{
	const uint32_t base = s02_store_off(run->slot);

	return run->slot <= 1u && off >= base && len <= S02_STORE_SIZE &&
	       off - base <= S02_STORE_SIZE - len;
}

static int sector_read(const struct s02_env *env, const struct s02_store_slot *run)
{
	const uint32_t base = s02_store_off(run->slot);

	if (!in_store(run, base, S02_STORE_SIZE))
		return -1;
	return env->flash->read(env->flash->ctx, base, env->scratch, S02_STORE_SIZE);
}

/* The sector erased and read back erased. */
static enum s02_store_rc erase_sector(const struct s02_env *env, const struct s02_store_slot *run)
{
	const uint32_t base = s02_store_off(run->slot);

	if (!in_store(run, base, S02_STORE_SIZE) ||
	    env->flash->erase(env->flash->ctx, base) != 0 || sector_read(env, run) != 0)
		return S02_STORE_IO;
	for (uint32_t i = 0; i < S02_STORE_PAGES; i++)
		if (!page_erased(env->scratch + i * S02_PAGE_SIZE))
			return S02_STORE_VERIFY;
	return S02_STORE_OK;
}

bool s02_store_read(const struct s02_env *env, const struct s02_store_slot *run,
		    struct s02_store *out)
{
	s02_store_defaults(out);
	if (!usable(env, run) || sector_read(env, run) != 0)
		return false;
	return s02_store_pick(env->scratch, run->rec.crc32, out);
}

static bool same(const struct s02_store *a, const struct s02_store *b)
{
	return a->dev_mode == b->dev_mode && a->color == b->color &&
	       a->mtp_macos == b->mtp_macos && a->start_menu == b->start_menu;
}

enum s02_store_rc s02_store_write(const struct s02_env *env, const struct s02_store_slot *run,
				  const struct s02_store *s)
{
	uint8_t rec[S02_STORE_WRITTEN];
	struct s02_store now;
	uint32_t used, at;
	size_t n;

	if (!usable(env, run))
		return S02_STORE_UNUSABLE;
	n = s02_store_encode(s, run->rec.crc32, rec, sizeof(rec));
	if (n == 0u || sector_read(env, run) != 0)
		return S02_STORE_IO;
	if (s02_store_pick(env->scratch, run->rec.crc32, &now) && same(&now, s))
		return S02_STORE_OK;
	if (!layout(env->scratch, &used) || used == S02_STORE_PAGES) {
		const enum s02_store_rc rc = erase_sector(env, run);

		if (rc != S02_STORE_OK)
			return rc;
		used = 0u;
	}
	/* one program, at a page's start, within that page (S-02.019) */
	at = s02_store_off(run->slot) + used * S02_PAGE_SIZE;
	if (!in_store(run, at, (uint32_t)n) ||
	    env->flash->program(env->flash->ctx, at, rec, (uint32_t)n) != 0)
		return S02_STORE_IO;
	if (env->flash->read(env->flash->ctx, at, env->scratch, (uint32_t)n) != 0)
		return S02_STORE_IO;
	return wb_equal(env->scratch, rec, n) ? S02_STORE_OK : S02_STORE_VERIFY;
}

enum s02_store_rc s02_store_clear(const struct s02_env *env, const struct s02_store_slot *run)
{
	uint32_t used;

	if (!usable(env, run))
		return S02_STORE_UNUSABLE;
	if (sector_read(env, run) != 0)
		return S02_STORE_IO;
	if (layout(env->scratch, &used) && used == 0u)
		return S02_STORE_OK;
	return erase_sector(env, run);
}
