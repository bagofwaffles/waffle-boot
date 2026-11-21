/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The installer's survey build (make installer-1f-survey,
 * wiki/Installer-Reference.md): its write engine refuses every form but the
 * status, its gate refuses every erase and program even when armed, and the
 * driver's erase and program that its link wraps refuse too. Against a RAM
 * flash, as test_board_installer_engine.c.
 */
#define OPXY_TEST_OS 1
#define OPXY_TESTOS_CHECK 1
#define OPXY_TESTOS_INSTALLER 1
#define OPXY_TESTOS_1F 1
#define OPXY_TESTOS_SURVEY 1
#define wb_spiflash_read ram_read
#define wb_spiflash_erase ram_erase
#define wb_spiflash_program ram_program
#define wb_spiflash_status ram_status
#define opxy_install_image inst_image
#define opxy_install_len inst_len
#define opxy_install_crc inst_crc
#include "../../src/testos/installer_engine.c"
#include "../../src/testos/check_logic.c"
#include "../../src/testos/installer_logic.c"

#include <stdio.h>

#include "waffle/board/opxy_ops.h"
#include "waffle/device.h"

#include "support/check.h"

/* A stream S-01 boots from a slot: an INIT block of 4 bytes, an empty block
 * and the FINAL block (test_board_installer_engine.c's). */
const uint8_t inst_image[52] = {
	0x04, 0x00, 0x1c, 0xad, 0x00, 0x00, 0xa0, 0x11, 0x04, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x10, 0x17, 0x1e, 0x25, 0x04, 0x08, 0x10, 0xad, 0x00, 0x00,
	0xa0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x80, 0x98,
	0xad, 0x00, 0x00, 0xa0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
const uint32_t inst_len = sizeof inst_image;
const uint32_t inst_crc = 0xDD3F022Cu;

static uint8_t mem[S02_FLASH_SIZE], before[S02_FLASH_SIZE];
static unsigned writes;

int ram_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if ((uint64_t)off + len > S02_FLASH_SIZE)
		return -1;
	memcpy(buf, mem + off, len);
	return 0;
}

int ram_erase(void *ctx, uint32_t sector_off)
{
	(void)ctx;
	if (sector_off % S02_SECTOR_SIZE != 0u || sector_off >= S02_FLASH_SIZE)
		return -1;
	memset(mem + sector_off, 0xFF, S02_SECTOR_SIZE);
	writes++;
	return 0;
}

int ram_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if ((uint64_t)off + len > S02_FLASH_SIZE)
		return -1;
	for (uint32_t i = 0; i < len; i++)
		mem[off + i] &= buf[i];
	writes++;
	return 0;
}

int ram_status(const struct wb_spiflash *f, uint8_t *sr)
{
	(void)f;
	*sr = 0;
	return 0;
}

static struct opxy_master m;
static uint8_t scratch[S02_SECTOR_SIZE];

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* An erased flash, slot 0 good, the selector {0, verified}, the OP-1F. */
static void setup(void)
{
	uint8_t *slot = mem + s02_slot_base(0);
	const uint32_t n = 0x1800u;

	memset(mem, 0xFF, sizeof mem);
	memset(mem, 0x5A, S02_MIRROR_OFF);
	for (uint32_t i = 0; i < n; i++)
		slot[i] = (uint8_t)(i * 7u + 3u);
	put32(mem + s02_record_off(0), opxy_crc32_long(0u, slot, n));
	put32(mem + s02_record_off(0) + 4, n);
	put32(mem + s02_record_off(0) + 8, S02_RECORD_TAG);
	mem[S02_SELECTOR_OFF] = 0x00u;
	mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	memcpy(before, mem, sizeof mem);
	writes = 0;
	memset(&m, 0, sizeof m);
	m.dev = &wb_device_op1f;
	m.flash.crc32 = opxy_crc32_long;
	m.flash.scratch = scratch;
	m.flash.scratch_len = sizeof scratch;
	m.flash.meta_size = 0x28u;
}

static void test_forms_refused(void)
{
	uint8_t out[OPXY_INSTALL_REPLY];

	setup();
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == OPXY_INSTALL_REFUSED_SURVEY);
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == OPXY_INSTALL_REFUSED_SURVEY);
	CHECK(writes == 0u && memcmp(mem, before, sizeof mem) == 0);
	/* the status still reads */
	(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
	CHECK(out[0] == S02_OK && out[2] == 0x00u && out[3] == 0x01u);
	CHECK(out[4] == OPXY_INSTALL_SLOT_GOOD);
	CHECK(writes == 0u);
	CHECK(strcmp(inst_rc_words(OPXY_INSTALL_REFUSED_SURVEY),
		     "A survey build writes nothing to the SPI flash.") == 0);
}

