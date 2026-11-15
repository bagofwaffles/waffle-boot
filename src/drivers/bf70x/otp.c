/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OTP item reads through the ROM (see waffle/drivers/bf70x_otp.h). */
#include "waffle/drivers/bf70x_otp.h"

#include "waffle/drivers/mmr.h"

/*
 * Table 13-1's field sizes. GP0 (2624 bits in Table 13-1, 16 words in
 * otp_data), OTP timing and the anti-rollback counter (512 bits in Table
 * 13-1, one word in otp_data) disagree or are not given: left at 0.
 */
static const uint8_t item_words[WB_OTP_ITEMS] = {
	[WB_OTP_HUK] = 8u,
	[WB_OTP_PVT_128KEY0] = 4u,
	[WB_OTP_PVT_128KEY1] = 4u,
	[WB_OTP_PVT_128KEY2] = 4u,
	[WB_OTP_PVT_128KEY3] = 4u,
	[WB_OTP_EK] = 8u,
	[WB_OTP_SECURE_EMU_KEY] = 4u,
	[WB_OTP_PUBLIC_KEY0] = 16u,
	[WB_OTP_PUBLIC_KEY1] = 16u,
	[WB_OTP_BOOT_INFO] = 16u,
	[WB_OTP_GP1] = 16u,
};

unsigned wb_otp_words(uint32_t item)
{
	return item < WB_OTP_ITEMS ? (unsigned)item_words[item] : 0u;
}

int wb_otp_read(wb_otp_get_fn get, uint32_t item, uint32_t *data,
		size_t nwords)
{
	unsigned words = wb_otp_words(item);
	size_t i;

	if (get == NULL || data == NULL || words == 0 || nwords < words)
		return WB_OTP_ERR_ARG;
	for (i = 0; i < nwords; i++)
		data[i] = 0;
	if (!get(item, data)) {
		for (i = 0; i < nwords; i++)
			data[i] = 0;
		return WB_OTP_ERR_READ;
	}
	return WB_OTP_OK;
}
