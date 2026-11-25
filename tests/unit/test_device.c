/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-18 devices (src/core/boot/device.c): S-18.002, S-18.003, S-18.004, S-18.005,
 * S-18.020, S-18.030, S-18.031, against tests/oracle/S-18-devices/vectors/.
 */
#include <stdio.h>
#include <string.h>

#include "support/check.h"
#include "s02_ref_crc.h"
#include "vectors/s18_vectors.h"

#include "front/fr_handoff.h"
#include "waffle/board/opxy.h"
#include "waffle/boot/device.h"

static const struct wb_device *const devs[2] = { &wb_device_opxy, &wb_device_op1f };

static bool same_str(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* S-18.030: every value of both columns, and the texts per device. */
static void test_profiles(void)
{
	for (unsigned d = 0; d < 2u; d++) {
		const struct wb_device *dev = devs[d];
		const struct s18v_profile *p = &s18v_profiles[d];
		unsigned i, j;

		CHECK(dev->id == (enum wb_dev_id)d && wb_device_of(d) == dev);
		CHECK(same_str(dev->device_id, p->device_id) && same_str(dev->contract, p->contract));
		/* identity */
		CHECK(dev->ident.product_number == p->product_number);
		CHECK(dev->ident.product_base == p->product_base);
		CHECK(same_str(dev->ident.product_text, p->product_text));
		CHECK(same_str(dev->ident.sku_prefix, p->sku_prefix));
		CHECK(dev->ident.usb_pid == p->usb_pid && dev->ident.midi_pid == p->midi_pid);
		CHECK(dev->ident.midi_byte == p->midi_byte);
		CHECK(memcmp(dev->ident.midi_family, p->midi_family, 2) == 0);
		CHECK(dev->ident.member_from_sku == (p->member_from_sku != 0));
		CHECK(memcmp(dev->ident.midi_member, p->midi_member, 2) == 0);
		/* memory and media */
		CHECK(dev->mem.metadata_size == p->metadata_size);
		CHECK(dev->mem.ramdisk_one_region == (p->ramdisk_one != 0));
		CHECK(dev->mem.ramdisk_base == p->ramdisk_base && dev->mem.ramdisk_len == p->ramdisk_len);
		CHECK(dev->mem.ramdisk_blocks == p->ramdisk_blocks);
		CHECK(dev->mem.emmc_width == p->emmc_width && dev->mem.emmc_sectors == p->emmc_sectors);
		CHECK(dev->mem.emmc_sectors_at_least == (p->emmc_at_least != 0));
		CHECK(wb_has(dev, WB_F_FAT_REGION) == (p->fat_erase != 0));
		CHECK(dev->mem.reset_only_n == p->reset_only_n);
		CHECK((dev->mem.reset_only == NULL) == (p->reset_only_n == 0u));
		for (i = 0; dev->mem.reset_only != NULL && i < p->reset_only_n &&
			    i < dev->mem.reset_only_n; i++)
			CHECK(same_str(dev->mem.reset_only[i], p->reset_only[i]));
		/* a single region holds the disk's blocks */
		if (dev->mem.ramdisk_one_region)
			CHECK(dev->mem.ramdisk_len == dev->mem.ramdisk_blocks * 512u);
		/* pins */
		for (i = 0; i < 3u; i++)
			CHECK(dev->pins.mux[i] == p->mux[i] && dev->pins.fer[i] == p->fer[i]);
		CHECK(dev->pins.gpio_n == p->gpio_n);
		for (i = 0; i < p->gpio_n && i < dev->pins.gpio_n; i++)
			CHECK(dev->pins.gpio[i].pin == p->gpio[i].pin &&
			      dev->pins.gpio[i].kind == p->gpio[i].kind);
		CHECK(dev->pins.panel_dc == p->panel_dc);
		/* keys */
		CHECK(dev->keys.count == p->key_count && dev->keys.bitmap_bytes == p->bitmap_bytes);
		CHECK(dev->keys.count <= WB_KEYS_MAX && dev->keys.bitmap_bytes <= WB_BITMAP_MAX);
		CHECK(dev->keys.bitmap_bytes == (dev->keys.count + 7u) / 8u);
		CHECK(memcmp(dev->keys.hold, p->hold, 2) == 0 && memcmp(dev->keys.debug, p->debug, 2) == 0);
		CHECK(dev->keys.debug_both == (p->debug_both != 0));
		CHECK(dev->keys.precomp == p->precomp);
		CHECK(memcmp(dev->keys.digit, p->digit, sizeof p->digit) == 0);
		CHECK(dev->keys.digit_knob == p->digit_knob);
		CHECK(dev->keys.shift == p->shift && dev->keys.confirm == p->confirm);
		CHECK(dev->keys.com == p->com && dev->keys.recovery == p->recovery);
		/* the com key is one of the two hold keys (S-18.030) */
		CHECK(dev->keys.com == dev->keys.hold[0] || dev->keys.com == dev->keys.hold[1]);
		/* every key named is one the device has */
		for (i = 0; i < 8u; i++)
			for (j = 0; j < 3u; j++)
				CHECK(dev->keys.digit[i][j] == WB_KEY_NONE ||
				      dev->keys.digit[i][j] < dev->keys.count);
	}
	/* an id no device has: the default */
	CHECK(wb_device_of(2) == &wb_device_opxy && wb_device_of(0xFFFFFFFFu) == &wb_device_opxy);
	CHECK(same_str(WB_DEVICE_ID_UNKNOWN, S18V_DEVICE_ID_UNKNOWN));
}

/* S-18.020: the bits in the table's order, and each device's column. */
static void test_features(void)
{
	CHECK(ARRAY_LEN(s18v_features) == WB_FEATURE_COUNT);
	for (unsigned f = 0; f < ARRAY_LEN(s18v_features); f++)
		for (unsigned d = 0; d < 2u; d++)
			CHECK(wb_has(devs[d], (enum wb_feature)(1u << f)) == (s18v_features[f].has[d] != 0));
	for (unsigned d = 0; d < 2u; d++)
		CHECK((devs[d]->features >> WB_FEATURE_COUNT) == 0u);
	CHECK(WB_F_SECOND_CPU == 1u << 0 && WB_F_MTP_TWO_ITEMS == 1u << 10);
}

/* S-18.003's table: one row per device, and nothing else in it. */
static void test_table(void)
{
	enum wb_dev_id id;

	CHECK(ARRAY_LEN(s18v_table) == WB_DEV_COUNT);
	for (unsigned i = 0; i < ARRAY_LEN(s18v_table); i++) {
		const struct s18v_row *r = &s18v_table[i];
		const struct wb_device *dev = wb_device_of((unsigned)r->device);

		CHECK(dev->first_stage_n == r->n && dev->first_stage_crc == r->c);
		id = (enum wb_dev_id)9;
		CHECK(wb_first_stage_lookup(r->n, r->c, &id) && id == (enum wb_dev_id)r->device);
		CHECK(!wb_first_stage_lookup(r->n + 1u, r->c, &id));
		CHECK(!wb_first_stage_lookup(r->n, r->c ^ 1u, &id));
	}
	/* each row's CRC at the other's length */
	CHECK(!wb_first_stage_lookup(s18v_table[0].n, s18v_table[1].c, &id));
	CHECK(!wb_first_stage_lookup(s18v_table[1].n, s18v_table[0].c, &id));
	CHECK(S18V_ARG_OFF == WB_FS_ARG_OFF && S18V_HEAD == WB_FS_HEAD && S18V_N_MAX == WB_FS_N_MAX);
}

/* ---- a flash that counts each byte's reads ---- */

#define FLASH_LEN 0xD000u

static uint8_t g_flash[FLASH_LEN];
static uint8_t g_reads[FLASH_LEN];
static unsigned g_calls, g_fail_at;	/* g_fail_at: this call fails (1 first), 0 none */
static bool g_past;			/* a read past FLASH_LEN */

static int flash_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (++g_calls == g_fail_at)
		return -1;
	if ((uint64_t)off + len > FLASH_LEN) {
		g_past = true;
		return -1;
	}
	memcpy(buf, g_flash + off, len);
	for (uint32_t i = 0; i < len; i++)
		g_reads[off + i]++;
	return 0;
}

