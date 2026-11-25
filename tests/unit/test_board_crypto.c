/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The package check's crypto on the packet engine (src/board/install/crypto.c,
 * src/drivers/bf70x/pkte.c) against a model of the engine as a unit shows
 * it: one descriptor in autonomous ring mode, a hash continued from the
 * state record, a decryption that leaves the state's IV alone and ends with
 * the pad error set.
 *
 * Covered: with the engine absent, failing or silent part-way, giving a
 * wrong digest state, writing to a hash's destination, or giving a wrong
 * first or last block of a piece's plaintext with a good status, the digest
 * and the plaintext are the software's and the engine is not used again.
 * Not covered here, and not by the code under test: a wrong block between
 * a piece's first and last with a good status passes through; the zip's
 * CRC-32s catch it at extraction (S-11.030). The engine's use ends with a
 * zero key in it and its registers as reset leaves them.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "bearssl_block.h"
#include "bearssl_hash.h"

#include "waffle/board/boot.h"
#include "waffle/board/services.h"
#include "waffle/drivers/bf70x/pkte.h"
#include "waffle/install/crypto.h"

#define N 100000u

static struct opxy_master m;

/* The model. */
static struct {
	bool present;
	unsigned ops, results, bad;	/* bad: a sequence the unit never saw */
	unsigned fail_at, silent_at;	/* the operation (from 1) that fails */
	bool wrong;			/* every answer is off by a bit */
	bool copies;			/* a hash writes its destination */
	/* the operation whose plaintext is off by a bit at this offset from
	 * its start (flip_end: from its end), with a good status */
	unsigned flip_at;
	uint32_t flip_off;
	bool flip_end;
	bool key;			/* the last key was not zero */
} eng;

static uint32_t low(const void *p)
{
	return (uint32_t)(uintptr_t)p;
}

static void put_be(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
}

