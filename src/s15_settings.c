/* SPDX-License-Identifier: GPL-2.0-or-later */
/* S-15.172: the settings file's form. */
#include "waffle/s15_settings.h"

#include "waffle/bytes.h"

static const char header[] = "waffle-boot settings 1";

/* The keys in the order they are written; values[0] is "true". */
struct key {
	const char *name;
	const char *values[2];
};

static const struct key keys[] = {
	{ "developer_mode", { "on", "off" } },
	{ "color", { "on", "off" } },
	{ "mtp_target", { "macos", "windows" } },
};

/* S-15.172: the other key for developer mode, read only when no
 * developer_mode line gives it; never written. */
static const struct key old_dev = { "developer_menu", { "shown", "hidden" } };

#define NKEYS (sizeof keys / sizeof keys[0])

static bool *field(struct s15_settings *s, size_t k)
{
	return k == 0u ? &s->dev_mode : k == 1u ? &s->color : &s->mtp_macos;
}

static bool cfield(const struct s15_settings *s, size_t k)
{
	return k == 0u ? s->dev_mode : k == 1u ? s->color : s->mtp_macos;
}

/* Whether the n bytes at p are exactly the string t. */
static bool same(const uint8_t *p, size_t n, const char *t)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (t[i] == '\0' || p[i] != (uint8_t)t[i])
			return false;
	return t[n] == '\0';
}

void s15_settings_defaults(struct s15_settings *s)
{
	s->dev_mode = false;
	s->color = true;
	s->mtp_macos = true;
}

/* What the reading has seen of developer mode: a developer_mode line,
 * and the old key's last value (-1: none). */
struct seen {
	bool mode;
	int old;
};

/* Which of key's two values the value bytes are: 0, 1, or -1. */
static int value_of(const struct key *key, const uint8_t *p, size_t n)
{
	size_t v;

	for (v = 0; v < 2u; v++)
		if (same(p, n, key->values[v]))
			return (int)v;
	return -1;
}

/* One line after the header, its end (LF, CR LF) already cut. */
static void line(const uint8_t *p, size_t n, struct s15_settings *s,
		 struct seen *seen)
{
	size_t eq, k;
	int v;

	for (eq = 0; eq < n && p[eq] != '='; eq++)
		;
	if (eq == n)
		return;
	if (same(p, eq, old_dev.name)) {
		v = value_of(&old_dev, p + eq + 1u, n - eq - 1u);
		if (v >= 0)
			seen->old = v;
		return;
	}
	for (k = 0; k < NKEYS; k++) {
		if (!same(p, eq, keys[k].name))
			continue;
		v = value_of(&keys[k], p + eq + 1u, n - eq - 1u);
		if (v < 0)
			continue;
		*field(s, k) = v == 0;
		if (k == 0u)
			seen->mode = true;
	}
}

bool s15_settings_read(const uint8_t *p, size_t n, struct s15_settings *s)
{
	size_t i, start, end;
	bool first = true;
	struct seen seen = { false, -1 };

	s15_settings_defaults(s);
	if (n > S15_SETTINGS_MAX)
		return false;
	for (i = 0; i < n; i++)
		if (p[i] != '\n' && p[i] != '\r' && (p[i] < 0x20u || p[i] > 0x7eu))
			return false;
	start = 0;
	while (start <= n) {
		bool lf;

		for (end = start; end < n && p[end] != '\n'; end++)
			;
		lf = end < n;
		i = end;
		/* a CR just before an LF belongs to the line's end */
		if (lf && i > start && p[i - 1u] == '\r')
			i--;
		if (first) {
			if (!same(p + start, i - start, header)) {
				s15_settings_defaults(s);
				return false;
			}
			first = false;
		} else {
			line(p + start, i - start, s, &seen);
		}
		if (!lf)
			break;
		start = end + 1u;
	}
	if (!seen.mode && seen.old >= 0)
		s->dev_mode = seen.old == 0;
	return true;
}

static size_t put(uint8_t *buf, size_t at, size_t cap, const char *t)
{
	size_t n = wb_strlen(t), i;

	if (at > cap || n > cap - at)
		return cap + 1u;
	for (i = 0; i < n; i++)
		buf[at + i] = (uint8_t)t[i];
	return at + n;
}

size_t s15_settings_write(const struct s15_settings *s, uint8_t *buf,
			  size_t cap)
{
	size_t at = 0, k;

	at = put(buf, at, cap, header);
	at = put(buf, at, cap, "\n");
	for (k = 0; k < NKEYS; k++) {
		at = put(buf, at, cap, keys[k].name);
		at = put(buf, at, cap, "=");
		at = put(buf, at, cap, keys[k].values[cfield(s, k) ? 0 : 1]);
		at = put(buf, at, cap, "\n");
	}
	return at > cap ? 0u : at;
}
