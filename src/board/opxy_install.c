/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11's run on the master: the metadata writes (S-04.045 M1-M4), the
 * progress record on the slave's /yaffs2 (S-11.062), the Nordic's
 * exchanges around the run (S-11.061, S-11.064, S-17.021 step 2), and the
 * steps' writers over the staged images (S-11.066-S-11.071): S-03's OS
 * write, S-02's slot upgrade, S-08's DFU transfer.
 *
 * The staged images are read whole through the S-09 file client into the
 * heap: S-01.016's staging area lies under the RAM disk's second part.
 * Before a step writes or sends one it is checked by S-11.095, against the
 * member's stated size and CRC-32 (staged()).
 *
 * The run goes through run_ops.step (s11_run()), which reports each step
 * to S-15 around s11_step_result(); the run holds the loop, so each report
 * is drawn as it is made. The same phases go
 * to S-10c's progress sink (S-10c.100-S-10c.104): the finishing link for a
 * host transfer, CDC otherwise.
 */
#include "waffle/board/opxy_services.h"

#include <stdlib.h>
#include <string.h>

#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_bridge.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/s07_program.h"
#include "waffle/s11_content.h"
#include "waffle/s11_package.h"
#include "waffle/s15_select.h"
#include "waffle/wire_crc.h"

#define STAGE "upgrade/"
#define DEVICE_CONTENT 0x06u	/* the TE header's device id (S-11.004) */
/* The body's payload key index (S-11.010, S-11.015) and its plaintext. */
#define KEY_INDEX_AT (S11_HEADER_LEN + 4u)
#define KEY_PLAIN 0xFEu

/*
 * S-11.073: the steps this build runs. A step set false reports Skipped
 * when the package carries its component, and neither writes nor sends
 * anything; setting its line true runs it again. Only the Nordic and PSoC
 * steps can be switched off. Step 6 is reserved and does nothing.
 */
static const bool step_on[S11_STEP_DONE] = {
	[S11_STEP_CONTENT] = true,
	[S11_STEP_OS] = true,
	[S11_STEP_BOOTLOADER] = true,
	[S11_STEP_BLE] = false,
	[S11_STEP_BLE_BOOTLOADER] = false,
	[S11_STEP_KEYBOARD] = false,
	[S11_STEP_RESERVED] = true,
};

uint8_t opxy_steps_off(void)
{
	uint8_t off = 0;

	for (unsigned step = 0; step < S11_STEP_DONE; step++) {
		if (!step_on[step])
			off |= S11_STEP_BIT(step);
	}
	return off & S11_STEPS_SWITCHABLE;
}

/* The run in progress: one per boot at a time. */
static struct {
	struct s11_staged st;
	struct s11_step_env env;
	bool refused_start;	/* S-02.090 refused the bootloader step */
	bool already_written;	/* in S-11.068 item 5's case */
	/* For write_failure: S11_BAD_FILE or S11_INTERNAL when the step's
	 * image was refused or could not be read (S-11.095, S-02.091) before
	 * any byte of it was written or sent; S11_OK otherwise. */
	enum s11_result refusal;
	int write_status;	/* the last S-02 writer's, for refused_start */
	unsigned skipped;	/* the steps that reported Skipped, a bit each */
	/* Why the step failed, for its row and the log; "" when the result
	 * says it all. */
	char cause[S15_CAUSE_MAX + 1];
	enum s15_note note;	/* S-11.069, S-11.070: under the failure */
} cur;

static void cause(const char *text)
{
	strncpy(cur.cause, text, S15_CAUSE_MAX);
	cur.cause[S15_CAUSE_MAX] = '\0';
}

/* "chunk 12/49 refused": text, then a/b, then more text. */
static void cause_of(const char *before, unsigned a, unsigned b,
		     const char *after)
{
	struct s15_buf t;

	s15_buf_init(&t);
	s15_buf_str(&t, before);
	s15_buf_dec(&t, a);
	if (b != 0u) {
		s15_buf_str(&t, "/");
		s15_buf_dec(&t, b);
	}
	s15_buf_str(&t, after);
	cause(t.s);
}

static void show(struct opxy_master *m)
{
	opxy_menu_show(m);
}

/* The last report sent to the host: one per change of label or percent
 * (S-10c.103), so a phase's reads do not flood the link. */
static struct {
	const char *label;
	uint32_t pct;
} sent;

static void host_progress(struct opxy_master *m, uint32_t done,
			  uint32_t total, const char *label)
{
	const uint32_t pct = total == 0u ? 100u :
		(uint32_t)(((uint64_t)done * 200u + total) / (2u * (uint64_t)total));

	if (label == sent.label && pct == sent.pct)
		return;
	sent.label = label;
	sent.pct = pct;
	s10c_host_progress(&m->sv.host, done, total, label);
}

/* A version as text, NUL-terminated, into out[S11_VERSION_TEXT_MAX + 1]. */
static void version_str(const struct s11_version *v, char *out)
{
	out[s11_version_text(v, out, S11_VERSION_TEXT_MAX)] = '\0';
}

/* --- the metadata record (S-04.043) --- */

/* n bytes at off read twice, the reads equal (S-02.064 step 0): a read that
 * returns wrong bytes with no error is not written into both copies. */
static bool read_twice(const struct s02_flash_ops *f, uint32_t off, uint8_t *buf, uint32_t n)
{
	uint8_t again[S02_MIRROR_SIZE];

	return n <= sizeof(again) && f->read(f->ctx, off, buf, n) == 0 &&
	       f->read(f->ctx, off, again, n) == 0 && memcmp(buf, again, n) == 0;
}

/*
 * The record every write keeps its other bytes from, read now: 0xC000 when it
 * holds the magic, else a valid mirror (S-02.063), else 0xC000 as it reads,
 * which then holds no record to lose.  False when neither can be read, or
 * two reads of what is used disagree.
 */
