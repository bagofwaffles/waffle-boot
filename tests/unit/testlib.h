/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Helpers for the unit tests: checks, hex, and the vector files that
 * tests/vectors/export.py writes into $WB_VECTORS (default build/vectors).
 * Define _POSIX_C_SOURCE 200809L before including this (getline).
 */
#ifndef WAFFLE_TESTLIB_H
#define WAFFLE_TESTLIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl_hash.h"

static unsigned int tl_failures;
static unsigned int tl_checks;

#define CHECK(cond, ...)                                                   \
	do {                                                               \
		tl_checks++;                                               \
		if (!(cond)) {                                             \
			tl_failures++;                                     \
			printf("  %s:%d: check failed: %s: ", __FILE__,    \
			       __LINE__, #cond);                           \
			printf(__VA_ARGS__);                               \
			printf("\n");                                      \
		}                                                          \
	} while (0)

/* Prints the verdict of test name and returns main()'s exit status. */
static inline int tl_done(const char *name)
{
	if (tl_failures) {
		printf("FAIL %s: %u of %u checks failed\n", name, tl_failures,
		       tl_checks);
		return 1;
	}
	printf("PASS %s (%u checks)\n", name, tl_checks);
	return 0;
}

static inline void *tl_xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p) {
		printf("out of memory (%zu bytes)\n", n);
		exit(2);
	}
	return p;
}

static inline int tl_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Decodes hex (or "-": no bytes) into a new buffer; *len gets its length.
 * Exits on malformed input: the vector files are ours.
 */
static inline uint8_t *tl_unhex(const char *hex, size_t *len)
{
	size_t n = strcmp(hex, "-") ? strlen(hex) : 0;
	uint8_t *out = tl_xmalloc(n / 2);
	size_t i;

	if (n % 2) {
		printf("odd hex string\n");
		exit(2);
	}
	for (i = 0; i < n / 2; i++) {
		int hi = tl_nibble(hex[2 * i]), lo = tl_nibble(hex[2 * i + 1]);

		if (hi < 0 || lo < 0) {
			printf("bad hex string\n");
			exit(2);
		}
		out[i] = (uint8_t)(hi << 4 | lo);
	}
	*len = n / 2;
	return out;
}

/* Decodes exactly n bytes of hex into out; returns 0, or -1 on a mismatch. */
static inline int tl_unhex_n(const char *hex, uint8_t *out, size_t n)
{
	size_t len;
	uint8_t *b = tl_unhex(hex, &len);
	int ok = len == n;

	if (ok)
		memcpy(out, b, n);
	free(b);
	return ok ? 0 : -1;
}

/* Writes the 2n hex digits of buf and a NUL to out. */
static inline void tl_hex(char *out, const void *buf, size_t n)
{
	static const char digits[] = "0123456789abcdef";
	const uint8_t *p = buf;
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = digits[p[i] >> 4];
		out[2 * i + 1] = digits[p[i] & 15];
	}
	out[2 * n] = '\0';
}

/* The SHA-256 of buf, as 64 hex digits and a NUL in out. */
static inline void tl_sha256_hex(char out[65], const void *buf, size_t n)
{
	br_sha256_context ctx;
	uint8_t d[32];

	br_sha256_init(&ctx);
	br_sha256_update(&ctx, buf, n);
	br_sha256_out(&ctx, d);
	tl_hex(out, d, sizeof(d));
}

/* The path of file name in the vector directory, in a static buffer. */
static inline const char *tl_vector_path(const char *name)
{
	static char path[4096];
	const char *dir = getenv("WB_VECTORS");

	snprintf(path, sizeof(path), "%s/%s", dir ? dir : "build/vectors",
		 name);
	return path;
}

static inline FILE *tl_open_vectors(const char *name)
{
	const char *path = tl_vector_path(name);
	FILE *f = fopen(path, "r");

	if (!f) {
		printf("cannot open %s (run make check, which exports it)\n",
		       path);
		exit(2);
	}
	return f;
}

/*
 * Reads the next line of f and splits it at spaces into at most max
 * fields. Returns the number of fields, or -1 at the end of the file. The
 * fields point into *line, which the caller frees once at the end.
 */
