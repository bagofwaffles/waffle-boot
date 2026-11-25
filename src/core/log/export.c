/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-14.051, S-14.052: the log export.
 *
 * Candidates are taken one at a time in the export order by scanning the
 * directory again for the first entry after the previous one, so that no
 * list of entries has to be held in memory.
 */
#include "waffle/log/export.h"

#include "waffle/util/bytes.h"

struct cand {
	char name[S14_EXPORT_NAME_MAX + 1u];
	uint32_t mtime;
	uint32_t size;
};

/* Byte order of names; a name that is a prefix of another comes first. */
static int name_cmp(const char *a, const char *b)
{
	size_t i = 0;

	while (a[i] != '\0' && a[i] == b[i])
		i++;
	return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
}

/* S-14.051: newest first; equal times in ascending byte order of the name. */
static bool before(uint32_t mt_a, const char *a, uint32_t mt_b, const char *b)
{
	if (mt_a != mt_b)
		return mt_a > mt_b;
	return name_cmp(a, b) < 0;
}

/* S-14.051, S-16.082: regular files directly in logs/, every name but "."
 * and "..". */
static bool candidate(const struct s14_export_entry *e)
{
	size_t n;

	if (!e->regular || e->name == NULL)
		return false;
	n = wb_strlen(e->name);
	if (n == 0u || n > S14_EXPORT_NAME_MAX)
		return false;
	if (e->name[0] == '.' &&
	    (n == 1u || (n == 2u && e->name[1] == '.')))
		return false;
	return true;
}

static void keep(struct cand *c, const struct s14_export_entry *e)
{
	size_t i;

	for (i = 0; e->name[i] != '\0'; i++)
		c->name[i] = e->name[i];
	c->name[i] = '\0';
	c->mtime = e->mtime;
	c->size = e->size;
}

/* The first candidate after prev in the export order (any, if prev is
 * NULL). Returns false when there is none. */
static bool next_candidate(const struct s14_export_ops *ops,
			   const struct cand *prev, struct cand *out)
{
	struct s14_export_entry e;
	bool found = false;

	ops->rewind(ops->ctx);
	while (ops->next(ops->ctx, &e)) {
		if (!candidate(&e))
			continue;
		if (prev != NULL &&
		    !before(prev->mtime, prev->name, e.mtime, e.name))
			continue;
		if (!found || before(e.mtime, e.name, out->mtime, out->name)) {
			keep(out, &e);
			found = true;
		}
	}
	return found;
}

/* S-14.051, S-14.052 */
size_t s14_export_run(const struct s14_export_ops *ops)
{
	struct cand cur[2];
	unsigned which = 0;
	const struct cand *prev = NULL;
	uint64_t total = 0;
	size_t copied = 0;

	while (next_candidate(ops, prev, &cur[which])) {
		struct cand *c = &cur[which];

		/* The first candidate that would take the total above 5 MiB
		 * ends the export, decided before any copy is tried. */
		if (total + c->size > S14_EXPORT_LIMIT)
			break;
		/* An empty candidate is not copied and adds nothing. */
		if (c->size != 0u) {
			if (ops->copy(ops->ctx, c->name, c->mtime)) {
				total += c->size;
				copied++;
			} else {
				/* S-14.052: no partial file stays; it adds
				 * nothing, and the next older one follows. */
				ops->discard(ops->ctx, c->name);
			}
		}
		prev = c;
		which ^= 1u;
	}
	return copied;
}
