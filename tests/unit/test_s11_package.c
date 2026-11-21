/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11 packages, checks 1-15. Checks S-11.001, S-11.002, S-11.003, S-11.004,
 * S-11.005, S-11.006, S-11.007, S-11.008, S-11.009, S-11.010, S-11.011,
 * S-11.012, S-11.013, S-11.014, S-11.015, S-11.016, S-11.017, S-11.018,
 * S-11.020, S-11.021, S-11.022, S-11.023, S-11.024, S-11.025, S-11.026,
 * S-11.027, S-11.036, S-11.037, S-11.047, S-11.048, S-11.055, S-11.091, and
 * SC-11-REFUSE's read
 * errors (S-11.020). The checks read the package once, in order (S-16.038),
 * and their reason is the first failing check's even when the
 * zip was written out in that pass. S-11.012's curve
 * arithmetic is the vendored library's: the test checks the digest, r and s handed to the
 * verifier, that it is asked only for a present signature with key index
 * 0x00 and Q0 read, and the signature state (S-11.021), which refuses
 * nothing. S-11.024: refused and accepted packages run in turn through one
 * checker. Package files are read from tests/oracle/S-11-packages/vectors/pkg/ at run
 * time: run from the repository root (make check does).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "waffle/device.h"
#include "waffle/s11_package.h"
#include "waffle/s11_version.h"

#include "s11_check.h"
#include "s11_ref.h"
#include "vectors/s11_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define B(r) (s11v_blob + (r).off)

struct fake {
	const struct s11v_package *v;
	uint8_t *data;
	size_t size;
	struct ref_sha256 sha;
	uint8_t zip[1 << 20];
	size_t zip_len;
	unsigned verifies;
	/* read errors: every read touching [fail_lo, fail_hi), and every read
	 * from call number fail_from on (0: none) */
	uint32_t fail_lo, fail_hi;
	unsigned long reads, fail_from, watch_call;
	uint32_t watch_off;
	/* the pass: the end of the furthest read, the bytes read, whether a
	 * read went back, and zip writes that fail */
	uint32_t read_end;
	uint64_t read_bytes;
	bool went_back, fail_write;
};

static struct fake F;

static uint16_t f_crc16(void *ctx, uint16_t crc, const uint8_t *p, size_t n)
{
	(void)ctx;
	return ref_crc16(crc, p, n);
}

static void f_sha_init(void *ctx)
{
	ref_sha256_init(&((struct fake *)ctx)->sha);
}

static void f_sha_update(void *ctx, const uint8_t *p, size_t n)
{
	ref_sha256_update(&((struct fake *)ctx)->sha, p, n);
}

static void f_sha_final(void *ctx, uint8_t d[32])
{
	ref_sha256_final(&((struct fake *)ctx)->sha, d);
}

static void f_aes(void *ctx, const uint8_t key[32], uint8_t iv[16],
		  const uint8_t *in, uint8_t *out, size_t n)
{
	(void)ctx;
	ref_aes256_cbc_decrypt(key, iv, in, out, n);
}

/* Stands in for the vendored verifier: the inputs must be the ones S-11.012 gives,
 * and the verdict is the conformance verifier's. */
static bool f_p224(void *ctx, const uint8_t q[56], const uint8_t digest[32],
		   const uint8_t r[28], const uint8_t s[28])
{
	struct fake *f = ctx;
	const uint8_t *want = B(f->v->sig);

	f->verifies++;
	CHECK(f->v->has_sig && memcmp(q, s11v_q0, 56) == 0 &&
	      memcmp(digest, want, 32) == 0 && memcmp(r, want + 32, 28) == 0 &&
	      memcmp(s, want + 60, 28) == 0, "%s: verifier inputs", f->v->name);
	return f->v->verdict != 0;
}

static bool f_q0(void *ctx, uint8_t q[56])
{
	memcpy(q, s11v_q0, 56);
	return ((struct fake *)ctx)->v->q0 != 0;
}

static bool f_k0(void *ctx, uint8_t k[32])
{
	memcpy(k, s11v_k0, 32);
	return ((struct fake *)ctx)->v->k0 != 0;
}