static void put_stage(const struct s18v_stage *s)
{
	memset(g_flash, 0xFF, sizeof g_flash);
	memset(g_reads, 0, sizeof g_reads);
	g_calls = 0;
	g_fail_at = 0;
	g_past = false;
	if (s == NULL)
		return;
	for (uint32_t i = 0; i < s->length && i < FLASH_LEN; i++)
		g_flash[i] = s->seed < 0 ? 0xFFu : (uint8_t)(i * 37u + (unsigned)s->seed);
	if (s->has_arg) {
		g_flash[S18V_ARG_OFF + 0] = (uint8_t)s->argument;
		g_flash[S18V_ARG_OFF + 1] = (uint8_t)(s->argument >> 8);
		g_flash[S18V_ARG_OFF + 2] = (uint8_t)(s->argument >> 16);
		g_flash[S18V_ARG_OFF + 3] = (uint8_t)(s->argument >> 24);
	}
	for (unsigned k = 0; k < s->npatch; k++)
		memcpy(g_flash + s->patch[k].off, s->patch[k].b, 4);
}

/* Bytes [0, end) each read once, none after it. */
static bool read_once(uint32_t end)
{
	for (uint32_t i = 0; i < FLASH_LEN; i++)
		if (g_reads[i] != (i < end ? 1u : 0u))
			return false;
	return !g_past;
}