static void test_gate_refuses(void)
{
	struct gate g = { &m.hw.flash, 0u, 0u, true, true, false, 0u, false, false };

	setup();
	g.slot = s02_slot_base(1);
	g.record = s02_record_off(1);
	/* armed, the selector open: still nothing */
	CHECK(gate_allows(&g, s02_slot_base(1), S02_SECTOR_SIZE));
	CHECK(gate_erase(&g, s02_slot_base(1)) != 0);
	CHECK(gate_erase(&g, S02_SELECTOR_OFF) != 0);
	CHECK(gate_program(&g, s02_slot_base(1), scratch, 4u) != 0);
	CHECK(gate_program(&g, S02_SELECTOR_OFF, scratch, 1u) != 0);
	CHECK(g.refused == 4u);
	CHECK(writes == 0u && memcmp(mem, before, sizeof mem) == 0);
	/* the link's wrapped driver calls */
	CHECK(__wrap_wb_spiflash_erase(&m.hw.flash, s02_slot_base(1)) != 0);
	CHECK(__wrap_wb_spiflash_program(&m.hw.flash, 0u, scratch, 4u) != 0);
	CHECK(writes == 0u);
}

/* The survey's facts: bytes on the wire and the part of check.txt. */
static void test_survey_facts(void)
{
	static char rep[2048];
	struct inst_survey sv;
	uint8_t b[INST_SURVEY_LEN];
	size_t n;

	setup();
	memset(&sv, 0, sizeof sv);
	sv.device = WB_DEV_OP1F;
	sv.how = WB_DEV_BY_TABLE;
	sv.boot_mode = 1u;
	sv.fs_n = 0x8600u;
	sv.fs_c = 0x49267EB8u;
	sv.pa2 = 1u;
	sv.ext_read = true;
	sv.part_config = 0x48u;
	sv.sec_count = 0x1D5A000u;
	sv.boot_kib = 4096u;
	sv.os = INST_OS_ACTIVE;
	opxy_installer_survey_read(&m, &sv);
	CHECK(writes == 0u);
	CHECK(inst_survey_bytes(&sv, b) == INST_SURVEY_LEN);
	CHECK(b[0] == INST_ST_OK && b[1] == WB_DEV_OP1F && b[2] == WB_DEV_BY_TABLE && b[3] == 1u);
	CHECK(wb_get_le32(b + 4) == 0x8600u && wb_get_le32(b + 8) == 0x49267EB8u);
	CHECK(b[12] == 1u && b[13] == 0u && b[14] == 1u && b[15] == 1u);
	CHECK(wb_get_le32(b + 16) == 0x1D5A000u && wb_get_le32(b + 20) == 4096u);
	CHECK(b[24] == 1u && b[25] == 0x48u && b[26] == INST_OS_ACTIVE);
	CHECK(b[28] == (INST_SV_RECORD | INST_SV_GOOD | INST_SV_LOG));
	CHECK(wb_get_le32(b + 28 + 8) == 0x1800u && wb_get_le32(b + 28 + 12) == S02_RECORD_TAG);
	CHECK(b[28 + 24] == (INST_SV_RECORD | INST_SV_LOG));
	CHECK(wb_get_le32(b + 28 + 24 + 8) == 0xFFFFFFFFu);
	strcpy(rep, "head\n");
	n = inst_survey_report(rep, 5u, sizeof rep, &sv);
	CHECK(n == strlen(rep));
	CHECK(strstr(rep, "\nsurvey (nothing was written to the SPI flash)\nboot mode: 1\n"
		     "first stage: n 34304, c 49267eb8, the table's OP-1F\nselector: 00 01\n") != NULL);
	CHECK(strstr(rep, "slot 1 record: crc32 ") != NULL);
	CHECK(strstr(rep, ", length 00001800, tag 0000b004; verifies; not a waffle-boot stream\n"
		     "slot 1 start log: ff ff ff ff ff ff ff ff\n") != NULL);
	CHECK(strstr(rep, "slot 2 record: crc32 ffffffff, length ffffffff, tag ffffffff; does not "
		     "verify; not a waffle-boot stream\n") != NULL);
	CHECK(strstr(rep, "emmc: EXT_CSD[179] PART_CONFIG 48, SEC_COUNT 01d5a000, boot partition "
		     "4096 KiB\nPA2: high\nOS partition: made active\n") != NULL);
	/* never past the buffer */
	n = inst_survey_report(rep, 5u, 60u, &sv);
	CHECK(n == 59u && rep[59] == '\0');
}

int main(void)
{
	RUN(test_forms_refused);
	RUN(test_gate_refuses);
	RUN(test_survey_facts);
	return check_summary("test_board_installer_survey");
}
