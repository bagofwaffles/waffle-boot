/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/install/package.h"

#include "waffle/util/bytes.h"
#include "waffle/boot/device.h"
#include "waffle/flash/layout.h"
#include "waffle/install/engine.h"

#define CHUNK 0x8000u	/* a multiple of the AES block */

#define PREAMBLE_MAGIC 0xBABECAFEu
#define HEADER_MAGIC 0xBEEFCAFEu

static struct s11_version be_version(const uint8_t *p)
{
	struct s11_version v = { wb_get_be16(p), wb_get_be16(p + 2), wb_get_be16(p + 4),
				 wb_get_be16(p + 6) };

	return v;
}

static void wipe(uint8_t *p, size_t n)
{
	volatile uint8_t *v = p;

	while (n-- != 0)
		*v++ = 0;
}

bool s11_name_forces(const char *name)
{
	for (; name[0] != '\0'; name++) {
		if (name[0] == 'f' && name[1] == 'o' && name[2] == 'r' &&
		    name[3] == 'c' && name[4] == 'e')
			return true;
	}
	return false;
}

/* S-11.021: a blank signature is none; a present one is valid only when it
 * verifies. */
enum s11_sig s11_signature_state(bool present, bool verified)
{
	if (!present)
		return S11_SIG_NONE;
	return verified ? S11_SIG_VALID : S11_SIG_INVALID;
}

/* S-11.023 */
enum s11_reason s11_payload_key_rule(uint8_t index, bool k0_ok, bool k1_ok,
				     bool option)
{
	switch (index) {
	case 0x00:
		return k0_ok ? S11_REASON_NONE : S11_REASON_NOKEY;
	case 0x01:
		if (!option)
			return S11_REASON_KEY;
		return k1_ok ? S11_REASON_NONE : S11_REASON_NOKEY;
	case 0xFE:
		return S11_REASON_NONE;
	default:		/* 0xFF included: no fixed key (S-11.023) */
		return S11_REASON_KEY;
	}
}

/* S-11.091: only a valid package; a waffle-boot bootloader is an ordinary
 * update. */
bool s11_return_to_stock(bool option, bool valid, bool bootloader_waffle,
			 bool bootloader_selected)
{
	return option && valid && !bootloader_waffle && bootloader_selected;
}

/* S-11.012: SHA-256 of the manifest with +0x010-+0x04F zeroed, then the
 * verification with r and s from their last 28 bytes. */
static bool signature_ok(const struct s11_package_ops *ops,
			 const uint8_t *man)
{
	static const uint8_t zeros[0x10];
	uint8_t q[56], digest[32];

	if (!ops->unit->read_q0(ops->ctx, q))
		return false;
	ops->crypto->sha256_init(ops->ctx);
	ops->crypto->sha256_update(ops->ctx, man, 0x10);
	for (unsigned i = 0; i < 4u; i++)		/* +0x010-+0x04F */
		ops->crypto->sha256_update(ops->ctx, zeros, sizeof(zeros));
	ops->crypto->sha256_update(ops->ctx, man + 0x50, S11_MANIFEST_LEN - 0x50);
	ops->crypto->sha256_final(ops->ctx, digest);
	return ops->crypto->p224_verify(ops->ctx, q, digest, man + 0x14,
					man + 0x34);
}

/* S-11.012: present unless sig_r (+0x10) and sig_s (+0x30) are all zero. */
static bool signature_present(const uint8_t *man)
{
	for (unsigned i = 0x10; i < 0x50; i++) {
		if (man[i] != 0u)
			return true;
	}
	return false;
}

/* Check 8 (S-11.021, S-11.022): a plain package has no signature; the key
 * index selects Q0 alone. */
static enum s11_sig signature(const struct s11_package_ops *ops,
			      const uint8_t *man, bool plain)
{
	const bool present = !plain && signature_present(man);

	return s11_signature_state(present, present && man[0x89] == 0x00 &&
					    signature_ok(ops, man));
}

struct mem_image {
	const uint8_t *p;
	uint32_t len;
};

static int mem_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	const struct mem_image *m = ctx;

	if (off > m->len || len > m->len - off)
		return -1;
	for (uint32_t i = 0; i < len; i++)
		buf[i] = m->p[off + i];
	return 0;
}

bool s11_bootloader_is_waffle(const uint8_t *img, uint32_t len,
			      uint32_t (*crc32)(uint32_t crc, const uint8_t *p,
						size_t n))
{
	struct mem_image m = { img, len };

	return img != NULL && s02_stream_is_waffle(mem_read, &m, 0u, len, crc32);
}

