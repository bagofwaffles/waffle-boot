/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-01 memory map, stream and layout checks, the display-reset handshake.
 * Requirements: S-01.004-.010 (the stream rules of .005, .008-.010; the
 * handshake rule of .006; the ROM stack of .007), S-01.011-.017, S-01.019.
 * Vectors: tests/oracle/S-01-boot-chain/memory_map.json and vectors/layout_*.json
 * (vectors/s05_vectors.h); the S-05 compatibility streams for the
 * os profile, read at run time.
 */
#include <stdio.h>
#include <string.h>

#include "waffle/boot/memmap.h"
#include "waffle/boot/handoff.h"
#include "s02_check.h"
#include "vectors/s05_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static struct s01_range rg(uint32_t s, uint64_t e)
{
	struct s01_range r;

	r.start = s;
	r.end = e;
	return r;
}

/* S-01.004, S-01.007, S-01.008, S-01.012-.014, S-01.019: the constants and the map. */
static void test_map(void)
{
	unsigned i, j;

	CHECK_EQ(N(v_spaces), 6);
	CHECK_EQ(v_spaces[0].start, S01_L2_START);
	CHECK_EQ(v_spaces[0].end, S01_L2_END);
	CHECK_EQ(v_spaces[3].start, S01_L1_CODE_START);
	CHECK_EQ(v_spaces[3].end, S01_L1_CODE_END);
	CHECK_EQ(v_spaces[5].start, S01_DDR_START);
	CHECK(v_spaces[5].end == S01_DDR_END);
	for (i = 0; i < N(v_spaces); i++) {
		CHECK(s01_inside_spaces(rg(v_spaces[i].start, v_spaces[i].end)));
		for (j = 0; j < N(v_spaces); j++)	/* test_spaces_disjoint */
			if (i != j)
				CHECK(!s01_meets(rg(v_spaces[i].start, v_spaces[i].end),
						 rg(v_spaces[j].start, v_spaces[j].end)));
	}
	CHECK_EQ(V_RECORD_AND_ROM_STACK_START, S01_RECORD_START);
	CHECK_EQ(V_RECORD_AND_ROM_STACK_END, S01_RECORD_END);
	CHECK_EQ(V_REBOOT_RECORD_END, S01_RECORD_FLOOR);
	CHECK_EQ(V_RECORD_FLOOR, S01_RECORD_FLOOR);
	CHECK_EQ(V_ROM_BOOT_RESERVE_START, S01_BOOT_RESERVE_START);
	CHECK_EQ(V_ROM_BOOT_RESERVE_END, S01_BOOT_RESERVE_END);
	CHECK_EQ(V_STAGING_START, S01_STAGING_START);
	CHECK_EQ(V_STAGING_END, S01_STAGING_END);
	CHECK_EQ(S01_STAGING_END - S01_STAGING_START, S01_STAGING_MAX);
	CHECK_EQ(V_RESERVE0_START, S01_RESERVE_L2_START);
	CHECK_EQ(V_RESERVE0_END, S01_RESERVE_L2_END);
	CHECK_EQ(V_RESERVE1_START, S01_RESERVE_DDR_START);
	CHECK_EQ(V_RESERVE1_END, S01_RESERVE_DDR_END);
	CHECK_EQ(V_DISPLAY_RECORD_START, S01_DISPLAY_RECORD_START);
	CHECK_EQ(V_DISPLAY_RECORD_END, S01_DISPLAY_RECORD_END);
	CHECK_EQ(V_ROM_BOOT_KERNEL, S01_ROM_BOOT_KERNEL);
	CHECK_EQ(V_ROM_BOOT, S01_ROM_BOOT);
	CHECK_EQ(V_ROM_OTP_GET, S01_ROM_OTP_GET);
	CHECK_EQ(V_ROM_OTP_CFG, S01_ROM_OTP_CFG);
	CHECK_EQ(V_ROM_FIRST_STAGE_BOOT_COMMAND, S01_FIRST_STAGE_BOOT_CMD);
	CHECK_EQ(V_ROM_ROM_RESET_SP, S01_ROM_RESET_SP);
	CHECK_EQ(V_ROM_ROM_L2_ENTRY_SP_MIN, S01_ROM_L2_ENTRY_SP_MIN);
	CHECK_EQ(V_ROM_ROM_L1_ENTRY_SP, S01_ROM_L1_ENTRY_SP);
	CHECK_EQ(V_ROM_ENTRY_STACK_BUDGET, S01_ENTRY_STACK_BUDGET);
	CHECK_EQ(V_ROM_RESET_STACK_START, S01_ROM_RESET_STACK_START);
	CHECK_EQ(V_ROM_RESET_STACK_END, S01_ROM_RESET_STACK_END);
	CHECK_EQ(V_RCU0_SVECT0, S01_RCU0_SVECT0);
	/* S-01.007: the budget below the L1 entry SP ends at the record; below
	 * the lowest L2 entry SP, at the ROM stack's base, in the boot reserve. */
	CHECK_EQ(S01_ROM_L1_ENTRY_SP - S01_ENTRY_STACK_BUDGET, S01_RECORD_FLOOR);
	CHECK_EQ(S01_ROM_L2_ENTRY_SP_MIN - S01_ENTRY_STACK_BUDGET, S01_ROM_RESET_STACK_START);
	CHECK(S01_ROM_RESET_SP < S01_ROM_RESET_STACK_END);
	CHECK(S01_BOOT_RESERVE_START <= S01_ROM_RESET_STACK_START &&
	      S01_ROM_RESET_STACK_END <= S01_BOOT_RESERVE_END);
	CHECK(!s01_meets_reserve(rg(S01_BOOT_RESERVE_START, S01_BOOT_RESERVE_END)));
	/* test_reserve_inside_spaces; the staging area and the display record in DDR, apart. */
	CHECK(s01_inside_spaces(rg(S01_RESERVE_L2_START, S01_RESERVE_L2_END)));
	CHECK(s01_inside_spaces(rg(S01_RESERVE_DDR_START, S01_RESERVE_DDR_END)));
	CHECK(!s01_meets_reserve(rg(S01_STAGING_START, S01_STAGING_END)));
	CHECK(!s01_meets_reserve(rg(S01_DISPLAY_RECORD_START, S01_DISPLAY_RECORD_END)));
	CHECK(!s01_meets(rg(S01_STAGING_START, S01_STAGING_END),
			 rg(S01_DISPLAY_RECORD_START, S01_DISPLAY_RECORD_END)));
	/* The fatal-halt record lies in the L2 reserve. */
	CHECK(s01_meets_reserve(rg(V_FATAL_RECORD_START, V_FATAL_RECORD_END)));
}

