/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The picolibc subset the target uses, run on the host: the build renames
 * its symbols with a pl_ prefix so that they cannot clash with the host's
 * C library (src/third_party/third_party.mk). Known answers from the C
 * standard's definitions of each function.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>

#include "testlib.h"

void *pl_memchr(const void *s, int c, size_t n);
int pl_memcmp(const void *a, const void *b, size_t n);
void *pl_memcpy(void *dst, const void *src, size_t n);
void *pl_memmove(void *dst, const void *src, size_t n);
void *pl_memset(void *s, int c, size_t n);
char *pl_strcat(char *dst, const char *src);
char *pl_strchr(const char *s, int c);
int pl_strcmp(const char *a, const char *b);
char *pl_strcpy(char *dst, const char *src);
size_t pl_strcspn(const char *s, const char *reject);
size_t pl_strlen(const char *s);
char *pl_strncat(char *dst, const char *src, size_t n);
int pl_strncmp(const char *a, const char *b, size_t n);
char *pl_strncpy(char *dst, const char *src, size_t n);
size_t pl_strnlen(const char *s, size_t n);
char *pl_strrchr(const char *s, int c);
size_t pl_strspn(const char *s, const char *accept);
char *pl_strstr(const char *haystack, const char *needle);
int pl_snprintf(char *buf, size_t n, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
long pl_strtol(const char *s, char **end, int base);
unsigned long pl_strtoul(const char *s, char **end, int base);
void pl_qsort(void *base, size_t n, size_t size,
	      int (*cmp)(const void *, const void *));
void *pl_malloc(size_t n);
void *pl_calloc(size_t n, size_t size);
void *pl_realloc(void *p, size_t n);
void pl_free(void *p);
extern int pl_errno;

/* picolibc's malloc takes its memory from sbrk(): a fixed arena here. */
void *pl_sbrk(ptrdiff_t incr);

static unsigned char arena[1 << 20] __attribute__((aligned(16)));
static size_t arena_used;

void *pl_sbrk(ptrdiff_t incr)
{
	void *p = arena + arena_used;

	if (incr < 0 || (size_t)incr > sizeof(arena) - arena_used)
		return (void *)-1;
	arena_used += (size_t)incr;
	return p;
}

/*
 * memcpy and memset copy by words (src/glue/libc/picolibc.h): every
 * alignment of each end and every length through the byte, word and
 * four-word paths, with the bytes around the destination left alone.
 * This is the host's build of them: a word here is 8 bytes, 4 on the
 * target, and a misaligned access here does not trap as it does there.
 */
static void check_word_copies(void)
{
	static uint8_t src[128], dst[128];
	unsigned so, d, n, i;
	bool cpy = true, set = true, ret = true;

	for (i = 0; i < sizeof(src); i++)
		src[i] = (uint8_t)(i * 7u + 1u);
	for (so = 0; so < 8u; so++) {
		for (d = 8u; d < 16u; d++) {
			for (n = 0; n <= 80u; n++) {
				memset(dst, 0xEE, sizeof(dst));
				ret = ret && pl_memcpy(dst + d, src + so, n) == dst + d;
				for (i = 0; i < sizeof(dst); i++)
					cpy = cpy && dst[i] == (i >= d && i < d + n ?
								src[so + i - d] : 0xEE);
			}
		}
	}
	for (d = 8u; d < 16u; d++) {
		for (n = 0; n <= 80u; n++) {
			memset(dst, 0xEE, sizeof(dst));
			ret = ret && pl_memset(dst + d, 0x1A5, n) == dst + d;
			for (i = 0; i < sizeof(dst); i++)
				set = set && dst[i] == (i >= d && i < d + n ? 0xA5 : 0xEE);
		}
	}
	CHECK(cpy, "memcpy at every alignment and length");
	CHECK(set, "memset at every alignment and length");
	CHECK(ret, "memcpy and memset return the destination");
}

static void check_mem(void)
{
	uint8_t a[32], b[32];
	size_t i;

	for (i = 0; i < sizeof(a); i++)
		a[i] = (uint8_t)i;
	CHECK(pl_memcpy(b, a, sizeof(a)) == b, "memcpy result");
	CHECK(!memcmp(a, b, sizeof(a)), "memcpy copies");
	CHECK(pl_memset(b, 0xA5, 7) == b && b[6] == 0xA5 && b[7] == 7,
	      "memset sets exactly n bytes");
	pl_memmove(a + 4, a, 16);
	CHECK(a[4] == 0 && a[19] == 15 && a[20] == 20, "memmove up");
	pl_memmove(a, a + 4, 16);
	CHECK(a[0] == 0 && a[15] == 15 && a[16] == 12, "memmove down");
	CHECK(pl_memcmp("\x80", "\x7f", 1) > 0, "memcmp compares unsigned");
	CHECK(pl_memcmp("abc", "abd", 3) < 0 && !pl_memcmp("abc", "abd", 2),
	      "memcmp");
	CHECK(pl_memchr("abcabc", 'c', 6) == NULL ? 0 : 1, "memchr finds");
	CHECK(pl_memchr("abc", 'z', 3) == NULL, "memchr misses");
}

static void check_str(void)
{
	static const char s[] = "hello, world";
	char buf[32];

	CHECK(pl_strlen(s) == 12 && pl_strlen("") == 0, "strlen");
	CHECK(pl_strnlen(s, 5) == 5 && pl_strnlen(s, 99) == 12, "strnlen");
	CHECK(pl_strcmp("abc", "abc") == 0 && pl_strcmp("ab", "abc") < 0 &&
		      pl_strcmp("\xe9", "e") > 0, "strcmp (unsigned bytes)");
	CHECK(pl_strncmp("abcx", "abcy", 3) == 0 &&
		      pl_strncmp("abcx", "abcy", 4) < 0, "strncmp");
	CHECK(pl_strchr(s, 'o') == s + 4 && pl_strchr(s, '\0') == s + 12 &&
		      pl_strchr(s, 'z') == NULL, "strchr");
	CHECK(pl_strrchr(s, 'o') == s + 8 && pl_strrchr(s, 'z') == NULL,
	      "strrchr");
	CHECK(pl_strstr(s, "world") == s + 7 && pl_strstr(s, "") == s &&
		      pl_strstr(s, "worlds") == NULL, "strstr");
	CHECK(pl_strspn(s, "ehlo") == 5 && pl_strcspn(s, ",") == 5, "spans");
	CHECK(pl_strcpy(buf, "ab") == buf && !strcmp(buf, "ab"), "strcpy");
	CHECK(pl_strcat(buf, "cd") == buf && !strcmp(buf, "abcd"), "strcat");
	pl_strncat(buf, "efgh", 2);
	CHECK(!strcmp(buf, "abcdef"), "strncat appends at most n");
	memset(buf, 'x', sizeof(buf));
	pl_strncpy(buf, "ab", 5);
	CHECK(!memcmp(buf, "ab\0\0\0x", 6), "strncpy pads with NULs");
	pl_strncpy(buf, "abcdef", 3);
	CHECK(!memcmp(buf, "abc\0\0x", 6), "strncpy does not terminate");
}

static void fmt(const char *want, int want_ret, const char *got, int ret)
{
	CHECK(!strcmp(got, want) && ret == want_ret,
	      "snprintf gave \"%s\" (%d), want \"%s\" (%d)", got, ret, want,
	      want_ret);
}

static void check_snprintf(void)
{
	char b[64];
	int r;

	r = pl_snprintf(b, sizeof(b), "%d %i %u", -42, 7, 4294967295u);
	fmt("-42 7 4294967295", 16, b, r);
	r = pl_snprintf(b, sizeof(b), "[%5d|%-5d|%05d|%+d]", 42, 42, 42, 42);
	fmt("[   42|42   |00042|+42]", 23, b, r);
	r = pl_snprintf(b, sizeof(b), "%x %X %#x %08x", 0xbeefu, 0xbeefu,
			0xbeefu, 0x1234u);
	fmt("beef BEEF 0xbeef 00001234", 25, b, r);
	r = pl_snprintf(b, sizeof(b), "%lld %llu", LLONG_MIN, ULLONG_MAX);
	fmt("-9223372036854775808 18446744073709551615", 41, b, r);
	r = pl_snprintf(b, sizeof(b), "%ld %lu %lx", -1L, 3UL, 255UL);
	fmt("-1 3 ff", 7, b, r);
	r = pl_snprintf(b, sizeof(b), "%zu %hhx %hd", (size_t)123,
			(unsigned char)0xAB, (short)-2);
	fmt("123 ab -2", 9, b, r);
	r = pl_snprintf(b, sizeof(b), "%s|%.2s|%5s|%-4s|%c|%%", "abc", "abc",
			"ab", "ab", 'z');
	fmt("abc|ab|   ab|ab  |z|%", 21, b, r);
	r = pl_snprintf(b, sizeof(b), "v. %u.%u.%u+%u", 1u, 1u, 2u, 4512u);
	fmt("v. 1.1.2+4512", 13, b, r);
	/* Truncation: the result counts what would have been written. */
	r = pl_snprintf(b, 4, "%d", 12345);
	fmt("123", 5, b, r);
	r = pl_snprintf(NULL, 0, "%s", "hello");
	CHECK(r == 5, "snprintf(NULL, 0) gave %d", r);
	b[0] = 'x';
	r = pl_snprintf(b, 1, "%s", "hello");
	fmt("", 5, b, r);
}

static void tol(const char *s, int base, long want, size_t used, int erange)
{
	char *end;
	long v;

	pl_errno = 0;
	v = pl_strtol(s, &end, base);
	CHECK(v == want && (size_t)(end - s) == used &&
		      (pl_errno == ERANGE) == erange,
	      "strtol(\"%s\", %d) = %ld, used %td, errno %d", s, base, v,
	      end - s, pl_errno);
}

static void toul(const char *s, int base, unsigned long want, size_t used,
		 int erange)
{
	char *end;
	unsigned long v;

	pl_errno = 0;
	v = pl_strtoul(s, &end, base);
	CHECK(v == want && (size_t)(end - s) == used &&
		      (pl_errno == ERANGE) == erange,
	      "strtoul(\"%s\", %d) = %lu, used %td, errno %d", s, base, v,
	      end - s, pl_errno);
}

static void check_strto(void)
{
	tol("123", 10, 123, 3, 0);
	tol("  -0x1f", 0, -31, 7, 0);
	tol("0777", 0, 0777, 4, 0);
	tol("0x", 16, 0, 1, 0);
	tol("12abc", 10, 12, 2, 0);
	tol("z", 36, 35, 1, 0);
	tol("", 10, 0, 0, 0);
	tol("  +", 10, 0, 0, 0);
	tol("99999999999999999999", 10, LONG_MAX, 20, 1);
	tol("-99999999999999999999", 10, LONG_MIN, 21, 1);
	toul("4294967295", 10, 4294967295UL, 10, 0);
	toul("0xFFFFFFFF", 0, 0xFFFFFFFFUL, 10, 0);
	toul("-1", 10, ULONG_MAX, 2, 0);
	toul("99999999999999999999", 10, ULONG_MAX, 20, 1);
}

static int cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	return (x > y) - (x < y);
}