static void put_le(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static void engine_run(void)
{
	struct wb_pkte *p = &opxy_pkte;
	const uint32_t cmd0 = p->sa[0];
	uint32_t status = 0;
	unsigned i;

	eng.ops++;
	if (fake_mmr_get(PKTE_CFG) != PKTE_CFG_MODE_ARM ||
	    fake_mmr_get(PKTE_RING_CFG) != 1u ||
	    fake_mmr_get(PKTE_CDRBASE_ADDR) != low(p->cdr) ||
	    fake_mmr_get(PKTE_RDRBASE_ADDR) != low(p->rdr) ||
	    (p->cdr[0][0] & 3u) != PKTE_CTL_HOSTRDY ||
	    p->cdr[0][1] != low(p->src) || p->cdr[0][2] != low(p->dst) ||
	    p->cdr[0][3] != low(p->sa) || p->cdr[0][4] != low(p->state) ||
	    p->cdr[0][7] != (PKTE_LEN_HSTRDY | p->len) || p->len == 0u ||
	    p->len > WB_PKTE_MAX || ((low(p->src) | low(p->dst)) & 3u) != 0u)
		eng.bad++;
	if (eng.ops == eng.silent_at)
		return;
	if (eng.ops == eng.fail_at) {
		status = 0x88u;
	} else if ((cmd0 & 7u) == PKTE_CMD0_HASH) {
		br_sha256_context c;
		uint8_t st[32];

		if (p->len % 64u != 0u || p->dst == NULL ||
		    (cmd0 & (3u << 26)) != PKTE_CMD0_HASH_STATE)
			eng.bad++;
		for (i = 0; i < 8u; i++)
			put_be(st + 4u * i, p->state[PKTE_STATE_DIGEST + i]);
		br_sha256_init(&c);
		br_sha256_set_state(&c, st, p->state[PKTE_STATE_COUNT]);
		br_sha256_update(&c, p->src, p->len);
		(void)br_sha256_state(&c, st);
		for (i = 0; i < 8u; i++)
			p->state[PKTE_STATE_DIGEST + i] =
				(uint32_t)st[4u * i] << 24 |
				(uint32_t)st[4u * i + 1u] << 16 |
				(uint32_t)st[4u * i + 2u] << 8 | st[4u * i + 3u];
		p->state[PKTE_STATE_COUNT] += p->len;
		if (eng.wrong)
			p->state[PKTE_STATE_DIGEST] ^= 1u;
		if (eng.copies)
			memcpy(p->dst, p->src, p->len);
	} else {
		br_aes_big_cbcdec_keys k;
		uint8_t key[32], iv[16];

		if (p->len % 16u != 0u ||
		    (p->sa[1] & (7u << 24)) != PKTE_CMD1_KEY_256)
			eng.bad++;
		eng.key = false;
		for (i = 0; i < 8u; i++) {
			put_le(key + 4u * i, p->sa[PKTE_SA_KEY + i]);
			eng.key = eng.key || p->sa[PKTE_SA_KEY + i] != 0u;
		}
		for (i = 0; i < 4u; i++)
			put_le(iv + 4u * i, p->state[i]);
		memcpy(p->dst, p->src, p->len);
		br_aes_big_cbcdec_init(&k, key, sizeof(key));
		br_aes_big_cbcdec_run(&k, iv, p->dst, p->len);
		if (eng.wrong)
			((uint8_t *)p->dst)[p->len - 1u] ^= 1u;
		if (eng.ops == eng.flip_at)
			((uint8_t *)p->dst)[eng.flip_end ?
					    p->len - 1u - eng.flip_off :
					    eng.flip_off] ^= 1u;
		status = PKTE_STATUS_PAD;
	}
	p->rdr[0][0] = PKTE_CTL_PERDY | status << 16;
	eng.results = 1;
}

static bool hook(void *ctx, enum fake_mmr_op op, unsigned w, wb_mmr_addr addr,
		 uint32_t *v)
{
	(void)ctx;
	(void)w;
	if (op == FAKE_MMR_READ && addr == PKTE_CLK_CTL)
		*v = eng.present ? 0x17u : 0u;
	if (op == FAKE_MMR_READ && addr == PKTE_RDSC_CNT)
		*v = eng.results;
	if (op == FAKE_MMR_WRITE && addr == PKTE_CFG &&
	    (*v & PKTE_CFG_RSTRING) != 0u)
		eng.results = 0;
	if (op == FAKE_MMR_WRITE && addr == PKTE_CDSC_INCR)
		engine_run();
	return true;
}

static const struct s11_crypto_ops *start(bool present)
{
	const struct s11_crypto_ops *ops;

	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(1000000u);	/* a silent engine times out soon */
	fake_mmr_set_hook(hook, NULL);
	memset(&eng, 0, sizeof(eng));
	eng.present = present;
	memset(&m, 0, sizeof(m));
	m.hw.time.cycles_per_us = 400u;
	ops = opxy_crypto(&m);
	fake_mmr_clear_log();
	return ops;
}

static _Alignas(4) uint8_t data[N + 4u], got[N + 4u], want[N + 4u];
static struct wire_crypto ctx_a, ctx_b;

/* The digest of data[off..off+N) taken in the given pieces by ops is the
 * software's. */
static bool same_digest(const struct s11_crypto_ops *ops, size_t off,
			const size_t *piece, size_t pieces)
{
	uint8_t a[32], b[32];
	size_t at = 0, i;

	ops->sha256_init(&ctx_a);
	for (i = 0; at < N; i++) {
		size_t n = i < pieces ? piece[i] : N - at;

		if (n > N - at)
			n = N - at;
		ops->sha256_update(&ctx_a, data + off + at, n);
		at += n;
		fake_mmr_clear_log();
	}
	ops->sha256_final(&ctx_a, a);
	wire_crypto_ops.sha256_init(&ctx_b);
	wire_crypto_ops.sha256_update(&ctx_b, data + off, N);
	wire_crypto_ops.sha256_final(&ctx_b, b);
	return memcmp(a, b, sizeof(a)) == 0;
}

/* The plaintext of data's first len bytes decrypted in place in pieces by
 * ops, and the IV left, are the software's. */
static bool same_plain(const struct s11_crypto_ops *ops, size_t off,
		       size_t len, size_t piece)
{
	static const uint8_t key[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	uint8_t iv_a[16] = { 9, 8, 7 }, iv_b[16] = { 9, 8, 7 };
	size_t at;

	memcpy(got + off, data, len);
	memcpy(want, data, len);
	for (at = 0; at < len; at += piece) {
		const size_t n = len - at < piece ? len - at : piece;

		ops->aes256_cbc_decrypt(&ctx_a, key, iv_a, got + off + at,
					got + off + at, n);
		fake_mmr_clear_log();
	}
	wire_crypto_ops.aes256_cbc_decrypt(&ctx_b, key, iv_b, want, want, len);
	return memcmp(got + off, want, len) == 0 &&
	       memcmp(iv_a, iv_b, sizeof(iv_a)) == 0;
}

static const size_t check_pieces[] = { 0x100, 0x8000, 0x8000 };
static const size_t odd_pieces[] = { 1000, 5000, 64, 40000 };

static void test_absent(void)
{
	const struct s11_crypto_ops *ops = start(false);

	CHECK(!opxy_crypto_engine() && eng.ops == 0u);
	CHECK(same_digest(ops, 0, check_pieces, ARRAY_LEN(check_pieces)));
	CHECK(same_plain(ops, 0, 0x18000u, 0x8000u));
	CHECK(eng.ops == 0u);
}

static void test_engine(void)
{
	const struct s11_crypto_ops *ops = start(true);
	unsigned before;

	/* the test of each: one hash, one decryption */
	CHECK(opxy_crypto_engine() && eng.ops == 2u && eng.bad == 0u);
	/* the key does not stay in the record the engine fetched */
	for (unsigned i = 0; i < PKTE_SA_WORDS; i++)
		CHECK(opxy_pkte.sa[i] == 0u);

	/* the check's pieces: the name block is the software's, each whole
	 * piece the engine's, and the last, longer than a piece here, two
	 * operations and a part of a block for the software */
	before = eng.ops;
	CHECK(same_digest(ops, 0, check_pieces, ARRAY_LEN(check_pieces)));
	CHECK(eng.ops == before + 4u);
	/* pieces that leave part of a block, and bytes that are not aligned */
	CHECK(same_digest(ops, 0, odd_pieces, ARRAY_LEN(odd_pieces)));
	before = eng.ops;
	CHECK(same_digest(ops, 1, check_pieces, ARRAY_LEN(check_pieces)));
	CHECK(eng.ops == before);

	before = eng.ops;
	CHECK(same_plain(ops, 0, 0x18000u, 0x8000u));
	CHECK(eng.ops == before + 3u);
	CHECK(same_plain(ops, 0, 0x18100u, 0x8000u));	/* a short last piece */
	CHECK(same_plain(ops, 0, 0x18000u - 0x10u, 0x10000u)); /* larger pieces */
	before = eng.ops;
	CHECK(same_plain(ops, 1, 0x10000u, 0x8000u));	/* not aligned */
	CHECK(eng.ops == before);
	CHECK(opxy_crypto_engine() && eng.bad == 0u);
}

/* An operation that fails or never answers part-way through a digest or a
 * decryption: the software takes that piece and the rest. */
static void test_failures(void)
{
	for (unsigned silent = 0; silent < 2u; silent++) {
		for (unsigned k = 3; k <= 5u; k++) {
			const struct s11_crypto_ops *ops = start(true);

			*(silent ? &eng.silent_at : &eng.fail_at) = k;
			CHECK(same_digest(ops, 0, check_pieces,
					  ARRAY_LEN(check_pieces)));
			CHECK(!opxy_crypto_engine() && eng.ops == k);

			ops = start(true);
			*(silent ? &eng.silent_at : &eng.fail_at) = k;
			CHECK(same_plain(ops, 0, 0x18000u, 0x8000u));
			CHECK(!opxy_crypto_engine() && eng.ops == k);
			CHECK(eng.bad == 0u);
		}
	}
}

/* An engine whose answers to the test differ from the software's, or
 * that writes to a hash's destination, is not used. */
static void test_wrong(void)
{
	for (unsigned copies = 0; copies < 2u; copies++) {
		const struct s11_crypto_ops *ops;

		fake_mmr_reset();
		fake_core_reset();
		fake_mmr_set_hook(hook, NULL);
		memset(&eng, 0, sizeof(eng));
		eng.present = true;
		eng.wrong = copies == 0u;
		eng.copies = copies != 0u;
		ops = opxy_crypto(&m);
		CHECK(!opxy_crypto_engine() && eng.ops == 1u);
		eng.wrong = eng.copies = false;
		CHECK(same_digest(ops, 0, check_pieces, ARRAY_LEN(check_pieces)));
		CHECK(eng.ops == 1u);
	}
}

/* A piece's plaintext wrong in its first or its last block, the status
 * good: the software's check of those blocks sees it and redoes the piece.
 * A wrong block between them is not seen here (the zip's CRC-32s see it). */
static void test_wrong_plain(void)
{
	static const struct {
		uint32_t off;
		bool end, seen;
	} flip[] = {
		{ 0, false, true }, { 15, false, true }, { 0, true, true },
		{ 15, true, true }, { 16, false, false }, { 16, true, false },
	};

	for (size_t i = 0; i < ARRAY_LEN(flip); i++) {
		const struct s11_crypto_ops *ops = start(true);

		eng.flip_at = 4;		/* the second piece */
		eng.flip_off = flip[i].off;
		eng.flip_end = flip[i].end;
		CHECK(same_plain(ops, 0, 0x18000u, 0x8000u) == flip[i].seen);
		CHECK(opxy_crypto_engine() == !flip[i].seen);
		CHECK(eng.ops == (flip[i].seen ? 4u : 5u) && eng.bad == 0u);
	}
}

/* The end of the engine's use: a zero key was the last it took, and the
 * registers the driver writes read as reset leaves them. */
static void test_end(void)
{
	const struct s11_crypto_ops *ops = start(true);
	unsigned before;

	CHECK(same_plain(ops, 0, 0x8000u, 0x8000u) && eng.key);
	before = eng.ops;
	opxy_crypto_end();
	CHECK(eng.ops == before + 1u && !eng.key && eng.bad == 0u);
	CHECK(fake_mmr_get(PKTE_CFG) == 0u && fake_mmr_get(PKTE_RING_CFG) == 0u &&
	      fake_mmr_get(PKTE_CDRBASE_ADDR) == 0u &&
	      fake_mmr_get(PKTE_RDRBASE_ADDR) == 0u);
	for (unsigned i = 0; i < PKTE_SA_WORDS; i++)
		CHECK(opxy_pkte.sa[i] == 0u);
	/* the ops are the software's from then on */
	CHECK(!opxy_crypto_engine());
	CHECK(same_plain(ops, 0, 0x8000u, 0x8000u) && eng.ops == before + 1u);

	/* an absent engine is not written */
	ops = start(false);
	(void)ops;
	opxy_crypto_end();
	CHECK(fake_mmr_count() == 0u);
}

/* The driver's own refusals. */
static void test_driver(void)
{
	static struct wb_pkte p;
	uint32_t h[8] = { 0 };
	const uint8_t key[32] = { 0 }, iv[16] = { 0 };

	(void)start(true);
	wb_pkte_init(&p, &m.hw.time);
	CHECK(!wb_pkte_sha256(&p, h, 0, data, 100, got));	/* not blocks */
	CHECK(!wb_pkte_sha256(&p, h, 0, data + 1, 64, got));	/* not aligned */
	CHECK(!wb_pkte_sha256(&p, h, 0, data, 64, got + 1));
	CHECK(!wb_pkte_sha256(&p, h, 0, data, 64, NULL));	/* no destination */
	CHECK(!wb_pkte_sha256(&p, h, 0, data, 0, got));
	CHECK(!wb_pkte_sha256(&p, h, 0, data, WB_PKTE_MAX + 64u, got));
	CHECK(!wb_pkte_aes256_cbc_decrypt(&p, key, iv, data, got, 24));
	CHECK(!wb_pkte_aes256_cbc_decrypt(&p, key, iv, data, got + 2, 32));
	CHECK(eng.ops == 2u);		/* none reached the engine */
}

int main(void)
{
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 31u + (i >> 7) + 5u);
	RUN(test_absent);
	RUN(test_engine);
	RUN(test_failures);
	RUN(test_wrong);
	RUN(test_wrong_plain);
	RUN(test_end);
	RUN(test_driver);
	return check_summary("test_board_crypto");
}