static bool meta_before(const struct opxy_master *m, uint8_t before[S04_META_SIZE])
{
	const struct s02_flash_ops *f = m->flash.flash;
	uint8_t mirror[S02_MIRROR_SIZE];
	bool read = read_twice(f, S02_META_OFF, before, S04_META_SIZE);

	if (read && before[0] == (uint8_t)S04_META_MAGIC &&
	    before[1] == (uint8_t)(S04_META_MAGIC >> 8) &&
	    before[2] == (uint8_t)(S04_META_MAGIC >> 16) &&
	    before[3] == (uint8_t)(S04_META_MAGIC >> 24))
		return true;
	if (read_twice(f, S02_MIRROR_OFF, mirror, S02_MIRROR_SIZE) &&
	    s02_mirror_valid(mirror)) {
		memcpy(before, mirror, S04_META_SIZE);
		return true;
	}
	return read;
}

/*
 * S-04.043 over S-02.064.  Refused when the record could not be read at boot
 * (m->meta then holds no values to write) or its restore failed.  Factory
 * mode is S-10c.053's, off at each boot until a host unlocks it.
 */
bool opxy_meta_write(struct opxy_master *m)
{
	uint8_t before[S04_META_SIZE], raw[S04_META_SIZE];
	int r;

	if (m->meta_failed || m->meta_restore_failed || !meta_before(m, before))
		return false;
	s04_meta_build(before, &m->meta, m->sv.host.factory, raw);
	opxy_flash_writer(&m->hw, true);	/* S-13b.017 */
	r = s02_meta_write(&m->flash, raw);
	opxy_flash_writer(&m->hw, false);
	if (r != 0)
		return false;
	memcpy(m->meta_raw, raw, sizeof(raw));
	return true;
}

/*
 * A failed write puts the values from before it back here.  The flash may
 * hold either record: after a failure late in s02_meta_write() the mirror
 * already holds the new one, which the next boot's restore makes the record
 * (S-02.065).
 */
static bool meta_change(struct opxy_master *m, const struct s04_meta *next)
{
	const struct s04_meta before = m->meta;

	m->meta = *next;
	if (opxy_meta_write(m))
		return true;
	m->meta = before;
	s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG, "metadata write failed");
	return false;
}

static void version_words(const struct s11_version *v, uint16_t w[4])
{
	w[0] = v->major;
	w[1] = v->minor;
	w[2] = v->patch;
	w[3] = v->build;
}

/* --- s11_run_ops --- */

/* M1 (state 0xAB0D) and M4 (state 0). S-08.013: the Nordic's greetings are
 * answered by the state from the moment it is set, written or not. */
static bool run_meta_state(void *ctx, uint16_t state)
{
	struct opxy_master *m = ctx;
	struct s04_meta next = m->meta;

	next.state = state;
	s08_set_state(&m->nordic, (int)state);
	return meta_change(m, &next);
}

/* M3: os_version and state 0 in one write */
static bool run_meta_complete(void *ctx, const struct s11_version *v)
{
	struct opxy_master *m = ctx;
	struct s04_meta next = m->meta;

	version_words(v, next.os_version);
	next.state = S04_STATE_IDLE;
	s08_set_state(&m->nordic, S08_STATE_IDLE);
	return meta_change(m, &next);
}

/* S-11.062 */
static bool run_record_write(void *ctx, uint8_t step)
{
	struct opxy_master *m = ctx;

	return s09_files_record_write(&m->sv.files, step);
}

/* S-11.061 step 2, S-08.031 */
static bool run_nordic_enter(void *ctx)
{
	struct opxy_master *m = ctx;

	return s08_enter_dfu(&m->nordic);
}

/*
 * S-11.061 step 3, S-17.021 step 2: the mode, and in the bootloader the
 * second reply's application and bootloader versions. Returns whether the
 * Nordic reported its bootloader mode.
 */
static bool run_nordic_versions(void *ctx)
{
	struct opxy_master *m = ctx;
	uint8_t info[S17_REPLY_MAX];
	long n;

	cur.env.nordic_in_dfu = false;
	if (s08_mode_query(&m->nordic) != S08_IN_DFU)
		return false;
	n = s08_info(&m->nordic, info, sizeof(info));
	if (n >= 0) {
		struct s14_text t;

		s17_copro_dfu_reply(&m->sv.copro, info, (size_t)n);
		/* What the Nordic said of itself, as data: the slot and the
		 * versions the Nordic steps go by. */
		s14_text_init(&t);
		s14_text_str(&t, "dfu info: ");
		s14_text_bytes(&t, info, (size_t)n);
		s14_log_text(&m->sv.log, S14_INFO, S14_TAG("nordic"), &t);
	}
	cur.env.nordic_in_dfu = true;		/* for the steps */
	return true;
}

/* S-11.064 step 3, S-08.035: the mode query, the exit when in DFU. */
static void run_nordic_exit(void *ctx)
{
	struct opxy_master *m = ctx;

	(void)s08_run_end(&m->nordic);
}

/* The component whose version a step's OK line shows. */
static const int8_t step_component[S11_STEP_DONE] = {
	-1, S11_C_OS, S11_C_BOOTLOADER, S11_C_BLE,
	S11_C_NORDIC_BL0,	/* one version file for both (S-11.070) */
	S11_C_BACKPLANE, -1,
};

static enum s15_result shown(enum s11_result r, enum s11_skip why)
{
	switch (r) {
	case S11_OK:
		return S15_RES_OK;
	case S11_NOT_FOUND:
		return S15_RES_NOT_FOUND;
	case S11_ALREADY:
		return S15_RES_ALREADY;
	case S11_SKIPPED:
		return why == S11_SKIP_SWITCHED_OFF ? S15_RES_SKIPPED_OFF :
		       why == S11_SKIP_NOT_SELECTED ?
		       S15_RES_SKIPPED_UNSELECTED :
		       why == S11_SKIP_ALREADY_WRITTEN ?
		       S15_RES_SKIPPED_WRITTEN : S15_RES_SKIPPED;
	case S11_BAD_FILE:
		return S15_RES_BAD_FILE;
	case S11_INTERNAL:
		return S15_RES_INTERNAL;
	case S11_FAILURE:
	default:
		return S15_RES_FAILURE;
	}
}

