/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef S11_REF_H
#define S11_REF_H

/*
 * Plain reference implementations for the S-11 tests, written from the
 * public standards and the parameters S-11 gives: CRC-16/XMODEM (S-11.008),
 * CRC-32 (the zip CRC, ISO 3309), SHA-256 (FIPS 180-4) and AES-256
 * decryption (FIPS 197). Test use only: src/ reaches the vendored libraries through
 * its ops structs.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* CRC-16/XMODEM: poly 0x1021, init 0, not reflected, no final XOR. */
static inline uint16_t ref_crc16(uint16_t crc, const uint8_t *p, size_t n)
{
	while (n-- != 0) {
		crc = (uint16_t)(crc ^ (*p++ << 8));
		for (int k = 0; k < 8; k++)
			crc = (uint16_t)((crc & 0x8000u) ?
					 ((unsigned)crc << 1) ^ 0x1021u :
					 (unsigned)crc << 1);
	}
	return crc;
}

/* CRC-32: reflected poly 0xEDB88320, init and final XOR 0xFFFFFFFF; `crc`
 * is the running value (0 to start). */
static inline uint32_t ref_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
	crc = ~crc;
	while (n-- != 0) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
	}
	return ~crc;
}

/* ---- SHA-256 ---- */

struct ref_sha256 {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	size_t n;
};

static inline uint32_t ref_ror(uint32_t x, unsigned r)
{
	return x >> r | x << (32u - r);
}

static inline void ref_sha256_block(struct ref_sha256 *s, const uint8_t *b)
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
	uint32_t w[64], a, bb, c, d, e, f, g, h;

	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 |
		       (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ref_ror(w[i - 15], 7) ^ ref_ror(w[i - 15], 18) ^
			      (w[i - 15] >> 3);
		uint32_t s1 = ref_ror(w[i - 2], 17) ^ ref_ror(w[i - 2], 19) ^
			      (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0]; bb = s->h[1]; c = s->h[2]; d = s->h[3];
	e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = h + (ref_ror(e, 6) ^ ref_ror(e, 11) ^ ref_ror(e, 25)) +
			      ((e & f) ^ (~e & g)) + k[i] + w[i];
		uint32_t t2 = (ref_ror(a, 2) ^ ref_ror(a, 13) ^ ref_ror(a, 22)) +
			      ((a & bb) ^ (a & c) ^ (bb & c));

		h = g; g = f; f = e; e = d + t1;
		d = c; c = bb; bb = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += bb; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static inline void ref_sha256_init(struct ref_sha256 *s)
{
	static const uint32_t h0[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(s->h, h0, sizeof(h0));
	s->len = 0;
	s->n = 0;
}

static inline void ref_sha256_update(struct ref_sha256 *s, const uint8_t *p,
				     size_t n)
{
	s->len += n;
	while (n-- != 0) {
		s->buf[s->n++] = *p++;
		if (s->n == 64) {
			ref_sha256_block(s, s->buf);
			s->n = 0;
		}
	}
}

static inline void ref_sha256_final(struct ref_sha256 *s, uint8_t out[32])
{
	uint64_t bits = s->len * 8u;
	uint8_t pad = 0x80;

	ref_sha256_update(s, &pad, 1);
	pad = 0;
	while (s->n != 56)
		ref_sha256_update(s, &pad, 1);
	for (int i = 7; i >= 0; i--) {
		uint8_t b = (uint8_t)(bits >> (8 * i));

		ref_sha256_update(s, &b, 1);
	}
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(s->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)s->h[i];
	}
}

static inline void ref_sha256(const uint8_t *p, size_t n, uint8_t out[32])
{
	struct ref_sha256 s;

	ref_sha256_init(&s);
	ref_sha256_update(&s, p, n);
	ref_sha256_final(&s, out);
}

/* ---- AES-256 decryption ---- */

static inline uint8_t ref_xtime(uint8_t a)
{
	return (uint8_t)(((unsigned)a << 1) ^ ((a & 0x80u) ? 0x1Bu : 0u));
}

static inline uint8_t ref_gmul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;

	while (b != 0) {
		if (b & 1u)
			p ^= a;
		a = ref_xtime(a);
		b >>= 1;
	}
	return p;
}

