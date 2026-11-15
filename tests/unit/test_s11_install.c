/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11 install from end to end. Checks S-11.020 (a refusal at checks 1-15
 * runs no zip check, one at 16-17 starts no run), S-11.051 (the outcome),
 * S-11.055 (forcing reaches the steps), S-11.090 (the option of the checks
 * holds for the run), S-11.091 (return to the factory bootloader decided before step 1), S-11.073
 * (switched-off steps: no DFU entry, nothing sent), S-11.096 (the
 * confirmations: their order, before the run's first write, each protected
 * step writing only after its own); S-17.021 (the start-of-run version read
 * is reported). The package is built here with the reference CRC-16 and
 * SHA-256; the P-224 verifier is a fake.
 */
#include <string.h>

#include "waffle/s11_package.h"
#include "waffle/s11_run.h"

#include "s11_check.h"
#include "s11_ref.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* ---- a package: TE header, manifest, plaintext payload (key 0xFE) ---- */

static uint8_t pkg[0x40 + 0x300 + 0x100 + 32];
static size_t pkg_len;

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void build(uint8_t device, uint8_t sig_index)
{
	uint8_t *h = pkg, *m = pkg + 0x40, *pl = pkg + 0x340;
	uint16_t crc;

	memset(pkg, 0, sizeof(pkg));
	pkg_len = sizeof(pkg);
	put_be32(h, 0xBEEFCAFEu);
	h[4] = device;
	put_be32(h + 0x17, 0x00084000u);
	memcpy(pl, "firmware.zip", 12);			/* the name block */
	memset(pl + 0x100, 'z', 32);			/* the zip */
	m[0x04] = 0xFE;
	put_le32(m + 0x80, 32);
	memcpy(m + 0x84, "TESG", 4);
	m[0x88] = 0x01;
	m[0x89] = sig_index;
	put_le32(m + 0x8C, 0x00084000u);
	put_le32(m + 0x98, 0x120);
	ref_sha256(pl, 0x120, m + 0x9C);
	crc = ref_crc16(0, pkg + 0x40, pkg_len - 0x40);
	h[9] = (uint8_t)(crc >> 8);
	h[10] = (uint8_t)crc;
}

static struct ref_sha256 sha;

static uint16_t c_crc16(void *ctx, uint16_t crc, const uint8_t *p, size_t n)
{
	(void)ctx;
	return ref_crc16(crc, p, n);
}

static void c_init(void *ctx)
{
	(void)ctx;
	ref_sha256_init(&sha);
}

static void c_update(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	ref_sha256_update(&sha, p, n);
}

static void c_final(void *ctx, uint8_t d[32])
{
	(void)ctx;
	ref_sha256_final(&sha, d);
}

static void c_aes(void *ctx, const uint8_t key[32], uint8_t iv[16],
		  const uint8_t *in, uint8_t *out, size_t n)
{
	(void)ctx;
	ref_aes256_cbc_decrypt(key, iv, in, out, n);
}

static bool c_verify(void *ctx, const uint8_t q[56], const uint8_t d[32],
		     const uint8_t r[28], const uint8_t s[28])
{
	(void)ctx;
	(void)q;
	(void)d;
	(void)r;
	(void)s;
	return true;	/* a signature that verifies with Q0 */
}

static bool u_key(void *ctx, uint8_t k[32])
{
	(void)ctx;
	memset(k, 0, 32);
	return true;
}

static bool u_q0(void *ctx, uint8_t q[56])
{
	(void)ctx;
	memset(q, 0, 56);
	return true;
}

static bool p_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	(void)ctx;
	if (off > pkg_len || n > pkg_len - off)
		return false;
	memcpy(buf, pkg + off, n);
	return true;
}

static bool p_zip_write(void *ctx, uint32_t off, uint32_t at, const uint8_t *p,
			size_t n)
{
	(void)ctx;
	(void)off;
	(void)at;
	(void)p;
	(void)n;
	return true;
}

