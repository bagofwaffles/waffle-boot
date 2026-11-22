/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tests for ports, pin multiplexing and straps (src/drivers/bf70x/port.c). */
#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x_port.h"

static const struct wb_time cclk400 = { 400u };

/* S-13b.003 (vectors/pinmux.json). */
static const struct wb_pinmux master = {
	{ { 0x30002000u, 0x4040u }, { 0x1400FFFFu, 0xFFFFu },
	  { 0x00855100u, 0x0BD3u } },
	(const uint8_t[]){ WB_PIN(WB_PORT_B, 10), WB_PIN(WB_PORT_C, 4) },
	2,
};
static const struct wb_pinmux slave = {
	{ { 0x00000200u, 0x0000u }, { 0x00000000u, 0xFF00u },
	  { 0x055AAA00u, 0x3FF0u } },
	(const uint8_t[]){ WB_PIN(WB_PORT_B, 10) },
	1,
};

/*
 * Models the SET and CLR registers and checks S-13b.004 on every write:
 * no FER bit is set on a port whose MUX differs from the table, and no MUX
 * write happens before the table's inputs are inputs.
 */
struct port_model {
	const struct wb_pinmux *table;
	unsigned violations;
	unsigned mux_writes;
	uint32_t cycles_dir_clr, cycles_inen_set, cycles_read;
};

static bool inputs_done(const struct wb_pinmux *m)
{
	unsigned i;

	for (i = 0; i < m->n_inputs_first; i++) {
		uint8_t pin = m->inputs_first[i];

		if ((fake_mmr_get(PORT_DIR(WB_PIN_PORT(pin))) & WB_PIN_BIT(pin)) ||
		    !(fake_mmr_get(PORT_INEN(WB_PIN_PORT(pin))) & WB_PIN_BIT(pin)))
			return false;	/* S-13b.004: an input, INEN set */
	}
	return true;
}

static uint32_t changed_pins(uint32_t a, uint32_t b)
{
	uint32_t pins = 0;
	unsigned i;

	for (i = 0; i < 16; i++)
		if (((a ^ b) >> (2u * i)) & 3u)
			pins |= 1u << i;
	return pins;
}

/* The hook type fixes value's type. */
static bool port_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		      /* cppcheck-suppress constParameterCallback */
		      wb_mmr_addr addr, uint32_t *value)
{
	struct port_model *pm = ctx;
	uint32_t p, off;

	(void)width;
	if (addr < PORT_BASE(0) || addr >= PORT_BASE(WB_PORTS))
		return true;
	p = (addr - PORT_BASE(0)) / 0x80u;
	off = (addr - PORT_BASE(0)) % 0x80u;
	if (op == FAKE_MMR_READ) {
		if (off == 0x0Cu)
			pm->cycles_read = (uint32_t)fake_core_peek_cycles();
		return true;
	}
	switch (off) {
	case 0x04u: case 0x10u: case 0x1Cu: case 0x28u:
		fake_mmr_set(addr - 4u, fake_mmr_get(addr - 4u) | *value);
		if (off == 0x28u)
			pm->cycles_inen_set = (uint32_t)fake_core_peek_cycles();
		if (off == 0x04u && pm->table &&
		    fake_mmr_get(PORT_MUX(p)) != pm->table->port[p].mux)
			pm->violations++;
		return false;
	case 0x08u: case 0x14u: case 0x20u: case 0x2Cu:
		fake_mmr_set(addr - 8u, fake_mmr_get(addr - 8u) & ~*value);
		if (off == 0x20u)
			pm->cycles_dir_clr = (uint32_t)fake_core_peek_cycles();
		return false;
	case 0x30u:
		pm->mux_writes++;
		if (pm->table && !inputs_done(pm->table))
			pm->violations++;
		/* FER must already be clear for every changing field. */
		if (fake_mmr_get(PORT_FER(p)) &
		    changed_pins(fake_mmr_get(addr), *value))
			pm->violations++;
		return true;
	default:
		return true;
	}
}

static void reset(struct port_model *pm, const struct wb_pinmux *table)
{
	fake_mmr_reset();
	fake_core_reset();
	*pm = (struct port_model){ 0 };
	pm->table = table;
	fake_mmr_set_hook(port_hook, pm);
}