/* The result's name in the log, as S-11.058 names it. */
static const char *result_name(enum s11_result r)
{
	switch (r) {
	case S11_BAD_FILE:
		return "bad file";
	case S11_INTERNAL:
		return "internal error";
	case S11_OK:
	case S11_NOT_FOUND:
	case S11_ALREADY:
	case S11_SKIPPED:
	case S11_FAILURE:
	default:
		return "failure";
	}
}

/* A step that stopped the run: the cause its writer left, or what the
 * result alone says, for the row; one error line in the log. */
static void step_failed(struct opxy_master *m, unsigned step,
			enum s11_result r)
{
	struct s14_text t;

	if (cur.cause[0] == '\0') {
		if (r == S11_BAD_FILE)
			cause("bad image file");
		else if (r == S11_INTERNAL)
			cause("internal error");
	}
	s14_text_init(&t);
	s14_text_str(&t, "install: step ");
	s14_text_dec(&t, step);
	s14_text_str(&t, " (");
	s14_text_str(&t, s15_step_label((enum s15_step)step));
	s14_text_str(&t, ") ");
	s14_text_str(&t, result_name(r));
	if (cur.cause[0] != '\0') {
		s14_text_str(&t, ": ");
		s14_text_str(&t, cur.cause);
	}
	s14_log_text(&m->sv.log, S14_ERROR, S14_NO_TAG, &t);
}

/*
 * S-11.057's step with its start and its result on the screen and to the
 * host. The reserved step 6 does nothing and shows nothing, on the screen
 * or to the host.
 */
static enum s11_result run_step(void *ctx, unsigned step)
{
	struct opxy_master *m = ctx;
	struct s15_menu *menu = &m->sv.menu;
	char v[S11_VERSION_TEXT_MAX + 1];
	enum s11_result r;
	int c;

	if (step >= S11_STEP_DONE)
		return S11_INTERNAL;
	if (step == S11_STEP_RESERVED)
		return s11_step_result(step, &cur.env, &m->sv.step);
	/* S-08.014: a greeting that arrived during the last step is answered
	 * before this one begins. */
	s08_answer_greetings(&m->nordic);
	cur.refused_start = cur.already_written = false;
	cur.refusal = S11_OK;
	cur.cause[0] = '\0';
	cur.note = S15_NOTE_NONE;
	host_progress(m, 0u, 1u, s10c_step_label(step));
	s15_up_step_start(menu, (enum s15_step)step);
	show(m);
	opxy_usb_breathe();
	r = s11_step_result(step, &cur.env, &m->sv.step);
	opxy_usb_breathe();
	c = step_component[step];
	v[0] = '\0';
	if (c >= 0)
		version_str(&cur.st.version[c], v);
	if (!s11_result_continues(r))
		step_failed(m, step, r);
	s15_up_step_result(menu, (enum s15_step)step,
			   shown(r, s11_result_skip(step, &cur.env, r)),
			   c >= 0 ? v : NULL, cur.refused_start, cur.cause);
	s15_up_step_note(menu, cur.note);
	show(m);
	host_progress(m, 1u, 1u, s10c_step_label(step));
	if (r == S11_SKIPPED) {
		char label[S10C_SKIP_LABEL_MAX + 1];
		const enum s11_skip why = s11_result_skip(step, &cur.env, r);

		/* S-15.115: the summary names what was skipped but for the
		 * steps the person left out on the selection */
		if (why != S11_SKIP_NOT_SELECTED)
			cur.skipped |= 1u << step;
		/* S-10c.106: which part was skipped, and why */
		s10c_skip_label(step, why, label);
		s10c_host_progress(&m->sv.host, 0u, 0u, label);
		sent.label = NULL;
	}
	return r;
}

/* --- s11_step_ops --- */

/*
 * S-11.095: a staged image is checked before a step uses it. S-11.030's
 * CRC-32 is checked as the member is staged, S-09's replies carry no check
 * of their own (S-09.013), and S-02.047's read-back compares the flash with
 * the buffer that was written, so a reply damaged in transit would
 * otherwise be written, recorded as good and trusted on every later boot.
 * The bytes read must have the member's stated size and CRC-32, kept in
 * sv->extracted_sizes. A failed check reports Bad file, a failed read
 * Internal error, nothing of the image is written or sent in either case,
 * and the bytes a step writes are the ones checked, with no further read
 * (the caller gets this buffer).
 */
static enum s11_result staged_check(const struct opxy_master *m,
				    enum s11_component c, const uint8_t *image,
				    uint32_t len)
{
	const struct opxy_services *sv = &m->sv;
	const struct s11_staged_read read = { true, len,
					      opxy_crc32_long(0u, image, len) };
	const struct s11_staged_read stated = { sv->extracted,
						sv->extracted_sizes.size[c],
						sv->extracted_sizes.crc32[c] };

	/* A run always follows its own extraction; without the stated
	 * values there is nothing to check against. */
	if (!stated.ok)
		return S11_INTERNAL;
	return s11_staged_result(s11_staged_check(&stated, &read));
}

/*
 * A staged component's image, read whole into the heap and checked
 * (S-11.095); the caller writes or sends these bytes and frees them. NULL
 * when it could not be read (cur.refusal = S11_INTERNAL: a stat or read
 * that fails, a size that is not the scan's or over its limit, S-11.035,
 * no memory) or did not pass its check (S11_BAD_FILE); write_failure
 * reports cur.refusal. The step has found the image present (S-11.034)
 * before it calls this.
 */
