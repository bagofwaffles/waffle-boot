/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BearSSL's SHA-256: the FIPS 180-4 examples, and S-11's two digests of
 * every package vector, the manifest digest of S-11.012 (the 0x300
 * manifest bytes with +0x010 to +0x04F zeroed) and the payload digest of
 * S-11.014, as Python's hashlib computed them (packages.txt).
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "bearssl_hash.h"

static void sha256(uint8_t out[32], const void *buf, size_t n)
{
	br_sha256_context ctx;

	br_sha256_init(&ctx);
	br_sha256_update(&ctx, buf, n);
	br_sha256_out(&ctx, out);
}

static void check_hex(const char *what, const uint8_t d[32], const char *want)
{
	char got[65];

	tl_hex(got, d, 32);
	CHECK(!strcmp(got, want), "%s: %s, want %s", what, got, want);
}

static void check_fips(void)
{
	static const char m448[] =
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	br_sha256_context ctx;
	uint8_t d[32], a[1000];
	int i;

	sha256(d, "abc", 3);
	check_hex("\"abc\"", d,
		  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	sha256(d, "", 0);
	check_hex("empty", d,
		  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	sha256(d, m448, strlen(m448));
	check_hex("448 bits", d,
		  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	memset(a, 'a', sizeof(a));
	br_sha256_init(&ctx);
	for (i = 0; i < 1000; i++)
		br_sha256_update(&ctx, a, sizeof(a));
	br_sha256_out(&ctx, d);
	check_hex("a million a", d,
		  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void check_package(const struct tl_package *p)
{
	uint8_t m[0x300], d[32];
	br_sha256_context ctx;
	size_t cut;

	memcpy(m, p->manifest, sizeof(m));
	memset(m + 0x10, 0, 0x40);
	sha256(d, m, sizeof(m));
	CHECK(!memcmp(d, p->digest, 32), "%s: manifest digest", p->name);

	sha256(d, p->payload, p->payload_len);
	CHECK(!memcmp(d, p->payload_sha, 32), "%s: payload digest", p->name);

	/* The same digest in uneven pieces. */
	br_sha256_init(&ctx);
	for (cut = 0; cut < p->payload_len;) {
		size_t n = p->payload_len - cut < 61 ? p->payload_len - cut : 61;

		br_sha256_update(&ctx, p->payload + cut, n);
		cut += n;
	}
	br_sha256_out(&ctx, d);
	CHECK(!memcmp(d, p->payload_sha, 32), "%s: payload digest in pieces",
	      p->name);
}

int main(void)
{
	FILE *f = tl_open_vectors("packages.txt");
	struct tl_package p;
	int n = 0;

	check_fips();
	while (!tl_next_package(f, &p, NULL)) {
		check_package(&p);
		free(p.file);
		n++;
	}
	fclose(f);
	CHECK(n >= 80, "only %d packages", n);
	return tl_done("test_sha256");
}
