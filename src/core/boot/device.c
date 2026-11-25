/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-18: the two devices' features and profile values. The decision between
 * them is boot/device_identify.c's, apart, so that a module that only reads a
 * profile links no CRC.
 */
#include "waffle/boot/device.h"

#include <stddef.h>

#define PA WB_DEV_PA
#define PC WB_DEV_PC
#define NONE WB_KEY_NONE

/* S-18.030: outputs first, then inputs. */
static const struct wb_dev_gpio gpio_opxy[] = {
	{ PA(3), WB_GPIO_OUT_LOW },
	{ PC(11), WB_GPIO_OUT_HIGH },
	{ PA(6), WB_GPIO_IN },
	{ PA(7), WB_GPIO_IN },
	{ PC(2), WB_GPIO_IN },
	{ PC(5), WB_GPIO_IN },
};

static const struct wb_dev_gpio gpio_op1f[] = {
	{ PA(0), WB_GPIO_OUT_KEEP },
	{ PA(3), WB_GPIO_OUT_KEEP },
	{ PA(4), WB_GPIO_OUT_KEEP },
	{ PA(6), WB_GPIO_OUT_KEEP },
	{ PC(13), WB_GPIO_OUT_KEEP },
	{ PA(1), WB_GPIO_IN },
	{ PA(2), WB_GPIO_IN },
	{ PA(7), WB_GPIO_IN },
	{ PC(2), WB_GPIO_IN },
	{ PC(11), WB_GPIO_IN },
	{ PC(14), WB_GPIO_IN },
};

static const char *const reset_op1f[] = { "/yaffs2/user", "/yaffs2/logs" };

const struct wb_device wb_device_opxy = {
	.id = WB_DEV_OPXY,
	.name = "OP-XY",
	.device_id = "xy",
	.contract = "XY-HANDOFF-2",			/* S-05.023 */
	.features = WB_F_SECOND_CPU | WB_F_STRAPS | WB_F_PMIC_PASS_THROUGH |
		    WB_F_DIGITAL_GAMMA | WB_F_KEY_LEDS | WB_F_CONTENT_PACKS |
		    WB_F_FAT_REGION | WB_F_FACTORY_WORD | WB_F_CV_CALIBRATION |
		    WB_F_MTP_TWO_ITEMS,
	.first_stage_n = 0x86C0u,
	.first_stage_crc = 0xF3C93780u,
	.ident = {
		.product_number = 0x21u,
		.product_base = 0x00084000u,
		.product_text = "OP-XY",
		.sku_prefix = "TE033AS",
		.usb_pid = 0x0021u,
		.midi_pid = 0x8021u,
		.midi_byte = 0x21u,
		.midi_family = { 0x21u, 0x00u },
		.midi_member = { 0x01u, 0x00u },
		.member_from_sku = false,
	},
	.mem = {
		.metadata_size = 0x34u,
		.ramdisk_one_region = false,
		.ramdisk_base = 0u,
		.ramdisk_len = 0u,
		.ramdisk_blocks = 454656u,
		.emmc_width = 8u,
		.emmc_sectors = 0x1CF8000u,
		.emmc_sectors_at_least = false,
		.reset_only = NULL,
		.reset_only_n = 0u,
	},
	.pins = {
		.mux = { 0x30002000u, 0x1400FFFFu, 0x00855100u },
		.fer = { 0x00004040u, 0x0000FFFFu, 0x00000BD3u },
		.gpio = gpio_opxy,
		.gpio_n = (uint8_t)(sizeof(gpio_opxy) / sizeof(gpio_opxy[0])),
		.panel_dc = PA(5),
	},
	.keys = {
		.count = 72u,
		.bitmap_bytes = 9u,
		.hold = { 0x18u, 0x0Du },
		.debug = { 0x0Au, 0x0Bu },
		.debug_both = false,
		.precomp = 0x17u,
		.digit = {
			{ 0x06u, 0x0Au, 0x0Eu },
			{ 0x07u, 0x0Bu, 0x0Fu },
			{ 0x08u, 0x0Cu, 0x10u },
			{ 0x09u, 0x0Du, 0x11u },
			{ 0x12u, NONE, NONE },
			{ 0x13u, NONE, NONE },
			{ 0x14u, NONE, NONE },
			{ 0x15u, NONE, NONE },
		},
		.digit_knob = 1u,
		.shift = 0x37u,
		.confirm = 0x0Du,
		.com = 0x18u,
		.recovery = 0x09u,
	},
};

