/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's devices (src/front/fr_device.c): its copy of each device's
 * values against src/core/boot/device.c's profiles and
 * tests/oracle/S-18-devices/vectors/{first_stage,profile}.json (S-18.003:
 * one table everywhere; S-18.030), and fr_identify() over every synthetic
 * first stage (S-18.003, S-18.004): each byte of [0, n) read once per try,
 * nothing written, the device and fingerprint the vectors give, a failed
 * read or an unlisted fingerprint tried again (the vectors' tries, with
 * wb_first_stage_identify_tries() alongside); and fr_decide()'s order
 * (S-18.001, S-18.002, the fallback sample).
 */
#include <string.h>

#include "support/check.h"
#include "vectors/s18_vectors.h"

#include "front/fr_device.h"
#include "front/fr_handoff.h"
#include "front/fr_psoc.h"
#include "front/fr_slots.h"
#include "front/fr_usb.h"
#include "waffle/boot/device.h"
#include "waffle/drivers/bf70x_port.h"

static const struct fr_dev *const frs[2] = { &fr_dev_opxy, &fr_dev_op1f };
static const struct wb_device *const wbs[2] = { &wb_device_opxy, &wb_device_op1f };

static bool same_str(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* The profile's GPIO entry for pin, or NULL. */
static const struct wb_dev_gpio *profile_gpio(const struct wb_device *wb, uint8_t pin)
{
	for (unsigned i = 0; i < wb->pins.gpio_n; i++)
		if (wb->pins.gpio[i].pin == pin)
			return &wb->pins.gpio[i];
	return NULL;
}

/* S-18.030: every value the front holds, per device. */
static void test_profiles(void)
{
	CHECK(FR_DEV_COUNT == WB_DEV_COUNT);
	for (unsigned d = 0; d < 2u; d++) {
		const struct fr_dev *fr = frs[d];
		const struct wb_device *wb = wbs[d];
		const struct s18v_profile *p = &s18v_profiles[d];
		char product[32];

		CHECK(fr->id == (unsigned)wb->id && fr_dev_of(d) == fr);
		/* S-18.003's row */
		CHECK(fr->fs_n == wb->first_stage_n && fr->fs_c == wb->first_stage_crc);
		/* S-18.020 */
		CHECK(fr_dev_has(fr, FR_DEV_F_STRAPS) == wb_has(wb, WB_F_STRAPS));
		CHECK(fr_dev_has(fr, FR_DEV_F_GAMMA) == wb_has(wb, WB_F_DIGITAL_GAMMA));
		CHECK(fr_dev_has(fr, FR_DEV_F_KEY_LEDS) == wb_has(wb, WB_F_KEY_LEDS));
		CHECK((fr->flags & ~(unsigned)(FR_DEV_F_STRAPS | FR_DEV_F_GAMMA |
					       FR_DEV_F_KEY_LEDS)) == 0u);
		/* pins (S-13b.003) and the panel's D/C (S-13c.001), a pin of port A */
		for (unsigned i = 0; i < 3u; i++) {
			CHECK(fr->mux[i] == wb->pins.mux[i] && fr->mux[i] == p->mux[i]);
			CHECK(fr->fer[i] == wb->pins.fer[i] && fr->fer[i] == p->fer[i]);
		}
		CHECK(fr->panel_dc == wb->pins.panel_dc && fr->panel_dc == p->panel_dc);
		CHECK(WB_PIN_PORT(fr->panel_dc) == WB_PORT_A);
		/* keys */
		CHECK(fr->key_count == wb->keys.count && fr->key_count == p->key_count);
		CHECK(fr->key_bytes == wb->keys.bitmap_bytes && fr->key_bytes == p->bitmap_bytes);
		CHECK(fr->key_bytes <= FR_DEV_KEY_BYTES_MAX && FR_KEYS_BYTES == FR_DEV_KEY_BYTES_MAX);
		CHECK(fr->hold[0] == p->hold[0] && fr->hold[1] == p->hold[1]);
		CHECK(fr->precomp == p->precomp && FR_KEY_NONE == WB_KEY_NONE);
		CHECK(fr->key_back == p->digit[7][0] && wb_key_digit(wb, fr->key_back) == 8u);
		CHECK(fr->key_recovery == p->recovery && wb_key_digit(wb, fr->key_recovery) == 4u);
		/* recovery's keys 1 to 4 are each digit's first key on both */
		CHECK(p->digit[0][0] == FR_KEY_1 && p->digit[1][0] == FR_KEY_2 &&
		      p->digit[2][0] == FR_KEY_3 && p->digit[3][0] == FR_KEY_4);
		/* S-02.093: device.usb_pid, device.product_text + " Recovery" */
		CHECK(fr->usb_pid == wb->ident.usb_pid && fr->usb_pid == p->usb_pid);
		CHECK(strlen(p->product_text) + 10u < sizeof product);
		strcpy(product, p->product_text);
		strcat(product, " Recovery");
		CHECK(same_str(fr->usb_product, product));
	}
	/* the default device (S-18.004) */
	CHECK(fr_dev_of(2) == &fr_dev_opxy && fr_dev_of(0xFFu) == &fr_dev_opxy);
	CHECK(fr_dev_or_default(NULL) == &fr_dev_opxy &&
	      fr_dev_or_default(&fr_dev_op1f) == &fr_dev_op1f);
	CHECK(fr_dev_of(WB_DEV_DEFAULT) == &fr_dev_opxy);
	/* the record's codes are boot/device.h's */
	CHECK(FR_HO_DEV_OPXY == WB_DEV_OPXY && FR_HO_DEV_OP1F == WB_DEV_OP1F);
	CHECK(FR_HO_HOW_TABLE == WB_DEV_BY_TABLE && FR_HO_HOW_SECOND == WB_DEV_SECOND_CPU &&
	      FR_HO_HOW_UNKNOWN == WB_DEV_UNKNOWN);
	CHECK(FR_FS_ARG_OFF == WB_FS_ARG_OFF && FR_FS_HEAD == WB_FS_HEAD &&
	      FR_FS_N_MAX == WB_FS_N_MAX);
	CHECK(FR_FS_ARG_OFF == S18V_ARG_OFF && FR_FS_HEAD == S18V_HEAD && FR_FS_N_MAX == S18V_N_MAX);
}

/*
 * S-13b.004, S-13b.006: the front's GPIOs. On the OP-1F they are the
 * profile's, in its order, with no inputs-first pin and PA8 never an
 * input. On the OP-XY the front sets the profile's but PC11 and PC5, the
 * bridge's pins, which the full bootloader sets, with PA7 before PA6, and
 * makes PB10 and PC4 inputs first.
 */
static void test_gpio(void)
{
	static const uint8_t first_xy[2] = { WB_PIN(WB_PORT_B, 10), WB_PIN(WB_PORT_C, 4) };
	const struct fr_dev *xy = &fr_dev_opxy, *f = &fr_dev_op1f;
	const uint8_t pa7 = WB_PIN(WB_PORT_A, 7), pa6 = WB_PIN(WB_PORT_A, 6);
	unsigned at7 = 99, at6 = 99;

	/* the OP-1F: the same list */
	CHECK(f->gpio_n == wb_device_op1f.pins.gpio_n && f->gpio_n == s18v_profiles[1].gpio_n);
	for (unsigned i = 0; i < f->gpio_n && i < wb_device_op1f.pins.gpio_n; i++) {
		CHECK(f->gpio[i].pin == wb_device_op1f.pins.gpio[i].pin);
		CHECK(f->gpio[i].kind == wb_device_op1f.pins.gpio[i].kind);
		CHECK(f->gpio[i].pin != WB_PIN(WB_PORT_A, 8));
	}
	CHECK(f->inputs_first_n == 0u);
	/* the OP-XY: the profile's but the bridge's two */
	CHECK(xy->gpio_n + 2u == wb_device_opxy.pins.gpio_n);
	for (unsigned i = 0; i < xy->gpio_n; i++) {
		const struct wb_dev_gpio *g = profile_gpio(&wb_device_opxy, xy->gpio[i].pin);

		CHECK(g != NULL && g->kind == xy->gpio[i].kind);
		if (xy->gpio[i].pin == pa7)
			at7 = i;
		if (xy->gpio[i].pin == pa6)
			at6 = i;
	}
	CHECK(at7 < at6 && at6 < xy->gpio_n);
	for (unsigned i = 0; i < wb_device_opxy.pins.gpio_n; i++) {
		const uint8_t pin = wb_device_opxy.pins.gpio[i].pin;
		bool in_front = false;

		for (unsigned j = 0; j < xy->gpio_n; j++)
			in_front = in_front || xy->gpio[j].pin == pin;
		CHECK(in_front || pin == WB_PIN(WB_PORT_C, 11) || pin == WB_PIN(WB_PORT_C, 5));
	}
	CHECK(xy->inputs_first_n == 2u && memcmp(xy->inputs_first, first_xy, 2) == 0);
	/* the kinds' codes are boot/device.h's */
	CHECK(FR_GPIO_IN == WB_GPIO_IN && FR_GPIO_LOW == WB_GPIO_OUT_LOW &&
	      FR_GPIO_HIGH == WB_GPIO_OUT_HIGH && FR_GPIO_KEEP == WB_GPIO_OUT_KEEP);
}

/* S-18.003's table, the front's copy against the vectors' */
static void test_table(void)
{
	CHECK(ARRAY_LEN(s18v_table) == FR_DEV_COUNT);
	for (unsigned i = 0; i < ARRAY_LEN(s18v_table); i++) {
		const struct fr_dev *fr = fr_dev_of((unsigned)s18v_table[i].device);

		CHECK(fr->id == (unsigned)s18v_table[i].device);
		CHECK(fr->fs_n == s18v_table[i].n && fr->fs_c == s18v_table[i].c);
	}
}

/* ---- a flash that counts each byte's reads and refuses every write ---- */

#define FLASH_LEN 0xD000u

static uint8_t g_flash[FLASH_LEN];
static uint8_t g_reads[FLASH_LEN];
/* g_fail_at: this read fails (1 first), FR_FLASH_IO; g_fail_off: every
 * read at this offset fails, with g_fail_rc */
static unsigned g_calls, g_fail_at, g_writes;
static uint32_t g_fail_off;
static int g_fail_rc;
static bool g_past;
/* the order of the decision's calls: B boot mode, S strap, F flash bus,
 * R a flash read */
static char g_ev[1024];
static size_t g_nev;

static void ev(char c)
{
	if (g_nev + 1u < sizeof g_ev)
		g_ev[g_nev++] = c;
}

static int f_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	ev('R');
	/* a failed read reads nothing that counts */
	if (++g_calls == g_fail_at)
		return FR_FLASH_IO;
	if (off == g_fail_off)
		return g_fail_rc;
	if ((uint64_t)off + len > FLASH_LEN) {
		g_past = true;
		return -1;
	}
	memcpy(buf, g_flash + off, len);
	for (uint32_t i = 0; i < len; i++)
		g_reads[off + i]++;
	return 0;
}

