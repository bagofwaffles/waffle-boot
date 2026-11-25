/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11 install from end to end. Checks S-11.020 (a refusal at checks 1-15
 * runs no zip check, one at 16-17 starts no run), S-11.051 (the outcome),
 * S-11.055 (forcing reaches the steps), S-11.090 (the option of the checks
 * holds for the run), S-11.021 (the signature state reaches the result; it gates no step, the
 * bootloader included, S-11.090), S-11.091 (return to the factory bootloader, wording only, decided
 * before step 1, from the option, the signature state, the selection and whether the bootloader is
 * a waffle-boot build), S-11.073
 * (switched-off steps: no DFU entry, nothing sent), S-11.096 (the
 * selection: before the run's first write, only the steps selected run,
 * a cancel starts no run); S-11.064 (the package version only with the
 * OS); S-11.034 (a first-stage file is never looked for); S-17.021 (the start-of-run version read
 * is reported). The package is built here with the reference CRC-16 and
 * SHA-256; the P-224 verifier is a fake.
 */
#include <string.h>

#include "waffle/install/package.h"
#include "waffle/boot/device.h"
#include "waffle/install/run.h"

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

/* sig: the state the package is built to have, valid by default: a
 * present signature with key index 0x00 the fake verifier passes, one it
 * fails, or a blank one. */
static bool verify_ok;

static void build(uint8_t device, enum s11_sig sig)
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
	m[0x89] = sig == S11_SIG_NONE ? 0xFF : 0x00;
	if (sig != S11_SIG_NONE)
		memset(m + 0x14, 0x5A, 0x3C);		/* r and s present */
	verify_ok = sig == S11_SIG_VALID;
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
	return verify_ok;	/* whether the signature verifies with Q0 */
}

static bool u_q0(void *ctx, uint8_t q[56])
{
	(void)ctx;
	memset(q, 0, 56);
	return true;
}

static bool u_key(void *ctx, uint8_t k[32])
{
	(void)ctx;
	memset(k, 0, 32);
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
	unsigned zip_calls, run_calls, writes_bl, fs_looked, sends, enters;
	bool staged_ble, staged_bl, staged_fs, staged_os;
	unsigned writes_os;
	/* S-11.096: the answer and the steps given, a bit each; how often
	 * it was asked, the run's calls before each ask, what was offered */
	enum s11_answer answer;
	unsigned answers, n_asked, calls_at_ask, offered;
	unsigned meta_versions;		/* meta_complete calls */
	/* S-11.091: the staged bootloader is a waffle-boot build; how often
	 * that was asked */
	bool bl_waffle;
	unsigned waffle_asks;
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
	if (strstr(path, "spiboot") != NULL) {
		W.fs_looked++;
		return W.staged_fs;
	}
	if (strstr(path, "ble.bin") != NULL)
		return W.staged_ble;
	if (strstr(path, "os.ldr") != NULL)
		return W.staged_os;
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
	W.run_calls++;
	W.meta_versions++;
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
	W.writes_os++;
	return true;
}

static bool s_bootloader(void *ctx)
{
	(void)ctx;
	W.writes_bl++;
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
	s_content, s_write, s_sw, s_bootloader, NULL, s_send, s_slot,
	s_write, NULL,
};

/* S-11.096: the user's answer on the unit */
static bool u_waffle(void *ctx)
{
	(void)ctx;
	W.waffle_asks++;
	return W.bl_waffle;
}

static enum s11_answer u_select(void *ctx, const struct s11_step_env *env,
				uint8_t *selected)
{
	(void)ctx;
	W.n_asked++;
	W.calls_at_ask += W.run_calls;
	W.offered = s11_pickable(env);
	*selected = (uint8_t)W.answers;
	return W.answer;
}

/* Every step switched on, unless a test says otherwise. */
static struct s11_install_ops ops = {
	&pkg_ops, &zip_ops, &fs_ops, &run_ops, &step_ops, u_select, NULL,
	u_waffle, NULL, 0,
};

#define YES_ALL 0x3Fu

static void reset(bool ble, bool bl, struct s17_copro *c)
{
	memset(&W, 0, sizeof(W));
	W.staged_ble = ble;
	W.staged_bl = bl;
	W.answer = S11_ANSWER_INSTALL;
	W.answers = YES_ALL;
	s17_copro_init(c);
	c->nordic_app = (struct s11_version){ 1, 0, 1, 0 };	/* = staged */
}

/* S-11.096, S-11.073, S-11.064 through s11_install() */
static void test_install_select(struct s11_check_in *in,
				struct s17_copro *copro)
{
	struct s11_install_result out;

	/* asked before the run's first write, the bootloader offered with
	 * the option on, then written */
	in->option = true;
	reset(false, true, copro);
	s11_install(&ops, in, copro, &out);
	CHECK(W.n_asked == 1 && W.calls_at_ask == 0 &&
	      (W.offered & S11_STEP_BIT(S11_STEP_BOOTLOADER)) != 0u,
	      "asked before any write of the run, the bootloader offered");
	CHECK(W.writes_bl == 1 && out.outcome.kind == S11_OUTCOME_DONE &&
	      !out.canceled && (out.selected & S11_STEP_BIT(S11_STEP_BOOTLOADER)) != 0u,
	      "selected: written");

