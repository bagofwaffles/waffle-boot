/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The S-13c bus over the BF70x panel driver (src/drivers/bf70x/display.c),
 * on the fake MMR: each callback reaches the driver with the adapter's struct
 * wb_lcd, and the S-13c begin/start sequence comes out on the bank.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x/display.h"
#include "waffle/drivers/bf70x/port.h"

static const struct wb_time cclk400 = { 400u };

static long find_w32(wb_mmr_addr addr, uint32_t value)
{
	const struct fake_mmr_access *a = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (a[i].op == FAKE_MMR_WRITE && a[i].width == 32u &&
		    a[i].addr == addr && a[i].value == value)
			return (long)i;
	return -1;
}

/* The bytes written to the bank, in order. */
static size_t bank_bytes(uint8_t *out, size_t max)
{
	const struct fake_mmr_access *a = fake_mmr_log();
	size_t i, n = 0;

	for (i = 0; i < fake_mmr_count(); i++)
		if (a[i].op == FAKE_MMR_WRITE && a[i].addr == WB_LCD_SMC_BANK0) {
			if (n < max)
				out[n] = (uint8_t)a[i].value;
			n++;
		}
	return n;
}

static void test_bus(void)
{
	struct wb_display_bf70x a;
	const struct s13c_bus *b;
	const uint8_t p[2] = { 0x12, 0x34 };
	uint8_t got[8] = { 0 };

	fake_mmr_reset();
	fake_core_reset();
	b = wb_display_bf70x_init(&a, &cclk400);
	CHECK(b != NULL);
	if (b == NULL)
		return;
	CHECK(b->ctx == &a.lcd && b->read != NULL);
	CHECK(fake_mmr_get(SMC0_B0CTL) & SMC_CTL_EN);
	fake_mmr_clear_log();
	b->reset(b->ctx, false);
	CHECK(find_w32(PORT_DATA_CLR(WB_PORT_A), 1u << 8) >= 0);
	b->select(b->ctx, true);
	CHECK(a.lcd.selected);
	fake_mmr_clear_log();
	b->cmd(b->ctx, 0x2A);
	b->data(b->ctx, p, 2);
	CHECK(bank_bytes(got, sizeof got) == 3);
	CHECK(got[0] == 0x2A && got[1] == 0x12 && got[2] == 0x34);
	fake_core_set_cycles(400u * 1234u);
	CHECK(b->now_us(b->ctx) == 1234u);
}

int main(void)
{
	RUN(test_bus);
	return check_summary("test_display_bf70x");
}