static bool p_zip_readable(void *ctx, uint32_t len)
{
	(void)ctx;
	return len == 32;
}

static const struct s11_crypto_ops crypto = {
	c_crc16, c_init, c_update, c_final, c_aes, c_verify,
};
static const struct s11_unit_ops unit = { u_q0, u_key, u_key };
static const struct s11_package_ops pkg_ops = {
	&crypto, &unit, p_read, p_zip_write, p_zip_readable, NULL,
};

/* ---- the zip (scripted), the staging area, the run, the steps ---- */

struct world {
	enum s11_reason zip_reason;
	unsigned zip_calls, run_calls, writes_bl, writes_fs, sends, enters;
	bool staged_ble, staged_bl, staged_fs;
	/* S-11.096: the answer for each component, a bit each; what was
	 * asked, in order, and the run's calls before each ask */
	unsigned answers, n_asked, calls_at_ask;
	enum s11_component asked[4];
};

static struct world W;

static bool z_member(void *ctx, size_t i, struct s11_member *m)
{
	(void)ctx;
	(void)i;
	(void)m;
	return false;
}

static bool z_zip64(void *ctx)
{
	(void)ctx;
	W.zip_calls++;
	return W.zip_reason == S11_REASON_MEMBER;
}

static bool z_extract(void *ctx, size_t i, s11_sink_fn sink, void *arg)
{
	(void)ctx;
	(void)i;
	(void)sink;
	(void)arg;
	return false;
}

static uint32_t z_crc32(void *ctx, uint32_t crc, const uint8_t *p, size_t n)
{
	(void)ctx;
	return ref_crc32(crc, p, n);
}

static bool z_free(void *ctx, uint64_t *b)
{
	(void)ctx;
	*b = 1u << 30;
	return true;
}

static bool z_ok(void *ctx)
{
	(void)ctx;
	return true;
}

static bool z_path(void *ctx, const char *p)
{
	(void)ctx;
	(void)p;
	return true;
}

static bool z_write(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	(void)p;
	(void)n;
	return true;
}

static const struct s11_zip_ops zip_ops = {
	z_member, z_zip64, z_extract, z_crc32, z_free, z_ok, z_path, z_path,
	z_write, z_ok, NULL,
};

/* The staging area: the bootloader and/or the Nordic application, version
 * 1.0.1 each, and firmware.version 1.2.0. */
static bool f_stat(void *ctx, const char *path, bool *dir, uint32_t *size)
{
	(void)ctx;
	*dir = false;
	*size = 8;
	if (strcmp(path, "upgrade") == 0)
		return *dir = true;
	if (strcmp(path, S11_RECORD_PATH) == 0) {
		*size = 2;
		return true;
	}
	if (strstr(path, "bootloader.ldr") != NULL)
		return W.staged_bl;
	if (strstr(path, "spiboot.ldr") != NULL)
		return W.staged_fs;
	if (strstr(path, "ble.bin") != NULL)
		return W.staged_ble;
	return strstr(path, ".version") != NULL;
}

static size_t f_read(void *ctx, const char *path, uint8_t *buf, size_t cap)
{
	const char *v = strstr(path, "firmware.version") ? "1.2.0+0" : "1.0.1";
	size_t n = strlen(v) < cap ? strlen(v) : cap;

	(void)ctx;
	if (strcmp(path, S11_RECORD_PATH) == 0 && cap >= 2) {
		buf[0] = 1;	/* {1, 0}: a record the run never reads */
		buf[1] = 0;
		return 2;
	}
	memcpy(buf, v, n);
	return n;
}

static const struct s11_fs_ops fs_ops = {
	f_stat, f_read, NULL, NULL, NULL, NULL, NULL,
};

static bool r_meta(void *ctx, uint16_t s)
{
	(void)ctx;
	(void)s;
	W.run_calls++;
	return true;
}

