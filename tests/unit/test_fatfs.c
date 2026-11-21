/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FatFs on the RAM disk (S-12 section 1), with the configuration of S-12
 * section 3, against fat.txt:
 * - the format of wb_fatfs_format (S-12.003 to S-12.007): sectors 0, 63,
 *   64 and 175 as the vectors give them, the FAT and root directory zero
 *   beyond that, and no other sector written;
 * - the OP-1F's format (S-12.008): its table's values on a disk of 260,096
 *   sectors, the label OP-1, and no sector written but 0, 63 and 64 to 222;
 * - the volumes a host may leave on the disk (S-12.020, S-12.021) and the
 *   populated RAM disk, each read to its tree;
 * - files written and read back on a fresh volume.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "ff.h"
#include "wb_fatfs.h"

#define MAX_ENTRIES 64

struct entry {
	char path[600];
	char type[16];
	unsigned long len;
	char sha[65];
};

static uint8_t *disk;
static uint32_t disk_sectors;
static struct wb_blockdev bdev;
static struct wb_ramdisk ram;

static uint8_t *sector(uint32_t n)
{
	return disk + (size_t)n * WB_SECTOR_SIZE;
}

/* Whether len bytes of disk d from sector s, offset off, are all value. */
static int is_filled_in(const uint8_t *d, uint32_t s, size_t off, size_t len,
			uint8_t value)
{
	const uint8_t *p = d + (size_t)s * WB_SECTOR_SIZE + off;
	size_t i;

	for (i = 0; i < len; i++)
		if (p[i] != value)
			return 0;
	return 1;
}

static int is_filled(uint32_t first, uint32_t last, uint8_t value)
{
	size_t i, n = (size_t)(last - first + 1) * WB_SECTOR_SIZE;
	const uint8_t *p = sector(first);

	for (i = 0; i < n; i++)
		if (p[i] != value)
			return 0;
	return 1;
}

/* Compares the first len bytes of sector s, naming each differing byte. */
static void check_bytes(uint32_t s, const uint8_t *want, size_t len)
{
	const uint8_t *got = sector(s);
	size_t i;

	for (i = 0; i < len; i++)
		CHECK(got[i] == want[i], "sector %lu byte 0x%03zx: %02x, want %02x",
		      (unsigned long)s, i, got[i], want[i]);
}

static void check_format(FILE *f)
{
	static uint8_t work[FF_MAX_SS * 4];
	char *line = NULL, *fld[4];
	size_t cap = 0;
	uint32_t prev_end = 0;
	FATFS fs;
	int n;

	/* Every sector the format does not write keeps this pattern. */
	memset(disk, 0xA5, (size_t)disk_sectors * WB_SECTOR_SIZE);
	CHECK(wb_fatfs_format(work, sizeof(work), "OP-XY") == FR_OK, "format");
	CHECK(f_mount(&fs, "", 1) == FR_OK, "mount after format");
	CHECK(fs.fs_type == FS_FAT16, "FAT type %u", fs.fs_type);
	CHECK(fs.n_fatent - 2 == 28403, "%lu clusters",
	      (unsigned long)fs.n_fatent - 2);
	CHECK(fs.fatbase == 64 && fs.dirbase == 175 && fs.database == 207,
	      "layout %lu %lu %lu", (unsigned long)fs.fatbase,
	      (unsigned long)fs.dirbase, (unsigned long)fs.database);
	CHECK(f_mount(NULL, "", 0) == FR_OK, "unmount");

	while ((n = tl_fields(f, &line, &cap, fld, 4)) >= 0) {
		if (n == 3 && !strcmp(fld[0], "sector")) {
			uint32_t s = (uint32_t)strtoul(fld[1], NULL, 10);
			uint8_t want[512];

			if (tl_unhex_n(fld[2], want, sizeof(want)))
				exit(2);
			check_bytes(s, want, sizeof(want));
		} else if (n == 3 && !strcmp(fld[0], "head")) {
			uint32_t s = (uint32_t)strtoul(fld[1], NULL, 10);
			size_t len;
			uint8_t *want = tl_unhex(fld[2], &len);

			check_bytes(s, want, len);
			free(want);
		} else if (n == 3 && !strcmp(fld[0], "written")) {
			uint32_t a = (uint32_t)strtoul(fld[1], NULL, 10);
			uint32_t b = (uint32_t)strtoul(fld[2], NULL, 10);

			/* Nothing between the written ranges was touched. */
			if (a > prev_end + 1)
				CHECK(is_filled(prev_end + 1, a - 1, 0xA5),
				      "sectors %lu-%lu written",
				      (unsigned long)prev_end + 1,
				      (unsigned long)a - 1);
			prev_end = b;
		} else if (n == 1 && !strcmp(fld[0], "volume")) {
			break;
		}
		if (n >= 1 && !strcmp(fld[0], "volume"))
			break;
	}
	CHECK(prev_end == 206, "written ranges end at %lu",
	      (unsigned long)prev_end);
	CHECK(is_filled(207, disk_sectors - 1, 0xA5), "data area written");
	/* The FAT beyond its first entries, and the root after the label. */
	CHECK(sector(64)[4] == 0 && is_filled(65, 174, 0), "FAT not zero");
	CHECK(is_filled(176, 206, 0), "root directory not zero");
	free(line);
}

