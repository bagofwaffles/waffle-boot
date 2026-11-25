/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_PKTE_H
#define WAFFLE_DRIVERS_BF70X_PKTE_H

/*
 * The Security Packet Engine (ADSP-BF70x Hardware Reference, chapter 15) in
 * autonomous ring mode, one operation at a time, polled: SHA-256 over whole
 * blocks and AES-256-CBC decryption. The engine reads and writes memory by
 * itself, at about 42 MiB/s for the cipher and 150 MiB/s for the hash with
 * SCLK1 at 200 MHz, whatever an operation's size.
 *
 * What a unit shows beyond the manual: the state record's IV is not
 * written back after a decryption, so every operation is given its own; the
 * digest words are H0 to H7 as numbers; and a decryption ends with the pad
 * error bit set, having no pad to check, with its output whole.
 *
 * Data cache: the caller's bytes must be in memory, as they are on a
 * write-through page; the driver invalidates an operation's whole
 * destination before it or the caller reads it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x/time.h"

#define PKTE_BASE          0x200E0000u
#define PKTE_CDRBASE_ADDR  (PKTE_BASE + 0x080u)
#define PKTE_RDRBASE_ADDR  (PKTE_BASE + 0x084u)
#define PKTE_RING_CFG      (PKTE_BASE + 0x088u)
#define PKTE_CDSC_INCR     (PKTE_BASE + 0x090u)
#define PKTE_RDSC_CNT      (PKTE_BASE + 0x094u)
#define PKTE_CFG           (PKTE_BASE + 0x100u)
#define PKTE_CLK_CTL       (PKTE_BASE + 0x1E8u)

#define PKTE_CFG_RSTPE     (1u << 0)
#define PKTE_CFG_RSTRING   (1u << 1)
#define PKTE_CFG_MODE_ARM  (3u << 8)
#define PKTE_CLK_CTL_ENPECLK (1u << 0)
#define PKTE_RDSC_CNT_MASK 0x7FFu

/* A descriptor's first and last words (PKTE_CTL_STAT, PKTE_LEN). */
#define PKTE_CTL_HOSTRDY   (1u << 0)
#define PKTE_CTL_PERDY     (1u << 1)
#define PKTE_CTL_STATUS(w) ((w) >> 16 & 0xFFu)
#define PKTE_STATUS_PAD    0x02u
#define PKTE_LEN_HSTRDY    (1u << 22)

/* PKTE_SA_CMD0, PKTE_SA_CMD1 */
#define PKTE_CMD0_DECRYPT  0u			/* OPCD: basic cipher */
#define PKTE_CMD0_HASH     3u			/* OPCD: basic hash */
#define PKTE_CMD0_INBOUND  (1u << 3)
#define PKTE_CMD0_AES      (3u << 8)
#define PKTE_CMD0_NO_CIPHER (15u << 8)
#define PKTE_CMD0_SHA256   (3u << 12)
#define PKTE_CMD0_NO_HASH  (15u << 12)
#define PKTE_CMD0_DIGEST_8 (8u << 20)
#define PKTE_CMD0_IV_STATE (2u << 24)
#define PKTE_CMD0_HASH_STATE (2u << 26)
#define PKTE_CMD0_SAVE_IV  (1u << 28)
#define PKTE_CMD0_SAVE_HASH (1u << 29)
#define PKTE_CMD1_COPY_PAYLOAD (1u << 2)
#define PKTE_CMD1_CBC      (1u << 8)
#define PKTE_CMD1_KEY_256  (4u << 24)

/* The records' words (tables 15-17, 15-19, 15-20). */
#define PKTE_DESC_WORDS    8u
#define PKTE_SA_WORDS      32u
#define PKTE_SA_KEY        2u
#define PKTE_STATE_WORDS   14u
#define PKTE_STATE_COUNT   4u
#define PKTE_STATE_DIGEST  6u

/* An operation's most bytes: PKTE_LEN holds 20 bits, and these are whole
 * SHA-256 and AES blocks. */
#define WB_PKTE_MAX        0xFFFC0u
#define WB_PKTE_TIMEOUT_US 200000u

struct wb_pkte {
	const struct wb_time *time;
	/* What the engine fetches and writes back: a ring of two entries,
	 * one used. */
	uint32_t cdr[2][PKTE_DESC_WORDS], rdr[2][PKTE_DESC_WORDS];
	uint32_t sa[PKTE_SA_WORDS], state[PKTE_STATE_WORDS];
	/* wb_pkte_end()'s block. */
	uint32_t block[2][4];
	/* The operation's memory: len bytes at each. */
	const void *src;
	void *dst;
	uint32_t len;
};

void wb_pkte_init(struct wb_pkte *p, const struct wb_time *t);

/* Whether the engine's clock is on, as it is from reset. */
bool wb_pkte_present(void);

/*
 * SHA-256's state h (H0 to H7) taken over len more bytes at data: whole
 * blocks, at most WB_PKTE_MAX, data 4-byte aligned; total is the bytes
 * hashed before. False, with h as it was, when the engine fails.
 *
 * spare is len bytes of the caller's, 4-byte aligned: every operation has
 * a destination, and this is the hash's. A unit's engine writes nothing
 * there; the caller can see whether it did.
 */
bool wb_pkte_sha256(struct wb_pkte *p, uint32_t h[8], uint64_t total,
		    const void *data, uint32_t len, void *spare);

/*
 * AES-256-CBC decryption of len bytes (whole blocks, at most WB_PKTE_MAX)
 * from in to out, both 4-byte aligned and not overlapping. False when the
 * engine fails; out may then hold anything.
 */
bool wb_pkte_aes256_cbc_decrypt(struct wb_pkte *p, const uint8_t key[32],
				const uint8_t iv[16], const void *in,
				void *out, uint32_t len);

/*
 * The engine as reset leaves it and holding no key of the caller's: one
 * block decrypted under a zero key, then the reset state of the registers
 * this driver writes.
 */
void wb_pkte_end(struct wb_pkte *p);

#endif /* WAFFLE_DRIVERS_BF70X_PKTE_H */