/* S-01.017, S-01.008: the OS destination rule, with its edges. */
static void test_os_destinations(void)
{
	CHECK_EQ(s01_os_destination(0x08002000u, 0x100u), S01_OS_OK);
	CHECK_EQ(s01_os_destination(S01_BOOT_RESERVE_START, 4u), S01_OS_BOOT_RESERVE);
	CHECK_EQ(s01_os_destination(0x08001FFFu, 2u), S01_OS_BOOT_RESERVE);
	CHECK_EQ(s01_os_destination(0x08001F00u, 0x10u), S01_OS_BOOT_RESERVE);
	CHECK_EQ(s01_os_destination(0x11A00000u, 13u), S01_OS_OK);
	CHECK_EQ(s01_os_destination(S01_RESERVE_L2_START - 4u, 4u), S01_OS_OK);
	CHECK_EQ(s01_os_destination(S01_RESERVE_L2_START - 4u, 5u), S01_OS_RESERVE);
	CHECK_EQ(s01_os_destination(S01_RESERVE_L2_END, 4u), S01_OS_OK);
	CHECK_EQ(s01_os_destination(S01_RESERVE_DDR_START + 8u, 1u), S01_OS_RESERVE);
	CHECK_EQ(s01_os_destination(S01_STAGING_START - 1u, 2u), S01_OS_STAGING);
	CHECK_EQ(s01_os_destination(S01_STAGING_END, 16u), S01_OS_OK);
	CHECK_EQ(s01_os_destination(S01_RECORD_START, 4u), S01_OS_RECORD);
	CHECK_EQ(s01_os_destination(S01_RECORD_END - 1u, 1u), S01_OS_RECORD);
	CHECK_EQ(s01_os_destination(S01_RECORD_START - 16u, 16u), S01_OS_OK);
	CHECK_EQ(s01_os_destination(S01_ROM_BASE, 4u), S01_OS_SPACE);
	CHECK_EQ(s01_os_destination(S01_RCU0_SVECT0, 4u), S01_OS_SPACE);
	CHECK_EQ(s01_os_destination(0x11B01FFFu, 2u), S01_OS_SPACE);	/* past L1 scratch */
	CHECK_EQ(s01_os_destination(S01_L2_END - 4u, 8u), S01_OS_SPACE);	/* across L2's end */
	CHECK_EQ(s01_os_destination(0x8FFFFFF0u, 0x10u), S01_OS_OK);	/* the last DDR bytes */
	CHECK_EQ(s01_os_destination(0x8FFFFFF0u, 0x11u), S01_OS_SPACE);
	CHECK_EQ(s01_os_destination(0xFFFFFFF0u, 0x20u), S01_OS_SPACE);	/* no wrap */
	CHECK_EQ(s01_os_destination(0x80000000u, 0x80000000u), S01_OS_SPACE);
	/* S-01.017: the display record is no reason to refuse. */
	CHECK_EQ(s01_os_destination(S01_DISPLAY_RECORD_START, 0x2Cu), S01_OS_OK);
	/* S-01.005: on-chip is L1 or L2. */
	CHECK(s01_inside_on_chip(rg(0x08000000u, 0x08001000u)));
	CHECK(s01_inside_on_chip(rg(0x11900000u, 0x11900010u)));
	CHECK(!s01_inside_on_chip(rg(0x80000000u, 0x80000010u)));
	CHECK(s01_inside_l1(rg(S01_L1_CODE_START, S01_L1_CODE_END)));
	CHECK(!s01_inside_l1(rg(S01_L2_START, S01_L2_START + 4u)));
}