/* The S-box, computed from the field inverse and the affine map. */
static inline void ref_aes_sboxes(uint8_t sbox[256], uint8_t inv[256])
{
	for (unsigned x = 0; x < 256; x++) {
		uint8_t y = 0, s;

		for (unsigned c = 1; c < 256 && x != 0; c++) {
			if (ref_gmul((uint8_t)x, (uint8_t)c) == 1) {
				y = (uint8_t)c;
				break;
			}
		}
		s = (uint8_t)(y ^ (uint8_t)(y << 1 | y >> 7) ^
			      (uint8_t)(y << 2 | y >> 6) ^
			      (uint8_t)(y << 3 | y >> 5) ^
			      (uint8_t)(y << 4 | y >> 4) ^ 0x63);
		sbox[x] = s;
		inv[s] = (uint8_t)x;
	}
}

struct ref_aes256 {
	uint8_t rk[240];
	uint8_t sbox[256], inv[256];
};

static inline void ref_aes256_init(struct ref_aes256 *a, const uint8_t key[32])
{
	uint8_t rcon = 1;

	ref_aes_sboxes(a->sbox, a->inv);
	memcpy(a->rk, key, 32);
	for (unsigned i = 8; i < 60; i++) {
		uint8_t t[4];

		memcpy(t, a->rk + 4 * (i - 1), 4);
		if (i % 8 == 0) {
			uint8_t t0 = t[0];

			t[0] = (uint8_t)(a->sbox[t[1]] ^ rcon);
			t[1] = a->sbox[t[2]];
			t[2] = a->sbox[t[3]];
			t[3] = a->sbox[t0];
			rcon = ref_xtime(rcon);
		} else if (i % 8 == 4) {
			for (int k = 0; k < 4; k++)
				t[k] = a->sbox[t[k]];
		}
		for (int k = 0; k < 4; k++)
			a->rk[4 * i + (unsigned)k] =
				(uint8_t)(a->rk[4 * (i - 8) + (unsigned)k] ^ t[k]);
	}
}

static inline void ref_aes256_decrypt_block(const struct ref_aes256 *a,
					    const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16], t[16];

	for (int i = 0; i < 16; i++)
		s[i] = (uint8_t)(in[i] ^ a->rk[224 + i]);
	for (int round = 13; round >= 0; round--) {
		/* InvShiftRows, InvSubBytes */
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				t[4 * ((c + r) % 4) + r] = a->inv[s[4 * c + r]];
		for (int i = 0; i < 16; i++)
			s[i] = (uint8_t)(t[i] ^ a->rk[16 * round + i]);
		if (round == 0)
			break;
		for (int c = 0; c < 4; c++) {	/* InvMixColumns */
			uint8_t *col = s + 4 * c, x[4];

			memcpy(x, col, 4);
			for (int r = 0; r < 4; r++)
				col[r] = (uint8_t)(ref_gmul(x[r], 14) ^
						   ref_gmul(x[(r + 1) % 4], 11) ^
						   ref_gmul(x[(r + 2) % 4], 13) ^
						   ref_gmul(x[(r + 3) % 4], 9));
		}
	}
	memcpy(out, s, 16);
}

static inline void ref_aes256_cbc_decrypt(const uint8_t key[32], uint8_t iv[16],
					  const uint8_t *in, uint8_t *out,
					  size_t n)
{
	static struct ref_aes256 a;
	static uint8_t last[32];
	static int ready;

	if (!ready || memcmp(last, key, 32) != 0) {	/* the S-box is slow */
		ref_aes256_init(&a, key);
		memcpy(last, key, 32);
		ready = 1;
	}
	for (size_t i = 0; i + 16 <= n; i += 16) {
		uint8_t c[16];

		memcpy(c, in + i, 16);
		ref_aes256_decrypt_block(&a, c, out + i);
		for (int k = 0; k < 16; k++)
			out[i + (size_t)k] ^= iv[k];
		memcpy(iv, c, 16);
	}
}

#endif /* S11_REF_H */