static int f_erase(void *ctx, uint32_t off)
{
	(void)ctx;
	(void)off;
	g_writes++;
	return -1;
}

static int f_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	(void)ctx;
	(void)off;
	(void)buf;
	(void)len;
	g_writes++;
	return -1;
}

static const struct fr_flash flash = { f_read, f_erase, f_program, NULL };

/* As test_device.c builds them: the fill, the argument, the patches. */
static void put_stage(const struct s18v_stage *s)
{
	memset(g_flash, 0xFF, sizeof g_flash);
	memset(g_reads, 0, sizeof g_reads);
	g_calls = 0;
	g_fail_at = 0;
	g_fail_off = 0xFFFFFFFFu;
	g_fail_rc = FR_FLASH_IO;
	g_writes = 0;
	g_past = false;
	memset(g_ev, 0, sizeof g_ev);
	g_nev = 0;
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

/* Bytes [0, end) each read k times, none after it. */
static bool read_times(uint32_t end, unsigned k)
{
	for (uint32_t i = 0; i < FLASH_LEN; i++)
		if (g_reads[i] != (i < end ? k : 0u))
			return false;
	return !g_past;
}

static bool read_once(uint32_t end)
{
	return read_times(end, 1u);
}

/* The main image's reading, through the same flash. */
static int wb_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	return f_read(ctx, off, buf, len);
}