/* S-01.011, S-01.013-.016, S-01.019: check_layout.py's rules on the layout vectors. */
static void test_layouts(void)
{
	static struct s01_region r[16];
	unsigned i, j;

	for (i = 0; i < N(v_layouts); i++) {
		const struct v_layout *v = &v_layouts[i];

		for (j = 0; j < v->n && j < N(r); j++) {
			r[j].start = v->r[j].start;
			r[j].end = v->r[j].end;
			r[j].kind = (enum s01_kind)v->r[j].kind;
			r[j].handover = v->r[j].handover != 0;
		}
		/* The rules the vector names fail, and exactly what check_layout.py reports. */
		CHECK_EQ(s01_layout_check(r, v->n) & v->fail, v->fail);
		CHECK_EQ(s01_layout_check(r, v->n), v->exact);
		if (v->all)
			CHECK_EQ(s01_layout_check(r, v->n), 0);
	}
}

/* --- streams --- */

static uint8_t g_s[1u << 18];
static uint32_t g_n;

static void blk(uint16_t flags, uint32_t target, uint32_t count, uint32_t arg)
{
	uint8_t *h = g_s + g_n;
	uint32_t code = 0xAD000000u | flags | 4u, w[4], i, x = 0;

	w[0] = code;
	w[1] = target;
	w[2] = count;
	w[3] = arg;
	for (i = 0; i < 16u; i++)
		h[i] = (uint8_t)(w[i / 4u] >> (8u * (i % 4u)));
	for (i = 0; i < 16u; i++)
		x ^= h[i];
	h[2] = (uint8_t)(h[2] ^ x);	/* the checksum byte makes the XOR 0 */
	g_n += 16u;
	if (!(flags & S05_FLAG_FILL)) {
		memset(g_s + g_n, 0x5A, count);
		g_n += count;
	}
}