static bool r_complete(void *ctx, const struct s11_version *v)
{
	(void)ctx;
	(void)v;
	return true;
}

static bool r_record(void *ctx, uint8_t s)
{
	(void)ctx;
	(void)s;
	W.run_calls++;
	return true;
}

static bool r_true(void *ctx)
{
	(void)ctx;
	W.run_calls++;
	return true;
}

static bool r_enter(void *ctx)
{
	(void)ctx;
	W.run_calls++;
	W.enters++;
	return true;
}

static void r_exit(void *ctx)
{
	(void)ctx;
}

static enum s11_result r_step(void *ctx, unsigned s)
{
	(void)ctx;
	(void)s;
	return S11_INTERNAL;	/* never used: the step ops decide */
}

static const struct s11_run_ops run_ops = {
	r_meta, r_complete, r_record, r_enter, r_true, r_exit, r_step, NULL,
};

static enum s11_result s_content(void *ctx)
{
	(void)ctx;
	return S11_NOT_FOUND;
}

static bool s_write(void *ctx)
{
	(void)ctx;
	return true;
}

static bool s_bootloader(void *ctx)
{
	(void)ctx;
	W.writes_bl++;
	return true;
}

static bool s_spiboot(void *ctx)
{
	(void)ctx;
	W.writes_fs++;
	return true;
}

static void s_sw(void *ctx, const struct s11_version *v)
{
	(void)ctx;
	(void)v;
}

static bool s_send(void *ctx, enum s11_component c)
{
	(void)ctx;
	(void)c;
	W.sends++;
	return true;
}

static int s_slot(void *ctx)
{
	(void)ctx;
	return 0;
}

static const struct s11_step_ops step_ops = {
	s_content, s_write, s_sw, s_bootloader, s_spiboot, NULL, s_send, s_slot,
	s_write, NULL,
};

/* S-11.096: the user's answer on the unit */
static bool u_confirm(void *ctx, enum s11_component c)
{
	(void)ctx;
	if (W.n_asked < N(W.asked))
		W.asked[W.n_asked] = c;
	W.n_asked++;
	W.calls_at_ask += W.run_calls;
	return ((W.answers >> c) & 1u) != 0u;
}

/* Every step switched on, unless a test says otherwise. */
static struct s11_install_ops ops = {
	&pkg_ops, &zip_ops, &fs_ops, &run_ops, &step_ops, u_confirm, NULL, 0,
};

#define YES_ALL ((1u << S11_C_BOOTLOADER) | (1u << S11_C_SPIBOOT))

static void reset(bool ble, bool bl, struct s17_copro *c)
{
	memset(&W, 0, sizeof(W));
	W.staged_ble = ble;
	W.staged_bl = bl;
	W.answers = YES_ALL;
	s17_copro_init(c);
	c->nordic_app = (struct s11_version){ 1, 0, 1, 0 };	/* = staged */
}

/* S-11.096: s11_confirm() alone */
static struct s11_staged confirm_st;