static void check_state(const struct wb_pinmux *m)
{
	uint32_t p;

	for (p = 0; p < WB_PORTS; p++) {
		CHECK(fake_mmr_get(PORT_MUX(p)) == m->port[p].mux);
		CHECK(fake_mmr_get(PORT_FER(p)) == m->port[p].fer);
	}
}

static void test_pinmux_from_reset(void)
{
	struct port_model pm;

	reset(&pm, &master);
	/* Both inputs-first pins driven, as a previous stage might leave. */
	fake_mmr_set(PORT_DIR(WB_PORT_B), 1u << 10);
	fake_mmr_set(PORT_DIR(WB_PORT_C), 1u << 4);
	CHECK(wb_pinmux_apply(&master) == WB_PORT_OK);
	check_state(&master);
	CHECK(fake_mmr_get(PORT_DIR(WB_PORT_B)) == 0);
	CHECK(fake_mmr_get(PORT_DIR(WB_PORT_C)) == 0);
	CHECK(fake_mmr_get(PORT_INEN(WB_PORT_B)) == 1u << 10);
	CHECK(fake_mmr_get(PORT_INEN(WB_PORT_C)) == 1u << 4);
	CHECK(pm.violations == 0);
	CHECK(pm.mux_writes == 3);
	CHECK(fake_mmr_errors() == 0);

	reset(&pm, &slave);
	fake_mmr_set(PORT_DIR(WB_PORT_B), 0xFFFFu);
	CHECK(wb_pinmux_apply(&slave) == WB_PORT_OK);
	check_state(&slave);
	/* Only the listed pin changed direction. */
	CHECK(fake_mmr_get(PORT_DIR(WB_PORT_B)) == 0xFBFFu);
	CHECK(pm.violations == 0);
	/* Port B's MUX is already 0. */
	CHECK(pm.mux_writes == 2);
}

/* From one complete table to the other: FER bits go off before MUX. */
static void test_pinmux_change(void)
{
	struct port_model pm;
	uint32_t p;

	reset(&pm, &master);
	for (p = 0; p < WB_PORTS; p++) {
		fake_mmr_set(PORT_MUX(p), slave.port[p].mux);
		fake_mmr_set(PORT_FER(p), slave.port[p].fer);
	}
	CHECK(wb_pinmux_apply(&master) == WB_PORT_OK);
	check_state(&master);
	CHECK(pm.violations == 0);

	reset(&pm, &slave);
	for (p = 0; p < WB_PORTS; p++) {
		fake_mmr_set(PORT_MUX(p), master.port[p].mux);
		fake_mmr_set(PORT_FER(p), master.port[p].fer);
	}
	CHECK(wb_pinmux_apply(&slave) == WB_PORT_OK);
	check_state(&slave);
	CHECK(pm.violations == 0);
}

/* Applying the table already in place writes no MUX or FER register. */
static void test_pinmux_idempotent(void)
{
	static const struct fake_mmr_access seq[] = {
		/* S-13b.004: per pin, DIR cleared, then INEN set */
		FAKE_W32(PORT_DIR_CLR(WB_PORT_B), 1u << 10),
		FAKE_W32(PORT_INEN_SET(WB_PORT_B), 1u << 10),
		FAKE_W32(PORT_DIR_CLR(WB_PORT_C), 1u << 4),
		FAKE_W32(PORT_INEN_SET(WB_PORT_C), 1u << 4),
		FAKE_R32(PORT_MUX(0), 0x30002000u),
		FAKE_R32(PORT_FER(0), 0x4040u),
		FAKE_R32(PORT_MUX(1), 0x1400FFFFu),
		FAKE_R32(PORT_FER(1), 0xFFFFu),
		FAKE_R32(PORT_MUX(2), 0x00855100u),
		FAKE_R32(PORT_FER(2), 0x0BD3u),
		FAKE_SYNC,
	};
	struct port_model pm;
	uint32_t p;

	reset(&pm, &master);
	for (p = 0; p < WB_PORTS; p++) {
		fake_mmr_set(PORT_MUX(p), master.port[p].mux);
		fake_mmr_set(PORT_FER(p), master.port[p].fer);
	}
	CHECK(wb_pinmux_apply(&master) == WB_PORT_OK);
	CHECK(fake_mmr_expect("idempotent", seq, ARRAY_LEN(seq)));
}