static uint8_t *staged(struct opxy_master *m, enum s11_component c,
		       uint32_t *len)
{
	struct s09_files *f = &m->sv.files;
	char path[sizeof(STAGE) + S11_PATH_MAX];
	struct s09_stat st;
	uint8_t *image;
	enum s11_result r;

	cur.refusal = S11_INTERNAL;
	*len = 0;
	memcpy(path, STAGE, sizeof(STAGE) - 1u);
	strcpy(path + sizeof(STAGE) - 1u, s11_components[c].image);
	if (s09_files_stat(f, path, &st) != 0 || st.size == 0u ||
	    st.size > s11_components[c].limit) {
		cause("staged image not found");
		return NULL;
	}
	image = malloc(st.size);
	if (image == NULL) {
		cause("out of memory");
		return NULL;
	}
	if (!s09_files_read_whole(f, path, image, st.size, len)) {
		free(image);
		*len = 0;
		cause("staged image: read error");
		return NULL;
	}
	r = staged_check(m, c, image, *len);
	if (r != S11_OK) {
		free(image);
		*len = 0;
		cur.refusal = r;
		cause("staged image: check failed");
		return NULL;
	}
	cur.refusal = S11_OK;
	return image;
}

/* Step 0 (S-11.066): content_only is S-11.056's, from the staging area as
 * the run found it; step 0 is the first to change it. */
static enum s11_result step_content(void *ctx)
{
	struct opxy_master *m = ctx;
	struct s11_staged st;
	enum s11_result r;

	s11_staged_scan(&m->sv.staging, &st);
	r = s11_install_content(&m->sv.staging, s11_staged_content_only(&st));
	if (r == S11_FAILURE)
		cause("a file was not installed");	/* S-11.079 */
	return r;
}

/* S-11.067, S-03 section 5 */
static bool step_os_write(void *ctx)
{
	struct opxy_master *m = ctx;
	struct s03_write w;
	uint32_t len;
	uint8_t *image;

	/* The test OS (waffle/testos.h): the inactive boot partition holds the
	 * installed OS, so the step reports Internal error (S-11.067's result for
	 * a refused write) and writes nothing. */
	OPXY_TESTOS((void)m; cause("OS write off in test OS"); return false;)
	/* cppcheck-suppress unreachableCode ; true of the test OS's build only (waffle/testos.h) */
	image = staged(m, S11_C_OS, &len);
	if (image == NULL)
		return false;
	s03_write_os(&m->emmc, image, len, &w);
	free(image);
	/* S-03.063: reported; the slave refuses the volume until a select 0
	 * succeeds, so the run's later steps fail rather than write. */
	if (w.select0_failed) {
		s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG,
			    "os step: select 0 failed");
	}
	if (w.result == S03_WRITE_REFUSED)
		cause("OS image refused");		/* S-03.051 */
	else if (w.result != S03_WRITE_OK)
		cause_of("eMMC error, stage ", (unsigned)w.stage, 0u, "");
	return w.result == S03_WRITE_OK;
}

/* M2 */
static void step_sw_version(void *ctx, const struct s11_version *v)
{
	struct opxy_master *m = ctx;
	struct s04_meta next = m->meta;

	version_words(v, next.sw_version);
	(void)meta_change(m, &next);
}

/* Why an S-02 writer did not write. S-02.090's refusal has a line of its
 * own on the screen (refused_start). */
static void flash_cause(int status)
{
	switch (status) {
	case S02_OK:
	case S02_ERR_START:
		break;
	case S02_ERR_SIZE:
		cause("image too large");
		break;
	case S02_ERR_VERIFY:
		cause("read-back mismatch");
		break;
	case S02_ERR_STREAM:
		cause("not a bootable image");		/* S-02.091 */
		break;
	case S02_ERR_IO:
	default:
		cause("flash error");
		break;
	}
}

/*
 * S-11.068 item 5: S-02.090 refused the start for `verified` alone (the
 * selected slot good), the selector read is the one this power-on's own
 * bootloader step wrote, and the selected slot's record has the length
 * and CRC-32 of the image that was checked.
 */
static bool already_written(const struct opxy_master *m,
			    const struct s02_start *s, const uint8_t *image,
			    uint32_t len)
{
	struct s02_record rec;

	if (!m->own_selector_set || !s->read || s02_verified(s->sel.verified) ||
	    !s->selected_good || s->sel.index != m->own_selector.index ||
	    s->sel.verified != m->own_selector.verified)
		return false;
	if (s02_read_record(&m->flash, s02_selected_slot(s->sel.index), &rec) !=
	    S02_OK)
		return false;
	return s02_tag_ok(rec.tag) && rec.length == len &&
	       rec.crc32 == m->flash.crc32(0u, image, len);
}

/* S-11.068, S-02.042-S-02.053. S-11 runs the step only with the option
 * on (S-11.090); the writer is given the session's option all the same
 * (S-02.056), so an S-11 that ran it with the option off writes nothing. */
static bool step_bootloader(void *ctx)
{
	struct opxy_master *m = ctx;
	struct s02_start start;
	unsigned target;
	uint32_t len;
	uint8_t *image = staged(m, S11_C_BOOTLOADER, &len);
	int r;

	cur.write_status = S02_ERR_IO;
	if (image == NULL)
		return false;
	opxy_flash_writer(&m->hw, true);	/* S-13b.017 */
	r = s02_slot_upgrade_start(&m->flash, cur.env.option, image, len,
				   &target, &start);
	if (r == S02_ERR_START)
		cur.already_written = already_written(m, &start, image, len);
	opxy_flash_writer(&m->hw, false);
	free(image);
	if (r == S02_OK) {
		m->own_selector_set = true;	/* S-02.048's {target, 0x00} */
		m->own_selector.index = (uint8_t)target;
		m->own_selector.verified = 0x00u;
	} else if (r != S02_ERR_START && r != S02_ERR_SIZE &&
		   r != S02_ERR_STREAM && r != S02_SKIPPED) {
		m->own_selector_set = false;	/* the selector may have changed */
	}
	cur.refused_start = r == S02_ERR_START && !cur.already_written;
	cur.write_status = r;
	cur.refusal = r == S02_ERR_STREAM ? S11_BAD_FILE :	/* S-02.091 */
		      cur.already_written ? S11_SKIPPED : S11_OK;
	if (cur.already_written)
		s14_log_str(&m->sv.log, S14_INFO, S14_NO_TAG,
			    "install: bootloader already written in this "
			    "power-on, start refused for verified");
	flash_cause(r);
	return r == S02_OK;
}