static inline int tl_fields(FILE *f, char **line, size_t *cap, char **field,
			    int max)
{
	ssize_t len = getline(line, cap, f);
	int n = 0;
	char *p;

	if (len < 0)
		return -1;
	p = *line;
	while (*p && n < max) {
		while (*p == ' ' || *p == '\n')
			*p++ = '\0';
		if (!*p)
			break;
		field[n++] = p;
		while (*p && *p != ' ' && *p != '\n')
			p++;
	}
	if (*p == '\n')
		*p = '\0';
	return n;
}

/* Reads a whole file into a new buffer; exits if it cannot. */
static inline uint8_t *tl_read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long size;

	if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET)) {
		printf("cannot read %s\n", path);
		exit(2);
	}
	buf = tl_xmalloc((size_t)size);
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		printf("cannot read %s\n", path);
		exit(2);
	}
	fclose(f);
	*len = (size_t)size;
	return buf;
}

/*
 * One package of S-11's vectors (packages.txt): where its manifest and
 * payload are, and what an independent implementation computed from them.
 */
struct tl_package {
	char name[64];
	uint8_t *file;
	size_t file_len;
	const uint8_t *manifest;     /* 0x300 bytes */
	const uint8_t *payload;
	size_t payload_len;
	uint8_t digest[32];          /* S-11.012's manifest digest */
	uint8_t payload_sha[32];
	uint8_t sig[56];             /* r then s, 28 bytes each */
	int sig_valid;               /* under the test Q0 */
	unsigned int key_index;
	int has_key;
	uint8_t key[32];             /* the test K0 or K1 */
	uint8_t iv[16];
	uint32_t zip_len;
	int has_zip_sha;
	uint8_t zip_sha[32];         /* of the decrypted (or plaintext) zip */
};

/*
 * Reads the next package of packages.txt; the q0 line, if q0 is not NULL,
 * fills it with the test cloud key (x then y). Returns 0, or -1 at the end.
 * Free p->file when done with p.
 */
static inline int tl_next_package(FILE *f, struct tl_package *p,
				  uint8_t q0[56])
{
	static char *line;
	static size_t cap;
	char *fld[16], path[1024];
	int n;

	while ((n = tl_fields(f, &line, &cap, fld, 16)) >= 0) {
		if (n == 2 && !strcmp(fld[0], "q0")) {
			if (q0 && tl_unhex_n(fld[1], q0, 56))
				exit(2);
			continue;
		}
		if (n != 15 || strcmp(fld[0], "pkg"))
			continue;
		memset(p, 0, sizeof(*p));
		snprintf(p->name, sizeof(p->name), "%s", fld[1]);
		snprintf(path, sizeof(path), "tests/oracle/%s", fld[2]);
		p->file = tl_read_file(path, &p->file_len);
		p->manifest = p->file + strtoul(fld[3], NULL, 10);
		p->payload_len = strtoul(fld[4], NULL, 10);
		p->payload = p->manifest + 0x300;
		if (tl_unhex_n(fld[5], p->digest, 32) ||
		    tl_unhex_n(fld[6], p->payload_sha, 32) ||
		    tl_unhex_n(fld[7], p->sig, 28) ||
		    tl_unhex_n(fld[8], p->sig + 28, 28) ||
		    tl_unhex_n(fld[12], p->iv, 16))
			exit(2);
		p->sig_valid = !strcmp(fld[9], "1");
		p->key_index = (unsigned int)strtoul(fld[10], NULL, 16);
		p->has_key = strcmp(fld[11], "-") != 0;
		if (p->has_key && tl_unhex_n(fld[11], p->key, 32))
			exit(2);
		p->zip_len = (uint32_t)strtoul(fld[13], NULL, 10);
		p->has_zip_sha = strcmp(fld[14], "-") != 0;
		if (p->has_zip_sha && tl_unhex_n(fld[14], p->zip_sha, 32))
			exit(2);
		return 0;
	}
	free(line);
	line = NULL;
	cap = 0;
	return -1;
}

#endif /* WAFFLE_TESTLIB_H */