/* S-18.003, S-18.004: every synthetic first stage, as the vectors say and
 * as the full bootloader's own decision says. */
static void test_identify(void)
{
	for (unsigned i = 0; i < ARRAY_LEN(s18v_stages); i++) {
		const struct s18v_stage *s = &s18v_stages[i];
		struct fr_ident id;
		struct wb_dev_decision w;
		uint32_t n, c;

		put_stage(s);
		memset(&id, 0xA5, sizeof id);
		fr_identify(&flash, &id);
		CHECK(id.n == s->n && id.c == (s->has_c ? s->c : 0u));
		if (s->device >= 0)
			CHECK(id.dev == fr_dev_of((unsigned)s->device) && id.how == FR_HO_HOW_TABLE);
		else
			CHECK(id.dev == &fr_dev_opxy && id.how == FR_HO_HOW_UNKNOWN);
		/* S-18.003: an unlisted fingerprint is tried again, each try
		 * reading each byte once */
		CHECK(read_times(s->has_c ? s->n : FR_FS_HEAD, s->device >= 0 ? 1u : FR_FS_TRIES));
		CHECK(g_writes == 0u);
		put_stage(s);
		w = wb_first_stage_identify(wb_read, NULL, &n, &c);
		CHECK((unsigned)w.id == id.dev->id && (unsigned)w.how == id.how);
		CHECK(n == id.n && c == id.c);
		CHECK(read_once(s->has_c ? s->n : FR_FS_HEAD));	/* the later look: one try */
		put_stage(s);
		w = wb_first_stage_identify_tries(wb_read, NULL, &n, &c);
		CHECK((unsigned)w.id == id.dev->id && (unsigned)w.how == id.how);
		CHECK(n == id.n && c == id.c);
		CHECK(read_times(s->has_c ? s->n : FR_FS_HEAD, s->device >= 0 ? 1u : WB_FS_TRIES));
	}
}

