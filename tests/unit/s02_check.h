/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal test harness shared by the s01-s05 unit tests: CHECK() records a
 * failure with its line and goes on; finish() prints the summary line and
 * gives main's return value.
 */
#ifndef S02_CHECK_H
#define S02_CHECK_H

#include <stdio.h>

static int g_checks;
static int g_failures;

#define CHECK(cond) check_at((cond) ? 1 : 0, #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check_eq_at((unsigned long)(a), (unsigned long)(b), \
				   #a " == " #b, __FILE__, __LINE__)

static inline int check_at(int ok, const char *what, const char *file, int line)
{
	g_checks++;
	if (!ok) {
		g_failures++;
		if (g_failures <= 40)
			printf("  FAIL %s:%d: %s\n", file, line, what);
	}
	return ok;
}

static inline int check_eq_at(unsigned long a, unsigned long b, const char *what,
			      const char *file, int line)
{
	g_checks++;
	if (a != b) {
		g_failures++;
		if (g_failures <= 40)
			printf("  FAIL %s:%d: %s (0x%lx != 0x%lx)\n", file, line, what, a, b);
	}
	return a == b;
}

static inline int finish(const char *name)
{
	if (g_failures != 0) {
		printf("FAIL %s: %d of %d checks failed\n", name, g_failures, g_checks);
		return 1;
	}
	printf("PASS %s (%d checks)\n", name, g_checks);
	return 0;
}

#endif /* S02_CHECK_H */
