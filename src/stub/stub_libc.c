/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The calls GCC may emit for a copy or a clear, for the stub's own link (it
 * links no C library); zlib's own copies are zutil.c's under Z_SOLO.
 * Target only, and built without loop pattern distribution, so that these
 * loops do not become calls to themselves.
 */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (d <= s || d >= s + n)
		return memcpy(dst, src, n);
	while (n--)
		d[n] = s[n];
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;

	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}
