/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "uECC.h"
#include "unzip_mem.h"

#include "waffle/util/crc.h"
#include "waffle/install/crypto.h"

/* ---- crypto ---- */

static void sha_init(void *ctx)
{
	br_sha256_init(&((struct wire_crypto *)ctx)->sha);
}

static void sha_update(void *ctx, const uint8_t *p, size_t n)
{
	br_sha256_update(&((struct wire_crypto *)ctx)->sha, p, n);
}

static void sha_final(void *ctx, uint8_t digest[32])
{
	br_sha256_out(&((struct wire_crypto *)ctx)->sha, digest);
}

/* aes_big_cbcdec decrypts in place and leaves iv holding the last
 * ciphertext block, as s11_crypto_ops asks. */
static void aes_decrypt(void *ctx, const uint8_t key[32], uint8_t iv[16],
			const uint8_t *in, uint8_t *out, size_t n)
{
	struct wire_crypto *c = ctx;

	if (out != in)
		memmove(out, in, n);
	br_aes_big_cbcdec_init(&c->aes, key, 32);
	br_aes_big_cbcdec_run(&c->aes, iv, out, n);
}

/* uECC_verify() checks 0 < r, s < n and takes the digest's leftmost 224
 * bits (S-11.012); it does not check the key. */
static bool p224_verify(void *ctx, const uint8_t q[56],
			const uint8_t digest[32], const uint8_t r[28],
			const uint8_t s[28])
{
	uint8_t sig[56];

	(void)ctx;
	if (!uECC_valid_public_key(q, uECC_secp224r1()))
		return false;
	memcpy(sig, r, 28);
	memcpy(sig + 28, s, 28);
	return uECC_verify(q, digest, 32, sig, uECC_secp224r1()) != 0;
}

const struct s11_crypto_ops wire_crypto_ops = {
	wire_s11_crc16, sha_init, sha_update, sha_final, aes_decrypt,
	p224_verify,
};

/* ---- the package ---- */

/* cppcheck-suppress constParameterPointer ; wire_source fixes the type */
bool wire_mem_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	const struct wire_mem *m = ctx;

	if (off > m->len || n > m->len - off)
		return false;
	memcpy(buf, m->buf + off, n);
	return true;
}

/* cppcheck-suppress constParameterCallback ; s11_unit_ops fixes the type */
static bool read_q0(void *ctx, uint8_t q[56])
{
	const struct wire_package *p = ctx;

	return p->unit->read_q0(p->unit_ctx, q);
}

/* cppcheck-suppress constParameterCallback ; s11_unit_ops fixes the type */
static bool read_k0(void *ctx, uint8_t key[32])
{
	const struct wire_package *p = ctx;

	return p->unit->read_k0(p->unit_ctx, key);
}

/* cppcheck-suppress constParameterCallback ; s11_unit_ops fixes the type */
static bool read_k1(void *ctx, uint8_t key[32])
{
	const struct wire_package *p = ctx;

	return p->unit->read_k1(p->unit_ctx, key);
}

static const struct s11_unit_ops unit_ops = { read_q0, read_k0, read_k1 };

/* cppcheck-suppress constParameterCallback ; s11_package_ops fixes the type */
static bool pkg_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	const struct wire_package *p = ctx;

	return p->src.read(p->src.ctx, off, buf, n);
}

/*
 * The zip arrives in order from offset 0; offset 0 starts a new one. In
 * place, each piece goes back where it was read from: s11_check_package
 * reads ahead of what it writes, so a CBC chain still sees its ciphertext
 * (S-11.016).
 */
static bool zip_write(void *ctx, uint32_t off, uint32_t at, const uint8_t *data,
		      size_t n)
{
	struct wire_package *p = ctx;

	if (off == 0) {
		p->zip_len = 0;
		p->zip_at = at;
	}
	if (off != p->zip_len)
		return false;
	if (p->zip == NULL) {
		if (at != p->zip_at + off || p->src.write == NULL ||
		    (n != 0 && !p->src.write(p->src.ctx, at, data, n)))
			return false;
	} else {
		if (n > p->zip_cap - p->zip_len)
			return false;
		if (n != 0)
			memcpy(p->zip + off, data, n);
	}
	p->zip_len += n;
	return true;
}

unzFile wire_package_unzip(const struct wire_package *p,
			   struct wb_unzip_mem *m)
{
	if (p->zip != NULL)
		return wb_unzip_open_mem(m, p->zip, p->zip_len);
	return wb_unzip_open_read(m, p->src.read, p->src.ctx, p->zip_at,
				  p->zip_len);
}

/* Check 15: minizip finds the end record and reads the whole central
 * directory. */
/* cppcheck-suppress constParameterCallback ; s11_package_ops fixes the type */
static bool zip_readable(void *ctx, uint32_t len)
{
	const struct wire_package *p = ctx;
	struct wb_unzip_mem mem;
	unzFile uf;
	int ret;

	if (len != p->zip_len)
		return false;
	uf = wire_package_unzip(p, &mem);
	if (uf == NULL)
		return false;
	for (ret = unzGoToFirstFile(uf); ret == UNZ_OK; ret = unzGoToNextFile(uf))
		;
	return unzClose(uf) == UNZ_OK && ret == UNZ_END_OF_LIST_OF_FILE;
}

void wire_package_init(struct wire_package *p, struct s11_package_ops *ops,
		       const struct wire_source *src,
		       const struct s11_unit_ops *unit, void *unit_ctx,
		       uint8_t *zip, size_t zip_cap)
{
	memset(p, 0, sizeof(*p));
	p->src = *src;
	p->unit = unit;
	p->unit_ctx = unit_ctx;
	p->zip = zip;
	p->zip_cap = zip_cap;
	ops->crypto = &wire_crypto_ops;
	ops->unit = &unit_ops;
	ops->read = pkg_read;
	ops->zip_write = zip_write;
	ops->zip_readable = zip_readable;
	ops->ctx = p;
}