static void test_pinmux_bad_pin(void)
{
	static const uint8_t bad[] = { WB_PIN(WB_PORT_A, 0), 48 };
	struct wb_pinmux m = master;
	struct port_model pm;

	reset(&pm, NULL);
	m.inputs_first = bad;
	m.n_inputs_first = 2;
	CHECK(wb_pinmux_apply(&m) == WB_PORT_ERR_ARG);
	CHECK(fake_mmr_count() == 0);
}

/* S-13b.001/.002: PA0, PA2, PA8 released, 10 µs, INEN, 1 µs, read. */
static void test_strap(void)
{
	static const struct wb_strap s = {
		WB_PORT_A, 0x0105u, 10u, 1u,
	};
	struct port_model pm;
	uint16_t v = 0xFFFFu;

	reset(&pm, NULL);
	fake_core_set_cycle_step(13);
	fake_mmr_set(PORT_DIR(WB_PORT_A), 0x0105u);
	fake_mmr_set(PORT_DATA(WB_PORT_A), 0xF0F4u);
	CHECK(wb_strap_read(&s, &cclk400, &v) == WB_PORT_OK);
	CHECK(v == 0x0004u);
	CHECK(fake_mmr_get(PORT_DIR(WB_PORT_A)) == 0);
	CHECK(fake_mmr_get(PORT_INEN(WB_PORT_A)) == 0);
	CHECK(pm.cycles_inen_set - pm.cycles_dir_clr >= 10u * 400u);
	CHECK(pm.cycles_read - pm.cycles_inen_set >= 1u * 400u);
	/* No MUX or FER access. */
	CHECK(pm.mux_writes == 0);
	CHECK(fake_mmr_get(PORT_FER(WB_PORT_A)) == 0);

	reset(&pm, NULL);
	CHECK(wb_strap_read(&(struct wb_strap){ 3u, 1u, 0, 0 }, &cclk400,
			    &v) == WB_PORT_ERR_ARG);
	CHECK(fake_mmr_count() == 0);
}

/* S-18.002's fallback sample: PA2's INEN set, 20 us, read; no DIR write,
 * INEN left set, the other pins' DATA bits masked. */
static void test_strap_inen(void)
{
	static const struct wb_strap s = { WB_PORT_A, 0x0004u, 20u, 0u };
	struct port_model pm;
	uint16_t v = 0xFFFFu;

	reset(&pm, NULL);
	fake_core_set_cycle_step(13);
	fake_mmr_set(PORT_DIR(WB_PORT_A), 0x0105u);
	fake_mmr_set(PORT_DATA(WB_PORT_A), 0xF0F4u);
	CHECK(wb_strap_inen_read(&s, &cclk400, &v) == WB_PORT_OK);
	CHECK(v == 0x0004u);
	CHECK(fake_mmr_get(PORT_DIR(WB_PORT_A)) == 0x0105u);
	CHECK(fake_mmr_get(PORT_INEN(WB_PORT_A)) == 0x0004u);
	CHECK(pm.cycles_read - pm.cycles_inen_set >= 20u * 400u);
	CHECK(pm.mux_writes == 0);
	CHECK(fake_mmr_get(PORT_FER(WB_PORT_A)) == 0);
	/* PA2 low */
	reset(&pm, NULL);
	fake_mmr_set(PORT_DATA(WB_PORT_A), 0xFFFBu);
	CHECK(wb_strap_inen_read(&s, &cclk400, &v) == WB_PORT_OK && v == 0u);
	/* no port or no pin: refused, nothing touched */
	reset(&pm, NULL);
	CHECK(wb_strap_inen_read(&(struct wb_strap){ 3u, 1u, 0, 0 }, &cclk400,
				 &v) == WB_PORT_ERR_ARG);
	CHECK(wb_strap_inen_read(&(struct wb_strap){ WB_PORT_A, 0u, 0, 0 },
				 &cclk400, &v) == WB_PORT_ERR_ARG);
	CHECK(fake_mmr_count() == 0);
}