/* S-18.003, S-18.004: every synthetic first stage. */
static void test_stages(void)
{
	for (unsigned i = 0; i < ARRAY_LEN(s18v_stages); i++) {
		const struct s18v_stage *s = &s18v_stages[i];
		struct wb_dev_decision d;
		uint32_t n = 1, c = 1;

		put_stage(s);
		/* the stage is built as the vector's model built it */
		if (s->has_c)
			CHECK(ref_crc32(0, g_flash, s->n) == s->c);
		d = wb_first_stage_identify(flash_read, NULL, &n, &c);
		if (n != s->n || c != s->c)
			printf("  %s: n %08x c %08x\n", s->name, (unsigned)n, (unsigned)c);
		CHECK(n == s->n);
		CHECK(c == (s->has_c ? s->c : 0u));
		if (s->device >= 0)
			CHECK(d.id == (enum wb_dev_id)s->device && d.how == WB_DEV_BY_TABLE);
		else
			CHECK(d.id == WB_DEV_DEFAULT && d.how == WB_DEV_UNKNOWN);
		/* read once: [0, n) in range, else the header alone */
		CHECK(read_once(s->has_c ? s->n : S18V_HEAD));
	}
}

/* No try: nothing read, n and c 0, the default device, not known. */
static void test_no_tries(void)
{
	struct wb_dev_decision d;
	uint32_t n = 7, c = 7;

	put_stage(&s18v_stages[0]);
	g_calls = 0;
	d = wb_first_stage_identify_n(flash_read, NULL, &n, &c, 0u);
	CHECK(n == 0u && c == 0u && g_calls == 0u);
	CHECK(d.id == WB_DEV_DEFAULT && d.how == WB_DEV_UNKNOWN);
}

/* S-18.002's fallback sample: PA2 as the row has it. */
static unsigned g_pa2_level, g_pa2_samples;

static uint16_t pa2_sample(void *ctx)
{
	(void)ctx;
	g_pa2_samples++;
	return g_pa2_level != 0u ? WB_STRAP_PA2 : 0u;
}

/* S-18.002 with S-18.003 and S-18.004: each row of identify.json. */
static void test_decide(void)
{
	for (unsigned i = 0; i < ARRAY_LEN(s18v_idents); i++) {
		const struct s18v_ident *r = &s18v_idents[i];
		const struct wb_device *dev;
		struct wb_dev_decision d;
		struct wb_dev_role role;
		uint32_t n, c;
		bool any = false;

		put_stage(r->stage < 0 ? NULL : &s18v_stages[r->stage]);
		g_pa2_level = r->pa2;
		g_pa2_samples = 0;
		wb_device_decide_role(r->boot_mode, pa2_sample, NULL, flash_read, NULL, &role);
		d = role.d;
		n = role.n;
		c = role.c;
		dev = wb_device_of(d.id);
		/* the fallback sample alone where the boot mode is not 1; its
		 * main CPU flagged (the record's flags bit 12) */
		CHECK(g_pa2_samples == (r->fallback_sample ? 1u : 0u));
		CHECK(role.fallback == (r->role_flag != 0) && role.main == (r->main_cpu != 0));
		CHECK((unsigned)d.id == r->device && (unsigned)d.how == r->how);
		CHECK(n == r->n && c == r->c);
		for (uint32_t k = 0; k < FLASH_LEN; k++)
			any = any || g_reads[k] != 0u;
		CHECK(any == (r->reads_flash != 0) && (g_calls != 0u) == (r->reads_flash != 0));
		CHECK((r->main_cpu != 0) == (d.how != WB_DEV_SECOND_CPU));
		/* S-18.031's entry */
		CHECK(same_str(d.how == WB_DEV_UNKNOWN ? WB_DEVICE_ID_UNKNOWN : dev->device_id,
			       r->device_id));
		/* the strap sample on a device with straps; the main CPU's
		 * disagreement with PA2 = 0 logged */
		CHECK(wb_has(dev, WB_F_STRAPS) == (r->strap_sample != 0));
		CHECK((r->main_cpu && wb_has(dev, WB_F_STRAPS) && r->pa2 == 0u) ==
		      (r->disagreement != 0));
		/* S-18.005: the record's codes */
		CHECK((unsigned)d.id == (d.id == WB_DEV_OP1F ? FR_HO_DEV_OP1F : FR_HO_DEV_OPXY));
	}
}