static const struct s18v_stage *stage(const char *name)
{
	for (unsigned i = 0; i < ARRAY_LEN(s18v_stages); i++)
		if (strcmp(s18v_stages[i].name, name) == 0)
			return &s18v_stages[i];
	CHECK(name == NULL);	/* every name is one of the vectors' */
	return &s18v_stages[0];
}

/* The reads of one whole try over a first stage of n bytes. */
static unsigned try_reads(uint32_t n)
{
	return 1u + (n - FR_FS_HEAD + 255u) / 256u;
}

/* S-18.003, S-18.004: a read error is not a row's absence. One failed
 * read anywhere in a try costs that try alone: the whole read again, its
 * bytes each read once in it. */
static void test_read_retries(void)
{
	const struct s18v_stage *xy = stage("xy_row"), *f1 = stage("1f_row");
	const unsigned per = try_reads(xy->n);
	struct fr_ident id;

	CHECK(FR_FS_TRIES == 3u);
	for (unsigned k = 1; k <= per; k += 7u) {
		put_stage(xy);
		g_fail_at = k;
		fr_identify(&flash, &id);
		CHECK(id.dev == &fr_dev_opxy && id.how == FR_HO_HOW_TABLE);
		CHECK(id.n == xy->n && id.c == xy->c && g_calls == k + per);
		CHECK(g_writes == 0u);
	}
	/* an OP-1F is not made an OP-XY by one failed read */
	put_stage(f1);
	g_fail_at = 40;
	fr_identify(&flash, &id);
	CHECK(id.dev == &fr_dev_op1f && id.how == FR_HO_HOW_TABLE);
	CHECK(id.n == f1->n && id.c == f1->c);
	/* and the last try's bytes are each read once in it */
	put_stage(f1);
	g_fail_at = 2;
	fr_identify(&flash, &id);
	CHECK(id.dev == &fr_dev_op1f && g_calls == 2u + try_reads(f1->n));
	for (uint32_t i = FR_FS_HEAD; i < f1->n; i++)
		CHECK(g_reads[i] == 1u);
	CHECK(g_reads[0] == 2u);		/* the first try's header and the last's */
}