static bool same(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t d = 0;

	for (size_t i = 0; i < n; i++)
		d = (uint8_t)(d | (a[i] ^ b[i]));
	return d == 0;
}

/* S-11.017 */
static bool name_ok(const uint8_t *block, struct s11_check_out *out)
{
	size_t n = 0;

	while (n < S11_NAME_MAX && block[n] != 0)
		n++;
	out->name_len = (uint8_t)n;
	for (size_t i = 0; i < n; i++)
		out->name[i] = (char)block[i];
	for (size_t i = 0; i + 4 <= n; i++) {
		if (block[i] == '.' && block[i + 1] == 'z' &&
		    block[i + 2] == 'i' && block[i + 3] == 'p')
			return true;
	}
	return false;
}

/*
 * The pass: every byte after the TE header is read once, in order, in
 * pieces that start CHUNK-aligned to `zip` (the payload's offset 0x100), so
 * no failed read holds bytes of both the zip and what comes before it, and
 * the reads make one count over the package (S-16.038).
 */
struct pass {
	const struct s11_package_ops *ops;
	uint32_t zip;			/* the zip's package offset */
	uint16_t crc;			/* check 6 */
	bool sha;			/* check 11 is reached */
	/* check 15's recovery: decrypt (key) and write out `padded` bytes
	 * from `zip`, `zip_len` of them kept (S-11.016, S-11.018) */
	bool recover;
	bool wrote;			/* every zip_write so far succeeded */
	const uint8_t *key;		/* NULL: plaintext */
	uint8_t iv[16];
	uint32_t zip_len, padded;
};

/* Word-aligned: a device may move a sector's bytes by words. */
static _Alignas(4) uint8_t chunk[CHUNK];

/* S-11.020: a read error refuses ZIP in the zip's bytes, SHORT before
 * them. */
static enum s11_reason read_error(uint32_t off, uint32_t zip)
{
	return off >= zip ? S11_REASON_ZIP : S11_REASON_SHORT;
}

/* Checks 6 and 11 over the bytes as read, then check 15's recovery of the
 * zip's: the CRC and the digest cover the ciphertext. A zip write that
 * fails stops the writes; check 15 reports it. */
static void take(struct pass *p, uint32_t off, uint8_t *buf, uint32_t n)
{
	const struct s11_crypto_ops *c = p->ops->crypto;
	uint32_t z, d, keep;

	p->crc = c->crc16(p->ops->ctx, p->crc, buf, n);
	if (p->sha)
		c->sha256_update(p->ops->ctx, buf, n);
	if (!p->recover || !p->wrote || off - p->zip >= p->padded)
		return;
	z = off - p->zip;
	d = p->padded - z < n ? p->padded - z : n;
	keep = p->zip_len - z < d ? p->zip_len - z : d;
	if (p->key != NULL)
		c->aes256_cbc_decrypt(p->ops->ctx, p->key, p->iv, buf, buf, d);
	p->wrote = p->ops->zip_write(p->ops->ctx, z, off, buf, keep);
}

/* The payload's bytes [from, end) through take(); a read error's reason,
 * or none. */
static enum s11_reason read_range(struct pass *p, uint32_t from, uint32_t end)
{
	while (from < end) {
		const uint32_t n = end - from < CHUNK ? end - from : CHUNK;

		if (!p->ops->read(p->ops->ctx, from, chunk, n))
			return read_error(from, p->zip);
		take(p, from, chunk, n);
		from += n;
	}
	return S11_REASON_NONE;
}

/*
 * S-11.020 in one pass. Checks 6 and 11 need every byte and check 15 the
 * zip's, so the manifest and the name block are read first and checks 7-14
 * other than 11 are decided on them; when those pass, the zip is decrypted
 * and written out in the pass that computes the CRC and the digest. The
 * reason is still the first failing check's, in the table's order, so a
 * package refused CRC or HASH may have had its zip written out: the caller
 * discards it as it does any refused package's. Bytes that cannot be read
 * refuse the package at once, as check 6 reads every byte first: ZIP when
 * they are the zip's, SHORT otherwise.
 */