/* S-11.095, S-11.068: Bad file when the image's check failed or
 * S-02.091 refused it, Internal error when a read of it failed; S11_OK
 * when the write or transfer itself failed (the step's own result). */
static enum s11_result step_write_failure(void *ctx)
{
	(void)ctx;
	return cur.refusal;
}

/* Where and how a DFU transfer stopped (S-08.042, S-08.047). */
static void transfer_cause(const struct s08_failure *f)
{
	static const char *const what[] = {
		[S08_AT_PREPARE] = "prepare", [S08_AT_HEADER] = "header",
		[S08_AT_PERFORM] = "perform",
	};
	const char *const how = f->reply == S08_REPLY_NONE ? " not answered" :
				f->reply == S08_REPLY_BUSY ? ": busy, then silent" :
							     " refused";
	struct s15_buf t;

	switch (f->at) {
	case S08_AT_QUERY:
		cause(f->reply == S08_REPLY_NONE ? "BLE chip does not answer" :
						   "BLE not in update mode");
		break;
	case S08_AT_CHUNK:
		cause_of("chunk ", f->chunk, f->chunks, how);
		break;
	case S08_AT_PREPARE:
	case S08_AT_HEADER:
	case S08_AT_PERFORM:
		s15_buf_init(&t);
		s15_buf_str(&t, what[f->at]);
		s15_buf_str(&t, how);
		cause(t.s);
		break;
	case S08_AT_NONE:
	default:
		break;
	}
}

/* S-08.042-S-08.047: the staged image with its staged version, the type
 * of its component and this power-on's product word (S-08.045). */
static bool step_nordic_send(void *ctx, enum s11_component c)
{
	struct opxy_master *m = ctx;
	struct s11_staged st;
	uint16_t version[4];
	uint32_t len;
	uint8_t *image;
	bool ok;

	s11_staged_scan(&m->sv.staging, &st);
	version_words(&st.version[c], version);
	image = staged(m, c, &len);
	if (image == NULL)
		return false;
	ok = s08_transfer(&m->nordic, image, len, version,
			  c == S11_C_BLE ? S08_TYPE_APP : S08_TYPE_BOOTLOADER,
			  s08_product(&m->nordic));
	free(image);
	if (!ok)
		transfer_cause(&m->nordic.failure);
	/* S-11.069: no application; S-11.070: the slot choice erased */
	if (!ok && s08_after_prepare(&m->nordic.failure))
		cur.note = c == S11_C_BLE ? S15_NOTE_NO_APP :
					    S15_NOTE_MAY_NOT_START;
	return ok;
}

/* S-08.022, S-16.124 */
static int step_nordic_slot(void *ctx)
{
	struct opxy_master *m = ctx;
	const long slot = s08_boot_slot_query(&m->nordic);

	if (slot == 0 || slot == 1)
		return (int)slot;
	if (slot >= 0)
		cause_of("slot ", (unsigned)slot, 0u, " cannot be written");
	else
		cause(m->nordic.replied ? "slot answer not readable" :
					  "no slot answer");
	return -1;
}

/*
 * S-11.071, S-07.080-S-07.085: the staged .cyacd text through the PSoC
 * programmer. The board supplies no PSoC unlock (S-07.081), so the
 * programmer fails the step before entry and reports why; the image is
 * not read for it. The step is switched off (S-11.073).
 */
static bool step_psoc(void *ctx)
{
	struct opxy_master *m = ctx;
	uint8_t unlock[S07_CY_UNLOCK_LEN];
	const bool has_unlock = opxy_psoc_unlock(unlock);
	uint32_t len = 0;
	uint8_t *image = NULL;
	bool ok = false;
	int r;

	if (has_unlock) {
		image = staged(m, S11_C_BACKPLANE, &len);
		if (image == NULL)
			return false;
	}
	r = s07_program(&m->psoc, (const char *)image, len,
			has_unlock ? unlock : NULL, &ok);
	free(image);
	if (!has_unlock)
		cause("no PSoC unlock");		/* S-07.081 */
	/*
	 * S-11.058: a step a power-off interrupts has no
	 * result. The power-off of S-07.051 has run; nothing more of the run
	 * is done, no result, record or metadata written, and the next boot
	 * finds the install interrupted (S-11.065).
	 */
	if (r == S07_POWEROFF)
		opxy_powered_off(m);
	return ok;
}

void opxy_install_ops(struct opxy_master *m, struct s11_run_ops *run,
		      struct s11_step_ops *step)
{
	run->ctx = m;
	run->meta_state = run_meta_state;
	run->meta_complete = run_meta_complete;
	run->record_write = run_record_write;
	run->nordic_enter_dfu = run_nordic_enter;
	run->nordic_read_versions = run_nordic_versions;
	run->nordic_exit_dfu = run_nordic_exit;
	run->step = run_step;
	step->ctx = m;
	step->content = step_content;
	step->os_write = step_os_write;
	step->meta_sw_version = step_sw_version;
	step->bootloader_write = step_bootloader;
	step->write_failure = step_write_failure;
	step->nordic_send = step_nordic_send;
	step->nordic_boot_slot = step_nordic_slot;
	step->psoc_program = step_psoc;
}

/* --- a run's start and end --- */

void opxy_install_begin(struct opxy_master *m, bool option, bool forced,
			uint8_t selected)
{
	s11_staged_scan(&m->sv.staging, &cur.st);
	cur.env.staged = &cur.st;
	cur.env.option = option;
	cur.env.forced = forced;
	cur.env.nordic_in_dfu = false;
	cur.env.copro = &m->sv.copro;
	cur.env.off = opxy_steps_off();
	cur.env.selected = selected;
	cur.skipped = 0;
	cur.refusal = S11_OK;
	cur.cause[0] = '\0';
	cur.note = S15_NOTE_NONE;
	sent.label = NULL;
}

