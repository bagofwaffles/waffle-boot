/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * micro-ecc's ECDSA P-224 verification (S-11.012) on every package vector
 * of S-11, against the verdict of the conformance package's P-224 under the
 * test cloud key Q0 (packages.txt), and on signatures and keys altered from
 * a valid one.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "uECC.h"

/* The order n of P-224, big-endian. */
static const uint8_t p224_n[28] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0x16, 0xA2, 0xE0, 0xB8, 0xF0, 0x3E,
	0x13, 0xDD, 0x29, 0x45, 0x5C, 0x5C, 0x2A, 0x3D,
};

static int verify(const uint8_t q[56], const uint8_t digest[32],
		  const uint8_t sig[56])
{
	return uECC_verify(q, digest, 32, sig, uECC_secp224r1());
}

/* Alterations of a valid signature that must all fail. */
static void check_altered(const uint8_t q0[56], const struct tl_package *p)
{
	uint8_t sig[56], d[32], q[56];
	size_t bit;

	for (bit = 0; bit < 56 * 8; bit += 37) {
		memcpy(sig, p->sig, 56);
		sig[bit / 8] ^= (uint8_t)(1u << (bit % 8));
		CHECK(!verify(q0, p->digest, sig), "%s: signature bit %zu",
		      p->name, bit);
	}
	for (bit = 0; bit < 32 * 8; bit += 29) {
		memcpy(d, p->digest, 32);
		d[bit / 8] ^= (uint8_t)(1u << (bit % 8));
		/* Bits past the leftmost 224 are not part of e (S-11.012). */
		CHECK(verify(q0, d, p->sig) == (bit >= 224),
		      "%s: digest bit %zu", p->name, bit);
	}
	memcpy(sig, p->sig, 56);
	memset(sig, 0, 28);
	CHECK(!verify(q0, p->digest, sig), "%s: r = 0", p->name);
	memcpy(sig, p->sig, 56);
	memset(sig + 28, 0, 28);
	CHECK(!verify(q0, p->digest, sig), "%s: s = 0", p->name);
	memcpy(sig + 28, p224_n, 28);
	CHECK(!verify(q0, p->digest, sig), "%s: s = n", p->name);
	memcpy(sig, p->sig, 56);
	memcpy(sig, p224_n, 28);
	CHECK(!verify(q0, p->digest, sig), "%s: r = n", p->name);

	/* Another valid point (the generator), and a point off the curve. */
	memcpy(q, q0, 56);
	q[55] ^= 1;
	CHECK(!uECC_valid_public_key(q, uECC_secp224r1()), "off-curve key");
	CHECK(!verify(q, p->digest, p->sig), "%s: off-curve key", p->name);
}

int main(void)
{
	FILE *f = tl_open_vectors("packages.txt");
	struct tl_package p;
	uint8_t q0[56], g[56];
	int n = 0, valid = 0, altered = 0;

	if (tl_unhex_n("b70e0cbd6bb4bf7f321390b94a03c1d356c21122343280d6115c1d21"
		       "bd376388b5f723fb4c22dfe6cd4375a05a07476444d5819985007e34",
		       g, 56))
		return 2;
	memset(q0, 0, sizeof(q0));
	while (!tl_next_package(f, &p, q0)) {
		int got = verify(q0, p.digest, p.sig);

		CHECK(got == p.sig_valid, "%s: verify gave %d, want %d", p.name,
		      got, p.sig_valid);
		if (p.sig_valid) {
			valid++;
			CHECK(!verify(g, p.digest, p.sig), "%s: under another key",
			      p.name);
			if (!altered++)
				check_altered(q0, &p);
		}
		free(p.file);
		n++;
	}
	fclose(f);
	CHECK(uECC_valid_public_key(q0, uECC_secp224r1()), "Q0 is valid");
	CHECK(n >= 80 && valid >= 50 && valid < n,
	      "%d packages, %d valid signatures", n, valid);
	return tl_done("test_p224");
}