/* Three failed tries: not known, the default taken, the last try's n. */
static void test_read_fails(void)
{
	const struct s18v_stage *xy = stage("xy_row"), *f1 = stage("1f_row");
	struct fr_ident id;

	/* the header fails every time */
	put_stage(xy);
	g_fail_off = 0;
	fr_identify(&flash, &id);
	CHECK(id.dev == &fr_dev_opxy && id.how == FR_HO_HOW_UNKNOWN && id.n == 0u && id.c == 0u);
	CHECK(g_calls == FR_FS_TRIES);
	/* a later piece fails every time: n known, c not */
	put_stage(f1);
	g_fail_off = FR_FS_HEAD + 256u * 10u;
	fr_identify(&flash, &id);
	CHECK(id.dev == &fr_dev_opxy && id.how == FR_HO_HOW_UNKNOWN);
	CHECK(id.n == f1->n && id.c == 0u && g_calls == FR_FS_TRIES * 12u);
	CHECK(g_writes == 0u);
	/* an argument whose n does not fit: 0xFFFFFFFF, the header alone read
	 * in each try (no row holds it) */
	put_stage(xy);
	memset(g_flash + FR_FS_ARG_OFF, 0xFF, 4);
	fr_identify(&flash, &id);
	CHECK(id.n == 0xFFFFFFFFu && id.c == 0u && id.how == FR_HO_HOW_UNKNOWN);
	CHECK(read_times(FR_FS_HEAD, FR_FS_TRIES));
	/* an erased flash: three tries of its header */
	put_stage(stage("erased"));
	fr_identify(&flash, &id);
	CHECK(id.how == FR_HO_HOW_UNKNOWN && g_calls == FR_FS_TRIES);
}

/* ---- the vectors' tries (S-18.003), in the front and in the full
 * bootloader's wb_first_stage_identify_tries() ---- */

static const struct s18v_try *g_t;
static unsigned g_try;		/* the try under way, 1 first */
static bool g_wb;		/* the full bootloader's return codes */

static int t_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	int f;

	(void)ctx;
	if (off == 0u)
		g_try++;
	g_calls++;
	f = g_try <= g_t->nfault ? g_t->fault[g_try - 1u] : S18V_F_WHOLE;
	if ((off == 0u && (f == S18V_F_HEAD || f == S18V_F_BUSY_HEAD)) ||
	    (off >= FR_FS_HEAD && (f == S18V_F_LATER || f == S18V_F_BUSY_LATER))) {
		const bool busy = f == S18V_F_BUSY_HEAD || f == S18V_F_BUSY_LATER;

		return busy ? (g_wb ? WB_FS_READ_BUSY : FR_FLASH_BUSY) : (g_wb ? -1 : FR_FLASH_IO);
	}
	if ((uint64_t)off + len > FLASH_LEN) {
		g_past = true;
		return -1;
	}
	memcpy(buf, g_flash + off, len);
	if (off == FR_FS_HEAD && f == S18V_F_FLIP)
		buf[0] ^= 0x01u;
	return 0;
}

static const struct fr_flash t_flash = { t_read, f_erase, f_program, NULL };

