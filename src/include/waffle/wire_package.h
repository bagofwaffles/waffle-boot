/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_WIRE_PACKAGE_H
#define WAFFLE_WIRE_PACKAGE_H

/*
 * S-11 checks 1-15 on the vendored libraries: struct s11_crypto_ops from
 * BearSSL (SHA-256, AES-256-CBC with aes_big), micro-ecc (P-224) and src/glue/crc
 * (CRC-16/XMODEM), and struct s11_package_ops around them, which recovers
 * the zip (S-11.016) into RAM or in place in the package's own bytes
 * (S-11.018), and checks it with minizip (check 15).
 *
 * Every s11_package_ops function gets the same ctx, so the SHA-256 state,
 * the package's bytes, the unit's OTP reads and the zip's RAM are all in
 * struct wire_package. The OTP is the board's (struct s11_unit_ops with its
 * own context).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bearssl_block.h"
#include "bearssl_hash.h"
#include "unzip_mem.h"

#include "waffle/s11_package.h"

/*
 * The state of wire_crypto_ops. Each function's ctx points to one, or to a
 * struct whose first member is one (struct wire_package).
 */
struct wire_crypto {
	br_sha256_context sha;
	br_aes_big_cbcdec_keys aes;
};

/*
 * CRC-16/XMODEM; SHA-256; AES-256-CBC decryption, `in` and `out` the same
 * buffer or not overlapping; P-224 verification, which also refuses a
 * public key that is not a point of the curve (src/third_party/README.md).
 */
extern const struct s11_crypto_ops wire_crypto_ops;

/*
 * Where the package's bytes are: s11_package_ops.read, and write, which
 * replaces bytes in place (NULL when the source cannot be written).
 */
struct wire_source {
	bool (*read)(void *ctx, uint32_t off, uint8_t *buf, size_t n);
	void *ctx;
	bool (*write)(void *ctx, uint32_t off, const uint8_t *buf, size_t n);
};

/* A package held in memory (a host transfer, S-11.050). */
struct wire_mem {
	const uint8_t *buf;
	size_t len;
};

/* wire_source.read for a struct wire_mem: false past its end. */
bool wire_mem_read(void *ctx, uint32_t off, uint8_t *buf, size_t n);

struct wire_package {
	struct wire_crypto crypto;	/* first: wire_crypto_ops' state */
	struct wire_source src;
	const struct s11_unit_ops *unit;	/* the board's OTP reads, */
	void *unit_ctx;				/* called with this ctx */
	uint8_t *zip;			/* RAM for the recovered zip, or NULL */
	size_t zip_cap;
	size_t zip_len;			/* bytes recovered by the last check */
	uint32_t zip_at;		/* the package offset they came from */
};

/*
 * Sets up p and fills ops to check packages from src. With zip, the
 * recovered zip goes to the zip_cap bytes there, and a zip that does not
 * fit refuses the package (ZIP). With zip NULL, it is written back over the
 * package's own bytes through src->write (in place: S-11.018 bounds no
 * package by the bootloader's memory), and stays at src offset p->zip_at;
 * src->write must not be NULL then. The caller keeps p, src's context,
 * unit and zip alive while ops is used.
 */
void wire_package_init(struct wire_package *p, struct s11_package_ops *ops,
		       const struct wire_source *src,
		       const struct s11_unit_ops *unit, void *unit_ctx,
		       uint8_t *zip, size_t zip_cap);

/* Opens the zip the last check recovered for minizip (wb_unzip_mem.h);
 * NULL when minizip cannot read it as an archive. */
unzFile wire_package_unzip(const struct wire_package *p,
			   struct wb_unzip_mem *m);

#endif /* WAFFLE_WIRE_PACKAGE_H */
