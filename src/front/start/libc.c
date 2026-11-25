/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The four calls GCC may emit for a copy or a clear, for the front's own
 * link (it links no C library). Target only.
 */
#include <stddef.h>

#include "front/front.h"

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

void *memcpy(void *dst, const void *src, size_t n)
{
	fr_copy(dst, src, n);
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (d <= s || d >= s + n) {
		fr_copy(dst, src, n);
		return dst;
	}
	while (n--)
		d[n] = s[n];
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	fr_fill(dst, (uint8_t)c, n);
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x < *y ? -1 : 1;
	return 0;
}
