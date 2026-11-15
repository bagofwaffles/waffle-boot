/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal check helpers shared by the s14 and s15 unit tests (host only). */
#ifndef S14_CHECK_H
#define S14_CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures;
static int check_count;

#define CHECK(cond, ...)                                              \
	do {                                                          \
		check_count++;                                        \
		if (!(cond)) {                                        \
			check_failures++;                             \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
			printf(__VA_ARGS__);                          \
			printf("\n");                                 \
		}                                                     \
	} while (0)

/* Print the summary line and return the exit status. */
static inline int check_done(const char *name)
{
	if (check_failures != 0) {
		printf("FAIL %s: %d of %d checks failed\n", name,
		       check_failures, check_count);
		return 1;
	}
	printf("PASS %s (%d checks)\n", name, check_count);
	return 0;
}

static inline int mem_eq(const void *a, size_t alen, const void *b, size_t blen)
{
	return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

#endif /* S14_CHECK_H */