static void test_confirm_order(void)
{
	memset(&W, 0, sizeof(W));
	memset(&confirm_st, 0, sizeof(confirm_st));
	confirm_st.size[S11_C_BOOTLOADER] = confirm_st.size[S11_C_SPIBOOT] = 1u;
	confirm_st.size[S11_C_OS] = 1u;
	W.answers = YES_ALL;
	CHECK(s11_confirm(&confirm_st, true, u_confirm, NULL) ==
		      (S11_STEP_BIT(S11_STEP_BOOTLOADER) |
		       S11_STEP_BIT(S11_STEP_SPIBOOT)) &&
	      W.n_asked == 2 && W.asked[0] == S11_C_BOOTLOADER &&
	      W.asked[1] == S11_C_SPIBOOT,
	      "the bootloader first, then the first stage");
	memset(&W, 0, sizeof(W));
	W.answers = 1u << S11_C_SPIBOOT;
	CHECK(s11_confirm(&confirm_st, true, u_confirm, NULL) ==
		      S11_STEP_BIT(S11_STEP_SPIBOOT) && W.n_asked == 2,
	      "each answer for its own component");
	memset(&W, 0, sizeof(W));
	W.answers = YES_ALL;
	CHECK(s11_confirm(&confirm_st, false, u_confirm, NULL) == 0u &&
	      W.n_asked == 0, "option off: nothing asked, nothing confirmed");
	CHECK(s11_confirm(&confirm_st, true, NULL, NULL) == 0u,
	      "no way to ask: nothing confirmed");
	confirm_st.size[S11_C_BOOTLOADER] = 0;
	CHECK(s11_confirm(&confirm_st, true, u_confirm, NULL) ==
		      S11_STEP_BIT(S11_STEP_SPIBOOT) &&
	      W.n_asked == 1 && W.asked[0] == S11_C_SPIBOOT,
	      "only what the package carries is asked");
	confirm_st.size[S11_C_SPIBOOT] = 0;
	W.n_asked = 0;
	CHECK(s11_confirm(&confirm_st, true, u_confirm, NULL) == 0u &&
	      W.n_asked == 0, "no protected component: nothing asked");
}

/* S-11.096, S-11.073 through s11_install() */
static void test_install_confirm(struct s11_check_in *in,
				 struct s17_copro *copro)
{
	struct s11_install_result out;

	/* both carried, both confirmed: asked before the run's first write,
	 * the bootloader first; both written */
	in->option = true;
	reset(false, true, copro);
	W.staged_fs = true;
	s11_install(&ops, in, copro, &out);
	CHECK(W.n_asked == 2 && W.asked[0] == S11_C_BOOTLOADER &&
	      W.asked[1] == S11_C_SPIBOOT && W.calls_at_ask == 0,
	      "asked in order, before any write of the run");
	CHECK(W.writes_bl == 1 && W.writes_fs == 1 &&
	      out.outcome.kind == S11_OUTCOME_DONE, "both confirmed: both written");

	/* each step writes only after its own confirmation */
	for (unsigned a = 0; a < 4u; a++) {
		reset(false, true, copro);
		W.staged_fs = true;
		W.answers = ((a & 1u) != 0u ? 1u << S11_C_BOOTLOADER : 0u) |
			    ((a & 2u) != 0u ? 1u << S11_C_SPIBOOT : 0u);
		s11_install(&ops, in, copro, &out);
		CHECK(W.writes_bl == ((a & 1u) != 0u ? 1u : 0u) &&
		      W.writes_fs == ((a & 2u) != 0u ? 1u : 0u) &&
		      out.run.end != S11_RUN_FAILED,
		      "answers %u: bootloader %u, first stage %u written", a,
		      W.writes_bl, W.writes_fs);
	}

	/* the option off: nothing asked, nothing written */
	in->option = false;
	reset(false, true, copro);
	W.staged_fs = true;
	s11_install(&ops, in, copro, &out);
	CHECK(W.n_asked == 0 && W.writes_bl == 0 && W.writes_fs == 0,
	      "option off: nothing asked");

	/* a repeated install asks again: nothing is stored */
	in->option = true;
	reset(false, true, copro);
	s11_install(&ops, in, copro, &out);
	CHECK(W.n_asked == 1 && W.writes_bl == 1, "the first install");
	W.answers = 0;
	W.writes_bl = 0;
	W.n_asked = 0;
	s11_install(&ops, in, copro, &out);
	CHECK(W.n_asked == 1 && W.writes_bl == 0,
	      "the same package again: asked again, not written unconfirmed");