static int cmp_entry(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->path,
		      ((const struct entry *)b)->path);
}

static void hash_file(const char *path, struct entry *e)
{
	br_sha256_context ctx;
	uint8_t buf[4096], d[32];
	FIL fil;
	UINT got;

	br_sha256_init(&ctx);
	e->len = 0;
	if (f_open(&fil, path, FA_READ) != FR_OK) {
		CHECK(0, "cannot open %s", path);
		return;
	}
	while (f_read(&fil, buf, sizeof(buf), &got) == FR_OK && got) {
		br_sha256_update(&ctx, buf, got);
		e->len += got;
	}
	f_close(&fil);
	br_sha256_out(&ctx, d);
	tl_hex(e->sha, d, 32);
}

/* Lists dir (without a trailing /) into e[*n...]. */
static void walk(const char *dir, struct entry *e, int *n)
{
	FILINFO fno;
	DIR d;

	if (f_opendir(&d, dir) != FR_OK) {
		CHECK(0, "cannot open directory %s", dir);
		return;
	}
	while (f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
		struct entry *x;

		if (*n >= MAX_ENTRIES) {
			CHECK(0, "too many entries");
			break;
		}
		x = &e[(*n)++];
		snprintf(x->path, sizeof(x->path), "%s%s%s", dir,
			 *dir ? "/" : "", fno.fname);
		if (fno.fattrib & AM_DIR) {
			snprintf(x->type, sizeof(x->type), "dir");
			walk(x->path, e, n);
		} else {
			snprintf(x->type, sizeof(x->type), "file");
			hash_file(x->path, x);
		}
	}
	f_closedir(&d);
}

static void load_image(const char *file)
{
	char path[256];
	FILE *img;
	size_t size = (size_t)disk_sectors * WB_SECTOR_SIZE;

	snprintf(path, sizeof(path), "s12/%s", file);
	img = fopen(tl_vector_path(path), "rb");
	if (!img || fread(disk, 1, size, img) != size) {
		printf("cannot read %s\n", tl_vector_path(path));
		exit(2);
	}
	fclose(img);
}

static void check_volume(const char *kind, int readable, struct entry *want,
			 int n_want)
{
	struct entry got[MAX_ENTRIES];
	int n = 0, i;
	FATFS fs;
	FRESULT fr = f_mount(&fs, "", 1);

	if (!readable) {
		CHECK(fr == FR_NO_FILESYSTEM, "%s: mount gave %d", kind, fr);
		f_mount(NULL, "", 0);
		return;
	}
	CHECK(fr == FR_OK, "%s: mount gave %d", kind, fr);
	if (fr != FR_OK) {
		f_mount(NULL, "", 0);
		return;
	}
	walk("", got, &n);
	f_mount(NULL, "", 0);
	qsort(got, (size_t)n, sizeof(got[0]), cmp_entry);
	qsort(want, (size_t)n_want, sizeof(want[0]), cmp_entry);
	CHECK(n == n_want, "%s: %d entries, want %d", kind, n, n_want);
	for (i = 0; i < n && i < n_want; i++) {
		CHECK(!strcmp(got[i].path, want[i].path) &&
			      !strcmp(got[i].type, want[i].type),
		      "%s: entry %s %s, want %s %s", kind, got[i].path,
		      got[i].type, want[i].path, want[i].type);
		if (!strcmp(want[i].type, "file"))
			CHECK(got[i].len == want[i].len &&
				      !strcmp(got[i].sha, want[i].sha),
			      "%s: contents of %s", kind, want[i].path);
	}
}

