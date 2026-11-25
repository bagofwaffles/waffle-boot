/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/drivers/bf70x/pkte.h"

#include <string.h>

#include "waffle/util/bytes.h"
#include "waffle/drivers/bf70x/cplb.h"
#include "waffle/drivers/mmr.h"

static uint32_t at(const void *p)
{
	return (uint32_t)(uintptr_t)p;
}

static void put(wb_mmr_addr reg, uint32_t v)
{
	wb_mmr_write32(reg, v);
	wb_mmr_sync();
}

void wb_pkte_init(struct wb_pkte *p, const struct wb_time *t)
{
	memset(p, 0, sizeof(*p));
	p->time = t;
}

bool wb_pkte_present(void)
{
	return (wb_mmr_read32(PKTE_CLK_CTL) & PKTE_CLK_CTL_ENPECLK) != 0u;
}

/* The engine out of use, with no key left in the record it fetched. */
static void stop(struct wb_pkte *p)
{
	put(PKTE_CFG, PKTE_CFG_RSTPE | PKTE_CFG_RSTRING);
	memset(p->sa, 0, sizeof(p->sa));
}

/*
 * One operation on the records as written: the engine is reset into ring
 * mode, fetches the one descriptor, and its result is waited for. The
 * status byte, or 0xFF when no result came.
 */
static uint32_t run(struct wb_pkte *p, uint32_t ctl)
{
	const uint64_t end = wb_time_deadline(p->time, WB_PKTE_TIMEOUT_US);
	uint32_t status = 0xFFu;

	memset(p->cdr, 0, sizeof(p->cdr));
	memset(p->rdr, 0, sizeof(p->rdr));
	p->cdr[0][0] = PKTE_CTL_HOSTRDY | ctl;
	p->cdr[0][1] = at(p->src);
	p->cdr[0][2] = at(p->dst);
	p->cdr[0][3] = at(p->sa);
	p->cdr[0][4] = at(p->state);
	p->cdr[0][7] = PKTE_LEN_HSTRDY | p->len;

	put(PKTE_CFG, PKTE_CFG_RSTPE | PKTE_CFG_RSTRING);
	put(PKTE_CFG, PKTE_CFG_MODE_ARM);
	put(PKTE_RING_CFG, 1u);			/* two entries */
	put(PKTE_CDRBASE_ADDR, at(p->cdr));
	put(PKTE_RDRBASE_ADDR, at(p->rdr));
	put(PKTE_CDSC_INCR, 1u);
	while ((wb_mmr_read32(PKTE_RDSC_CNT) & PKTE_RDSC_CNT_MASK) == 0u) {
		if (wb_time_passed(end)) {
			stop(p);
			return status;
		}
	}
	/* what the engine wrote is not what the cache holds */
	wb_dcache_invalidate_range(at(p->rdr), sizeof(p->rdr));
	wb_dcache_invalidate_range(at(p->state), sizeof(p->state));
	wb_dcache_invalidate_range(at(p->dst), p->len);
	if ((p->rdr[0][0] & PKTE_CTL_PERDY) != 0u)
		status = PKTE_CTL_STATUS(p->rdr[0][0]);
	stop(p);
	return status;
}

static bool usable(const void *a, const void *b, uint32_t len, uint32_t block)
{
	return ((at(a) | at(b)) & 3u) == 0u && len != 0u &&
	       len <= WB_PKTE_MAX && len % block == 0u;
}

bool wb_pkte_sha256(struct wb_pkte *p, uint32_t h[8], uint64_t total,
		    const void *data, uint32_t len, void *spare)
{
	unsigned i;

	if (spare == NULL || !usable(data, spare, len, 64u))
		return false;
	memset(p->sa, 0, sizeof(p->sa));
	p->sa[0] = PKTE_CMD0_HASH | PKTE_CMD0_NO_CIPHER | PKTE_CMD0_SHA256 |
		   PKTE_CMD0_DIGEST_8 | PKTE_CMD0_HASH_STATE |
		   PKTE_CMD0_SAVE_HASH;
	memset(p->state, 0, sizeof(p->state));
	p->state[PKTE_STATE_COUNT] = (uint32_t)total;
	p->state[PKTE_STATE_COUNT + 1u] = (uint32_t)(total >> 32);
	for (i = 0; i < 8u; i++)
		p->state[PKTE_STATE_DIGEST + i] = h[i];
	p->src = data;
	p->dst = spare;
	p->len = len;
	if (run(p, 0u) != 0u)		/* not final: no padding */
		return false;
	for (i = 0; i < 8u; i++)
		h[i] = p->state[PKTE_STATE_DIGEST + i];
	return true;
}

bool wb_pkte_aes256_cbc_decrypt(struct wb_pkte *p, const uint8_t key[32],
				const uint8_t iv[16], const void *in,
				void *out, uint32_t len)
{
	unsigned i;

	if (!usable(in, out, len, 16u))
		return false;
	memset(p->sa, 0, sizeof(p->sa));
	p->sa[0] = PKTE_CMD0_DECRYPT | PKTE_CMD0_INBOUND | PKTE_CMD0_AES |
		   PKTE_CMD0_NO_HASH | PKTE_CMD0_IV_STATE | PKTE_CMD0_SAVE_IV;
	p->sa[1] = PKTE_CMD1_KEY_256 | PKTE_CMD1_CBC | PKTE_CMD1_COPY_PAYLOAD;
	for (i = 0; i < 8u; i++)
		p->sa[PKTE_SA_KEY + i] = wb_get_le32(key + 4u * i);
	memset(p->state, 0, sizeof(p->state));
	for (i = 0; i < 4u; i++)
		p->state[i] = wb_get_le32(iv + 4u * i);
	p->src = in;
	p->dst = out;
	p->len = len;
	/* The pad error is every decryption's: there is no pad. */
	return (run(p, 0u) & ~PKTE_STATUS_PAD) == 0u;
}

void wb_pkte_end(struct wb_pkte *p)
{
	static const uint8_t zero[32];

	memset(p->block, 0, sizeof(p->block));
	(void)wb_pkte_aes256_cbc_decrypt(p, zero, zero, p->block[0],
					 p->block[1], sizeof(p->block[0]));
	put(PKTE_CFG, PKTE_CFG_RSTPE | PKTE_CFG_RSTRING);
	put(PKTE_CFG, 0u);
	put(PKTE_RING_CFG, 0u);
	put(PKTE_CDRBASE_ADDR, 0u);
	put(PKTE_RDRBASE_ADDR, 0u);
}