static void test_tries(void)
{
	CHECK(S18V_TRIES == FR_FS_TRIES && S18V_TRIES == WB_FS_TRIES);
	for (unsigned i = 0; i < ARRAY_LEN(s18v_tries); i++) {
		const struct s18v_try *t = &s18v_tries[i];
		struct fr_ident id;
		struct wb_dev_decision w;
		uint32_t n, c;

		g_t = t;
		put_stage(&s18v_stages[t->stage]);
		g_try = 0;
		g_wb = false;
		fr_identify(&t_flash, &id);
		CHECK(g_try == t->tries && !g_past && g_writes == 0u);
		CHECK(id.n == t->n && id.c == t->c && id.how == t->how);
		CHECK(id.dev == (t->device >= 0 ? fr_dev_of((unsigned)t->device) : &fr_dev_opxy));
		put_stage(&s18v_stages[t->stage]);
		g_try = 0;
		g_wb = true;
		w = wb_first_stage_identify_tries(t_read, NULL, &n, &c);
		CHECK(g_try == t->tries && !g_past);
		CHECK(n == t->n && c == t->c && (unsigned)w.how == t->how && (unsigned)w.id == id.dev->id);
	}
}

/* FR_FLASH_BUSY: the wait before the read ran its bound; no try after it,
 * so a flash that never reads ready costs one wait. */
static void test_read_busy(void)
{
	const struct s18v_stage *f1 = stage("1f_row");
	struct fr_ident id;

	put_stage(f1);
	g_fail_off = 0;
	g_fail_rc = FR_FLASH_BUSY;
	fr_identify(&flash, &id);
	CHECK(id.dev == &fr_dev_opxy && id.how == FR_HO_HOW_UNKNOWN && g_calls == 1u);
	/* busy in the second try: no third */
	put_stage(f1);
	g_fail_at = 1;
	g_fail_off = FR_FS_HEAD;
	g_fail_rc = FR_FLASH_BUSY;
	fr_identify(&flash, &id);
	CHECK(g_calls == 3u && id.how == FR_HO_HOW_UNKNOWN);
	/* busy after the header: n known, c not */
	put_stage(f1);
	g_fail_off = FR_FS_HEAD;
	g_fail_rc = FR_FLASH_BUSY;
	fr_identify(&flash, &id);
	CHECK(g_calls == 2u && id.n == f1->n && id.c == 0u && id.how == FR_HO_HOW_UNKNOWN);
}

/* ---- fr_decide(): the order of S-18.001 and S-18.002 ---- */

static unsigned g_bmode, g_straps, g_buses, g_pa2s;
static uint16_t g_strap, g_pa2;

static unsigned d_boot_mode(void *ctx)
{
	(void)ctx;
	ev('B');
	return g_bmode;
}

static uint16_t d_pa2(void *ctx)
{
	(void)ctx;
	ev('P');
	g_pa2s++;
	return g_pa2;
}

static uint16_t d_strap(void *ctx)
{
	(void)ctx;
	ev('S');
	g_straps++;
	return g_strap;
}

static void d_bus(void *ctx)
{
	(void)ctx;
	ev('F');
	g_buses++;
}

static const struct fr_decide_hw dhw = { d_boot_mode, d_pa2, d_strap, d_bus, &flash, NULL };

/* strap: S-13b.001's sample; its PA2 is also what the fallback reads. */
static void decide(const char *name, unsigned bmode, uint16_t strap, struct fr_role *r)
{
	put_stage(stage(name));
	g_bmode = bmode;
	g_strap = strap;
	g_pa2 = strap & FR_STRAP_PA2;
	g_straps = 0;
	g_pa2s = 0;
	g_buses = 0;
	memset(r, 0xA5, sizeof *r);
	fr_decide(&dhw, r);
}

/* "B", then F, then reads alone, then what follows: tail. */
static bool order(const char *tail)
{
	size_t i = 0, t = strlen(tail);

	if (g_nev < 3u + t || g_ev[0] != 'B' || g_ev[1] != 'F')
		return false;
	for (i = 2; i < g_nev - t; i++)
		if (g_ev[i] != 'R')
			return false;
	return strcmp(g_ev + g_nev - t, tail) == 0;
}