static bool f_k1(void *ctx, uint8_t k[32])
{
	memcpy(k, s11v_k1, 32);
	return ((struct fake *)ctx)->v->k1 != 0;
}

static bool f_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	const struct fake *f = &F;	/* the ops signature fixes ctx */

	(void)ctx;
	F.reads++;
	if (off < F.read_end)
		F.went_back = true;
	F.read_end = off + (uint32_t)n;
	F.read_bytes += n;
	if (off == f->watch_off)
		F.watch_call = F.reads;
	if ((f->fail_from != 0 && F.reads >= f->fail_from) ||
	    (off < f->fail_hi && off + n > f->fail_lo))
		return false;
	if (off > f->size || n > f->size - off)
		return false;
	memcpy(buf, f->data + off, n);
	return true;
}

static bool f_zip_write(void *ctx, uint32_t off, uint32_t at, const uint8_t *p,
			size_t n)
{
	struct fake *f = ctx;

	/* the zip's bytes come from the payload's, after the name block */
	CHECK(at >= off + S11_NAME_BLOCK_LEN, "%s: zip at %u", f->v->name,
	      (unsigned)at);
	CHECK(at + n <= f->read_end, "%s: zip bytes written once read",
	      f->v->name);
	if (f->fail_write || off != f->zip_len ||
	    f->zip_len + n > sizeof(f->zip))
		return false;
	memcpy(f->zip + off, p, n);
	f->zip_len += n;
	return true;
}

/* The vendored zip reader: what Python's zipfile made of the same zip. */
static bool f_zip_readable(void *ctx, uint32_t len)
{
	struct fake *f = ctx;

	CHECK(len == f->zip_len, "%s: zip length", f->v->name);
	return f->v->reason != S11_REASON_ZIP;
}

static const struct s11_crypto_ops crypto = {
	f_crc16, f_sha_init, f_sha_update, f_sha_final, f_aes, f_p224,
};
static const struct s11_unit_ops unit = { f_q0, f_k0, f_k1 };
static const struct s11_package_ops ops = {
	&crypto, &unit, f_read, f_zip_write, f_zip_readable, &F,
};

static uint8_t *load(const char *rel, size_t *size)
{
	char path[256];
	FILE *fp;
	uint8_t *buf = NULL;
	long n;

	snprintf(path, sizeof(path), "tests/oracle/S-11-packages/vectors/%s", rel);
	fp = fopen(path, "rb");
	if (fp == NULL)
		return NULL;
	if (fseek(fp, 0, SEEK_END) == 0 && (n = ftell(fp)) >= 0 &&
	    fseek(fp, 0, SEEK_SET) == 0) {
		buf = malloc((size_t)n + 1);
		if (buf != NULL && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
			free(buf);
			buf = NULL;
		}
		*size = (size_t)n;
	}
	fclose(fp);
	return buf;
}

/* S-11.001-S-11.025; on the OP-1F S-11.026, its product word and its two
 * formats (S-11.027): a plain package's verifier is never called, since its
 * case has no signature (has_sig 0), and its state is none */
static void check_packages(const struct wb_device *dev,
			   const struct s11v_package *cases, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const struct s11v_package *v = &cases[i];
		struct s11_check_in in;
		struct s11_check_out out;
		enum s11_reason r;

		F.v = v;
		F.zip_len = 0;
		F.verifies = 0;
		F.data = load(v->file, &F.size);
		if (F.data == NULL) {
			CHECK(0, "%s: cannot read %s (run from the repository root)",
			      v->name, v->file);
			continue;
		}
		in.form = v->file_form ? S11_FORM_FILE : S11_FORM_BODY;
		in.size = (uint32_t)F.size;
		in.option = v->option != 0;
		in.file_name = v->file_name;
		in.fw_command = v->cdc != 0;
		in.dev = dev;
		r = s11_check_package(&ops, &in, &out);
		CHECK(r == v->reason, "%s %s: reason %s, want %s", dev->name, v->name,
		      s11_reason_name(r), s11_reason_name(v->reason));
		if (v->signature >= 0) {
			CHECK(out.sig == (enum s11_sig)v->signature,
			      "%s: signature state %d", v->name, (int)out.sig);
			CHECK(F.verifies == (v->has_sig ? 1u : 0u),
			      "%s: the verifier asked %u times", v->name,
			      F.verifies);
		}
		if (r == S11_REASON_NONE) {
			uint8_t sha[32];

			if (v->forced >= 0)
				CHECK(out.forced == (v->forced != 0), "%s: forced",
				      v->name);
			ref_sha256(F.zip, F.zip_len, sha);
			CHECK(v->zip_sha256.present &&
			      memcmp(sha, B(v->zip_sha256), 32) == 0,
			      "%s: the recovered zip", v->name);
		}
		free(F.data);
	}
}