static void test_streams(void)
{
	static const struct { const char *image; uint32_t want; } os[] = {
		{"refuse_l2_reserve.ldr", S01_SRULE_OUTSIDE_RESERVE},
		{"refuse_ddr_reserve.ldr", S01_SRULE_OUTSIDE_RESERVE},
		{"refuse_staging.ldr", S01_SRULE_OUTSIDE_STAGING},
		{"refuse_checksum.ldr", S01_SRULE_PARSE},
		{"refuse_no_final.ldr", S01_SRULE_PARSE},
		{"refuse_record.ldr", S01_SRULE_RECORD_AND_ROM_STACK},
		{"refuse_rom_stack.ldr", S01_SRULE_RECORD_AND_ROM_STACK},
		{"refuse_mmr.ldr", S01_SRULE_INSIDE_SPACES},
		{"refuse_rom.ldr", S01_SRULE_INSIDE_SPACES},
		{"refuse_unmapped.ldr", S01_SRULE_INSIDE_SPACES},
		{"refuse_rom_boot_reserve.ldr", S01_SRULE_ROM_BOOT_RESERVE},
		{"refuse_flag_init.ldr", 0}, /* S-01.017 accepts what S-05.024 refuses by flag */
		{"synthetic_os_a.ldr", 0},
		{"synthetic_os_b.ldr", 0},
	};
	struct s01_range fs = {0x11B00000u, 0x11B01000u};
	unsigned i;

	for (i = 0; i < N(os); i++) {
		char path[256];
		FILE *f;

		snprintf(path, sizeof path, "tests/oracle/S-05-os-handoff/vectors/%s", os[i].image);
		f = fopen(path, "rb");
		if (f == NULL) {
			CHECK(f != NULL);
			continue;
		}
		g_n = (uint32_t)fread(g_s, 1, sizeof g_s, f);
		fclose(f);
		if (g_n == sizeof g_s)
			continue;	/* synthetic_os_a is larger than this buffer */
		CHECK_EQ(s01_stream_check(S01_OS, g_s, g_n, NULL), os[i].want);
	}
	/* A bootloader stream: L1 blocks, the INIT block in L1 code, then DDR. */
	g_n = 0;
	blk(S05_FLAG_FIRST | S05_FLAG_IGNORE, 0x11A00000u, 0, 0);
	blk(0, 0x11A00000u, 0x40, 0);
	blk(S05_FLAG_INIT, 0x11A00100u, 0x20, 0);
	blk(0, 0x88000000u, 0x100, 0);
	blk(S05_FLAG_FILL, 0x11800000u, 0x400, 0);
	blk(S05_FLAG_FINAL, 0x11A00000u, 0x10, 0);
	CHECK_EQ(s01_stream_check(S01_BOOTLOADER, g_s, g_n, &fs), 0);
	CHECK_EQ(s01_stream_check(S01_FIRST_STAGE, g_s, g_n, NULL), S01_SRULE_ON_CHIP_ONLY);
	CHECK_EQ(s01_stream_check(S01_BOOTLOADER, g_s, g_n - 1u, &fs), S01_SRULE_PARSE);
	/* DDR before the INIT block; the INIT target outside L1 code. */
	g_n = 0;
	blk(0, 0x88000000u, 0x100, 0);
	blk(S05_FLAG_INIT, 0x11800000u, 0x20, 0);
	blk(S05_FLAG_FINAL, 0x11A00000u, 0x10, 0);
	CHECK_EQ(s01_stream_check(S01_BOOTLOADER, g_s, g_n, NULL),
		 S01_SRULE_INIT_BEFORE_DDR | S01_SRULE_INIT_IN_L1);
	/* Two INIT blocks; a block over the record; over the first stage's stack. */
	g_n = 0;
	blk(S05_FLAG_INIT, 0x11A00000u, 0x20, 0);
	blk(S05_FLAG_INIT, 0x11A00100u, 0x20, 0);
	blk(0, 0x11B01F04u, 4, 0);
	blk(S05_FLAG_FILL, 0x11B00800u, 0x10, 0);
	blk(S05_FLAG_FINAL, 0x11A00000u, 0x10, 0);
	CHECK_EQ(s01_stream_check(S01_BOOTLOADER, g_s, g_n, &fs),
		 S01_SRULE_ONE_INIT_BLOCK | S01_SRULE_RECORD_AND_ROM_STACK | S01_SRULE_SPARES_FS_STACK);
	CHECK_EQ(s01_stream_check(S01_FIRST_STAGE, g_s, g_n, NULL), S01_SRULE_RECORD_AND_ROM_STACK);
	CHECK_EQ(s01_stream_check(S01_OS, g_s, g_n, NULL), S01_SRULE_RECORD_AND_ROM_STACK);
	/* S-01.008: the ROM's boot reserve, in every profile; just above it passes. */
	g_n = 0;
	blk(0, 0x08001FFCu, 8, 0);
	blk(S05_FLAG_FINAL, 0x11A00000u, 0x10, 0);
	CHECK_EQ(s01_stream_check(S01_FIRST_STAGE, g_s, g_n, NULL), S01_SRULE_ROM_BOOT_RESERVE);
	CHECK_EQ(s01_stream_check(S01_OS, g_s, g_n, NULL), S01_SRULE_ROM_BOOT_RESERVE);
	g_n = 0;
	blk(0, 0x08002000u, 0x18, 0);
	blk(S05_FLAG_FINAL, 0x11A00000u, 0x10, 0);
	CHECK_EQ(s01_stream_check(S01_FIRST_STAGE, g_s, g_n, NULL), 0);
}

int main(void)
{
	test_map();
	test_streams();
	test_os_destinations();
	test_layouts();
	return finish("test_s01_memmap");
}