static enum s11_reason check(const struct s11_package_ops *ops,
			     const struct s11_check_in *in,
			     struct s11_check_out *out, uint8_t *key,
			     uint8_t *man)
{
	uint8_t hdr[S11_HEADER_LEN];
	uint8_t cipher[S11_NAME_BLOCK_LEN];
	uint8_t digest[32];
	uint32_t o = 0, m, payload = 0, n;
	bool encrypted = false, plain = false;
	struct s11_check_out named = { S11_SIG_NONE, false, 0, 0, { 0 } };
	struct pass p = { .ops = ops };
	struct s11_version v;
	enum s11_reason r, early = S11_REASON_NONE;
	unsigned early_at = 0;		/* the first of checks 7-14 to fail */

	if (in->form == S11_FORM_FILE) {
		/* checks 1, 2 (S-11.002, S-11.003) */
		if (in->size < S11_PREAMBLE_LEN ||
		    !ops->read(ops->ctx, 0, hdr, S11_PREAMBLE_LEN))
			return S11_REASON_SHORT;
		if (wb_get_be32(hdr) != PREAMBLE_MAGIC)
			return S11_REASON_PREAMBLE;
		if (hdr[4] != 0x00)
			return S11_REASON_TYPE;
		v = be_version(hdr + 0x07);
		if (s11_version_is_forced_marker(&v))
			out->forced = true;		/* S-11.055 */
		if (in->file_name != NULL && s11_name_forces(in->file_name))
			out->forced = true;		/* S-11.047 */
		o = S11_PREAMBLE_LEN;
	} else if (in->fw_command) {
		/* S-11.050, S-11.055, S-10c.075: on CDC or the MIDI carrier */
		out->forced = true;
	}

	/* checks 3-5 (S-11.004-S-11.007) */
	if (in->size - o < S11_HEADER_LEN ||
	    !ops->read(ops->ctx, o, hdr, S11_HEADER_LEN))
		return S11_REASON_SHORT;
	if (wb_get_be32(hdr) != HEADER_MAGIC)
		return S11_REASON_MAGIC;
	if (!s11_product_ok(in->dev, wb_get_be32(hdr + 0x17)))
		return S11_REASON_PRODUCT;
	/* S-11.006; S-11.026: on the OP-1F 0x01 alone */
	if (hdr[4] != 0x01 && (hdr[4] != 0x06 || in->dev->id == WB_DEV_OP1F))
		return S11_REASON_DEVICE;
	/* S-11.009: the length and address fields, the reserved bytes and the
	 * preamble's crc16 are never read */
	v = be_version(hdr + 0x0B);
	if (hdr[4] == 0x06 || s11_version_is_forced_marker(&v))
		out->forced = true;			/* S-11.005 */

	m = o + S11_HEADER_LEN;
	p.zip = m + S11_MANIFEST_LEN + S11_NAME_BLOCK_LEN;

	/* The manifest: checks 7 and 10 (S-11.010, S-11.011, S-11.014);
	 * check 8 follows check 6, and check 9 is not made: no manifest
	 * product word is read (S-11.013 withdrawn). S-11.027: the tag tells
	 * the format; on the OP-1F a block without TESG is the plain format's
	 * crypto header, which has no fields for checks 8, 10 and 11; its zip
	 * length is tested by check 14. A TESG block's version must be
	 * 0x01. */
	n = in->size - m < S11_MANIFEST_LEN ? in->size - m : S11_MANIFEST_LEN;
	if (n != 0 && !ops->read(ops->ctx, m, man, n))
		return S11_REASON_SHORT;
	p.crc = ops->crypto->crc16(ops->ctx, 0, man, n);
	if (n < S11_MANIFEST_LEN) {
		early = S11_REASON_SHORT;
		early_at = 7;
	} else {
		plain = man[0x84] != 'T' || man[0x85] != 'E' ||
			man[0x86] != 'S' || man[0x87] != 'G';
		if (plain ? in->dev->id != WB_DEV_OP1F : man[0x88] != 0x01) {
			early = S11_REASON_FORMAT;
			early_at = 7;
		}
	}
	m += S11_MANIFEST_LEN;			/* the payload */
	if (early_at == 0)
		payload = in->size - m;
	if (early_at == 0 && !plain && wb_get_le32(man + 0x98) != payload) {
		early = S11_REASON_LENGTH;		/* check 10 */
		early_at = 10;
	}

	/* The name block, the first bytes of check 11's digest */
	n = 0;
	if (in->size > m)
		n = in->size - m < S11_NAME_BLOCK_LEN ? in->size - m :
							S11_NAME_BLOCK_LEN;
	if (n != 0 && !ops->read(ops->ctx, m, cipher, n))
		return S11_REASON_SHORT;
	p.crc = ops->crypto->crc16(ops->ctx, p.crc, cipher, n);
	p.sha = early_at == 0 && !plain;
	if (p.sha) {
		ops->crypto->sha256_init(ops->ctx);
		ops->crypto->sha256_update(ops->ctx, cipher, n);
	}

	/* check 12 (S-11.015, S-11.023) */
	if (early_at == 0) {
		bool k0 = false, k1 = false;

		if (man[0x04] == 0x00)
			k0 = ops->unit->read_k0(ops->ctx, key);
		else if (man[0x04] == 0x01 && in->option)
			k1 = ops->unit->read_k1(ops->ctx, key);
		early = s11_payload_key_rule(man[0x04], k0, k1, in->option);
		if (early != S11_REASON_NONE)
			early_at = 12;
		encrypted = man[0x04] != 0xFE;
	}

	/* check 13 (S-11.017) */
	if (early_at == 0) {
		uint8_t block[S11_NAME_BLOCK_LEN];
		const uint8_t *name = cipher;	/* plaintext: the bytes as read */

		for (unsigned i = 0; i < 16; i++)
			p.iv[i] = man[0x70 + i];
		if (encrypted && payload >= S11_NAME_BLOCK_LEN) {
			ops->crypto->aes256_cbc_decrypt(ops->ctx, key, p.iv,
							cipher, block,
							S11_NAME_BLOCK_LEN);
			name = block;
		}
		if (payload < S11_NAME_BLOCK_LEN || !name_ok(name, &named)) {
			early = S11_REASON_NAME;
			early_at = 13;
		}
	}

	/* check 14 (S-11.018): the zip, rounded up to 16 when encrypted */
	if (early_at == 0) {
		uint64_t padded;

		p.zip_len = wb_get_le32(man + 0x80);
		padded = encrypted ? ((uint64_t)p.zip_len + 15u) / 16u * 16u :
				     p.zip_len;
		if ((uint64_t)payload < S11_NAME_BLOCK_LEN + padded) {
			early = S11_REASON_ZIPLEN;
			early_at = 14;
		} else {
			p.padded = (uint32_t)padded;
			p.recover = p.wrote = true;
			p.key = encrypted ? key : NULL;
		}
	}

	/* The rest: checks 6 and 11 to the end, check 15's recovery. The
	 * digest is taken now: check 8's own digest reuses the SHA-256. */
	r = read_range(&p, p.zip, in->size);
	if (r != S11_REASON_NONE)
		return r;
	if (p.sha)
		ops->crypto->sha256_final(ops->ctx, digest);

	if (p.crc != wb_get_be16(hdr + 0x09)) {		/* check 6 */
		if (s11_crc_engine != NULL)
			s11_crc_engine->crc16_refused();
		return S11_REASON_CRC;
	}
	if (early_at == 7)
		return early;
	/* check 8 (S-11.012, S-11.021, S-11.022): it refuses nothing */
	out->sig = signature(ops, man, plain);
	if (early_at == 10)
		return early;
	/* check 11: an integrity check only (S-11.014) */
	if (!plain && !same(digest, man + 0x9C, 32))
		return S11_REASON_HASH;
	if (early_at == 12)
		return early;
	out->name_len = named.name_len;
	for (unsigned i = 0; i < S11_NAME_MAX; i++)
		out->name[i] = named.name[i];
	if (early_at != 0)
		return early;				/* checks 13, 14 */
	/* check 15 */
	if (!p.wrote)
		return S11_REASON_ZIP;
	out->zip_len = p.zip_len;
	if (!ops->zip_readable(ops->ctx, p.zip_len))
		return S11_REASON_ZIP;
	return S11_REASON_NONE;
}

/* S-11.020. S-11.024: the checks keep no state, so a refused package has
 * no effect on the next. S-11.048: the zip written out is the one just
 * checked; nothing else is installed. */
enum s11_reason s11_check_package(const struct s11_package_ops *ops,
				  const struct s11_check_in *in,
				  struct s11_check_out *out)
{
	uint8_t key[32], man[S11_MANIFEST_LEN];
	enum s11_reason r;

	out->sig = S11_SIG_NONE;
	out->forced = false;
	out->zip_len = 0;
	out->name_len = 0;
	r = check(ops, in, out, key, man);
	wipe(key, sizeof(key));
	return r;
}
