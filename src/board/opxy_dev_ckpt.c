/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * OPXY_DEV_CKPT (waffle/devhost.h): the read-only checkpoint test of the
 * slave's volume (waffle/wire_ckpt.h), run on the master. The test device
 * reads the user area over the bridge's sector reads (S-09.040), which the
 * eMMC developer requests use too, and lives in the master's memory, so the
 * slave's yaffs2 state is not touched: its device, handles and heap stay as
 * they are, and it serves the reads and the tree walk's file requests as any
 * others. The scanned tree is the slave's mount, walked over the bridge.
 */
#include <stdlib.h>
#include <string.h>

#include "waffle/board/opxy_boot.h"
#include "waffle/bytes.h"
#include "waffle/devhost.h"
#include "waffle/wire_ckpt.h"
#include "wb_yaffs.h"

#define VOLUME "/yaffs2"
#define PATH_LEN S09_FILES_PATH_MAX
/* Room the test device's yaffs2 memory needs at most, asked of the heap
 * before the mount: block records, chunk bits, caches, the objects and
 * tnodes of a full volume. */
#define PROBE_BYTES (8u << 20)

static struct opxy_master *ck;
static struct s03_emmc_ops emmc;
static struct wb_blockdev raw;
static struct wire_ckpt_report report;
static bool have_report;

static int raw_read(void *ctx, uint32_t lba, void *buf, uint32_t count)
{
	(void)ctx;
	return emmc.read(emmc.ctx, lba, count, buf) == 0 ? 0 : -1;
}

static bool full(const char *rel, char *out)
{
	size_t m = sizeof VOLUME - 1u, r = wb_strlen(rel);

	if (m + 1u + r >= PATH_LEN)
		return false;
	memcpy(out, VOLUME, m);
	if (r != 0u) {
		out[m++] = '/';
		memcpy(out + m, rel, r);
	}
	out[m + r] = '\0';
	return true;
}

static bool scan_list(void *ctx, const char *dir, bool (*each)(void *ectx, const char *name),
		      void *ectx)
{
	char path[PATH_LEN];

	(void)ctx;
	return full(dir, path) && s09_files_list(&ck->sv.files, path, each, ectx);
}

#define MODE_FMT 0170000u

static bool scan_stat(void *ctx, const char *rel, struct wire_ckpt_obj *o)
{
	char path[PATH_LEN];
	struct s09_stat st;

	(void)ctx;
	if (!full(rel, path) || s09_files_stat(&ck->sv.files, path, &st) != 0)
		return false;
	o->ino = st.ino;
	switch (st.mode & MODE_FMT) {
	case 0040000u:
		o->type = 4u;
		break;
	case 0100000u:
		o->type = 8u;
		break;
	case 0120000u:
		o->type = 10u;
		break;
	default:
		o->type = 0u;
		break;
	}
	o->size = st.size;
	o->parent = 0;
	return true;
}

static const struct wire_ckpt_tree_ops scan_ops = { NULL, scan_list, scan_stat };

static uint64_t now_us(void)
{
	return wb_time_us(&ck->hw.time);
}

static unsigned texts(void)
{
	const unsigned d = report.diffs < WIRE_CKPT_KEPT ? report.diffs : WIRE_CKPT_KEPT;
	const unsigned g = report.disagree < WIRE_CKPT_KEPT ? report.disagree : WIRE_CKPT_KEPT;

	return 1u + d + g;
}

static const char *text(unsigned i)
{
	const unsigned d = report.diffs < WIRE_CKPT_KEPT ? report.diffs : WIRE_CKPT_KEPT;

	if (i == 0u)
		return report.why;
	if (i <= d)
		return report.diff[i - 1u];
	return report.disagreement[i - 1u - d];
}

static size_t summary(uint8_t *out)
{
	const struct wire_ckpt_report *r = &report;
	const uint32_t w[14] = {
		r->ckpt_blocks, r->mount_reads, r->mount_sectors, r->mount_us,
		r->writes_refused, r->objs_scan, r->objs_ckpt, r->diffs, r->ckpt_seq,
		(uint32_t)r->alloc_block, r->alloc_page, r->newer, r->disagree, r->test_us,
	};
	uint8_t *p = out + 1;

	*p++ = (uint8_t)r->outcome;
	*p++ = r->accepted ? 1u : 0u;
	*p++ = r->unmounted ? 1u : 0u;
	*p++ = (uint8_t)((r->scan_cut ? 1u : 0u) | (r->ckpt_cut ? 2u : 0u));
	*p++ = r->alloc_used ? 1u : 0u;
	for (unsigned i = 0; i < 14u; i++, p += 4)
		wb_put_le32(p, w[i]);
	*p++ = (uint8_t)texts();
	return (size_t)(p - out);
}

/* What keeps the test from running, as a status; OPXY_DEV_OK when nothing. */
static uint8_t refused(struct opxy_master *m)
{
	struct s09_statfs st;

	if (!m->slave_present || m->bridge.absent || s09_files_slave_lost(&m->sv.files))
		return OPXY_DEV_NO_SLAVE;
	if (!m->sv.files.mounted || s09_files_volume_refused(&m->sv.files) ||
	    s09_files_statfs(&m->sv.files, VOLUME, &st) != 0)
		return OPXY_DEV_REFUSED;
	/* a file being written may hold data the eMMC does not have yet */
	if (m->sv.files.stage_fd >= 0 || m->sv.menu.mtp != S15_MTP_OFF || m->sv.working)
		return OPXY_DEV_REFUSED;
	return OPXY_DEV_OK;
}

size_t opxy_dev_ckpt(struct opxy_master *m, const uint8_t *req, size_t n, uint8_t *out)
{
	const unsigned part = n >= 1u ? req[0] : 0xFFu;
	void *probe;

	if (n != 1u) {
		out[0] = OPXY_DEV_BAD;
		return 1u;
	}
	if (part != 0u) {
		const char *s;
		size_t len;

		if (!have_report || part > texts()) {
			out[0] = OPXY_DEV_BAD;
			return 1u;
		}
		s = text(part - 1u);
		len = wb_strlen(s);
		memcpy(out + 1, s, len + 1u);
		return 2u + len;
	}
	out[0] = refused(m);
	if (out[0] != OPXY_DEV_OK)
		return 1u;
	ck = m;
	s09_files_emmc_ops(&m->sv.files, &emmc);
	raw.ctx = NULL;
	raw.n_sectors = (WB_YAFFS_LAST_BLOCK + 1u) * WB_YAFFS_SECTORS_PER_BLOCK;
	raw.read = raw_read;
	raw.write = NULL;
	raw.erase = NULL;
	raw.erase_zeroes = NULL;
	raw.write_reliable = NULL;
	probe = malloc(PROBE_BYTES);
	if (probe == NULL) {
		memset(&report, 0, sizeof report);
		report.outcome = WIRE_CKPT_NO_MEMORY;
	} else {
		free(probe);
		(void)wire_ckpt_run(&raw, 0u, WB_YAFFS_LAST_BLOCK, &scan_ops, now_us, &report);
	}
	have_report = true;
	return summary(out);
}
