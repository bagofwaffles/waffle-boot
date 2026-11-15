/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * zlib's inflate and minizip's unzip (S-11.030), on:
 * - deflate streams, raw and zlib-wrapped, from Python's compressor;
 * - every zip of S-11's zips.json, read from memory, against what Python's
 *   zipfile reads from it (zips.txt).
 *
 * Where a member is not stored or deflated, is encrypted, or fails its
 * CRC-32 or its length, the test checks that minizip reports what S-11.030's
 * refusal needs to see; the refusal itself is the bootloader's. minizip
 * does not refuse other methods or encrypted members on its own, and does
 * not compare a member's length unless it is read to the end.
 */
#define _POSIX_C_SOURCE 200809L
#include "testlib.h"

#include "unzip_mem.h"
#include "zlib.h"

static void check_inflate(int wbits, const uint8_t *comp, size_t comp_len,
			  size_t want_len, const char *want_sha)
{
	uint8_t *out = tl_xmalloc(want_len + 1);
	z_stream zs;
	char sha[65];
	int ret;

	memset(&zs, 0, sizeof(zs));
	CHECK(inflateInit2(&zs, wbits) == Z_OK, "inflateInit2(%d)", wbits);
	zs.next_in = (Bytef *)(uintptr_t)comp;
	zs.avail_in = (uInt)comp_len;
	zs.next_out = out;
	zs.avail_out = (uInt)want_len + 1;
	ret = inflate(&zs, Z_FINISH);
	CHECK(ret == Z_STREAM_END, "inflate: %d (%s)", ret,
	      zs.msg ? zs.msg : "");
	CHECK(zs.total_out == want_len, "inflated %lu bytes, want %zu",
	      zs.total_out, want_len);
	tl_sha256_hex(sha, out, zs.total_out);
	CHECK(!strcmp(sha, want_sha), "inflated data (%zu bytes)", want_len);
	inflateEnd(&zs);

	/* A stream cut short never ends. */
	if (comp_len > 2) {
		memset(&zs, 0, sizeof(zs));
		inflateInit2(&zs, wbits);
		zs.next_in = (Bytef *)(uintptr_t)comp;
		zs.avail_in = (uInt)(comp_len - 2);
		zs.next_out = out;
		zs.avail_out = (uInt)want_len + 1;
		ret = inflate(&zs, Z_FINISH);
		CHECK(ret != Z_STREAM_END, "a truncated stream ended (%d)",
		      ret);
		inflateEnd(&zs);
	}
	free(out);
}

struct member {
	char name[512];
	unsigned long method, flags;
	char status[16];
	char sha[65];
};

/* Reads the member minizip is at, as S-11.030 reads it. */
static void check_member(const char *zip, unzFile uf, const struct member *m)
{
	unz_file_info64 info;
	char name[512];
	uint8_t buf[4096];
	br_sha256_context ctx;
	uint8_t d[32];
	char sha[65];
	unsigned long long total = 0;
	int ret, read_err = 0, close_ret;

	ret = unzGetCurrentFileInfo64(uf, &info, name, sizeof(name), NULL, 0,
				      NULL, 0);
	CHECK(ret == UNZ_OK, "%s: file info", zip);
	CHECK(!strcmp(name, m->name), "%s: member %s, want %s", zip, name,
	      m->name);
	CHECK(info.compression_method == m->method, "%s %s: method %lu", zip,
	      name, info.compression_method);
	CHECK(info.flag == m->flags, "%s %s: flags %lx", zip, name, info.flag);
	if (!strcmp(m->status, "encrypted")) {
		CHECK(info.flag & 1, "%s %s: encryption flag", zip, name);
		return;
	}

	if (!strcmp(m->status, "method")) {
		/*
		 * minizip without bzip2 support opens a bzip2 member and
		 * returns its compressed bytes: the caller must refuse every
		 * method but 0 and 8 from the file info itself.
		 */
		CHECK(info.compression_method != 0 &&
			      info.compression_method != Z_DEFLATED,
		      "%s %s: method %lu", zip, name, info.compression_method);
		return;
	}
	ret = unzOpenCurrentFile(uf);
	CHECK(ret == UNZ_OK, "%s %s: open %d", zip, name, ret);
	if (ret != UNZ_OK)
		return;
	br_sha256_init(&ctx);
	for (;;) {
		int n = unzReadCurrentFile(uf, buf, sizeof(buf));

		if (n < 0)
			read_err = n;
		if (n <= 0)
			break;
		br_sha256_update(&ctx, buf, (size_t)n);
		total += (unsigned int)n;
	}
	br_sha256_out(&ctx, d);
	tl_hex(sha, d, 32);
	close_ret = unzCloseCurrentFile(uf);

	if (!strcmp(m->status, "ok")) {
		CHECK(!read_err && close_ret == UNZ_OK, "%s %s: read %d close %d",
		      zip, name, read_err, close_ret);
		CHECK(total == info.uncompressed_size, "%s %s: length", zip,
		      name);
		CHECK(!strcmp(sha, m->sha), "%s %s: data", zip, name);
	} else if (!strcmp(m->status, "crc")) {
		CHECK(close_ret == UNZ_CRCERROR, "%s %s: CRC not reported (%d)",
		      zip, name, close_ret);
	} else {
		/* Damaged: an error, or fewer bytes than the stated size. */
		CHECK(read_err || close_ret != UNZ_OK ||
			      total != info.uncompressed_size,
		      "%s %s: damage not seen", zip, name);
	}
}

