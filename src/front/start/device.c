/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The devices the front tells apart (start/device.h). */
#include "front/start/device.h"

#include "front/handoff.h"
#include "front/slots/slots.h"
#include "front/recovery/usb.h"

#include "waffle/drivers/bf70x/port.h"

#define PA(b) WB_PIN(WB_PORT_A, b)
#define PB(b) WB_PIN(WB_PORT_B, b)
#define PC(b) WB_PIN(WB_PORT_C, b)

/* S-13b.004: PB10 and PC4, DIR cleared then INEN set, before the first
 * MUX write; none on the OP-1F, where PC4 is MSI0's. */
static const uint8_t first_opxy[] = { PB(10), PC(4) };

/* S-13b.006 as the front sets them: PA3 low; PA7 (the display's TE)
 * before PA6 (the boot link's ready); PC2. PC11 and PC5 are the bridge's,
 * which the full bootloader sets. */
static const struct fr_dev_gpio gpio_opxy[] = {
	{ PA(3), FR_GPIO_LOW },
	{ PA(7), FR_GPIO_IN },
	{ PA(6), FR_GPIO_IN },
	{ PC(2), FR_GPIO_IN },
};

/* S-13b.006 on the OP-1F: five outputs at the levels they had (no level
 * written; after a start from the flash, low), then six inputs. PA8 is
 * never an input (S-13c.027). */
static const struct fr_dev_gpio gpio_op1f[] = {
	{ PA(0), FR_GPIO_KEEP },
	{ PA(3), FR_GPIO_KEEP },
	{ PA(4), FR_GPIO_KEEP },
	{ PA(6), FR_GPIO_KEEP },
	{ PC(13), FR_GPIO_KEEP },
	{ PA(1), FR_GPIO_IN },
	{ PA(2), FR_GPIO_IN },
	{ PA(7), FR_GPIO_IN },
	{ PC(2), FR_GPIO_IN },
	{ PC(11), FR_GPIO_IN },
	{ PC(14), FR_GPIO_IN },
};

const struct fr_dev fr_dev_opxy = {
	.id = FR_HO_DEV_OPXY,
	.flags = FR_DEV_F_STRAPS | FR_DEV_F_GAMMA | FR_DEV_F_KEY_LEDS,
	.fs_n = 0x86C0u,
	.fs_c = 0xF3C93780u,
	.mux = { 0x30002000u, FR_PORTB_MUX, 0x00855100u },
	.fer = { 0x4040u, FR_PORTB_FER, 0x0BD3u },
	.inputs_first = first_opxy,
	.inputs_first_n = (uint8_t)sizeof first_opxy,
	.gpio = gpio_opxy,
	.gpio_n = (uint8_t)(sizeof gpio_opxy / sizeof gpio_opxy[0]),
	.panel_dc = PA(5),
	.key_count = 72u,
	.key_bytes = 9u,
	.hold = { 0x18u, 0x0Du },
	.precomp = 0x17u,
	.key_back = 0x15u,
	.key_recovery = 0x09u,
	.usb_pid = FR_USB_PID,
	.usb_product = FR_USB_PRODUCT,
};

const struct fr_dev fr_dev_op1f = {
	.id = FR_HO_DEV_OP1F,
	.flags = 0u,
	.fs_n = 0x8600u,
	.fs_c = 0x49267EB8u,
	.mux = { 0x30280000u, FR_PORTB_MUX, 0x000AAA00u },
	.fer = { 0x4620u, FR_PORTB_FER, 0x17F3u },
	.inputs_first = NULL,
	.inputs_first_n = 0u,
	.gpio = gpio_op1f,
	.gpio_n = (uint8_t)(sizeof gpio_op1f / sizeof gpio_op1f[0]),
	.panel_dc = PA(11),
	.key_count = 58u,
	.key_bytes = 8u,
	.hold = { 0x39u, 0x2Cu },
	.precomp = FR_KEY_NONE,		/* provisional */
	.key_back = 0x14u,
	.key_recovery = 0x09u,		/* provisional: the same roles */
	.usb_pid = FR_USB_PID_OP1F,
	.usb_product = FR_USB_PRODUCT_OP1F,
};