	/* written only when selected */
	for (unsigned a = 0; a < 2u; a++) {
		reset(false, true, copro);
		W.answers = a != 0u ? S11_STEP_BIT(S11_STEP_BOOTLOADER) : 0u;
		s11_install(&ops, in, copro, &out);
		CHECK(W.writes_bl == a && out.run.end != S11_RUN_FAILED,
		      "answer %u: bootloader %u written", a, W.writes_bl);
	}

	/* the option off: the bootloader cannot be selected, whatever is given */
	in->option = false;
	reset(false, true, copro);
	s11_install(&ops, in, copro, &out);
	CHECK(W.n_asked == 1 && W.writes_bl == 0 &&
	      (W.offered & S11_STEP_BIT(S11_STEP_BOOTLOADER)) == 0u &&
	      (out.selected & S11_STEP_BIT(S11_STEP_BOOTLOADER)) == 0u,
	      "option off: not offered, not written");

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
	      "the same package again: asked again, not written unselected");

	/* a cancel: no run, nothing written; reported as nothing to do */
	reset(true, true, copro);
	copro->nordic_app = (struct s11_version){ 0, 0, 0, 0 };
	W.answer = S11_ANSWER_CANCEL;
	s11_install(&ops, in, copro, &out);
	CHECK(out.canceled && out.reason == S11_REASON_NONE && W.run_calls == 0 &&
	      W.writes_bl == 0 && W.sends == 0 && W.enters == 0 &&
	      !out.run.versions_read && out.selected == 0u &&
	      out.run.end == S11_RUN_NOTHING_TO_DO &&
	      out.outcome.kind == S11_OUTCOME_NOTHING_TO_DO && !out.stock,
	      "canceled: no run, nothing written, nothing to do");

	/* the keys cannot be read: nothing asked, every step but the
	 * bootloader runs */
	reset(true, true, copro);
	copro->nordic_app = (struct s11_version){ 0, 0, 0, 0 };
	W.answer = S11_ANSWER_UNASKED;
	s11_install(&ops, in, copro, &out);
	CHECK(!out.canceled && W.writes_bl == 0 && W.sends == 1 &&
	      (out.selected & S11_STEP_BIT(S11_STEP_BLE)) != 0u &&
	      (out.selected & S11_STEP_BIT(S11_STEP_BOOTLOADER)) == 0u &&
	      out.outcome.kind == S11_OUTCOME_DONE,
	      "unasked: the Nordic step runs, the bootloader is not written");
	ops.select = NULL;
	reset(true, true, copro);
	copro->nordic_app = (struct s11_version){ 0, 0, 0, 0 };
	s11_install(&ops, in, copro, &out);
	CHECK(W.n_asked == 0 && W.writes_bl == 0 && W.sends == 1,
	      "no way to ask: as unasked");
	ops.select = u_select;

	/* S-11.064: the package version only with the OS installed */
	reset(false, true, copro);
	W.staged_os = true;
	s11_install(&ops, in, copro, &out);
	CHECK(W.writes_os == 1 && W.meta_versions == 1,
	      "the OS selected and written: the package version recorded");
	reset(false, true, copro);
	W.staged_os = true;
	W.answers = S11_STEP_BIT(S11_STEP_BOOTLOADER);
	s11_install(&ops, in, copro, &out);
	CHECK(W.writes_os == 0 && W.writes_bl == 1 && W.meta_versions == 0 &&
	      out.outcome.kind == S11_OUTCOME_DONE,
	      "the OS not selected: written nothing of it, no version recorded");

	/* S-11.061: a Nordic step not selected: no DFU entry, nothing sent */
	reset(true, false, copro);
	copro->nordic_app = (struct s11_version){ 0, 0, 0, 0 };
	W.answers = (unsigned)~S11_STEP_BIT(S11_STEP_BLE) & 0x3Fu;
	s11_install(&ops, in, copro, &out);
	CHECK(W.enters == 0 && W.sends == 0 && out.run.end != S11_RUN_FAILED,
	      "BLE not selected: no DFU entry, nothing sent");

	/* S-11.091: option on, a bootloader not waffle-boot's present: a
	 * return to the factory bootloader only when it is selected; a
	 * waffle-boot bootloader selected is an ordinary update */
	reset(false, true, copro);
	W.answers = 0;
	s11_install(&ops, in, copro, &out);
	CHECK(!out.stock && W.writes_bl == 0,
	      "not selected: no return to the factory bootloader");
	reset(false, true, copro);
	s11_install(&ops, in, copro, &out);
	CHECK(out.stock && !out.bootloader_waffle && W.waffle_asks == 1 &&
	      W.writes_bl == 1, "selected: a return to the factory bootloader");
	reset(false, true, copro);
	W.bl_waffle = true;
	s11_install(&ops, in, copro, &out);
	CHECK(!out.stock && out.bootloader_waffle && W.writes_bl == 1,
	      "a waffle-boot bootloader selected: written, no return to stock");

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

	/* S-11.034: a package carrying a first-stage file: with the option on
	 * and every step selected, the file is never looked for */
	in->option = true;
	reset(false, true, copro);
	W.staged_fs = true;
	s11_install(&ops, in, copro, &out);
	CHECK(W.n_asked == 1 && W.writes_bl == 1 && W.fs_looked == 0 &&
	      out.outcome.kind == S11_OUTCOME_DONE &&
	      out.run.step == 0xFF,
	      "first-stage file: not looked for, not asked, the run done");
	in->option = false;
}

