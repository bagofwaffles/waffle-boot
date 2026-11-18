/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-09.081 on the board: the OS image's copy read again once when its CRC
 * fails after sector data came in quad I/O (opxy_bridge_load_os()), and not
 * otherwise; and the developer request's answer (OPXY_DEV_QUAD_DATA,
 * opxy_bridge_qdata_report()). The quad data's own drop and the reads sent
 * again on single I/O are test_s09_qdata.c's.
 */
#include <string.h>

#include "s06_test.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_bridge.h"
#include "waffle/bytes.h"
#include "waffle/devhost.h"
#include "waffle/s03_emmc.h"
#include "waffle/wire_crc.h"

static struct opxy_master m;

#define IMAGE_LEN 70000u
static uint8_t bp1[S03_SECTOR + 140u * S03_SECTOR];
static uint8_t staging[S03_OS_MAX];
static _Alignas(4) uint8_t scratch[4096];
static unsigned image_reads;	/* reads of the image's first sector */
static unsigned bad_reads;	/* those reads that come back wrong */

static int em_select(void *ctx, unsigned part)
{
	(void)ctx;
	return part <= S03_PART_BP2 ? 0 : -1;
}

static int em_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	(void)ctx;
	if ((uint64_t)(sector + count) * S03_SECTOR > sizeof bp1)
		return -1;
	memcpy(buf, bp1 + (size_t)sector * S03_SECTOR, (size_t)count * S03_SECTOR);
	if (sector == 1u) {
		image_reads++;
		if (bad_reads != 0u) {
			bad_reads--;
			buf[700] ^= 0x01u;
		}
	}
	return 0;
}

static int em_write(void *ctx, uint32_t s, uint32_t c, const uint8_t *b)
{
	(void)ctx; (void)s; (void)c; (void)b;
	return -1;
}

static int em_erase(void *ctx, uint32_t f)
{
	(void)ctx; (void)f;
	return -1;
}

static int em_switch(void *ctx, uint8_t i, uint8_t v)
{
	(void)ctx; (void)i; (void)v;
	return 0;
}

static const struct s03_emmc_ops ops = {
	NULL, em_select, em_read, em_write, em_erase, em_switch,
};

static void fresh(uint32_t replies, unsigned bad)
{
	uint8_t hdr[S03_SECTOR];

	memset(&m, 0, sizeof m);
	for (uint32_t i = 0; i < IMAGE_LEN; i++)
		bp1[S03_SECTOR + i] = (uint8_t)(i * 31u + (i >> 8));
	s03_header_encode(IMAGE_LEN, wire_crc32(0u, bp1 + S03_SECTOR, IMAGE_LEN), 1u, hdr);
	memcpy(bp1, hdr, sizeof hdr);
	m.emmc.ops = &ops;
	m.emmc.crc32 = wire_crc32;
	m.emmc.boot_size = sizeof bp1 + S03_SECTOR;
	m.emmc.scratch = scratch;
	m.emmc.scratch_len = sizeof scratch;
	m.emmc.max_xfer = 128u;
	/* the slave absent: the drop sends nothing; the record says what
	 * the copy's sector data did */
	m.slave_present = false;
	m.bridge.qd.state = replies != 0u ? S09_QD_ON : S09_QD_NOT;
	m.bridge.qd.replies = replies;
	image_reads = 0;
	bad_reads = bad;
}

static void test_reread(void)
{
	struct s03_load ld;

	t_case = "good copy";
	fresh(3u, 0u);
	CHECK(!opxy_bridge_load_os(&m, staging, S03_OS_MAX, &ld));
	CHECK_EQ(ld.result, S03_LOAD_OK);
	CHECK_EQ(image_reads, 1u);

	t_case = "crc fails with quad data";
	fresh(3u, 1u);
	CHECK(opxy_bridge_load_os(&m, staging, S03_OS_MAX, &ld));
	CHECK_EQ(ld.result, S03_LOAD_OK);
	CHECK_EQ(ld.length, IMAGE_LEN);
	CHECK_EQ(image_reads, 2u);
	CHECK(memcmp(staging, bp1 + S03_SECTOR, IMAGE_LEN) == 0);

	t_case = "crc fails twice";
	fresh(3u, 2u);
	CHECK(opxy_bridge_load_os(&m, staging, S03_OS_MAX, &ld));
	CHECK_EQ(ld.result, S03_LOAD_CRC);
	CHECK_EQ(image_reads, 2u);	/* once more, no more */

	t_case = "crc fails in single I/O";
	fresh(0u, 1u);
	CHECK(!opxy_bridge_load_os(&m, staging, S03_OS_MAX, &ld));
	CHECK_EQ(ld.result, S03_LOAD_CRC);
	CHECK_EQ(image_reads, 1u);
}

static void test_report(void)
{
	uint8_t out[64];
	struct s09_qdata_rec *q = &m.bridge.qd;

	t_case = "report";
	memset(&m, 0, sizeof m);
	m.bridge.rung = S09_RUNG_CHECKED_FAST;
	q->state = S09_QD_DROPPED;
	q->why = S09_QW_DATA_CHECK;
	q->test_sent = 8u;
	q->test_whole = 8u;
	q->test_first = 0u;
	q->test_len = 0u;
	q->test_us = 41000u;
	q->replies = 20u;
	q->bytes = 20u * 65536u;
	q->failed = 1u;
	q->fault.at = S09_AT_CHECK;
	q->fault.words[3] = 9u;
	CHECK_EQ(opxy_bridge_qdata_report(&m.bridge, out) + 1u, OPXY_QDATA_REPLY);
	CHECK(out[0] == S09_QD_DROPPED && out[1] == S09_QW_DATA_CHECK &&
	      out[2] == S09_RUNG_CHECKED_FAST);
	CHECK(out[3] == 8u && out[4] == 0u && out[5] == 8u && out[6] == 0u && out[7] == 0u);
	CHECK_EQ(wb_get_le32(out + 12), 41000u);
	CHECK_EQ(wb_get_le32(out + 16), 20u);
	CHECK_EQ(wb_get_le32(out + 20), 20u * 65536u);
	CHECK_EQ(wb_get_le32(out + 24), 1u);
	CHECK_EQ(out[28], S09_AT_CHECK);
	CHECK_EQ(wb_get_le32(out + 41), 9u);
	/* every reason has its words */
	for (unsigned w = 1; w < S09_QW_COUNT; w++)
		CHECK(opxy_bridge_qdata_why(w)[0] != '\0');
}

int main(void)
{
	test_reread();
	test_report();
	return t_finish("test_board_qdata");
}