const struct fr_dev *fr_dev_of(unsigned id)
{
	return id == FR_HO_DEV_OP1F ? &fr_dev_op1f : &fr_dev_opxy;
}

const struct fr_dev *fr_dev_or_default(const struct fr_dev *dev)
{
	return dev != NULL ? dev : &fr_dev_opxy;
}

/* The bytes are read in pieces of this many, each byte once. */
#define CHUNK 256u

/* One try of the read: FR_FLASH_OK with out->n and out->c set, or the
 * failed read's result. */
static int fingerprint(const struct fr_flash *flash, struct fr_ident *out)
{
	uint8_t buf[CHUNK];
	uint32_t a, n, off, crc;
	int rc;

	out->n = 0;
	out->c = 0;
	rc = flash->read(flash->ctx, 0, buf, FR_FS_HEAD);
	if (rc != FR_FLASH_OK)
		return rc;
	a = fr_le32(buf + FR_FS_ARG_OFF);
	n = a > 0xFFFFFFFFu - FR_FS_HEAD ? 0xFFFFFFFFu : a + FR_FS_HEAD;
	out->n = n;
	if (n <= FR_FS_HEAD || n > FR_FS_N_MAX)
		return FR_FLASH_OK;
	/* the header's bytes count once, from the read above */
	crc = fr_crc32(0, buf, FR_FS_HEAD);
	for (off = FR_FS_HEAD; off < n;) {
		const uint32_t k = n - off < CHUNK ? n - off : CHUNK;

		rc = flash->read(flash->ctx, off, buf, k);
		if (rc != FR_FLASH_OK)
			return rc;
		crc = fr_crc32(crc, buf, k);
		off += k;
	}
	out->c = crc;
	return FR_FLASH_OK;
}

void fr_identify(const struct fr_flash *flash, struct fr_ident *out)
{
	static const struct fr_dev *const all[FR_DEV_COUNT] = { &fr_dev_opxy, &fr_dev_op1f };

	out->dev = &fr_dev_opxy;	/* S-18.004: the default */
	out->how = FR_HO_HOW_UNKNOWN;
	for (unsigned t = 0; t < FR_FS_TRIES; t++) {
		const int rc = fingerprint(flash, out);

		if (rc == FR_FLASH_BUSY)
			return;		/* no try after a full busy wait */
		if (rc != FR_FLASH_OK || out->n <= FR_FS_HEAD || out->n > FR_FS_N_MAX)
			continue;
		for (unsigned i = 0; i < FR_DEV_COUNT; i++) {
			if (all[i]->fs_n == out->n && all[i]->fs_c == out->c) {
				out->dev = all[i];
				out->how = FR_HO_HOW_TABLE;
				return;
			}
		}
	}
}

void fr_decide(const struct fr_decide_hw *hw, struct fr_role *out)
{
	out->main = true;
	out->by_strap = false;
	out->strapped = false;
	out->strap = 0;
	if (hw->boot_mode(hw->ctx) != FR_BOOT_MODE_MAIN) {
		/* S-18.002: the fallback sample, the one before the flash read */
		if ((hw->pa2(hw->ctx) & FR_STRAP_PA2) == 0u) {
			/* the second CPU, no flash read; then its strap sample */
			out->main = false;
			out->ident.dev = &fr_dev_opxy;
			out->ident.how = FR_HO_HOW_SECOND;
			out->ident.n = 0;
			out->ident.c = 0;
			out->strap = hw->strap(hw->ctx);
			out->strapped = true;
			return;
		}
		out->by_strap = true;
	}
	/* S-18.001, S-18.003: no port write but PORTB's before the read */
	hw->flash_bus(hw->ctx);
	fr_identify(hw->flash, &out->ident);
	if (fr_dev_has(out->ident.dev, FR_DEV_F_STRAPS)) {
		out->strap = hw->strap(hw->ctx);
		out->strapped = true;
	}
}
