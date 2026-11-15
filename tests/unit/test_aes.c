/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BearSSL's table AES (aes_big), AES-256-CBC decryption: FIPS-197
 * appendix C.3, NIST SP 800-38A F.2.6, and the payload of every encrypted
 * package vector of S-11 (S-11.016: the name block and the zip padded to
 * 16 bytes are one CBC chain), checked against the SHA-256 of the zip that
 * the conformance package's AES decrypted (packages.txt).
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "bearssl_block.h"

/* Decrypts n bytes (a multiple of 16) in place, in pieces of piece bytes. */
static void cbc_decrypt(const uint8_t key[32], const uint8_t iv_in[16],
			uint8_t *buf, size_t n, size_t piece)
{
	br_aes_big_cbcdec_keys ctx;
	uint8_t iv[16];
	size_t done;

	memcpy(iv, iv_in, sizeof(iv));
	br_aes_big_cbcdec_init(&ctx, key, 32);
	for (done = 0; done < n; done += piece) {
		size_t len = n - done < piece ? n - done : piece;

		br_aes_big_cbcdec_run(&ctx, iv, buf + done, len);
	}
}

static void check_nist(void)
{
	uint8_t key[32], iv[16], buf[64], want[64];
	size_t i;

	/* FIPS-197 C.3: one block; CBC with a zero IV is the bare cipher. */
	for (i = 0; i < 32; i++)
		key[i] = (uint8_t)i;
	memset(iv, 0, sizeof(iv));
	if (tl_unhex_n("8ea2b7ca516745bfeafc49904b496089", buf, 16) ||
	    tl_unhex_n("00112233445566778899aabbccddeeff", want, 16))
		exit(2);
	cbc_decrypt(key, iv, buf, 16, 16);
	CHECK(!memcmp(buf, want, 16), "FIPS-197 C.3");

	/* SP 800-38A F.2.6, CBC-AES256.Decrypt: four blocks. */
	if (tl_unhex_n("603deb1015ca71be2b73aef0857d7781"
		       "1f352c073b6108d72d9810a30914dff4", key, 32) ||
	    tl_unhex_n("000102030405060708090a0b0c0d0e0f", iv, 16))
		exit(2);
	for (i = 16; i <= 64; i += 16) {
		if (tl_unhex_n("f58c4c04d6e5f1ba779eabfb5f7bfbd6"
			       "9cfc4e967edb808d679f777bc6702c7d"
			       "39f23369a9d9bacfa530e26304231461"
			       "b2eb05e2c39be9fcda6c19078c6a9d1b", buf, 64) ||
		    tl_unhex_n("6bc1bee22e409f96e93d7e117393172a"
			       "ae2d8a571e03ac9c9eb76fac45af8e51"
			       "30c81c46a35ce411e5fbc1191a0a52ef"
			       "f69f2445df4f9b17ad2b417be66c3710", want, 64))
			exit(2);
		cbc_decrypt(key, iv, buf, 64, i);
		CHECK(!memcmp(buf, want, 64), "SP 800-38A F.2.6 in %zu-byte pieces",
		      i);
	}
}

static int check_package(const struct tl_package *p)
{
	size_t padded = ((size_t)p->zip_len + 15) / 16 * 16;
	uint8_t *buf, d[32];
	br_sha256_context ctx;

	if (!p->has_key || !p->has_zip_sha)
		return 0;
	buf = tl_xmalloc(0x100 + padded);
	memcpy(buf, p->payload, 0x100 + padded);
	/* Name block and data as one chain, in two calls as a reader might. */
	cbc_decrypt(p->key, p->iv, buf, 0x100 + padded, 0x100);
	br_sha256_init(&ctx);
	br_sha256_update(&ctx, buf + 0x100, p->zip_len);
	br_sha256_out(&ctx, d);
	CHECK(!memcmp(d, p->zip_sha, 32), "%s: decrypted zip", p->name);
	free(buf);
	return 1;
}

int main(void)
{
	FILE *f = tl_open_vectors("packages.txt");
	struct tl_package p;
	int n = 0;

	check_nist();
	while (!tl_next_package(f, &p, NULL)) {
		n += check_package(&p);
		free(p.file);
	}
	fclose(f);
	CHECK(n >= 8, "only %d encrypted packages", n);
	return tl_done("test_aes");
}
