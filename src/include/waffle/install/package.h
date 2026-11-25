/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_INSTALL_PACKAGE_H
#define WAFFLE_INSTALL_PACKAGE_H

/*
 * S-11 sections 1-2: the firmware package container and checks 1-15 of
 * S-11.020. The cryptography (CRC-16, SHA-256, AES-256-CBC, ECDSA P-224,
 * the vendored libraries), the unit's OTP keys and the bytes of the package
 * come in through struct s11_package_ops; the recovered zip goes out
 * through it. The signature is checked but never refuses a package
 * (S-11.012, S-11.022): it sets the signature state (S-11.021).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/install/common.h"
#include "waffle/util/version.h"

#define S11_PREAMBLE_LEN 0x40u
#define S11_HEADER_LEN 0x40u
#define S11_MANIFEST_LEN 0x300u
#define S11_NAME_BLOCK_LEN 0x100u
#define S11_NAME_MAX 0x7Cu

struct s11_crypto_ops {
	/* CRC-16/XMODEM (S-11.008): continues `crc` over the bytes. */
	uint16_t (*crc16)(void *ctx, uint16_t crc, const uint8_t *p, size_t n);
	/* SHA-256, one digest at a time. */
	void (*sha256_init)(void *ctx);
	void (*sha256_update)(void *ctx, const uint8_t *p, size_t n);
	void (*sha256_final)(void *ctx, uint8_t digest[32]);
	/* AES-256-CBC decryption of `n` bytes (a multiple of 16); `iv` is
	 * left holding the last ciphertext block, so calls chain. */
	void (*aes256_cbc_decrypt)(void *ctx, const uint8_t key[32],
				   uint8_t iv[16], const uint8_t *in,
				   uint8_t *out, size_t n);
	/* ECDSA P-224 verification (FIPS 186-4): `q` is the public point x
	 * then y, 28 bytes each, big-endian; e is the digest's leftmost 224
	 * bits; r and s are 28-byte big-endian integers. Must check
	 * 0 < r, s < n. */
	bool (*p224_verify)(void *ctx, const uint8_t q[56],
			    const uint8_t digest[32], const uint8_t r[28],
			    const uint8_t s[28]);
};

struct s11_unit_ops {
	/* The unit's OTP reads (S-16.095); false when an item cannot be
	 * read. */
	bool (*read_q0)(void *ctx, uint8_t q[56]);
	bool (*read_k0)(void *ctx, uint8_t key[32]);
	bool (*read_k1)(void *ctx, uint8_t key[32]);
};

struct s11_package_ops {
	const struct s11_crypto_ops *crypto;
	const struct s11_unit_ops *unit;
	/* `n` bytes of the package from offset `off`; false on a read error. */
	bool (*read)(void *ctx, uint32_t off, uint8_t *buf, size_t n);
	/* The recovered zip's plaintext, in order from offset 0; `at` is the
	 * package offset these bytes were recovered from (the zip's offset
	 * `off` is package offset `at`), and has just been read. It comes in
	 * the pass that computes checks 6 and 11, before their verdicts. */
	bool (*zip_write)(void *ctx, uint32_t off, uint32_t at, const uint8_t *p,
			  size_t n);
	/* Check 15: the zip of `len` bytes can be read (the vendored zip reader). */
	bool (*zip_readable)(void *ctx, uint32_t len);
	void *ctx;
};

/* S-11.001 */
enum s11_form {
	S11_FORM_FILE,		/* preamble and body: the RAM disk (S-11.046) */
	S11_FORM_BODY,		/* body only: the host links (S-11.050) */
};

struct wb_device;

struct s11_check_in {
	enum s11_form form;
	uint32_t size;			/* the package's length in bytes */
	bool option;			/* the session upgrade option (S-11.090) */
	const char *file_name;		/* the RAM-disk file's name, or NULL */
	/* came over the firmware command 0D, on CDC or on the MIDI carrier
	 * (S-10c.075): forced. The MIDI firmware transfer is not. */
	bool fw_command;
	/* the device the package must be made for (S-11.007, S-11.026) */
	const struct wb_device *dev;
};

struct s11_check_out {
	enum s11_sig sig;		/* S-11.021 (NONE until check 8) */
	bool forced;			/* S-11.055 (markers seen so far) */
	uint32_t zip_len;		/* the recovered zip's length */
	uint8_t name_len;
	char name[S11_NAME_MAX];	/* the name block's name (S-11.017) */
};

/* Checks 1-15 (S-11.020), reading the package once, in order. Returns
 * S11_REASON_NONE when the package passes and its zip has been written out;
 * a package refused CRC or HASH may have had its zip written out too. */
enum s11_reason s11_check_package(const struct s11_package_ops *ops,
				  const struct s11_check_in *in,
				  struct s11_check_out *out);

/* Check 8 (S-11.021, S-11.022) refuses nothing: the state of a signature
 * that is `present` (sig_r and sig_s not both zero) and `verified` (with
 * signature key index 0x00, against a readable Q0, S-11.012). */
enum s11_sig s11_signature_state(bool present, bool verified);

/* Check 12's rule (S-11.023): the reason, or S11_REASON_NONE. */
enum s11_reason s11_payload_key_rule(uint8_t index, bool k0_ok, bool k1_ok,
				     bool option);

/* Forced by a RAM-disk file name (S-11.047): it contains "force". */
bool s11_name_forces(const char *name);

/* The run is a return to the factory bootloader (S-11.091): the option on,
 * the package valid (S-11.021), the bootloader step selected (S-11.096,
 * only when its image is present) and that image not a waffle-boot build
 * (s11_bootloader_is_waffle()). */
bool s11_return_to_stock(bool option, bool valid, bool bootloader_waffle,
			 bool bootloader_selected);

/* S-11.091: the staged bootloader image, len bytes, is a waffle-boot build:
 * its stream carries a valid image descriptor (s02_stream_is_waffle(),
 * S-01.023), as recovery's WB kind (S-02.106 rule 1) and the installer
 * read it. crc32 is S-02.015's CRC-32. */
bool s11_bootloader_is_waffle(const uint8_t *img, uint32_t len,
			      uint32_t (*crc32)(uint32_t crc, const uint8_t *p,
						size_t n));

#endif /* WAFFLE_INSTALL_PACKAGE_H */