static void test_packages(void)
{
	check_packages(&wb_device_opxy, s11v_packages, N(s11v_packages));
	check_packages(&wb_device_op1f, s11v_packages_1f, N(s11v_packages_1f));
}

static enum s11_reason check_on(const struct wb_device *dev,
			       const struct s11v_package *v)
{
	struct s11_check_in in = { v->file_form ? S11_FORM_FILE : S11_FORM_BODY,
				   (uint32_t)F.size, v->option != 0, v->file_name,
				   v->cdc != 0, dev };
	struct s11_check_out out;

	F.v = v;
	F.zip_len = 0;
	F.reads = 0;
	F.read_end = 0;
	F.read_bytes = 0;
	F.went_back = false;
	return s11_check_package(&ops, &in, &out);
}

static enum s11_reason check_with(const struct s11v_package *v)
{
	return check_on(&wb_device_opxy, v);
}

/* S-11.020 (SC-11-REFUSE): bytes that cannot be read refuse at the first
 * check that needs them, SHORT in checks 1-14 and ZIP in check 15. */
static void test_read_errors(void)
{
	const struct s11v_package *v = NULL;
	const uint32_t name_off = S11_PREAMBLE_LEN + S11_HEADER_LEN +
				  S11_MANIFEST_LEN;
	unsigned long name_call;

	for (size_t i = 0; i < N(s11v_packages); i++) {
		if (strcmp(s11v_packages[i].name, "form_file_ok") == 0)
			v = &s11v_packages[i];
	}
	memset(&F, 0, sizeof(F));
	F.data = v != NULL ? load(v->file, &F.size) : NULL;
	if (F.data == NULL) {
		CHECK(0, "form_file_ok cannot be read");
		return;
	}
	F.watch_off = name_off;
	CHECK(check_with(v) == S11_REASON_NONE, "read errors: the base package");
	name_call = F.watch_call;	/* check 13's read: the last at its offset */

	F.fail_lo = S11_PREAMBLE_LEN + 4u;
	F.fail_hi = F.fail_lo + 1u;
	CHECK(check_with(v) == S11_REASON_SHORT, "unreadable TE header: SHORT");
	F.fail_lo = 2u;
	F.fail_hi = 3u;
	CHECK(check_with(v) == S11_REASON_SHORT, "unreadable preamble: SHORT");
	/* check 6's CRC needs these bytes first; the zip's refuse ZIP */
	F.fail_lo = name_off + 8u;
	F.fail_hi = F.fail_lo + 1u;
	CHECK(check_with(v) == S11_REASON_SHORT,
	      "an unreadable name block: SHORT");
	F.fail_lo = name_off + S11_NAME_BLOCK_LEN + 8u;
	F.fail_hi = F.fail_lo + 1u;
	CHECK(check_with(v) == S11_REASON_ZIP, "unreadable zip bytes: ZIP");
	F.fail_lo = F.fail_hi = 0;
	F.fail_from = name_call;
	CHECK(check_with(v) == S11_REASON_SHORT,
	      "the name block's read failing: SHORT, not NAME");
	F.fail_from = name_call + 1u;
	CHECK(check_with(v) == S11_REASON_ZIP,
	      "the zip's first read failing: ZIP");
	free(F.data);
	memset(&F, 0, sizeof(F));
}

/*
 * S-16.038, S-11.020: every accepted package is read once, forward, each
 * byte once; the zip goes out in that pass, so a write that fails is check
 * 15's ZIP, after a CRC or digest that does not match.
 */