static void check_volumes(FILE *f)
{
	struct entry want[MAX_ENTRIES];
	char *line = NULL, *fld[6], kind[64] = "";
	size_t cap = 0;
	int n, n_want = 0, readable = 0, volumes = 0;

	rewind(f);
	while ((n = tl_fields(f, &line, &cap, fld, 6)) >= 0) {
		if (n == 4 && !strcmp(fld[0], "volume")) {
			snprintf(kind, sizeof(kind), "%s", fld[1]);
			readable = !strcmp(fld[3], "1");
			load_image(fld[2]);
			n_want = 0;
		} else if (n >= 3 && !strcmp(fld[0], "entry") &&
			   n_want < MAX_ENTRIES) {
			struct entry *e = &want[n_want++];
			size_t len;
			uint8_t *p = tl_unhex(fld[1], &len);

			memset(e, 0, sizeof(*e));
			snprintf(e->path, sizeof(e->path), "%.*s", (int)len,
				 (const char *)p);
			free(p);
			snprintf(e->type, sizeof(e->type), "%s", fld[2]);
			if (n == 5) {
				e->len = strtoul(fld[3], NULL, 10);
				snprintf(e->sha, sizeof(e->sha), "%s", fld[4]);
			}
		} else if (n == 1 && !strcmp(fld[0], "end") && *kind) {
			check_volume(kind, readable, want, n_want);
			volumes++;
			*kind = '\0';
		}
	}
	free(line);
	CHECK(volumes >= 10, "only %d volumes", volumes);
}

/* Writes files with long and short names on a fresh volume, reads them back. */
static void check_write(void)
{
	static const char *const names[] = {
		"systeminfo/version.txt", "systeminfo/serial.txt",
		"systeminfo/licenses.txt", "LOGS/BOOT.LOG",
		"logs/Mixed Case name.Log", "logs/caf\xc3\xa9 \xe2\x82\xac.txt",
	};
	static uint8_t work[FF_MAX_SS * 4];
	uint8_t data[20000], back[20000];
	FATFS fs;
	FIL fil;
	FILINFO fno;
	UINT done;
	size_t i, k;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 31 + (i >> 9));
	memset(disk, 0, (size_t)disk_sectors * WB_SECTOR_SIZE);
	CHECK(wb_fatfs_format(work, sizeof(work), "OP-XY") == FR_OK, "format");
	CHECK(f_mount(&fs, "", 1) == FR_OK, "mount");
	CHECK(f_mkdir("systeminfo") == FR_OK, "mkdir systeminfo");
	CHECK(f_mkdir("logs") == FR_OK, "mkdir logs");
	CHECK(f_mkdir("LOGS") == FR_EXIST, "names are case-insensitive");
	for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
		size_t len = 1000 + 3000 * k;

		CHECK(f_open(&fil, names[k], FA_WRITE | FA_CREATE_ALWAYS) ==
			      FR_OK, "create %s", names[k]);
		CHECK(f_write(&fil, data + k, (UINT)len, &done) == FR_OK &&
			      done == len, "write %s", names[k]);
		CHECK(f_close(&fil) == FR_OK, "close %s", names[k]);
	}
	/* S-12.016: an explicit write stamp, 2026-10-28 12:34:56. */
	fno.fdate = (WORD)((2026 - 1980) << 9 | 10 << 5 | 28);
	fno.ftime = (WORD)(12 << 11 | 34 << 5 | 56 / 2);
	CHECK(f_utime(names[0], &fno) == FR_OK, "f_utime");
	CHECK(f_mount(NULL, "", 0) == FR_OK, "unmount");

	CHECK(f_mount(&fs, "", 1) == FR_OK, "remount");
	for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
		size_t len = 1000 + 3000 * k;

		CHECK(f_stat(names[k], &fno) == FR_OK && fno.fsize == len,
		      "stat %s", names[k]);
		CHECK(f_open(&fil, names[k], FA_READ) == FR_OK, "open %s",
		      names[k]);
		CHECK(f_read(&fil, back, sizeof(back), &done) == FR_OK &&
			      done == len && !memcmp(back, data + k, len),
		      "read %s", names[k]);
		f_close(&fil);
	}
	CHECK(f_stat(names[0], &fno) == FR_OK &&
		      fno.fdate == ((2026 - 1980) << 9 | 10 << 5 | 28) &&
		      fno.ftime == (12 << 11 | 34 << 5 | 28),
	      "explicit stamp");
	CHECK(f_stat(names[1], &fno) == FR_OK && fno.fdate == 0x0021 &&
		      fno.ftime == 0, "fixed-clock stamp (S-12.015)");
	CHECK(f_unlink(names[3]) == FR_OK, "unlink");
	CHECK(f_stat(names[3], &fno) == FR_NO_FILE, "unlinked");
	CHECK(f_mount(NULL, "", 0) == FR_OK, "unmount");
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

