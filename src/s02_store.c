/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The boot store's record, S-02.096 - S-02.098 (waffle/s02_store.h). */
#include "waffle/s02_store.h"

#include "waffle/bytes.h"
#include "waffle/wire_crc.h"

void s02_store_defaults(struct s02_store *s)
{
	s->start_menu = false;
}

bool s02_store_decode(const uint8_t *p, size_t n, struct s02_store *out)
{
	uint32_t len;

	s02_store_defaults(out);
	if (n < S02_STORE_HEADER || wb_get_le32(p) != S02_STORE_MAGIC ||
	    wb_get_le16(p + 4) != S02_STORE_VERSION)
		return false;
	len = wb_get_le16(p + 6);
	if (len < 1u || len > S02_STORE_PAYLOAD_MAX ||
	    n < S02_STORE_HEADER + len + S02_STORE_CRC)
		return false;
	if (wire_crc32(0u, p, S02_STORE_HEADER + len) !=
	    wb_get_le32(p + S02_STORE_HEADER + len))
		return false;
	/* payload version 1: byte 0's bit 0; the rest is ignored */
	out->start_menu = (p[S02_STORE_HEADER] & S02_STORE_FLAG_START_MENU) != 0u;
	return true;
}

size_t s02_store_encode(const struct s02_store *s, uint8_t *buf, size_t cap)
{
	if (cap < S02_STORE_WRITTEN)
		return 0u;
	wb_put_le32(buf, S02_STORE_MAGIC);
	wb_put_le16(buf + 4, S02_STORE_VERSION);
	wb_put_le16(buf + 6, 1u);
	buf[S02_STORE_HEADER] = s->start_menu ? S02_STORE_FLAG_START_MENU : 0u;
	wb_put_le32(buf + S02_STORE_HEADER + 1u,
		    wire_crc32(0u, buf, S02_STORE_HEADER + 1u));
	return S02_STORE_WRITTEN;
}

/* Every flash operation's range is checked against the sector before it is
 * made: nothing else is ever touched. */
static bool in_store(uint32_t off, uint32_t len)
{
	return off >= S02_STORE_OFF && len <= S02_STORE_SIZE &&
	       off - S02_STORE_OFF <= S02_STORE_SIZE - len;
}

bool s02_store_usable(const struct s02_env *env)
{
	struct s02_record rec;

	if (s02_read_record(env, S02_STORE_SLOT, &rec) != S02_OK)
		return false;
	return s02_tag_ok(rec.tag) && rec.length >= 1u &&
	       rec.length <= S02_STORE_SLOT_MAX;
}

bool s02_store_read(const struct s02_env *env, struct s02_store *out)
{
	s02_store_defaults(out);
	if (env->scratch_len < S02_STORE_SIZE || !s02_store_usable(env) ||
	    !in_store(S02_STORE_OFF, S02_STORE_SIZE) ||
	    env->flash->read(env->flash->ctx, S02_STORE_OFF, env->scratch,
			     S02_STORE_SIZE) != 0)
		return false;
	return s02_store_decode(env->scratch, S02_STORE_SIZE, out);
}

enum s02_store_rc s02_store_write(const struct s02_env *env, const struct s02_store *s)
{
	uint8_t rec[S02_STORE_WRITTEN];
	const size_t n = s02_store_encode(s, rec, sizeof(rec));
	size_t at = 0;

	if (n == 0u || env->scratch_len < n || !s02_store_usable(env))
		return S02_STORE_UNUSABLE;
	if (!in_store(S02_STORE_OFF, S02_STORE_SIZE) ||
	    env->flash->erase(env->flash->ctx, S02_STORE_OFF) != 0)
		return S02_STORE_IO;
	/* within one page at a time (S-02.019) */
	while (at < n) {
		const uint32_t a = S02_STORE_OFF + (uint32_t)at;
		size_t k = S02_PAGE_SIZE - a % S02_PAGE_SIZE;

		if (k > n - at)
			k = n - at;
		if (!in_store(a, (uint32_t)k) ||
		    env->flash->program(env->flash->ctx, a, rec + at, (uint32_t)k) != 0)
			return S02_STORE_IO;
		at += k;
	}
	if (!in_store(S02_STORE_OFF, (uint32_t)n) ||
	    env->flash->read(env->flash->ctx, S02_STORE_OFF, env->scratch, (uint32_t)n) != 0)
		return S02_STORE_IO;
	return wb_equal(env->scratch, rec, n) ? S02_STORE_OK : S02_STORE_VERIFY;
}