static void test_one_pass(void)
{
	const struct s11v_package *base = NULL;
	const uint32_t zip_off = S11_PREAMBLE_LEN + S11_HEADER_LEN +
				 S11_MANIFEST_LEN + S11_NAME_BLOCK_LEN;

	for (size_t i = 0; i < N(s11v_packages); i++) {
		const struct s11v_package *v = &s11v_packages[i];

		if (v->reason != S11_REASON_NONE)
			continue;
		memset(&F, 0, sizeof(F));
		F.data = load(v->file, &F.size);
		if (F.data == NULL)
			continue;
		CHECK(check_with(v) == S11_REASON_NONE && !F.went_back &&
		      F.read_bytes == F.size, "%s: one pass, %llu of %zu bytes",
		      v->name, (unsigned long long)F.read_bytes, F.size);
		free(F.data);
		if (strcmp(v->name, "form_file_ok") == 0)
			base = v;
	}
	/* the OP-1F's accepted packages, signed and plain (S-11.027) */
	for (size_t i = 0; i < N(s11v_packages_1f); i++) {
		const struct s11v_package *v = &s11v_packages_1f[i];

		if (v->reason != S11_REASON_NONE)
			continue;
		memset(&F, 0, sizeof(F));
		F.data = load(v->file, &F.size);
		if (F.data == NULL)
			continue;
		CHECK(check_on(&wb_device_op1f, v) == S11_REASON_NONE &&
		      !F.went_back && F.read_bytes == F.size,
		      "%s: one pass, %llu of %zu bytes", v->name,
		      (unsigned long long)F.read_bytes, F.size);
		free(F.data);
	}
	memset(&F, 0, sizeof(F));
	F.data = base != NULL ? load(base->file, &F.size) : NULL;
	if (F.data == NULL || F.size <= zip_off + 8u) {
		CHECK(0, "form_file_ok cannot be read");
		free(F.data);
		return;
	}
	F.fail_write = true;
	CHECK(check_with(base) == S11_REASON_ZIP, "a zip write failing: ZIP");
	F.data[zip_off + 8u] ^= 0x01u;
	CHECK(check_with(base) == S11_REASON_CRC,
	      "a zip write failing and a bad CRC: CRC");
	F.fail_write = false;
	CHECK(check_with(base) == S11_REASON_CRC && F.zip_len != 0,
	      "the zip written out, the CRC bad: CRC");
	free(F.data);
	memset(&F, 0, sizeof(F));
}

/* S-11.023: the payload-key rule, every key index, key read and option;
 * dev mode has no effect, nor has the signature state (S-11.022). */
static void test_acceptance(void)
{
	for (size_t i = 0; i < N(s11v_acceptance); i++) {
		const struct s11v_acceptance *v = &s11v_acceptance[i];
		enum s11_reason r;

		r = s11_payload_key_rule((uint8_t)v->key_index, v->k0 != 0,
					 v->k1 != 0, v->option != 0);
		CHECK(r == v->verdict, "acceptance row %zu", i);
	}
	CHECK(N(s11v_acceptance) == 80u, "acceptance rows");
}

/* S-11.021: a blank signature is none; a present one valid only when it
 * verifies */
static void test_signature_state(void)
{
	CHECK(s11_signature_state(false, false) == S11_SIG_NONE &&
	      s11_signature_state(false, true) == S11_SIG_NONE &&
	      s11_signature_state(true, false) == S11_SIG_INVALID &&
	      s11_signature_state(true, true) == S11_SIG_VALID,
	      "signature states");
}