	/* S-11.091: TE-signed, option on, the bootloader present: a return to
	 * the factory bootloader only when it is confirmed */
	build(0x01, 0x00);
	reset(false, true, copro);
	W.answers = 0;
	s11_install(&ops, in, copro, &out);
	CHECK(out.te_signed && !out.stock && W.writes_bl == 0,
	      "not confirmed: no return to the factory bootloader");
	reset(false, true, copro);
	s11_install(&ops, in, copro, &out);
	CHECK(out.te_signed && out.stock && W.writes_bl == 1,
	      "confirmed: a return to the factory bootloader");
	build(0x01, 0xFF);

	/* S-11.073: steps 3-5 switched off; a Nordic image carried: no DFU
	 * entry, nothing sent, the run done */
	in->option = false;
	ops.off = S11_STEPS_SWITCHABLE;
	reset(true, false, copro);
	copro->nordic_app = (struct s11_version){ 0, 0, 0, 0 };	/* older */
	s11_install(&ops, in, copro, &out);
	CHECK(W.enters == 0 && W.sends == 0 && out.run.end != S11_RUN_FAILED &&
	      out.run.versions_read,
	      "switched off: no DFU entry, nothing sent, the versions read");
	ops.off = 0;
	reset(true, false, copro);
	copro->nordic_app = (struct s11_version){ 0, 0, 0, 0 };
	s11_install(&ops, in, copro, &out);
	CHECK(W.enters == 1 && W.sends == 1, "switched on: entered and sent");
}

int main(void)
{
	struct s11_check_in in = { S11_FORM_BODY, 0, false, NULL, false };
	struct s11_install_result out;
	struct s17_copro copro;

	/* a refusal at checks 1-15: no zip check, no run */
	build(0x7F, 0xFF);
	in.size = (uint32_t)pkg_len;
	reset(true, true, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.reason == S11_REASON_DEVICE && W.zip_calls == 0 &&
	      W.run_calls == 0 && out.outcome.kind == S11_OUTCOME_REFUSED &&
	      out.outcome.result == S11_BAD_FILE, "DEVICE: nothing after it");
	build(0x01, 0xFF);

	/* S-11.022: unsigned, the option off: accepted, the protected step
	 * skipped (S-11.090) */
	reset(true, true, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.reason == S11_REASON_NONE && !out.te_signed &&
	      W.zip_calls == 1u && W.writes_bl == 0u,
	      "unsigned, option off: accepted, the bootloader step skipped");

	/* a refusal at checks 16-17: no run */
	in.option = true;
	reset(true, true, &copro);
	W.zip_reason = S11_REASON_MEMBER;
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.reason == S11_REASON_MEMBER && W.zip_calls == 1 &&
	      W.run_calls == 0, "MEMBER: no run");

	/* accepted, not forced: the equal Nordic version is Already installed;
	 * the option of the checks lets the bootloader step write */
	reset(true, true, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.reason == S11_REASON_NONE && !out.forced && W.sends == 0 &&
	      W.writes_bl == 1 && out.run.end == S11_RUN_DONE &&
	      out.run.versions_read && out.run.nordic_in_dfu,
	      "accepted: ble Already installed, bootloader written");
	CHECK(!out.stock && !out.te_signed, "signature index 0xFF: not TE-signed");

	/* device id 0x06 forces: the equal version is sent again */
	build(0x06, 0xFF);
	reset(true, false, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.forced && W.sends == 1 && out.outcome.kind == S11_OUTCOME_DONE,
	      "forced reaches the Nordic step");

	/* TE-signed, option on, bootloader present: a return to the factory bootloader */
	build(0x01, 0x00);
	reset(false, true, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.te_signed && out.stock, "return to the factory bootloader");
	reset(false, false, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.te_signed && !out.stock, "no bootloader: not a return to the factory bootloader");

	/* option off: the bootloader step is Skipped, nothing written */
	in.option = false;
	reset(false, true, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(W.writes_bl == 0 && out.outcome.kind == S11_OUTCOME_NOTHING_TO_DO,
	      "option off: Skipped, nothing to do");

	test_confirm_order();
	test_install_confirm(&in, &copro);
	return s11_check_report("test_s11_install");
}
