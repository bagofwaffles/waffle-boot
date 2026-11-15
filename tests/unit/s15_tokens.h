/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The required-information check of S-15's terms (`info_match` of
 * tests/oracle/S-15-menus/s15_model.py), in C for the unit tests: a token matches
 * a run of at most `rows` consecutive rows in which every group has one of
 * its alternatives as a whole word (case ignored, a trailing '*' makes it a
 * word prefix, "$name" replaced by a value) and no `not` word appears.
 * Needs S15V_TOKENS from s15_vectors.h.
 */
#ifndef S15_TOKENS_H
#define S15_TOKENS_H

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static inline bool s15t_wordch(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* The alternative with "$name" replaced, lower case; *prefix set by '*'. */
static inline void s15t_alt(char *out, size_t cap, const char *alt,
			    const char *const *values, bool *prefix)
{
	size_t n = 0, i = 0, len = strlen(alt);

	*prefix = len > 0 && alt[len - 1] == '*';
	if (*prefix)
		len--;
	while (i < len && n + 1 < cap) {
		if (alt[i] == '$' && values != NULL) {
			size_t k;
			bool done = false;

			for (k = 0; values[k] != NULL; k += 2) {
				size_t kl = strlen(values[k]);

				if (strncmp(alt + i + 1, values[k], kl) == 0) {
					const char *v = values[k + 1];

					while (*v != '\0' && n + 1 < cap)
						out[n++] = (char)tolower((unsigned char)*v++);
					i += 1 + kl;
					done = true;
					break;
				}
			}
			if (done)
				continue;
		}
		out[n++] = (char)tolower((unsigned char)alt[i++]);
	}
	out[n] = '\0';
}

static inline bool s15t_word_in(const char *row, const char *alt,
				const char *const *values)
{
	char a[128], r[256];
	bool prefix;
	size_t i, al, rl = strlen(row);

	if (rl >= sizeof r)
		rl = sizeof r - 1;
	for (i = 0; i < rl; i++)
		r[i] = (char)tolower((unsigned char)row[i]);
	r[rl] = '\0';
	s15t_alt(a, sizeof a, alt, values, &prefix);
	al = strlen(a);
	for (i = 0; i + al <= rl; i++) {
		if (memcmp(r + i, a, al) != 0)
			continue;
		if (i > 0 && s15t_wordch(r[i - 1]))
			continue;
		if (!prefix && i + al < rl && s15t_wordch(r[i + al]))
			continue;
		return true;
	}
	return false;
}

static inline bool s15t_any_in(const char *const *rows, size_t from, size_t to,
			       const char *alt, const char *const *values)
{
	for (size_t k = from; k < to; k++)
		if (s15t_word_in(rows[k], alt, values))
			return true;
	return false;
}

static inline const struct s15v_token *s15t_find(const char *name)
{
	for (size_t i = 0; i < S15V_N(s15v_tokens); i++)
		if (strcmp(s15v_tokens[i].name, name) == 0)
			return &s15v_tokens[i];
	return NULL;
}

/* info_match(spec, rows, values) */
static inline bool s15t_match(const struct s15v_token *t,
			      const char *const *rows, size_t n,
			      const char *const *values)
{
	size_t i, j;

	for (i = 0; i < n; i++) {
		if (t->follows != NULL &&
		    (i == 0 || strcmp(rows[i - 1], t->follows) != 0))
			continue;
		for (j = i + 1; j <= n && j <= i + t->rows; j++) {
			bool bad = false, all = true;
			size_t g, a;

			for (a = 0; t->nots[a] != NULL; a++)
				if (s15t_any_in(rows, i, j, t->nots[a], values))
					bad = true;
			if (bad)
				continue;
			for (g = 0; t->groups[g] != NULL && all; g++) {
				bool one = false;

				for (a = 0; t->groups[g][a] != NULL && !one; a++)
					one = s15t_any_in(rows, i, j,
							  t->groups[g][a], values);
				all = one;
			}
			if (all)
				return true;
		}
	}
	return false;
}

static inline bool s15t_match_named(const char *token, const char *const *rows,
				    size_t n, const char *const *values)
{
	const struct s15v_token *t = s15t_find(token);

	return t != NULL && s15t_match(t, rows, n, values);
}

/* A line fits the console: 1..78 printable characters. */
static inline bool s15t_fits(const char *s)
{
	size_t n = strlen(s);

	if (n > 78)
		return false;
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e)
			return false;
	return true;
}

#endif /* S15_TOKENS_H */