static void test_decide_main(void)
{
	struct fr_role r;

	/* S-18.001: no port write but PORTB's (F) before the last read; the
	 * strap after it, on a device with straps */
	decide("xy_row", FR_BOOT_MODE_MAIN, 0x0105u, &r);
	CHECK(r.main && !r.by_strap && r.strapped && r.strap == 0x0105u);
	CHECK(r.ident.dev == &fr_dev_opxy && r.ident.how == FR_HO_HOW_TABLE);
	CHECK(order("S") && g_buses == 1u && g_straps == 1u && g_pa2s == 0u);
	/* a main CPU of the OP-XY whose PA2 reads 0 stays the master */
	decide("xy_row", FR_BOOT_MODE_MAIN, 0x0000u, &r);
	CHECK(r.main && !r.by_strap && r.strapped && r.strap == 0u && order("S"));
	/* the OP-1F: no strap sample */
	decide("1f_row", FR_BOOT_MODE_MAIN, 0x0105u, &r);
	CHECK(r.main && !r.by_strap && !r.strapped && r.strap == 0u);
	CHECK(r.ident.dev == &fr_dev_op1f && r.ident.how == FR_HO_HOW_TABLE);
	CHECK(order("") && g_straps == 0u);
	/* not known: the default, with its straps */
	decide("xy_length_other_crc", FR_BOOT_MODE_MAIN, 0x0004u, &r);
	CHECK(r.main && r.strapped && r.ident.dev == &fr_dev_opxy &&
	      r.ident.how == FR_HO_HOW_UNKNOWN && order("S"));
}

static void test_decide_second(void)
{
	static const unsigned modes[] = { 0u, 2u, 3u, 7u, 15u };
	struct fr_role r;

	for (unsigned i = 0; i < ARRAY_LEN(modes); i++) {
		/* S-18.002: the fallback sample; PA2 = 0, the second CPU, no
		 * flash, then its strap sample */
		decide("xy_row", modes[i], 0x0101u, &r);
		CHECK(!r.main && !r.by_strap && r.strapped && r.strap == 0x0101u);
		CHECK(r.ident.dev == &fr_dev_opxy && r.ident.how == FR_HO_HOW_SECOND);
		CHECK(r.ident.n == 0u && r.ident.c == 0u);
		CHECK(strcmp(g_ev, "BPS") == 0 && g_calls == 0u && g_buses == 0u);
		/* PA2 = 1: a main CPU all the same (flags bit 12); the fallback
		 * before the flash bus, the OP-XY's strap sample after the read */
		decide("xy_row", modes[i], 0x0005u, &r);
		CHECK(r.main && r.by_strap && r.strapped && r.strap == 0x0005u);
		CHECK(r.ident.dev == &fr_dev_opxy && r.ident.how == FR_HO_HOW_TABLE);
		CHECK(g_nev > 3u && g_ev[0] == 'B' && g_ev[1] == 'P' && g_ev[2] == 'F');
		CHECK(g_pa2s == 1u && g_straps == 1u && g_buses == 1u && g_ev[g_nev - 1u] == 'S');
		/* the OP-1F: no strap sample at all, the fallback alone */
		decide("1f_row", modes[i], 0x0004u, &r);
		CHECK(r.main && r.by_strap && r.ident.dev == &fr_dev_op1f && !r.strapped);
		CHECK(g_pa2s == 1u && g_straps == 0u && g_ev[g_nev - 1u] == 'R');
	}
}

int main(void)
{
	RUN(test_profiles);
	RUN(test_gpio);
	RUN(test_table);
	RUN(test_identify);
	RUN(test_read_retries);
	RUN(test_read_fails);
	RUN(test_read_busy);
	RUN(test_tries);
	RUN(test_decide_main);
	RUN(test_decide_second);
	return check_summary("test_front_device");
}
