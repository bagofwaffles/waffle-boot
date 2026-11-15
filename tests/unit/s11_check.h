/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef S11_CHECK_H
#define S11_CHECK_H

/* A minimal check harness shared by the s10c, s11 and s17 unit tests. */

#include <stdio.h>
#include <string.h>

static int s11_check_failures;
static int s11_check_count;

#define CHECK(cond, ...)                                                     \
	do {                                                                 \
		s11_check_count++;                                           \
		if (!(cond)) {                                               \
			s11_check_failures++;                                \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);        \
			printf(__VA_ARGS__);                                 \
			printf("\n");                                        \
		}                                                            \
	} while (0)

/* Compares a buffer of `n` characters with a NUL-terminated string. */
static inline int s11_text_eq(const char *buf, size_t n, const char *s)
{
	return s != NULL && strlen(s) == n && memcmp(buf, s, n) == 0;
}

static inline int s11_check_report(const char *name)
{
	if (s11_check_failures != 0) {
		printf("FAIL %s: %d of %d checks failed\n", name,
		       s11_check_failures, s11_check_count);
		return 1;
	}
	printf("PASS %s (%d checks)\n", name, s11_check_count);
	return 0;
}

#endif /* S11_CHECK_H */
