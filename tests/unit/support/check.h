/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_TESTS_CHECK_H
#define WAFFLE_TESTS_CHECK_H

/*
 * Minimal test helpers. A test file defines its cases as functions, runs
 * them with RUN(), and returns check_summary(name) from main.
 */

#include <stdio.h>

static unsigned check_failures;

#define CHECK(cond)                                                      \
	do {                                                             \
		if (!(cond)) {                                           \
			printf("  %s:%d: CHECK(%s) failed\n", __FILE__,  \
			       __LINE__, #cond);                         \
			check_failures++;                                \
		}                                                        \
	} while (0)

#define RUN(fn)                                                          \
	do {                                                             \
		unsigned before_ = check_failures;                       \
		fn();                                                    \
		if (check_failures != before_)                           \
			printf("  in %s\n", #fn);                        \
	} while (0)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static inline int check_summary(const char *name)
{
	if (check_failures) {
		printf("FAIL %s: %u check(s) failed\n", name, check_failures);
		return 1;
	}
	printf("PASS %s\n", name);
	return 0;
}

#endif /* WAFFLE_TESTS_CHECK_H */