/* A read that fails: not known, the default taken, nothing more read. */
static void test_read_fails(void)
{
	const struct s18v_stage *xy = &s18v_stages[0];
	struct wb_dev_decision d;
	uint32_t n, c;

	put_stage(xy);
	g_fail_at = 1;
	d = wb_first_stage_identify(flash_read, NULL, &n, &c);
	CHECK(d.id == WB_DEV_DEFAULT && d.how == WB_DEV_UNKNOWN && n == 0u && c == 0u);
	CHECK(g_calls == 1u);
	for (unsigned k = 2; k < 6u; k++) {
		put_stage(xy);
		g_fail_at = k;
		d = wb_first_stage_identify(flash_read, NULL, &n, &c);
		CHECK(d.id == WB_DEV_DEFAULT && d.how == WB_DEV_UNKNOWN);
		CHECK(n == xy->n && c == 0u && g_calls == k);
	}
	/* the same stage read whole names its device */
	put_stage(xy);
	d = wb_first_stage_identify(flash_read, NULL, &n, &c);
	CHECK(d.id == WB_DEV_OPXY && d.how == WB_DEV_BY_TABLE && n == xy->n && c == xy->c);
}

/* S-18.030: the digit each key names. */
static void test_key_digit(void)
{
	for (unsigned d = 0; d < 2u; d++) {
		const struct wb_device *dev = devs[d];
		unsigned count[9] = { 0 };

		for (unsigned k = 0; k < 0x100u; k++) {
			const unsigned g = wb_key_digit(dev, k);

			CHECK(g <= 8u);
			if (g <= 8u)
				count[g]++;
			if (g != 0u)
				CHECK(k < dev->keys.count);
		}
		for (unsigned g = 1; g <= 8u; g++)
			CHECK(count[g] == (g <= 4u ? 3u : 1u));
		CHECK(wb_key_digit(dev, dev->keys.recovery) == 4u);
		CHECK(wb_key_digit(dev, WB_KEY_NONE) == 0u);
	}
	/* the OP-XY's arithmetic (S-15.011): 0x06 to 0x11 by four, then 0x12 to 0x15 */
	for (unsigned k = 0; k < 0x100u; k++) {
		const unsigned want = k >= 0x06u && k <= 0x11u ? (k - 0x06u) % 4u + 1u :
				      k >= 0x12u && k <= 0x15u ? k - 0x12u + 5u : 0u;

		CHECK(wb_key_digit(&wb_device_opxy, k) == want);
	}
	/* the knob's click: the second key on the OP-XY, the third on the OP-1F */
	CHECK(wb_device_opxy.keys.digit[3][wb_device_opxy.keys.digit_knob] == 0x0Du);
	CHECK(wb_device_op1f.keys.digit[3][wb_device_op1f.keys.digit_knob] == 0x39u);
}

/* device.midi_member. */
static void test_midi_member(void)
{
	static const struct { uint16_t sku; uint8_t b[2]; } op1f[] = {
		{ 0xFFFFu, { 0x02u, 0x00u } }, { 0x0000u, { 0x00u, 0x00u } },
		{ 0x1234u, { 0x34u, 0x24u } }, { 0x3FFFu, { 0x7Fu, 0x7Fu } },
		{ 0x0080u, { 0x00u, 0x01u } }, { 0x007Fu, { 0x7Fu, 0x00u } },
	};
	uint8_t out[2];

	for (unsigned i = 0; i < ARRAY_LEN(op1f); i++) {
		wb_midi_member(&wb_device_op1f, op1f[i].sku, out);
		CHECK(memcmp(out, op1f[i].b, 2) == 0);
		wb_midi_member(&wb_device_opxy, op1f[i].sku, out);
		CHECK(out[0] == 0x01u && out[1] == 0x00u);
	}
}

/* The board's OP-XY tables and the record's codes agree with the profile. */
static void test_board_agrees(void)
{
	for (unsigned i = 0; i < 3u; i++) {
		CHECK(opxy_pinmux_master.port[i].mux == wb_device_opxy.pins.mux[i]);
		CHECK(opxy_pinmux_master.port[i].fer == wb_device_opxy.pins.fer[i]);
	}
	CHECK(WB_DEV_PIN(2u, 11u) == WB_PIN(WB_PORT_C, 11u) && WB_DEV_PA(5u) == WB_PIN(WB_PORT_A, 5u));
	CHECK(FR_HO_DEV_OPXY == WB_DEV_OPXY && FR_HO_DEV_OP1F == WB_DEV_OP1F);
	CHECK(FR_HO_HOW_TABLE == WB_DEV_BY_TABLE && FR_HO_HOW_SECOND == WB_DEV_SECOND_CPU &&
	      FR_HO_HOW_UNKNOWN == WB_DEV_UNKNOWN);
}

int main(void)
{
	RUN(test_profiles);
	RUN(test_features);
	RUN(test_table);
	RUN(test_stages);
	RUN(test_no_tries);
	RUN(test_decide);
	RUN(test_read_fails);
	RUN(test_key_digit);
	RUN(test_midi_member);
	RUN(test_board_agrees);
	return check_summary("test_device");
}