static void run_end(struct opxy_master *m, enum s11_run_end end)
{
	s15_up_run_end(&m->sv.menu,
		       end == S11_RUN_DONE ? S15_OUT_DONE :
		       end == S11_RUN_NOTHING_TO_DO ? S15_OUT_NOTHING :
						      S15_OUT_FAILED,
		       cur.skipped);
	show(m);
}

/* --- S-11.096, S-15.116: the selection of the steps --- */

static struct s15_select sel;
static struct s15_select_info sel_info;
/* The texts sel_info points to. */
static char sel_text[S15_STEP_COUNT][2][S11_VERSION_TEXT_MAX + 1];
static char sel_pack[S11_PATH_MAX];
/* The board's key hook (the menus'), kept while the selection's stands in
 * for it. */
static void (*menu_key)(void *ctx, uint8_t key, bool down);

static uint32_t now_ms(struct opxy_master *m)
{
	return (uint32_t)wb_time_ms(&m->hw.time);
}

/* S-07.031's events while the selection is shown: the menus keep their
 * key state, and the selection reads the event. No other source of keys
 * reaches it, so a host cannot answer. */
static void select_key(void *ctx, uint8_t key, bool down)
{
	struct opxy_master *m = ctx;

	if (menu_key != NULL)
		menu_key(ctx, key, down);
	s15_select_key(&sel, key, down, now_ms(m));
}

static void select_log(struct opxy_master *m, const char *how)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "install: ");
	s14_text_str(&t, how);
	s14_text_str(&t, ", steps selected 0x");
	s14_text_hex(&t, sel.selected, 2);
	s14_log_text(&m->sv.log, S14_INFO, S14_NO_TAG, &t);
}

/* Every PSoC message that has arrived is handled: the receive ring and the
 * link's inbox are empty, or a power-off came (S07_POWEROFF). */
static int psoc_drain(struct opxy_master *m)
{
	struct wb_uart *u = &m->hw.uart[OPXY_UART_PSOC];
	const struct s06_link *l = &m->link[S06_LINK_PSOC];

	for (unsigned i = 0; i < OPXY_PSOC_RING + S06_INBOX; i++) {
		int rc = s07_poll(&m->psoc);

		if (rc != S07_OK)
			return rc;
		if (wb_uart_rx_ready(u) == 0u && l->in_count == 0u)
			break;
	}
	return S07_OK;
}

static bool known(const struct s11_version *v)
{
	return v->major != 0u || v->minor != 0u || v->patch != 0u ||
	       v->build != 0u;
}

/* A row's version texts: the installed one when it is known (have NULL:
 * none), then the package's. */
static void row_versions(struct s15_select_row *r, unsigned step,
			 const struct s11_version *have,
			 const struct s11_version *pkg)
{
	r->installed = NULL;
	if (have != NULL && known(have)) {
		version_str(have, sel_text[step][0]);
		r->installed = sel_text[step][0];
	}
	version_str(pkg, sel_text[step][1]);
	r->package = sel_text[step][1];
}

/* What the rows show (S-15.116), from S-11's view of each step. */
static void select_info(struct opxy_master *m, const struct s11_step_env *env,
			bool te_signed)
{
	static const enum s15_row kind[] = {
		[S11_PICK_ABSENT] = S15_ROW_NONE,
		[S11_PICK_OPEN] = S15_ROW_PICK,
		[S11_PICK_SWITCHED_OFF] = S15_ROW_OFF,
		[S11_PICK_OPTION_OFF] = S15_ROW_OPTION_OFF,
		[S11_PICK_UP_TO_DATE] = S15_ROW_UP_TO_DATE,
	};
	const struct s11_staged *st = env->staged;
	const struct s17_copro *c = env->copro;
	struct s11_version os;
	struct s02_selector selector;

	for (unsigned step = 0; step < S15_STEP_COUNT; step++) {
		sel_info.row[step].kind = kind[s11_step_pick(step, env)];
		sel_info.row[step].installed = sel_info.row[step].package = NULL;
	}
	/* the unit's metadata, as Device Info shows it */
	os.major = m->meta.sw_version[0];
	os.minor = m->meta.sw_version[1];
	os.patch = m->meta.sw_version[2];
	os.build = m->meta.sw_version[3];
	row_versions(&sel_info.row[S11_STEP_OS], S11_STEP_OS,
		     m->meta_failed ? NULL : &os, &st->version[S11_C_OS]);
	row_versions(&sel_info.row[S11_STEP_BOOTLOADER], S11_STEP_BOOTLOADER,
		     NULL, &st->version[S11_C_BOOTLOADER]);
	sel_info.row[S11_STEP_BOOTLOADER].installed = m->sv.boot.version;
	/* S-17.034: the coprocessors' as they reported them */
	row_versions(&sel_info.row[S11_STEP_BLE], S11_STEP_BLE, &c->nordic_app,
		     &st->version[S11_C_BLE]);
	row_versions(&sel_info.row[S11_STEP_BLE_BOOTLOADER],
		     S11_STEP_BLE_BOOTLOADER, &c->nordic_bl,
		     &st->version[S11_C_NORDIC_BL0]);
	row_versions(&sel_info.row[S11_STEP_KEYBOARD], S11_STEP_KEYBOARD,
		     &c->psoc, &st->version[S11_C_BACKPLANE]);
	s11_pack_name(&m->sv.staging, sel_pack, sizeof(sel_pack)); /* S-11.039 */
	sel_info.row[S11_STEP_CONTENT].package = sel_pack[0] != '\0' ? sel_pack
								       : NULL;
	/* S-02.043: the slot the bootloader step would replace */
	sel_info.slot = -1;
	if (s02_read_selector(&m->flash, &selector) == S02_OK)
		sel_info.slot = (int)s02_other_slot(selector.index);
	sel_info.stock = s11_return_to_stock(env->option, te_signed, true);
}

/* The package's TE signature, for the return to stock on the screen. */
static bool sel_te_signed;

