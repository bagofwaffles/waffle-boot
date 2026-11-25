/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.203: the list of OS files of /yaffs2/os.
 */
#include "waffle/ui/osfiles.h"

void s15_os_files_init(struct s15_os_files *l)
{
	l->n = 0;
	l->more = 0;
	for (unsigned i = 0; i < S15_OS_FILES_MAX; i++) {
		l->name[i][0] = '\0';
		l->size[i] = 0;
	}
}

static char lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool s15_os_file_name(const char *name)
{
	static const char ext[] = ".ldr";
	size_t n = 0;

	while (name[n] != '\0')
		n++;
	if (name[0] == '.' || n < sizeof ext || n >= S15_OS_NAME_MAX)
		return false;		/* a name that is ".ldr" alone starts with '.' */
	for (size_t i = 0; i < sizeof ext - 1u; i++)
		if (lower(name[n - (sizeof ext - 1u) + i]) != ext[i])
			return false;
	return true;
}

/* Byte order, as unsigned bytes. */
static int name_cmp(const char *a, const char *b)
{
	size_t i = 0;

	while (a[i] != '\0' && a[i] == b[i])
		i++;
	return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

static void name_copy(char *to, const char *from)
{
	size_t i = 0;

	for (; from[i] != '\0' && i + 1u < S15_OS_NAME_MAX; i++)
		to[i] = from[i];
	to[i] = '\0';
}

void s15_os_files_offer(struct s15_os_files *l, const char *name)
{
	unsigned at = 0;

	if (!s15_os_file_name(name))
		return;
	while (at < l->n && name_cmp(l->name[at], name) <= 0)
		at++;
	if (at == S15_OS_FILES_MAX) {
		l->more++;		/* after every listed one */
		return;
	}
	if (l->n == S15_OS_FILES_MAX) {
		l->more++;		/* the last listed one goes */
		l->n--;
	}
	for (unsigned i = l->n; i > at; i--) {
		name_copy(l->name[i], l->name[i - 1u]);
		l->size[i] = l->size[i - 1u];
	}
	name_copy(l->name[at], name);
	l->size[at] = 0;
	l->n++;
}

void s15_os_files_drop(struct s15_os_files *l, unsigned i)
{
	if (i >= l->n)
		return;
	for (; i + 1u < l->n; i++) {
		name_copy(l->name[i], l->name[i + 1u]);
		l->size[i] = l->size[i + 1u];
	}
	l->n--;
	l->name[l->n][0] = '\0';
	l->size[l->n] = 0;
}

bool s15_os_file_path(const struct s15_os_files *l, unsigned i, char *out, size_t cap)
{
	static const char dir[] = S15_OS_DIR "/";
	size_t n = 0;

	if (i >= l->n || cap == 0u)
		return false;
	for (size_t k = 0; dir[k] != '\0'; k++) {
		if (n + 1u >= cap)
			return false;
		out[n++] = dir[k];
	}
	for (size_t k = 0; l->name[i][k] != '\0'; k++) {
		if (n + 1u >= cap)
			return false;
		out[n++] = l->name[i][k];
	}
	out[n] = '\0';
	return true;
}
