/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The package check's SHA-256 and AES-256-CBC (S-11.020, checks 11 and 15)
 * on the processor's packet engine, which is some forty times faster than
 * the core at them. The engine is used only after it has given the answers
 * the software gives to a test of each, and only for whole aligned pieces;
 * everything else, and everything after an operation fails, is the
 * software's (wire_crypto_ops). The SHA-256 state stays in the software's
 * context between calls, so either can take the next bytes.
 *
 * What stands behind the engine's answers: a wrong digest refuses the
 * package (check 11). Of each piece it decrypts, the first and the last
 * block are decrypted again in software and compared; the blocks between
 * are covered by each member's CRC-32 when the zip is extracted
 * (S-11.030), before anything is installed.
 */
#include <string.h>

#include "blockdev.h"

#include "waffle/board/boot.h"
#include "waffle/board/services.h"
#include "waffle/util/bytes.h"
#include "waffle/drivers/bf70x_pkte.h"
#include "waffle/install/crypto.h"

#define PIECE 0x8000u		/* the check's piece (src/core/install/package.c) */
#define LEAST 0x400u		/* below this the engine's set-up costs more */
#define UNTOUCHED 0x5Au		/* answers(): the hash's destination */

struct wb_pkte opxy_pkte;
static bool present;		/* the engine is there to be left as found */
static bool engine;		/* tested, and no operation has failed since */
/* The engine's destination: a decryption's plaintext, and a hash's, which
 * stays as it was. */
static _Alignas(4) uint8_t plain[PIECE];

static bool aligned(const void *p)
{
	return ((uintptr_t)p & 3u) == 0u;
}

static void sha_update(void *ctx, const uint8_t *p, size_t n)
{
	struct wire_crypto *c = ctx;

	while (engine && n >= LEAST && aligned(p)) {
		uint8_t st[32];
		uint32_t h[8];
		const uint64_t total = br_sha256_state(&c->sha, st);
		const uint32_t len = n > PIECE ? PIECE : (uint32_t)n & ~63u;
		unsigned i;

		if (total % 64u != 0u)
			break;		/* the software holds part of a block */
		for (i = 0; i < 8u; i++)
			h[i] = wb_get_be32(st + 4u * i);
		if (!wb_pkte_sha256(&opxy_pkte, h, total, p, len, plain)) {
			engine = false;
			break;
		}
		for (i = 0; i < 8u; i++) {
			st[4u * i] = (uint8_t)(h[i] >> 24);
			st[4u * i + 1u] = (uint8_t)(h[i] >> 16);
			st[4u * i + 2u] = (uint8_t)(h[i] >> 8);
			st[4u * i + 3u] = (uint8_t)h[i];
		}
		br_sha256_set_state(&c->sha, st, total + len);
		p += len;
		n -= len;
	}
	wire_crypto_ops.sha256_update(ctx, p, n);
}

/* Whether the block of `in` at off, whose chain value is prev, decrypts in
 * software to what the engine put in plain. */
static bool block_agrees(void *ctx, const uint8_t key[32], const uint8_t *prev,
			 const uint8_t *in, uint32_t off)
{
	uint8_t v[16], b[16];

	memcpy(v, prev, sizeof(v));
	wire_crypto_ops.aes256_cbc_decrypt(ctx, key, v, in + off, b, sizeof(b));
	return memcmp(b, plain + off, sizeof(b)) == 0;
}

/* The engine decrypts into a buffer of ours, so an operation that fails or
 * disagrees with the software leaves the caller's bytes for the software. */
static void aes_decrypt(void *ctx, const uint8_t key[32], uint8_t iv[16],
			const uint8_t *in, uint8_t *out, size_t n)
{
	while (engine && n >= LEAST && aligned(in)) {
		const uint32_t len = n > PIECE ? PIECE : (uint32_t)n & ~15u;
		const uint32_t last = len - 16u;
		uint8_t next[16];

		memcpy(next, in + last, sizeof(next));
		if (!wb_pkte_aes256_cbc_decrypt(&opxy_pkte, key, iv, in, plain,
						len) ||
		    !block_agrees(ctx, key, iv, in, 0u) ||
		    !block_agrees(ctx, key, in + last - 16u, in, last)) {
			engine = false;
			break;
		}
		wb_blockdev_copy(out, plain, len);
		memcpy(iv, next, sizeof(next));
		in += len;
		out += len;
		n -= len;
	}
	if (n != 0u)
		wire_crypto_ops.aes256_cbc_decrypt(ctx, key, iv, in, out, n);
}

/* On the CRC peripheral while opxy_crc_begin()'s use lasts. */
static uint16_t crc16(void *ctx, uint16_t crc, const uint8_t *p, size_t n)
{
	return opxy_crc16(ctx, crc, p, n);
}

static void sha_init(void *ctx)
{
	wire_crypto_ops.sha256_init(ctx);
}

static void sha_final(void *ctx, uint8_t digest[32])
{
	wire_crypto_ops.sha256_final(ctx, digest);
}

static bool verify(void *ctx, const uint8_t q[56], const uint8_t digest[32],
		   const uint8_t r[28], const uint8_t s[28])
{
	return wire_crypto_ops.p224_verify(ctx, q, digest, r, s);
}

static const struct s11_crypto_ops ops = {
	crc16, sha_init, sha_update, sha_final, aes_decrypt, verify,
};

/*
 * The engine against the software on LEAST bytes: the digest, with the
 * hash's destination left as it was, then the plaintext.
 */
static bool answers(void)
{
	static _Alignas(4) uint8_t in[LEAST], want[LEAST];
	static struct wire_crypto c;
	static const uint8_t key[32] = { 0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca };
	uint8_t iv[16] = { 0xA5 }, d[2][32];
	unsigned i, k;

	for (i = 0; i < LEAST; i++)
		in[i] = (uint8_t)(i * 7u + (i >> 5));
	memset(plain, UNTOUCHED, LEAST);
	for (k = 0; k < 2u; k++) {
		engine = k != 0u;
		ops.sha256_init(&c);
		ops.sha256_update(&c, in, LEAST);
		ops.sha256_final(&c, d[k]);
		if (k != 0u && !engine)
			return false;
	}
	if (memcmp(d[0], d[1], sizeof(d[0])) != 0)
		return false;
	for (i = 0; i < LEAST; i++) {
		if (plain[i] != UNTOUCHED)
			return false;
	}
	wire_crypto_ops.aes256_cbc_decrypt(&c, key, iv, in, want, LEAST);
	memset(iv, 0, sizeof(iv));
	iv[0] = 0xA5;
	return wb_pkte_aes256_cbc_decrypt(&opxy_pkte, key, iv, in, plain,
					  LEAST) &&
	       memcmp(plain, want, LEAST) == 0;
}

const struct s11_crypto_ops *opxy_crypto(struct opxy_master *m)
{
	wb_pkte_init(&opxy_pkte, &m->hw.time);
	present = wb_pkte_present();
	engine = present && answers();
	return &ops;
}

bool opxy_crypto_engine(void)
{
	return engine;
}

void opxy_crypto_end(void)
{
	if (present)
		wb_pkte_end(&opxy_pkte);
	present = engine = false;
}