/*
 * S-11.096: the steps to run, chosen on the unit before the run's first
 * write. The start-up key read tells whether the keys can be read
 * (S-07.032); when they cannot, nothing is shown or asked. The keys down
 * when the screen is shown, once every event that arrived before it is
 * taken, act only after an up and a down. The PSoC's messages, the
 * Nordic's greetings and the bus are served while it waits.
 */
static enum s11_answer select_ask(void *ctx, const struct s11_step_env *env,
				  uint8_t *selected)
{
	struct opxy_master *m = ctx;
	struct s15_menu *menu = &m->sv.menu;
	const uint8_t pickable = s11_pickable(env);
	uint8_t drawn_sel;
	bool drawn_hold;
	uint32_t drawn;

	if (!m->key_status.replied) {
		s14_log_str(&m->sv.log, S14_INFO, S14_NO_TAG,
			    "install: keys not readable, nothing asked");
		return S11_ANSWER_UNASKED;
	}
	select_info(m, env, sel_te_signed);
	s15_select_open(&sel, pickable, NULL, now_ms(m));
	s15_up_select(menu, &sel_info, &sel, now_ms(m));
	show(m);
	/* What arrived before the screen was shown chooses nothing. */
	if (psoc_drain(m) == S07_POWEROFF)
		goto off;
	s15_select_open(&sel, pickable, m->psoc.keys, now_ms(m));
	drawn_sel = sel.selected;
	drawn_hold = sel.holding;
	drawn = s15_menu_changes(menu);
	menu_key = m->psoc_ops.key_event;
	m->psoc_ops.key_event = select_key;
	while (s15_select_tick(&sel, now_ms(m)) == S15_SELECT_WAIT) {
		const uint32_t t = now_ms(m);

		if (sel.selected != drawn_sel || sel.holding != drawn_hold) {
			drawn_sel = sel.selected;
			drawn_hold = sel.holding;
			s15_up_select(menu, &sel_info, &sel, t);
		} else {
			s15_up_select_time(menu, &sel_info, &sel, t);
		}
		if (s15_menu_changes(menu) != drawn) {
			drawn = s15_menu_changes(menu);
			show(m);
		}
		if (s07_poll(&m->psoc) == S07_POWEROFF)
			goto off;
		s08_answer_greetings(&m->nordic);
		opxy_usb_breathe();
		s06_idle(&m->clock, s06_now(&m->clock) + 1000u);
	}
	m->psoc_ops.key_event = menu_key;
	*selected = sel.selected;
	select_log(m, sel.end == S15_SELECT_END_HELD ? "installing by a 5 s hold" :
		      sel.end == S15_SELECT_END_KEY ? "canceled with key 8" :
						      "canceled after 60 s");
	return sel.choice == S15_SELECT_INSTALL ? S11_ANSWER_INSTALL :
						  S11_ANSWER_CANCEL;
off:
	/* S-07.051: the PSoC removes the power. */
	opxy_powered_off(m);
}

/* --- an install from a package (S-11.020) --- */

/* The package's and the extraction's progress, through the ops' reads and
 * staging writes; ctx stays the reader's. */
static struct {
	struct opxy_master *m;
	const struct s11_package_ops *pkg;
	const struct s11_zip_ops *zip;
	const char *label;		/* the package's phase (S-10c.104) */
	uint32_t size;
	uint64_t done, total;
} prog;

static const char extracting[] = "[extracting firmware]";

/* USB is serviced at every piece of the check and of the extraction
 * (S-10a.049): the host never meets a device that has stopped answering. */
static bool pkg_read(void *ctx, uint32_t off, uint8_t *buf, size_t n)
{
	opxy_usb_breathe();
	if (!prog.pkg->read(ctx, off, buf, n))
		return false;
	s15_up_progress(&prog.m->sv.menu, off + (uint32_t)n, prog.size);
	show(prog.m);
	host_progress(prog.m, off + (uint32_t)n, prog.size, prog.label);
	return true;
}

static uint32_t of_total(uint64_t done, uint64_t total)
{
	/* the ratio, in 32 bits */
	while (total > UINT32_MAX) {
		total >>= 1;
		done >>= 1;
	}
	return (uint32_t)(done < total ? done : total);
}

/* cppcheck-suppress constParameterCallback ; s11_zip_ops fixes the type */
static bool zip_member(void *ctx, size_t index, struct s11_member *mb)
{
	opxy_usb_breathe();
	return prog.zip->member(ctx, index, mb);
}

static bool zip_write(void *ctx, const uint8_t *p, size_t n)
{
	opxy_usb_breathe();
	if (!prog.zip->staging_write(ctx, p, n))
		return false;
	prog.done += n;
	s15_up_progress(&prog.m->sv.menu, of_total(prog.done, prog.total),
			of_total(prog.total, prog.total));
	show(prog.m);
	host_progress(prog.m, of_total(prog.done, prog.total),
		      of_total(prog.total, prog.total), extracting);
	return true;
}

/* The longest the bus went unserviced in this install, for the log. */
static void usb_gap_log(struct opxy_master *m)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "usb: longest gap between services ");
	s14_text_dec(&t, opxy_usb_gap_end() / 1000u);
	s14_text_str(&t, " ms");
	s14_log_text(&m->sv.log, S14_INFO, S14_NO_TAG, &t);
}

static void refused(struct s11_install_result *out, enum s11_reason r)
{
	out->reason = r;
	out->outcome.kind = S11_OUTCOME_REFUSED;
	out->outcome.reason = r;
	out->outcome.step = 0;
	out->outcome.result = s11_reason_result(r);
}

/*
 * s11_install()'s order, with S-15's lines between: the checks, the
 * extraction, the selection of S-11.096, the run's header before its
 * first write, each step, the outcome. The steps run through run_step().
 */