static void check_zip(const char *zip, const uint8_t *raw, size_t len,
		      const struct member *m, int n)
{
	struct wb_unzip_mem mem;
	unzFile uf = wb_unzip_open_mem(&mem, raw, len);
	int i, ret;

	CHECK(uf != NULL, "%s: not opened", zip);
	if (!uf)
		return;
	ret = unzGoToFirstFile(uf);
	for (i = 0; i < n && ret == UNZ_OK; i++) {
		check_member(zip, uf, &m[i]);
		ret = unzGoToNextFile(uf);
	}
	CHECK(i == n && ret == UNZ_END_OF_LIST_OF_FILE, "%s: %d of %d members",
	      zip, i, n);
	CHECK(unzClose(uf) == UNZ_OK, "%s: close", zip);

	/* Without its end-of-central-directory record it is not a zip. */
	uf = wb_unzip_open_mem(&mem, raw, len - 22);
	CHECK(uf == NULL, "%s: opened without its end record", zip);
	if (uf)
		unzClose(uf);
}

int main(void)
{
	FILE *f = tl_open_vectors("zips.txt");
	char *line = NULL, *fld[8], zip[64] = "";
	size_t cap = 0, zlen = 0;
	uint8_t *raw = NULL;
	struct member m[64];
	int n, nm = 0, zips = 0, streams = 0;

	while ((n = tl_fields(f, &line, &cap, fld, 8)) >= 0) {
		if (n == 3 && !strcmp(fld[0], "zip")) {
			snprintf(zip, sizeof(zip), "%s", fld[1]);
			free(raw);
			raw = tl_unhex(fld[2], &zlen);
			nm = 0;
		} else if (n == 6 && !strcmp(fld[0], "member") && nm < 64) {
			size_t len;
			uint8_t *name = tl_unhex(fld[1], &len);

			snprintf(m[nm].name, sizeof(m[nm].name), "%.*s",
				 (int)len, (const char *)name);
			free(name);
			m[nm].method = strtoul(fld[2], NULL, 10);
			m[nm].flags = strtoul(fld[3], NULL, 10);
			snprintf(m[nm].status, sizeof(m[nm].status), "%s",
				 fld[4]);
			snprintf(m[nm].sha, sizeof(m[nm].sha), "%s", fld[5]);
			nm++;
		} else if (n == 1 && !strcmp(fld[0], "end")) {
			check_zip(zip, raw, zlen, m, nm);
			zips++;
		} else if (n == 5 && !strcmp(fld[0], "inflate")) {
			size_t clen;
			uint8_t *comp = tl_unhex(fld[2], &clen);

			check_inflate(atoi(fld[1]), comp, clen,
				      strtoul(fld[3], NULL, 10), fld[4]);
			free(comp);
			streams++;
		}
	}
	free(raw);
	free(line);
	fclose(f);
	CHECK(zips >= 30 && streams >= 30, "%d zips, %d streams", zips,
	      streams);
	return tl_done("test_zip");
}
