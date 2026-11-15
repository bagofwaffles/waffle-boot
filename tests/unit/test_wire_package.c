/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11 checks 1-15 on the vendored libraries (src/wire_package.c): every
 * package vector of tests/oracle/S-11-packages/vectors/pkg/ through
 * s11_check_package() with BearSSL's SHA-256 and AES, micro-ecc's P-224 and
 * minizip's check 15, against the reason, TE-signed and forced verdicts and
 * the recovered zip's SHA-256 of s11_vectors.h; micro-ecc's verdict on each
 * signature against the conformance verifier's; the AES chaining contract
 * of s11_crypto_ops; a zip too big for its RAM; and every package again
 * with its zip recovered in place in a writable copy (S-11.018), the bytes
 * around the zip left as they were. Package files are read at run time:
 * run from the repository root (make check does).
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/s11_package.h"
#include "waffle/wire_package.h"

#include "vectors/s11_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define B(r) (s11v_blob + (r).off)

/* The unit's OTP as the vector states it. */
/* cppcheck-suppress [constParameterCallback, constParameterPointer] ; s11_unit_ops fixes the type */
static bool otp_q0(void *ctx, uint8_t q[56])
{
	memcpy(q, s11v_q0, 56);
	return ((const struct s11v_package *)ctx)->q0 != 0;
}

/* cppcheck-suppress [constParameterCallback, constParameterPointer] ; s11_unit_ops fixes the type */
static bool otp_k0(void *ctx, uint8_t k[32])
{
	memcpy(k, s11v_k0, 32);
	return ((const struct s11v_package *)ctx)->k0 != 0;
}

/* cppcheck-suppress [constParameterCallback, constParameterPointer] ; s11_unit_ops fixes the type */
static bool otp_k1(void *ctx, uint8_t k[32])
{
	memcpy(k, s11v_k1, 32);
	return ((const struct s11v_package *)ctx)->k1 != 0;
}

static const struct s11_unit_ops otp = { otp_q0, otp_k0, otp_k1 };

static uint8_t zip_ram[1 << 20];

static uint8_t *load(const char *rel, size_t *len)
{
	char path[256];

	snprintf(path, sizeof(path), "tests/oracle/S-11-packages/vectors/%s", rel);
	return tl_read_file(path, len);
}

/* A writable package in memory: wire_source.write for the in-place zip. */
static uint8_t *rw_buf;
static size_t rw_len;

static bool rw_write(void *ctx, uint32_t off, const uint8_t *buf, size_t n)
{
	(void)ctx;
	if (off > rw_len || n > rw_len - off)
		return false;
	memcpy(rw_buf + off, buf, n);
	return true;
}

/* zip_cap 0: in place */
static void check_package(const struct s11v_package *v, size_t zip_cap)
{
	struct wire_package p;
	struct s11_package_ops ops;
	struct wire_mem mem;
	struct wire_source src = { wire_mem_read, &mem, NULL };
	struct s11_check_in in;
	struct s11_check_out out;
	enum s11_reason r;
	uint8_t *data, *orig = NULL;
	const uint8_t *zip = zip_ram;
	size_t len;

	data = load(v->file, &len);
	mem.buf = data;
	mem.len = len;
	if (zip_cap == 0) {
		orig = malloc(len);
		if (orig == NULL)
			exit(2);
		memcpy(orig, data, len);
		rw_buf = data;
		rw_len = len;
		src.write = rw_write;
	}
	wire_package_init(&p, &ops, &src, &otp, (void *)(uintptr_t)v,
			  zip_cap != 0 ? zip_ram : NULL, zip_cap);
	in.form = v->file_form ? S11_FORM_FILE : S11_FORM_BODY;
	in.size = (uint32_t)len;
	in.option = v->option != 0;
	in.file_name = v->file_name;
	in.fw_command = v->cdc != 0;
	memset(&out, 0, sizeof(out));
	r = s11_check_package(&ops, &in, &out);
	if (zip_cap != 0 && zip_cap < sizeof(zip_ram)) {
		/* the zip of a package that passes cannot be held */
		CHECK(r == S11_REASON_ZIP, "%s: zip over its RAM: %s", v->name,
		      s11_reason_name(r));
		free(data);
		return;
	}
	CHECK(r == v->reason, "%s: reason %s, want %s", v->name,
	      s11_reason_name(r), s11_reason_name(v->reason));
	if (v->te_signed >= 0)
		CHECK(out.te_signed == (v->te_signed != 0), "%s: TE-signed",
		      v->name);
	if (r == S11_REASON_NONE) {
		uint8_t d[32];
		br_sha256_context sha;

		if (v->forced >= 0)
			CHECK(out.forced == (v->forced != 0), "%s: forced",
			      v->name);
		if (zip_cap == 0) {
			zip = data + p.zip_at;
			/* only the zip's own bytes are replaced */
			CHECK(p.zip_at <= len && p.zip_len <= len - p.zip_at &&
			      !memcmp(data, orig, p.zip_at) &&
			      !memcmp(data + p.zip_at + p.zip_len,
				      orig + p.zip_at + p.zip_len,
				      len - p.zip_at - p.zip_len),
			      "%s: in place, the rest unchanged", v->name);
		}
		br_sha256_init(&sha);
		br_sha256_update(&sha, zip, p.zip_len);
		br_sha256_out(&sha, d);
		CHECK(out.zip_len == p.zip_len && v->zip_sha256.present &&
		      !memcmp(d, B(v->zip_sha256), 32), "%s: the recovered zip",
		      v->name);
	}
	free(orig);
	free(data);
}