/* S-11.036, S-11.037, S-11.007, S-11.008 */
static void test_leaves(void)
{
	for (size_t i = 0; i < N(s11v_version_parse); i++) {
		const struct s11v_version_parse *v = &s11v_version_parse[i];
		struct s11_version got = s11_version_file(B(v->file), v->file.len,
							  v->file.present != 0);

		CHECK(got.major == v->v[0] && got.minor == v->v[1] &&
		      got.patch == v->v[2] && got.build == v->v[3],
		      "version file %zu", i);
	}
	for (size_t i = 0; i < N(s11v_version_compare); i++) {
		const struct s11v_version_compare *v = &s11v_version_compare[i];
		struct s11_version a = { (uint16_t)v->a[0], (uint16_t)v->a[1],
					 (uint16_t)v->a[2], (uint16_t)v->a[3] };
		struct s11_version b = { (uint16_t)v->b[0], (uint16_t)v->b[1],
					 (uint16_t)v->b[2], (uint16_t)v->b[3] };

		CHECK(s11_version_cmp(&a, &b) == v->result &&
		      s11_version_cmp(&b, &a) == -v->result, "compare %zu", i);
	}
	for (size_t i = 0; i < N(s11v_crc16); i++)
		CHECK(ref_crc16(0, B(s11v_crc16[i].data), s11v_crc16[i].data.len) ==
		      s11v_crc16[i].crc, "reference CRC-16 %zu", i);
	for (size_t i = 0; i < N(s11v_product); i++)
		CHECK(s11_product_ok(&wb_device_opxy, s11v_product[i].word) ==
		      (s11v_product[i].passes != 0), "product %08x",
		      (unsigned)s11v_product[i].word);
	for (size_t i = 0; i < N(s11v_product_1f); i++)
		CHECK(s11_product_ok(&wb_device_op1f, s11v_product_1f[i].word) ==
		      (s11v_product_1f[i].passes != 0), "OP-1F product %08x",
		      (unsigned)s11v_product_1f[i].word);
}

/* S-10c.075: the firmware command forces on the MIDI carrier too */
static void test_fw_command_on_midi(void)
{
	for (size_t i = 0; i < N(s11v_packages); i++) {
		const struct s11v_package *v = &s11v_packages[i];
		struct s11_check_in in = { S11_FORM_BODY, 0, false, NULL, true,
					   &wb_device_opxy };
		struct s11_check_out out;

		if (strcmp(v->name, "midi_not_forced") != 0)
			continue;
		F.v = v;
		F.zip_len = 0;
		F.data = load(v->file, &F.size);
		if (F.data == NULL)
			break;
		in.size = (uint32_t)F.size;
		in.option = v->option != 0;
		CHECK(s11_check_package(&ops, &in, &out) == S11_REASON_NONE &&
		      out.forced, "midi_not_forced through the firmware command");
		free(F.data);
		return;
	}
	CHECK(0, "midi_not_forced not found");
}

/* S-11.047, S-11.091 */
static void test_small_rules(void)
{
	CHECK(s11_name_forces("op-xy_force.tfw") && !s11_name_forces("FORCE.tfw") &&
	      !s11_name_forces("forc.tfw"), "force in the file name");
	for (unsigned m = 0; m < 16; m++) {
		/* option, valid (S-11.021), a waffle-boot bootloader, the
		 * bootloader selected (S-11.096) */
		bool want = m == 0xB;

		CHECK(s11_return_to_stock(m & 1, m & 2, m & 4, m & 8) == want,
		      "return to the factory bootloader %u", m);
	}
}

/* S-11.091: a bootloader image is a waffle-boot build by its descriptor
 * (S-01.023), as recovery's WB kind reads it */
static void test_bootloader_kinds(void)
{
	for (size_t i = 0; i < N(s11v_bootloader_kinds); i++) {
		const struct s11v_bootloader_kind *v = &s11v_bootloader_kinds[i];

		CHECK(s11_bootloader_is_waffle(B(v->stream), (uint32_t)v->stream.len,
					       ref_crc32) == (v->waffle != 0),
		      "bootloader kind %s", v->name);
	}
	CHECK(!s11_bootloader_is_waffle(NULL, 0u, ref_crc32), "no image");
	CHECK(N(s11v_bootloader_kinds) >= 10u, "bootloader kinds");
}

int main(void)
{
	test_leaves();
	test_acceptance();
	test_signature_state();
	test_bootloader_kinds();
	test_packages();
	test_fw_command_on_midi();
	test_read_errors();
	test_one_pass();
	test_small_rules();
	return s11_check_report("test_s11_package");
}
