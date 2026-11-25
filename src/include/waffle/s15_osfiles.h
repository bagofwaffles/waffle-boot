/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.203: the list of OS files of /yaffs2/os, as the menu, the file
 * reads and the start make it; no screen text.
 */
#ifndef WAFFLE_S15_OSFILES_H
#define WAFFLE_S15_OSFILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S15_OS_DIR "/yaffs2/os"
/* S-15.203: the places the list holds (S-04.072's places, digits 1 to 7). */
#define S15_OS_FILES_MAX 7u
/* A listed file's name, its terminator included; longer names are not
 * listed. */
#define S15_OS_NAME_MAX 128u

/*
 * S-15.203: the OS files, the names of /yaffs2/os that end in ".ldr" (any
 * case) and do not start with '.', in byte order: the first
 * S15_OS_FILES_MAX of them, and how many more there are. The list is the
 * same wherever it is made from the same directory, so a place names the
 * same file at the next start (S-04.072, S-05.044).
 */
struct s15_os_files {
	unsigned n;				/* listed */
	unsigned more;				/* OS files past them */
	char name[S15_OS_FILES_MAX][S15_OS_NAME_MAX];
	uint32_t size[S15_OS_FILES_MAX];	/* bytes, set once each is read */
};

void s15_os_files_init(struct s15_os_files *l);
/* Whether a directory entry's name is an OS file's. */
bool s15_os_file_name(const char *name);
/* A directory entry offered: an OS file's name goes into the list in its
 * order, the last one listed then counted in `more` when there is no room;
 * any other name is left out. */
void s15_os_files_offer(struct s15_os_files *l, const char *name);
/* Place i taken out (a directory, or a file that could not be read); the
 * ones after it move up. */
void s15_os_files_drop(struct s15_os_files *l, unsigned i);
/* The path of place i, S15_OS_DIR "/" name; false past the list. */
bool s15_os_file_path(const struct s15_os_files *l, unsigned i, char *out, size_t cap);

#endif /* WAFFLE_S15_OSFILES_H */