/* micro-ecc's verdict is the conformance verifier's (S-11.012). */
static int check_verdicts(void)
{
	struct wire_crypto c;
	int n = 0;

	for (size_t i = 0; i < N(s11v_packages); i++) {
		const struct s11v_package *v = &s11v_packages[i];
		const uint8_t *sig = B(v->sig);

		if (!v->has_sig)
			continue;
		CHECK(wire_crypto_ops.p224_verify(&c, s11v_q0, sig, sig + 32,
						  sig + 60) == (v->verdict != 0),
		      "%s: P-224 verdict", v->name);
		if (v->verdict) {
			uint8_t q[56];

			/* a key off the curve is refused, not used */
			memcpy(q, s11v_q0, 56);
			q[55] ^= 1;
			CHECK(!wire_crypto_ops.p224_verify(&c, q, sig, sig + 32,
							   sig + 60),
			      "%s: off-curve key", v->name);
		}
		n++;
	}
	return n;
}

/* SP 800-38A F.2.6 in pieces, in place and not: iv chains the calls. */
static void check_aes_chain(void)
{
	struct wire_crypto c;
	uint8_t key[32], iv[16], in[64], want[64], out[64];

	if (tl_unhex_n("603deb1015ca71be2b73aef0857d7781"
		       "1f352c073b6108d72d9810a30914dff4", key, 32) ||
	    tl_unhex_n("f58c4c04d6e5f1ba779eabfb5f7bfbd6"
		       "9cfc4e967edb808d679f777bc6702c7d"
		       "39f23369a9d9bacfa530e26304231461"
		       "b2eb05e2c39be9fcda6c19078c6a9d1b", in, 64) ||
	    tl_unhex_n("6bc1bee22e409f96e93d7e117393172a"
		       "ae2d8a571e03ac9c9eb76fac45af8e51"
		       "30c81c46a35ce411e5fbc1191a0a52ef"
		       "f69f2445df4f9b17ad2b417be66c3710", want, 64))
		exit(2);
	for (size_t piece = 16; piece <= 64; piece *= 2) {
		if (tl_unhex_n("000102030405060708090a0b0c0d0e0f", iv, 16))
			exit(2);
		for (size_t off = 0; off < 64; off += piece)
			wire_crypto_ops.aes256_cbc_decrypt(&c, key, iv, in + off,
							   out + off, piece);
		CHECK(!memcmp(out, want, 64) && !memcmp(iv, in + 48, 16),
		      "AES-256-CBC in %zu-byte pieces", piece);
		memcpy(out, in, 64);
		if (tl_unhex_n("000102030405060708090a0b0c0d0e0f", iv, 16))
			exit(2);
		for (size_t off = 0; off < 64; off += piece)
			wire_crypto_ops.aes256_cbc_decrypt(&c, key, iv, out + off,
							   out + off, piece);
		CHECK(!memcmp(out, want, 64), "AES-256-CBC in place, %zu", piece);
	}
}

int main(void)
{
	size_t accepted = 0;

	check_aes_chain();
	for (size_t i = 0; i < N(s11v_packages); i++) {
		check_package(&s11v_packages[i], sizeof(zip_ram));
		check_package(&s11v_packages[i], 0);	/* in place */
		if (s11v_packages[i].reason == S11_REASON_NONE) {
			if (!accepted++)
				check_package(&s11v_packages[i], 31);
		}
	}
	CHECK(check_verdicts() >= 80, "signatures checked");
	CHECK(N(s11v_packages) >= 80 && accepted >= 30, "%zu packages, %zu accepted",
	      N(s11v_packages), accepted);
	return tl_done("test_wire_package");
}