int main(void)
{
	struct s11_check_in in = { S11_FORM_BODY, 0, false, NULL, false,
				   &wb_device_opxy };
	struct s11_install_result out;
	struct s17_copro copro;

	/* a refusal at checks 1-15: no zip check, no run */
	build(0x7F, S11_SIG_VALID);
	in.size = (uint32_t)pkg_len;
	reset(true, true, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.reason == S11_REASON_DEVICE && W.zip_calls == 0 &&
	      W.run_calls == 0 && out.outcome.kind == S11_OUTCOME_REFUSED &&
	      out.outcome.result == S11_BAD_FILE, "DEVICE: nothing after it");
	build(0x01, S11_SIG_NONE);

	/* S-11.022: unsigned, the option off: accepted, the protected step
	 * skipped (S-11.090), no return to stock and nothing asked of the
	 * bootloader image */
	reset(true, true, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.reason == S11_REASON_NONE && out.sig == S11_SIG_NONE &&
	      !out.stock && W.waffle_asks == 0 &&
	      W.zip_calls == 1u && W.writes_bl == 0u,
	      "unsigned, option off: accepted, the bootloader step skipped");

	/* S-11.090, S-11.096: invalid or none, the option on: accepted, the
	 * bootloader offered and written as for any package, the other steps
	 * run; not called a return to stock (S-11.091) */
	in.option = true;
	for (unsigned k = 0; k < 2u; k++) {
		const enum s11_sig sig = k == 0u ? S11_SIG_NONE : S11_SIG_INVALID;

		build(0x01, sig);
		reset(true, true, &copro);
		copro.nordic_app = (struct s11_version){ 0, 0, 0, 0 };
		s11_install(&ops, &in, &copro, &out);
		CHECK(out.reason == S11_REASON_NONE && out.sig == sig &&
		      W.n_asked == 1 &&
		      (W.offered & S11_STEP_BIT(S11_STEP_BOOTLOADER)) != 0u &&
		      (W.offered & S11_STEP_BIT(S11_STEP_BLE)) != 0u &&
		      (out.selected & S11_STEP_BIT(S11_STEP_BOOTLOADER)) != 0u &&
		      W.writes_bl == 1u && W.sends == 1u && !out.stock &&
		      !out.bootloader_waffle && W.waffle_asks == 0u &&
		      out.outcome.kind == S11_OUTCOME_DONE,
		      "state %d, option on: the bootloader offered and "
		      "written, the Nordic step sent", (int)sig);
	}
	build(0x01, S11_SIG_VALID);

	/* a refusal at checks 16-17: no run (the option stays on) */
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
	CHECK(out.sig == S11_SIG_VALID && out.stock && !out.bootloader_waffle,
	      "valid, a bootloader not waffle-boot's written: a return to stock");

	/* device id 0x06 forces: the equal version is sent again */
	build(0x06, S11_SIG_VALID);
	reset(true, false, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.forced && W.sends == 1 && out.outcome.kind == S11_OUTCOME_DONE,
	      "forced reaches the Nordic step");

	/* option on, valid, bootloader present: a return to the factory
	 * bootloader unless it is a waffle-boot build */
	build(0x01, S11_SIG_VALID);
	reset(false, true, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(out.stock, "return to the factory bootloader");
	reset(false, true, &copro);
	W.bl_waffle = true;
	s11_install(&ops, &in, &copro, &out);
	CHECK(!out.stock && out.bootloader_waffle, "a waffle-boot bootloader: an update");
	reset(false, false, &copro);
	W.bl_waffle = true;
	s11_install(&ops, &in, &copro, &out);
	CHECK(!out.stock && !out.bootloader_waffle && W.waffle_asks == 0,
	      "no bootloader: not a return to the factory bootloader, nothing asked");

	/* option off: the bootloader step is Skipped, nothing written */
	in.option = false;
	reset(false, true, &copro);
	s11_install(&ops, &in, &copro, &out);
	CHECK(W.writes_bl == 0 && out.outcome.kind == S11_OUTCOME_NOTHING_TO_DO,
	      "option off: Skipped, nothing to do");

	test_install_select(&in, &copro);
	return s11_check_report("test_s11_install");
}