const struct wb_device wb_device_op1f = {
	.id = WB_DEV_OP1F,
	.name = "OP-1F",
	.device_id = "1f",
	.contract = "1F-HANDOFF-1",			/* S-05.039 */
	.features = WB_F_EMMC_ON_MAIN,
	.first_stage_n = 0x8600u,
	.first_stage_crc = 0x49267EB8u,
	.ident = {
		.product_number = 0x02u,
		.product_base = 0x00008000u,
		.product_text = "OP-1",
		.sku_prefix = "TE002AS",
		.usb_pid = 0x0102u,
		.midi_pid = 0x8102u,
		.midi_byte = 0x02u,
		.midi_family = { 0x02u, 0x00u },
		.midi_member = { 0x02u, 0x00u },	/* the sku 0xFFFF's */
		.member_from_sku = true,
	},
	.mem = {
		.metadata_size = 0x28u,
		.ramdisk_one_region = true,
		.ramdisk_base = 0x80004000u,
		.ramdisk_len = 0x07F00000u,
		.ramdisk_blocks = 260096u,
		.emmc_width = 4u,
		.emmc_sectors = 0x1CF8000u,
		.emmc_sectors_at_least = true,
		.reset_only = reset_op1f,
		.reset_only_n = (uint8_t)(sizeof(reset_op1f) / sizeof(reset_op1f[0])),
	},
	.pins = {
		.mux = { 0x30280000u, 0x1400FFFFu, 0x000AAA00u },
		.fer = { 0x00004620u, 0x0000FFFFu, 0x000017F3u },
		.gpio = gpio_op1f,
		.gpio_n = (uint8_t)(sizeof(gpio_op1f) / sizeof(gpio_op1f[0])),
		.panel_dc = PA(11),
	},
	.keys = {
		.count = 58u,
		.bitmap_bytes = 8u,
		.hold = { 0x39u, 0x2Cu },
		.debug = { 0x0Au, 0x0Cu },
		.debug_both = true,
		.precomp = NONE,			/* provisional */
		.digit = {
			{ 0x06u, 0x2Du, 0x36u },
			{ 0x07u, 0x2Eu, 0x37u },
			{ 0x08u, 0x2Fu, 0x38u },
			{ 0x09u, 0x10u, 0x39u },
			{ 0x11u, NONE, NONE },
			{ 0x12u, NONE, NONE },
			{ 0x13u, NONE, NONE },
			{ 0x14u, NONE, NONE },
		},
		.digit_knob = 2u,
		.shift = 0x26u,
		.confirm = 0x39u,
		.com = 0x2Cu,
		.recovery = 0x09u,			/* provisional */
	},
};

const struct wb_device *wb_device_of(unsigned id)
{
	return id == WB_DEV_OP1F ? &wb_device_op1f : &wb_device_opxy;
}

unsigned wb_key_digit(const struct wb_device *dev, unsigned key)
{
	unsigned d, i;

	if (key == NONE)
		return 0;
	for (d = 0; d < 8u; d++)
		for (i = 0; i < 3u; i++)
			if (dev->keys.digit[d][i] == key)
				return d + 1u;
	return 0;
}

void wb_midi_member(const struct wb_device *dev, uint16_t sku, uint8_t out[2])
{
	if (!dev->ident.member_from_sku || sku == 0xFFFFu) {
		out[0] = dev->ident.midi_member[0];
		out[1] = dev->ident.midi_member[1];
		return;
	}
	/* two 7-bit bytes, low first */
	out[0] = (uint8_t)(sku & 0x7Fu);
	out[1] = (uint8_t)((sku >> 7) & 0x7Fu);
}
