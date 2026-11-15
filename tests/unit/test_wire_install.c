/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11 from end to end on the vendored libraries: every package of
 * tests/oracle/S-11-packages/vectors/pkg/ through s11_install() with checks 1-15 on
 * BearSSL and micro-ecc (src/wire_package.c), checks 16-17 on minizip
 * (src/wire_zip.c) and the staging area on a yaffs2 volume in RAM
 * (src/wire_yaffs.c). Against s11_vectors.h's reason, TE-signed and forced
 * verdicts and wire_vectors.h's model results: the reason of checks 16-17,
 * the staged tree (S-11.032) and the version a completed run records
 * (S-11.038, S-04.045 M3), read back by s11_staged_scan() from the volume;
 * each package once with its zip in RAM and once recovered in place in a
 * writable copy (S-11.018), as the board does.
 * The run's steps are fakes that succeed; the steps are test_s11_*'s
 * subject. Also the disk cases of S-11.048: the root laid out on a FAT RAM
 * disk (src/wire_fat.c), the package picked by s11_disk_pick() and
 * installed as FatFs reads it (the zip in place in the file's clusters,
 * wire_fat_open_package()), with the os.ldr staged the package's and not
 * the decoy zip's. Package files are read at run time: run from the
 * repository root (make check does).
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "waffle/s11_content.h"
#include "waffle/s11_package.h"
#include "waffle/s11_run.h"
#include "waffle/s17_revisions.h"
#include "waffle/wire_fat.h"
#include "waffle/wire_package.h"
#include "waffle/wire_yaffs.h"
#include "waffle/wire_zip.h"

#include "vectors/s11_vectors.h"
#include "vectors/wire_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define BLOCKS 64u
#define DISK_SECTORS 454656u		/* S-12.001 */

static struct wb_blockdev bd;
static struct wb_ramdisk ram;
static struct wire_yaffs Y;
static uint8_t zip_ram[1 << 20];
static uint8_t file_buf[1 << 16];

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

/* What the run wrote to the metadata. */
static struct {
	unsigned completes, zero_states;
	struct s11_version version;
} W;

static bool r_meta(void *ctx, uint16_t state)
{
	(void)ctx;
	W.zero_states += state == 0 ? 1u : 0u;
	return true;
}

static bool r_complete(void *ctx, const struct s11_version *v)
{
	(void)ctx;
	W.completes++;
	W.version = *v;
	return true;
}

static bool r_record(void *ctx, uint8_t s)
{
	(void)ctx;
	(void)s;
	return true;
}

static bool r_true(void *ctx)
{
	(void)ctx;
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
	r_meta, r_complete, r_record, r_true, r_true, r_exit, r_step, NULL,
};

static enum s11_result s_content(void *ctx)
{
	(void)ctx;
	return S11_NOT_FOUND;
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
	return true;
}

static int s_slot(void *ctx)
{
	(void)ctx;
	return 0;
}

static const struct s11_step_ops step_ops = {
	s_content, r_true, s_sw, r_true, r_true, NULL, s_send, s_slot, r_true, NULL,
};

/* The files staged under `dir`, each in wirev_files[first .. first+count)
 * with its size and SHA-256; returns how many were found. */
static size_t check_tree(const struct wirev_install *w, const char *dir,
			 const struct s11_fs_ops *fs)
{
	char name[S11_PATH_MAX], path[600], sha[65];
	size_t found = 0;
	bool is_dir;

	for (size_t i = 0; wire_yaffs_list(&Y, dir, i, name, sizeof(name), &is_dir,
					   NULL); i++) {
		const struct wirev_file *f = NULL;
		size_t n;

		snprintf(path, sizeof(path), "%s/%s", dir, name);
		if (is_dir) {
			found += check_tree(w, path, fs);
			continue;
		}
		found++;
		for (size_t k = w->first; k < w->first + w->count; k++) {
			if (!wirev_files[k].is_dir &&
			    !strcmp(wirev_files[k].path, path + strlen("upgrade/")))
				f = &wirev_files[k];
		}
		CHECK(f != NULL, "%s: %s staged, not in the zip", w->name, path);
		if (f == NULL)
			continue;
		n = fs->read(fs->ctx, path, file_buf, sizeof(file_buf));
		tl_sha256_hex(sha, file_buf, n);
		CHECK(n == f->size && !strcmp(sha, f->sha256), "%s: %s contents",
		      w->name, path);
	}
	return found;
}