static void install(struct opxy_master *m, const struct s11_package_ops *pkg,
		    const struct s11_zip_ops *zip, const struct s11_check_in *in,
		    struct s11_install_result *out)
{
	struct opxy_services *sv = &m->sv;
	struct s11_package_ops p = *pkg;
	struct s11_zip_ops z = *zip;
	struct s11_check_out checked;
	struct s11_staged_sizes sizes;
	struct s11_member member;
	struct s11_run_cfg cfg;
	char name[S11_PATH_MAX];
	char v[S11_VERSION_TEXT_MAX + 1];
	enum s11_reason r;
	const uint32_t body = in->form == S11_FORM_FILE ? S11_PREAMBLE_LEN : 0u;
	uint8_t device = 0, key = 0;
	size_t i;

	memset(out, 0, sizeof(*out));
	out->run.end = S11_RUN_FAILED;
	out->run.step = 0xFF;
	prog.m = m;
	prog.pkg = pkg;
	prog.zip = zip;
	prog.size = in->size;
	prog.done = prog.total = 0;
	sent.label = NULL;

	/* checks 1-15: S-16.038's one count over the package, named for its
	 * payload (S-10c.104) */
	(void)pkg->read(pkg->ctx, body + KEY_INDEX_AT, &key, 1);
	prog.label = key == KEY_PLAIN ? extracting : "[decrypting firmware]";
	p.read = pkg_read;
	p.crypto = opxy_crypto(m);
	s15_up_phase(&sv->menu, S15_PH_DECRYPT);
	show(m);
	r = s11_check_package(&p, in, &checked);
	opxy_crypto_end();
	out->te_signed = checked.te_signed;
	out->forced = checked.forced;
	if (r != S11_REASON_NONE) {
		refused(out, r);
		usb_gap_log(m);
		s15_up_refused(&sv->menu, s11_reason_name(r));
		show(m);
		return;
	}

	/* checks 16-17: the members' stated sizes are the count's total */
	(void)pkg->read(pkg->ctx, body + 4u, &device, 1);
	for (i = 0; zip_member(zip->ctx, i, &member); i++)
		prog.total += member.size;
	z.staging_write = zip_write;
	z.member = zip_member;
	s15_up_phase(&sv->menu, device == DEVICE_CONTENT ?
			       S15_PH_EXTRACT_CONTENT : S15_PH_EXTRACT_FIRMWARE);
	show(m);
	/* S-11.031: the staging area is replaced from here on, so an earlier
	 * extraction's stated values (S-11.095) no longer describe it. */
	sv->extracted = false;
	opxy_bridge_fast(m);		/* S-09.075: the link, for the bytes to come */
	r = s11_check_zip(&z, &sizes);
	opxy_bridge_report(m);
	if (r != S11_REASON_NONE) {
		opxy_bridge_plain(m);	/* S-09.078: no run */
		refused(out, r);
		/* A refused extraction starts no run; its failed
		 * outcome follows the phase row, the TE header's
		 * device id, not S-11.056's staging area. */
		usb_gap_log(m);
		s15_up_extract_refused(&sv->menu, s11_reason_name(r),
				       device == DEVICE_CONTENT);
		show(m);
		return;
	}

	out->reason = S11_REASON_NONE;
	sv->extracted_sizes = sizes;	/* S-11.095: kept for this run */
	sv->extracted = true;
	opxy_install_begin(m, in->option, checked.forced, 0);
	sel_te_signed = checked.te_signed;
	if (!s11_select(&cur.env, select_ask, m)) {
		/* S-11.096: no run, nothing written; reported as a run that
		 * had nothing to do */
		out->canceled = true;
		out->run.end = S11_RUN_NOTHING_TO_DO;
		s11_run_outcome(&out->run, &out->outcome);
		opxy_bridge_report(m);
		opxy_bridge_plain(m);	/* S-09.078: no run */
		usb_gap_log(m);
		s15_up_canceled(&sv->menu);
		show(m);
		return;
	}
	out->selected = cur.env.selected;
	out->stock = s11_return_to_stock(in->option, checked.te_signed,
					 (cur.env.selected &
					  S11_STEP_BIT(S11_STEP_BOOTLOADER)) != 0u);
	memset(&cfg, 0, sizeof(cfg));
	cfg.content_only = s11_staged_content_only(&cur.st);	/* S-11.056 */
	cfg.nordic = s11_run_nordic(&cur.env);
	cfg.has_version = cur.st.has_fw_version;		/* S-11.038 */
	cfg.version = cur.st.fw_version;
	version_str(&cur.st.fw_version, v);
	s11_pack_name(&sv->staging, name, sizeof(name));	/* S-11.039 */
	s15_up_run_start(&sv->menu,
			 cfg.content_only ? S15_RUN_CONTENT :
			 checked.forced ? S15_RUN_FORCED : S15_RUN_FIRMWARE,
			 cfg.has_version ? v : NULL, name, out->stock);
	show(m);
	(void)s11_run(&sv->run, &cfg, &out->run);
	s11_run_outcome(&out->run, &out->outcome);
	opxy_bridge_report(m);
	opxy_bridge_plain(m);		/* S-09.078: the run has ended */
	usb_gap_log(m);
	run_end(m, out->run.end);
}

/*
 * S-10a.049: the host hold for the whole install, every way it ends, so
 * that no host frame reaches the PSoC or the Nordic in the middle of a
 * step. The CRC peripheral's set-up (check 6's CRC-16 too) is inside the
 * metered span: its search runs with the bus unserviced.
 */
void opxy_install_package(struct opxy_master *m,
			  const struct s11_package_ops *pkg,
			  const struct s11_zip_ops *zip,
			  const struct s11_check_in *in,
			  struct s11_install_result *out)
{
	opxy_host_hold_begin(m);
	opxy_usb_gap_begin();
	opxy_crc_begin(m);
	install(m, pkg, zip, in, out);
	opxy_crc_end();
	opxy_host_hold_end(m);
}

/* --- S-11.080: an install a reset interrupted --- */

/* The state back to 0 and nothing else; held mode's screens say the rest. */
void opxy_install_interrupted(struct opxy_master *m)
{
	const bool cleared = s11_interrupted(&m->sv.run);

	s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG, cleared ?
		    "install: the last one did not finish; state cleared" :
		    "install: the last one did not finish; state not cleared");
}