/* S-12.008: the format on the OP-1F's disk, with its label (S-12.006). */
static void check_format_1f(void)
{
	static const uint8_t label[11] = { 'O', 'P', '-', '1', ' ', ' ', ' ', ' ',
					   ' ', ' ', ' ' };
	static uint8_t work[FF_MAX_SS * 4];
	const uint32_t n = 260096u;
	uint8_t *d = tl_xmalloc((size_t)n * WB_SECTOR_SIZE);
	struct wb_blockdev b;
	struct wb_ramdisk r;
	const uint8_t *s0 = d, *s63 = d + 63u * WB_SECTOR_SIZE;
	const uint8_t *s191 = d + 191u * WB_SECTOR_SIZE;
	FATFS fs;

	memset(d, 0xA5, (size_t)n * WB_SECTOR_SIZE);
	wb_ramdisk_init(&b, &r, d, n);
	wb_fatfs_attach(&b);
	CHECK(wb_fatfs_format(work, sizeof(work), "OP-1") == FR_OK, "1F format");
	CHECK(s0[0x1C3] == 0xFE && s0[0x1C4] == 0x3F && s0[0x1C5] == 0x10,
	      "1F end CHS %02x %02x %02x", s0[0x1C3], s0[0x1C4], s0[0x1C5]);
	CHECK(le32(s0 + 0x1C6) == 63u && le32(s0 + 0x1CA) == 260033u,
	      "1F partition %lu", (unsigned long)le32(s0 + 0x1CA));
	CHECK(s63[0x0D] == 8 && s63[0x16] == 127 && s63[0x17] == 0 &&
	      le32(s63 + 0x20) == 260033u && s63[0x10] == 1,
	      "1F BPB: %u per cluster, %u per FAT", s63[0x0D], s63[0x16]);
	CHECK(memcmp(s63 + 0x2B, "NO NAME    ", 11) == 0, "1F boot sector label");
	CHECK(memcmp(s191, label, 11) == 0 && s191[11] == 0x08 &&
	      is_filled_in(d, 191, 12, 20, 0), "1F label entry");
	CHECK(f_mount(&fs, "", 1) == FR_OK && fs.fs_type == FS_FAT16 &&
	      fs.n_fatent - 2 == 32484 && fs.csize == 8 && fs.fatbase == 64 &&
	      fs.dirbase == 191 && fs.database == 223, "1F layout");
	CHECK(f_mount(NULL, "", 0) == FR_OK, "1F unmount");
	/* S-12.007 with S-12.008: sectors 0, 63 and 64-222 alone */
	CHECK(is_filled_in(d, 1, 0, 62 * WB_SECTOR_SIZE, 0xA5) &&
	      is_filled_in(d, 223, 0, (size_t)(n - 223) * WB_SECTOR_SIZE, 0xA5),
	      "1F: no other sector written");
	CHECK(is_filled_in(d, 65, 0, (size_t)(190 - 64) * WB_SECTOR_SIZE, 0) &&
	      is_filled_in(d, 192, 0, (size_t)(222 - 191) * WB_SECTOR_SIZE, 0),
	      "1F: FAT and root directory zero");
	wb_fatfs_attach(&bdev);
	free(d);
}

int main(void)
{
	FILE *f = tl_open_vectors("fat.txt");
	char *line = NULL, *fld[2];
	size_t cap = 0;

	if (tl_fields(f, &line, &cap, fld, 2) != 2 || strcmp(fld[0], "disk"))
		return 2;
	disk_sectors = (uint32_t)strtoul(fld[1], NULL, 10);
	free(line);
	CHECK(disk_sectors == 454656, "disk of %lu sectors",
	      (unsigned long)disk_sectors);
	disk = tl_xmalloc((size_t)disk_sectors * WB_SECTOR_SIZE);
	wb_ramdisk_init(&bdev, &ram, disk, disk_sectors);
	wb_fatfs_attach(&bdev);

	check_format(f);
	check_format_1f();
	check_volumes(f);
	check_write();

	fclose(f);
	free(disk);
	return tl_done("test_fatfs");
}