static uint8_t *load(const char *rel, size_t *len)
{
	char path[sizeof("tests/oracle/S-11-packages/vectors/") + 256];

	snprintf(path, sizeof(path), "tests/oracle/S-11-packages/vectors/%s", rel);
	return tl_read_file(path, len);
}

static void check_input(const struct s11v_package *v,
			const struct wire_source *src, uint32_t len,
			const char *file_name, struct s11_install_result *out)
{
	struct wire_package p;
	struct wire_zip z;
	struct s11_package_ops pkg;
	struct s11_zip_ops zip;
	struct s11_fs_ops fs;
	/* no confirmation answers (S-11.096); every step switched on */
	struct s11_install_ops ops = { &pkg, &zip, &fs, &run_ops, &step_ops,
				       NULL, NULL, 0 };
	struct s11_check_in in;
	struct s17_copro copro;

	/* a source that can be written takes the zip in place (S-11.018), as
	 * on the board */
	wire_package_init(&p, &pkg, src, &otp, (void *)(uintptr_t)v,
			  src->write != NULL ? NULL : zip_ram,
			  src->write != NULL ? 0u : sizeof(zip_ram));
	wire_zip_init(&z, &zip, &p, NULL, 0, &Y);
	wire_yaffs_fs_ops(&Y, &fs);
	in.form = v->file_form ? S11_FORM_FILE : S11_FORM_BODY;
	in.size = len;
	in.option = v->option != 0;
	in.file_name = file_name;
	in.fw_command = v->cdc != 0;
	memset(&W, 0, sizeof(W));
	s17_copro_init(&copro);
	CHECK(wire_yaffs_staging_reset(&Y), "%s: reset", v->name);
	s11_install(&ops, &in, &copro, out);
	wire_zip_close(&z);
}

/* A writable package in memory, for the zip in place. */
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

static void check_install(const struct s11v_package *v,
			  const struct wirev_install *w, bool in_place)
{
	struct s11_fs_ops fs;
	struct wire_mem mem;
	struct wire_source src = { wire_mem_read, &mem, NULL };
	struct s11_install_result out;
	enum s11_reason want = v->reason != S11_REASON_NONE ? v->reason :
			       w->zip_reason;
	char path[256];
	size_t len, files = 0;
	uint8_t *data;
	bool dir;
	uint32_t size;

	data = load(v->file, &len);
	mem.buf = data;
	mem.len = len;
	if (in_place) {
		rw_buf = data;
		rw_len = len;
		src.write = rw_write;
	}
	wire_yaffs_fs_ops(&Y, &fs);
	check_input(v, &src, (uint32_t)len, v->file_name, &out);
	CHECK(out.reason == want, "%s: %s, want %s", v->name,
	      s11_reason_name(out.reason), s11_reason_name(want));
	if (v->te_signed >= 0)
		CHECK(out.te_signed == (v->te_signed != 0), "%s: TE-signed",
		      v->name);
	if (v->reason == S11_REASON_NONE && v->forced >= 0)
		CHECK(out.forced == (v->forced != 0), "%s: forced", v->name);
	if (out.reason != S11_REASON_NONE) {
		CHECK(W.completes == 0 && out.run.end == S11_RUN_FAILED &&
		      out.outcome.kind == S11_OUTCOME_REFUSED,
		      "%s: refused, no run", v->name);
		goto out;
	}

	/* the staged tree is the model's */
	if (wire_yaffs_stat(&Y, "upgrade", &dir, &size))
		files = check_tree(w, "upgrade", &fs);
	for (size_t k = w->first; k < w->first + w->count; k++) {
		const struct wirev_file *f = &wirev_files[k];

		snprintf(path, sizeof(path), "upgrade/%s", f->path);
		if (f->is_dir)
			CHECK(wire_yaffs_stat(&Y, path, &dir, &size) && dir,
			      "%s: directory %s staged", v->name, f->path);
		else
			files--;
	}
	CHECK(files == 0, "%s: the files staged are the zip's", v->name);

	/* S-11.038, S-11.064: the version recorded is firmware.version's */
	CHECK(out.run.end != S11_RUN_FAILED, "%s: the run completes", v->name);
	if (w->has_version && !w->content_only)
		CHECK(W.completes == 1 && W.version.major == w->version[0] &&
		      W.version.minor == w->version[1] &&
		      W.version.patch == w->version[2] &&
		      W.version.build == w->version[3],
		      "%s: recorded %u.%u.%u.%u, want %u.%u.%u.%u", v->name,
		      (unsigned)W.version.major, (unsigned)W.version.minor,
		      (unsigned)W.version.patch, (unsigned)W.version.build,
		      w->version[0], w->version[1], w->version[2],
		      w->version[3]);
	else
		CHECK(W.completes == 0 && W.zero_states != 0,
		      "%s: no version recorded, the state only", v->name);
out:
	free(data);
}

