/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tests for the register access helpers and the host fake behind them. */
#include "support/check.h"
#include "support/fake_mmr.h"

#define REG_A 0x20001000u
#define REG_B 0x20001004u

static void test_unwritten_reads_zero(void)
{
	static const struct fake_mmr_access want[] = {
		FAKE_R32(REG_A, 0),
	};

	fake_mmr_reset();
	CHECK(wb_mmr_read32(REG_A) == 0);
	CHECK(fake_mmr_expect("unwritten", want, ARRAY_LEN(want)));
	CHECK(fake_mmr_errors() == 0);
}

static void test_write_then_read(void)
{
	static const struct fake_mmr_access want[] = {
		FAKE_W32(REG_A, 0x12345678u),
		FAKE_SYNC,
		FAKE_R32(REG_A, 0x12345678u),
		FAKE_W16(REG_B, 0xbeefu),
		FAKE_R16(REG_B, 0xbeefu),
		FAKE_W8(REG_B + 2u, 0x5au),
		FAKE_R8(REG_B + 2u, 0x5au),
	};

	fake_mmr_reset();
	wb_mmr_write32(REG_A, 0x12345678u);
	wb_mmr_sync();
	CHECK(wb_mmr_read32(REG_A) == 0x12345678u);
	wb_mmr_write16(REG_B, 0xbeefu);
	CHECK(wb_mmr_read16(REG_B) == 0xbeefu);
	wb_mmr_write8(REG_B + 2u, 0x5au);
	CHECK(wb_mmr_read8(REG_B + 2u) == 0x5au);
	CHECK(fake_mmr_expect("write/read", want, ARRAY_LEN(want)));
	CHECK(fake_mmr_errors() == 0);
}

static void test_preset_is_not_recorded(void)
{
	fake_mmr_reset();
	fake_mmr_set(REG_A, 7u);
	CHECK(fake_mmr_count() == 0);
	CHECK(wb_mmr_read32(REG_A) == 7u);
	CHECK(fake_mmr_count() == 1);
}

static void test_queued_reads(void)
{
	fake_mmr_reset();
	fake_mmr_set(REG_A, 3u);
	fake_mmr_queue_read(REG_A, 1u);
	fake_mmr_queue_read(REG_B, 9u);
	fake_mmr_queue_read(REG_A, 2u);
	CHECK(wb_mmr_read32(REG_A) == 1u);
	CHECK(wb_mmr_read32(REG_A) == 2u);
	CHECK(wb_mmr_read32(REG_A) == 3u);
	CHECK(wb_mmr_read32(REG_B) == 9u);
	CHECK(wb_mmr_read32(REG_B) == 0u);
}

static void test_modify_helpers(void)
{
	static const struct fake_mmr_access want[] = {
		FAKE_R32(REG_A, 0xf0f0f0f0u),
		FAKE_W32(REG_A, 0xf0f0f0f3u),
		FAKE_R32(REG_A, 0xf0f0f0f3u),
		FAKE_W32(REG_A, 0x00f0f0f3u),
		FAKE_R32(REG_A, 0x00f0f0f3u),
		FAKE_W32(REG_A, 0x00f0a5f3u),
		FAKE_R16(REG_B, 0x00ffu),
		FAKE_W16(REG_B, 0x0f0fu),
	};
	static const struct fake_mmr_access want_writes[] = {
		FAKE_W32(REG_A, 0xf0f0f0f3u),
		FAKE_W32(REG_A, 0x00f0f0f3u),
		FAKE_W32(REG_A, 0x00f0a5f3u),
		FAKE_W16(REG_B, 0x0f0fu),
	};

	fake_mmr_reset();
	fake_mmr_set(REG_A, 0xf0f0f0f0u);
	fake_mmr_set(REG_B, 0x00ffu);
	wb_mmr_set32(REG_A, 0x3u);
	wb_mmr_clear32(REG_A, 0xf0000000u);
	wb_mmr_modify32(REG_A, 0x0000ff00u, 0x0000a500u);
	wb_mmr_modify16(REG_B, 0x00f0u, 0x0f00u);
	CHECK(fake_mmr_expect("modify", want, ARRAY_LEN(want)));
	CHECK(fake_mmr_expect_writes("modify writes", want_writes,
				     ARRAY_LEN(want_writes)));
}

/* A status register whose bit 0 is write-1-to-clear. */
static bool w1c_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		     wb_mmr_addr addr, uint32_t *value)
{
	unsigned *calls = ctx;

	(*calls)++;
	if (op != FAKE_MMR_WRITE || width != 32u || addr != REG_A)
		return true;
	*value = fake_mmr_get(REG_A) & ~(*value & 1u);
	return true;
}

static void test_hook(void)
{
	unsigned calls = 0;

	fake_mmr_reset();
	fake_mmr_set_hook(w1c_hook, &calls);
	fake_mmr_set(REG_A, 0x3u);
	wb_mmr_write32(REG_A, 0x1u);
	CHECK(wb_mmr_read32(REG_A) == 0x2u);
	wb_mmr_write32(REG_B, 0x5u);
	CHECK(wb_mmr_read32(REG_B) == 0x5u);
	CHECK(calls == 4u);
}

static void test_misaligned_is_an_error(void)
{
	fake_mmr_reset();
	(void)wb_mmr_read32(REG_A + 2u);
	wb_mmr_write16(REG_A + 1u, 0);
	CHECK(fake_mmr_errors() == 2u);
}

static void test_mismatch_is_reported(void)
{
	static const struct fake_mmr_access want[] = {
		FAKE_W32(REG_A, 1u),
	};

	fake_mmr_reset();
	CHECK(!fake_mmr_expect("(expected mismatch: missing)", want,
			       ARRAY_LEN(want)));
	wb_mmr_write32(REG_A, 2u);
	CHECK(!fake_mmr_expect("(expected mismatch: value)", want,
			       ARRAY_LEN(want)));
	fake_mmr_clear_log();
	wb_mmr_write32(REG_A, 1u);
	wb_mmr_write32(REG_A, 1u);
	CHECK(!fake_mmr_expect("(expected mismatch: extra)", want,
			       ARRAY_LEN(want)));
}

int main(void)
{
	RUN(test_unwritten_reads_zero);
	RUN(test_write_then_read);
	RUN(test_preset_is_not_recorded);
	RUN(test_queued_reads);
	RUN(test_modify_helpers);
	RUN(test_hook);
	RUN(test_misaligned_is_an_error);
	RUN(test_mismatch_is_reported);
	return check_summary("test_fake_mmr");
}
