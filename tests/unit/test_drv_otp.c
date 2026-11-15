/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OTP item reads through a stand-in for the ROM's adi_rom_otp_get(). */
#include "support/check.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x_otp.h"

static uint32_t last_item;
static bool rom_ok;

static bool rom_get(uint32_t item, uint32_t *data)
{
	unsigned i;

	last_item = item;
	for (i = 0; i < wb_otp_words(item); i++)
		data[i] = 0xA5000000u | (item << 8) | i;
	return rom_ok;
}

static void test_items(void)
{
	/* S-16.095's items: HUK 1, keys 3-6, public keys 9 and 0xA, GP1. */
	CHECK(wb_otp_words(WB_OTP_HUK) == 8u);
	CHECK(wb_otp_words(3) == 4u && wb_otp_words(6) == 4u);
	CHECK(wb_otp_words(9) == 16u && wb_otp_words(0xA) == 16u);
	CHECK(wb_otp_words(0xE) == 16u);
	CHECK(wb_otp_words(0) == 0u);
	CHECK(wb_otp_words(WB_OTP_GP0) == 0u);
	CHECK(wb_otp_words(WB_OTP_ITEMS) == 0u);
	CHECK(wb_otp_words(0xFFFFFFFFu) == 0u);
}

static void test_read(void)
{
	uint32_t buf[17];

	rom_ok = true;
	last_item = 0;
	CHECK(wb_otp_read(rom_get, WB_OTP_PUBLIC_KEY1, buf, 17) == WB_OTP_OK);
	CHECK(last_item == 0xAu);
	CHECK(buf[0] == 0xA5000A00u && buf[15] == 0xA5000A0Fu);
	CHECK(buf[16] == 0u);

	last_item = 0;
	CHECK(wb_otp_read(rom_get, WB_OTP_HUK, buf, 7) == WB_OTP_ERR_ARG);
	CHECK(wb_otp_read(rom_get, WB_OTP_GP0, buf, 17) == WB_OTP_ERR_ARG);
	CHECK(wb_otp_read(NULL, WB_OTP_HUK, buf, 8) == WB_OTP_ERR_ARG);
	CHECK(last_item == 0u);

	rom_ok = false;
	CHECK(wb_otp_read(rom_get, WB_OTP_PVT_128KEY0, buf, 4) ==
	      WB_OTP_ERR_READ);
	CHECK(buf[0] == 0u && buf[3] == 0u);
}

int main(void)
{
	RUN(test_items);
	RUN(test_read);
	return check_summary("test_drv_otp");
}