/* S-11.048: the disk's root laid out in order, the package picked from it
 * and installed as the FAT volume gives it. */
static void check_disk(const struct wirev_disk *d,
		       const struct wb_blockdev *disk,
		       const struct s11v_package *unit)
{
	static struct wire_fat fat;
	struct s11_fs_ops fs;
	struct wire_source src;
	struct s11_install_result out;
	char name[S11_PATH_MAX], sha[65];
	bool picked;
	uint32_t size = 0;

	CHECK(wire_fat_format(&fat, disk), "%s: format", d->name);
	for (const char *p = d->entries; *p != '\0';) {
		size_t n = strcspn(p, "\n"), k = strcspn(p, "="), len;
		char entry[S11_PATH_MAX], file[S11_PATH_MAX];
		uint8_t *data;

		snprintf(entry, sizeof(entry), "%.*s", (int)k, p);
		snprintf(file, sizeof(file), "%.*s", (int)(n - k - 1), p + k + 1);
		data = load(file, &len);
		CHECK(wire_fat_write_file(&fat, entry, data, len, false, 0),
		      "%s: %s on the disk", d->name, entry);
		free(data);
		p += n + (p[n] == '\n');
	}
	picked = s11_disk_pick(wire_fat_root, &fat, name);
	CHECK(d->package == NULL ? !picked :
	      picked && !strcmp(name, d->package), "%s: package picked",
	      d->name);
	/* as the scan opens it: the zip recovered in place (S-11.018) */
	if (!picked || !wire_fat_open_package(&fat, name, &src, &size))
		goto out;
	check_input(unit, &src, size, name, &out);
	wire_fat_close_file(&fat);
	CHECK(out.reason == d->reason, "%s: %s, want %s", d->name,
	      s11_reason_name(out.reason), s11_reason_name(d->reason));
	if (d->os_sha256 == NULL)
		goto out;
	wire_yaffs_fs_ops(&Y, &fs);
	size = (uint32_t)fs.read(fs.ctx, "upgrade/firmware/blackfin/bin/os.ldr",
				 file_buf, sizeof(file_buf));
	tl_sha256_hex(sha, file_buf, size);
	CHECK(!strcmp(sha, d->os_sha256) && strcmp(sha, d->decoy_os_sha256),
	      "%s: the os.ldr staged is the package's", d->name);
out:
	wire_fat_unmount(&fat);
}

int main(void)
{
	size_t len = (size_t)BLOCKS * 128u * 4096u;
	uint8_t *vol = calloc(1, len);

	if (vol == NULL)
		return 2;
	wb_ramdisk_init(&bd, &ram, vol, (uint32_t)(len / 512u));
	CHECK(wire_yaffs_add(&Y, "/yaffs2", &bd, 0, BLOCKS - 1) &&
	      wire_yaffs_mount(&Y) == WIRE_MOUNT_OK, "mount");
	CHECK(N(s11v_packages) == N(wirev_installs), "one model result a package");
	for (size_t i = 0; i < N(s11v_packages) && i < N(wirev_installs); i++) {
		CHECK(!strcmp(s11v_packages[i].name, wirev_installs[i].name),
		      "%s: in order", wirev_installs[i].name);
		check_install(&s11v_packages[i], &wirev_installs[i], false);
		check_install(&s11v_packages[i], &wirev_installs[i], true);
	}
	{
		struct wb_blockdev disk;
		struct wb_ramdisk ram_disk;
		uint8_t *mem = calloc(DISK_SECTORS, 512u);
		const struct s11v_package *unit = NULL;

		for (size_t i = 0; i < N(s11v_packages); i++)
			if (!strcmp(s11v_packages[i].name, "form_file_ok"))
				unit = &s11v_packages[i];
		CHECK(mem != NULL && unit != NULL, "RAM disk");
		if (mem != NULL && unit != NULL) {
			wb_ramdisk_init(&disk, &ram_disk, mem, DISK_SECTORS);
			for (size_t i = 0; i < N(wirev_disk); i++)
				check_disk(&wirev_disk[i], &disk, unit);
		}
		free(mem);
	}
	wire_yaffs_unmount(&Y);
	free(vol);
	return tl_done("test_wire_install");
}