static void test_gpio(void)
{
	static const struct fake_mmr_access seq[] = {
		/* Level first, then the driver. */
		FAKE_W32(PORT_DATA_SET(WB_PORT_C), 1u << 11),
		FAKE_W32(PORT_DIR_SET(WB_PORT_C), 1u << 11),
		FAKE_W32(PORT_DATA_CLR(WB_PORT_A), 1u << 3),
		FAKE_W32(PORT_DIR_SET(WB_PORT_A), 1u << 3),
		FAKE_W32(PORT_DIR_CLR(WB_PORT_B), 1u << 15),
		FAKE_W32(PORT_INEN_SET(WB_PORT_B), 1u << 15),
		FAKE_W32(PORT_DATA_CLR(WB_PORT_C), 1u << 11),
		FAKE_W32(PORT_FER_CLR(WB_PORT_C), 1u << 0),
		FAKE_W32(PORT_FER_SET(WB_PORT_C), 1u << 2),
	};
	struct port_model pm;

	reset(&pm, NULL);
	fake_mmr_set_hook(NULL, NULL);
	CHECK(wb_gpio_output(WB_PIN(WB_PORT_C, 11), true) == WB_PORT_OK);
	CHECK(wb_gpio_output(WB_PIN(WB_PORT_A, 3), false) == WB_PORT_OK);
	CHECK(wb_gpio_input(WB_PIN(WB_PORT_B, 15)) == WB_PORT_OK);
	CHECK(wb_gpio_set(WB_PIN(WB_PORT_C, 11), false) == WB_PORT_OK);
	CHECK(wb_gpio_gpio(WB_PIN(WB_PORT_C, 0)) == WB_PORT_OK);
	CHECK(wb_gpio_peripheral(WB_PIN(WB_PORT_C, 2)) == WB_PORT_OK);
	CHECK(fake_mmr_expect_writes("gpio", seq, ARRAY_LEN(seq)));

	fake_mmr_set(PORT_DATA(WB_PORT_B), 1u << 15);
	CHECK(wb_gpio_get(WB_PIN(WB_PORT_B, 15)));
	CHECK(!wb_gpio_get(WB_PIN(WB_PORT_B, 14)));

	fake_mmr_clear_log();
	CHECK(wb_gpio_output(48, true) == WB_PORT_ERR_ARG);
	CHECK(wb_gpio_input(48) == WB_PORT_ERR_ARG);
	CHECK(wb_gpio_set(48, true) == WB_PORT_ERR_ARG);
	CHECK(wb_gpio_gpio(48) == WB_PORT_ERR_ARG);
	CHECK(wb_gpio_peripheral(48) == WB_PORT_ERR_ARG);
	CHECK(!wb_gpio_get(48));
	CHECK(fake_mmr_count() == 0);
}

/* S-13b.006 on the OP-1F: DIR set, no DATA write. */
static void test_gpio_keep(void)
{
	static const struct fake_mmr_access seq[] = {
		FAKE_W32(PORT_DIR_SET(WB_PORT_A), 1u << 0),
		FAKE_W32(PORT_DIR_SET(WB_PORT_C), 1u << 13),
	};
	struct port_model pm;

	reset(&pm, NULL);
	fake_mmr_set_hook(NULL, NULL);
	CHECK(wb_gpio_output_keep(WB_PIN(WB_PORT_A, 0)) == WB_PORT_OK);
	CHECK(wb_gpio_output_keep(WB_PIN(WB_PORT_C, 13)) == WB_PORT_OK);
	CHECK(fake_mmr_expect_writes("keep", seq, ARRAY_LEN(seq)));
	fake_mmr_clear_log();
	CHECK(wb_gpio_output_keep(48) == WB_PORT_ERR_ARG);
	CHECK(fake_mmr_count() == 0);
}

int main(void)
{
	RUN(test_pinmux_from_reset);
	RUN(test_pinmux_change);
	RUN(test_pinmux_idempotent);
	RUN(test_pinmux_bad_pin);
	RUN(test_strap);
	RUN(test_strap_inen);
	RUN(test_gpio);
	RUN(test_gpio_keep);
	return check_summary("test_drv_port");
}
