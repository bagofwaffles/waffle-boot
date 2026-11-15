/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A plain SHA-256 (FIPS 180-4) for the unit tests only, to check the
 * hand-over frame against its published digest (S-15.132). The
 * bootloader's own SHA-256 is the vendored library's.
 */
#ifndef S15_SHA256_H
#define S15_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct s15_sha256 {
	uint32_t h[8];
	uint8_t block[64];
	size_t fill;
	uint64_t bytes;
};

static inline uint32_t s15_ror(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32u - n));
}

static inline void s15_sha256_block(struct s15_sha256 *s)
{
	static const uint32_t k[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
		0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
		0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
		0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
		0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
		0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
		0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
		0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
		0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
		0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
		0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
		0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};
	uint32_t w[64], a[8];
	unsigned i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)s->block[4 * i] << 24 |
		       (uint32_t)s->block[4 * i + 1] << 16 |
		       (uint32_t)s->block[4 * i + 2] << 8 | s->block[4 * i + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = s15_ror(w[i - 15], 7) ^ s15_ror(w[i - 15], 18) ^
			      (w[i - 15] >> 3);
		uint32_t s1 = s15_ror(w[i - 2], 17) ^ s15_ror(w[i - 2], 19) ^
			      (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	for (i = 0; i < 8; i++)
		a[i] = s->h[i];
	for (i = 0; i < 64; i++) {
		uint32_t S1 = s15_ror(a[4], 6) ^ s15_ror(a[4], 11) ^
			      s15_ror(a[4], 25);
		uint32_t ch = (a[4] & a[5]) ^ (~a[4] & a[6]);
		uint32_t t1 = a[7] + S1 + ch + k[i] + w[i];
		uint32_t S0 = s15_ror(a[0], 2) ^ s15_ror(a[0], 13) ^
			      s15_ror(a[0], 22);
		uint32_t mj = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]);
		uint32_t t2 = S0 + mj;

		a[7] = a[6];
		a[6] = a[5];
		a[5] = a[4];
		a[4] = a[3] + t1;
		a[3] = a[2];
		a[2] = a[1];
		a[1] = a[0];
		a[0] = t1 + t2;
	}
	for (i = 0; i < 8; i++)
		s->h[i] += a[i];
}

static inline void s15_sha256_init(struct s15_sha256 *s)
{
	static const uint32_t h0[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	for (unsigned i = 0; i < 8; i++)
		s->h[i] = h0[i];
	s->fill = 0;
	s->bytes = 0;
}

static inline void s15_sha256_byte(struct s15_sha256 *s, uint8_t b)
{
	s->block[s->fill++] = b;
	s->bytes++;
	if (s->fill == 64) {
		s15_sha256_block(s);
		s->fill = 0;
	}
}

/* The digest as 64 lower-case hex digits and a NUL. */
static inline void s15_sha256_hex(struct s15_sha256 *s, char out[65])
{
	static const char hex[] = "0123456789abcdef";
	uint64_t bits = s->bytes * 8u;
	unsigned i;

	s15_sha256_byte(s, 0x80);
	while (s->fill != 56)
		s15_sha256_byte(s, 0);
	for (i = 0; i < 8; i++)
		s15_sha256_byte(s, (uint8_t)(bits >> (56 - 8 * i)));
	for (i = 0; i < 32; i++) {
		uint8_t b = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));

		out[2 * i] = hex[b >> 4];
		out[2 * i + 1] = hex[b & 15];
	}
	out[64] = '\0';
}

#endif /* S15_SHA256_H */