static void check_qsort(void)
{
	int v[] = { 5, -1, 9, 0, 5, 3, -7, 2 };
	size_t i;

	pl_qsort(v, sizeof(v) / sizeof(v[0]), sizeof(v[0]), cmp_int);
	for (i = 1; i < sizeof(v) / sizeof(v[0]); i++)
		CHECK(v[i - 1] <= v[i], "qsort order at %zu", i);
	CHECK(v[0] == -7 && v[7] == 9, "qsort ends");
}

static void check_malloc(void)
{
	uint8_t *p, *q;
	size_t i, high;
	int round;

	p = pl_malloc(100);
	CHECK(p != NULL, "malloc");
	if (!p)
		return;
	for (i = 0; i < 100; i++)
		p[i] = (uint8_t)i;
	q = pl_realloc(p, 5000);
	CHECK(q != NULL, "realloc grows");
	if (!q)
		return;
	for (i = 0; i < 100; i++)
		CHECK(q[i] == (uint8_t)i, "realloc keeps byte %zu", i);
	pl_free(q);
	q = pl_calloc(64, 16);
	CHECK(q != NULL, "calloc");
	for (i = 0; q && i < 64 * 16; i++)
		CHECK(q[i] == 0, "calloc zeroes byte %zu", i);
	pl_free(q);
	/* Freed memory is reused: the arena does not grow without bound. */
	high = arena_used;
	for (round = 0; round < 1000; round++) {
		p = pl_malloc(1000);
		q = pl_malloc(3000);
		CHECK(p && q, "malloc in round %d", round);
		pl_free(p);
		pl_free(q);
	}
	CHECK(arena_used <= high + 8192, "arena grew to %zu", arena_used);
	CHECK(pl_malloc(sizeof(arena)) == NULL, "an oversized malloc fails");
}

int main(void)
{
	check_mem();
	check_word_copies();
	check_str();
	check_snprintf();
	check_strto();
	check_qsort();
	check_malloc();
	return tl_done("test_libc");
}
