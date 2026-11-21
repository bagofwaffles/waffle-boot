/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's pins (src/front/fr_hw_port.c, built here for the host since
 * the front links it for the target only): S-13b.004, PB10 and PC4 made
 * inputs before the first MUX write, each by two writes in order, DIR
 * cleared then INEN set, and no INEN of theirs cleared after; S-13b.003's
 * MUX and FER values at the end. The OP-XY's accesses are those of the
 * front before the devices, from several starting states; the OP-1F's
 * follow S-13b.004's and S-13b.006's rules for it (S-18.030). The flash
 * bus before the decision (S-18.001): nothing written when its pins are
 * SPI2's already, else PORTB alone, MUX before FER.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "front/fr_hw_port.c"

/* The index + 1 of the first write to addr with bit set, or 0. */
static size_t first_write(wb_mmr_addr addr, uint32_t bit)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == addr &&
		    (log[i].value & bit))
			return i + 1u;
	return 0;
}

static unsigned writes_to(wb_mmr_addr addr)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	unsigned n = 0;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == addr)
			n++;
	return n;
}

static size_t first_mux(void)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_WRITE &&
		    (log[i].addr == PORT_MUX(WB_PORT_A) ||
		     log[i].addr == PORT_MUX(WB_PORT_B) ||
		     log[i].addr == PORT_MUX(WB_PORT_C)))
			return i + 1u;
	return 0;
}

static void test_inputs_first(void)
{
	static const struct { uint32_t port, bit; } pins[] = {
		{ WB_PORT_B, 1u << 10 }, { WB_PORT_C, 1u << 4 },
	};
	size_t mux, i;

	fake_mmr_reset();
	fake_core_reset();
	fr_hw_pins(&fr_dev_opxy);
	mux = first_mux();
	CHECK(mux != 0u);
	for (i = 0; i < ARRAY_LEN(pins); i++) {
		const size_t dir = first_write(PORT_DIR_CLR(pins[i].port), pins[i].bit);
		const size_t inen = first_write(PORT_INEN_SET(pins[i].port), pins[i].bit);

		CHECK(dir != 0u && inen != 0u);
		CHECK(dir < inen && inen < mux);
		/* the INEN bit stays set */
		CHECK(first_write(PORT_INEN_CLR(pins[i].port), pins[i].bit) == 0u);
		CHECK(writes_to(PORT_INEN(pins[i].port)) == 0u);
	}
	CHECK(fake_mmr_get(PORT_MUX(WB_PORT_B)) == 0x1400FFFFu);
	CHECK(fake_mmr_get(PORT_MUX(WB_PORT_C)) == 0x00855100u);
}

/* ---- the OP-XY's writes as before the devices (the front of version 18) ---- */

static const struct { uint32_t mux; uint16_t fer; } old_func[WB_PORTS] = {
	{ 0x30002000u, 0x4040u }, { 0x1400FFFFu, 0xFFFFu }, { 0x00855100u, 0x0BD3u },
};
static const uint8_t old_first[] = { WB_PIN(WB_PORT_B, 10), WB_PIN(WB_PORT_C, 4) };
static const struct { uint8_t pin, dir; } old_gpio[] = {
	{ WB_PIN(WB_PORT_A, 3), 1u }, { WB_PIN(WB_PORT_A, 7), 0u },
	{ WB_PIN(WB_PORT_A, 6), 0u }, { WB_PIN(WB_PORT_C, 2), 0u },
};

static void old_pins(void)
{
	unsigned i;

	for (i = 0; i < sizeof old_first; i++) {
		wb_mmr_write32(PORT_DIR_CLR(WB_PIN_PORT(old_first[i])), WB_PIN_BIT(old_first[i]));
		wb_mmr_write32(PORT_INEN_SET(WB_PIN_PORT(old_first[i])), WB_PIN_BIT(old_first[i]));
	}
	for (uint32_t p = 0; p < WB_PORTS; p++) {
		const uint32_t mux = wb_mmr_read32(PORT_MUX(p));
		const uint32_t fer = wb_mmr_read32(PORT_FER(p));
		const uint32_t off = fer & (~(uint32_t)old_func[p].fer |
					    mux_changed(mux, old_func[p].mux));

		if (off != 0u)
			wb_mmr_write32(PORT_FER_CLR(p), off);
		if (mux != old_func[p].mux)
			wb_mmr_write32(PORT_MUX(p), old_func[p].mux);
		if ((fer & ~off) != old_func[p].fer)
			wb_mmr_write32(PORT_FER_SET(p), old_func[p].fer);
	}
	wb_mmr_sync();
	for (i = 0; i < ARRAY_LEN(old_gpio); i++) {
		const uint32_t p = WB_PIN_PORT(old_gpio[i].pin), b = WB_PIN_BIT(old_gpio[i].pin);

		if (old_gpio[i].dir == 0u) {
			wb_mmr_write32(PORT_DIR_CLR(p), b);
			wb_mmr_write32(PORT_INEN_SET(p), b);
		} else {
			wb_mmr_write32(PORT_DATA_CLR(p), b);
			wb_mmr_write32(PORT_DIR_SET(p), b);
		}
	}
}

#define LOG_MAX 256u
static struct fake_mmr_access log_a[LOG_MAX];

/* The register state the pins start from: the reset values, the boot
 * ROM's flash pins, and others. */
static void preset(unsigned k)
{
	static const uint32_t mux[4][3] = {
		{ 0, 0, 0 }, { 0, 0x00000000u, 0 }, { 0x30002000u, 0x1400FFFFu, 0x00855100u },
		{ 0xFFFFFFFFu, 0x55555555u, 0xAAAAAAAAu },
	};
	static const uint32_t fer[4][3] = {
		{ 0, 0, 0 }, { 0, 0x9C00u, 0 }, { 0x4040u, 0xFFFFu, 0x0BD3u },
		{ 0xFFFFu, 0x0F0Fu, 0x1234u },
	};

	fake_mmr_reset();
	fake_core_reset();
	for (uint32_t p = 0; p < WB_PORTS; p++) {
		fake_mmr_set(PORT_MUX(p), mux[k][p]);
		fake_mmr_set(PORT_FER(p), fer[k][p]);
	}
}

/* fr_hw_pins(&fr_dev_opxy) makes the same accesses, in the same order, as
 * the front before the devices, from each starting state. */
static void test_opxy_unchanged(void)
{
	for (unsigned k = 0; k < 4u; k++) {
		size_t n;

		preset(k);
		old_pins();
		n = fake_mmr_count();
		CHECK(n <= LOG_MAX);
		memcpy(log_a, fake_mmr_log(), (n < LOG_MAX ? n : LOG_MAX) * sizeof log_a[0]);
		preset(k);
		fr_hw_pins(&fr_dev_opxy);
		CHECK(fake_mmr_count() == n);
		for (size_t i = 0; i < n && i < fake_mmr_count() && i < LOG_MAX; i++) {
			const struct fake_mmr_access *b = &fake_mmr_log()[i];

			CHECK(b->op == log_a[i].op && b->width == log_a[i].width &&
			      b->addr == log_a[i].addr && b->value == log_a[i].value);
		}
	}
}

/* Any write with bit to one of the port's registers. */
static bool any_write(wb_mmr_addr addr, uint32_t bit)
{
	return first_write(addr, bit) != 0u;
}

/*
 * S-13b.003, S-13b.004, S-13b.006 on the OP-1F: no DIR or INEN write
 * before the first MUX write (no inputs-first pin, PC4 is MSI0's); the
 * MUX values and the FER set at the end; the five outputs made outputs
 * with no level written; the six inputs DIR cleared then INEN set; PA8
 * never touched; no whole-register INEN write.
 */
static void test_op1f(void)
{
	const struct fr_dev *d = &fr_dev_op1f;

	for (unsigned k = 0; k < 4u; k++) {
		const struct fake_mmr_access *log;
		size_t mux, i;

		preset(k);
		fr_hw_pins(d);
		log = fake_mmr_log();
		mux = first_mux();
		CHECK(mux != 0u || k == 2u);
		for (i = 0; mux != 0u && i + 1u < mux; i++)
			for (uint32_t p = 0; p < WB_PORTS; p++)
				CHECK(!(log[i].op == FAKE_MMR_WRITE &&
					(log[i].addr == PORT_DIR_CLR(p) || log[i].addr == PORT_DIR_SET(p) ||
					 log[i].addr == PORT_INEN_SET(p) ||
					 log[i].addr == PORT_INEN_CLR(p))));
		for (uint32_t p = 0; p < WB_PORTS; p++) {
			CHECK(fake_mmr_get(PORT_MUX(p)) == d->mux[p]);
			CHECK(writes_to(PORT_INEN(p)) == 0u && writes_to(PORT_DIR(p)) == 0u);
			CHECK(writes_to(PORT_DATA(p)) == 0u);
			/* no FER bit set while MUX differs: the set comes after
			 * the MUX write */
			if (first_write(PORT_FER_SET(p), 0xFFFFu) != 0u &&
			    first_write(PORT_MUX(p), 0xFFFFFFFFu) != 0u)
				CHECK(first_write(PORT_MUX(p), 0xFFFFFFFFu) <
				      first_write(PORT_FER_SET(p), 0xFFFFu));
		}
		for (i = 0; i < d->gpio_n; i++) {
			const uint32_t p = WB_PIN_PORT(d->gpio[i].pin), b = WB_PIN_BIT(d->gpio[i].pin);

			CHECK(!any_write(PORT_DATA_SET(p), b) && !any_write(PORT_DATA_CLR(p), b));
			if (d->gpio[i].kind == FR_GPIO_KEEP) {
				CHECK(any_write(PORT_DIR_SET(p), b) && !any_write(PORT_DIR_CLR(p), b));
				CHECK(!any_write(PORT_INEN_SET(p), b));
			} else {
				CHECK(d->gpio[i].kind == FR_GPIO_IN);
				CHECK(first_write(PORT_DIR_CLR(p), b) != 0u &&
				      first_write(PORT_DIR_CLR(p), b) < first_write(PORT_INEN_SET(p), b));
				CHECK(!any_write(PORT_DIR_SET(p), b));
			}
		}
		/* PA8 is the panel's reset alone (S-13c.027) */
		CHECK(!any_write(PORT_DIR_CLR(WB_PORT_A), 1u << 8) &&
		      !any_write(PORT_DIR_SET(WB_PORT_A), 1u << 8) &&
		      !any_write(PORT_INEN_SET(WB_PORT_A), 1u << 8) &&
		      !any_write(PORT_DATA_SET(WB_PORT_A), 1u << 8) &&
		      !any_write(PORT_DATA_CLR(WB_PORT_A), 1u << 8));
		/* PB10 and PC4 are not made inputs */
		CHECK(!any_write(PORT_INEN_SET(WB_PORT_B), 1u << 10) &&
		      !any_write(PORT_INEN_SET(WB_PORT_C), 1u << 4));
	}
}

/* ---- fr_hw_flash_bus(): PORTB before the decision (S-18.001) ---- */

/* FER_SET and FER_CLR act on FER, as the port does. The hook type fixes
 * value's type. */
static bool fer_hook(void *ctx, enum fake_mmr_op op, unsigned width, wb_mmr_addr addr,
		     /* cppcheck-suppress constParameterCallback */
		     uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op != FAKE_MMR_WRITE)
		return true;
	for (uint32_t p = 0; p < WB_PORTS; p++) {
		if (addr == PORT_FER_SET(p))
			fake_mmr_set(PORT_FER(p), fake_mmr_get(PORT_FER(p)) | *value);
		else if (addr == PORT_FER_CLR(p))
			fake_mmr_set(PORT_FER(p), fake_mmr_get(PORT_FER(p)) & ~*value);
	}
	return true;
}

static void bus_preset(uint32_t mux, uint32_t fer)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set_hook(fer_hook, NULL);
	fake_mmr_set(PORT_MUX(WB_PORT_B), mux);
	fake_mmr_set(PORT_FER(WB_PORT_B), fer);
}

static unsigned all_writes(void)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	unsigned n = 0;

	for (size_t i = 0; i < fake_mmr_count(); i++)
		n += log[i].op == FAKE_MMR_WRITE ? 1u : 0u;
	return n;
}

/* Writes to port registers other than PORTB_MUX and PORTB_FER's three. */
static unsigned other_port_writes(void)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	unsigned n = 0;

	for (size_t i = 0; i < fake_mmr_count(); i++) {
		const wb_mmr_addr a = log[i].addr;

		if (log[i].op != FAKE_MMR_WRITE)
			continue;
		if (a != PORT_MUX(WB_PORT_B) && a != PORT_FER(WB_PORT_B) &&
		    a != PORT_FER_SET(WB_PORT_B) && a != PORT_FER_CLR(WB_PORT_B))
			n++;
	}
	return n;
}

/* Bus pins already SPI2's: nothing written, whatever the other pins. */
static void test_flash_bus_as_found(void)
{
	static const uint32_t st[][2] = {
		{ 0x1400FFFFu, 0xFFFFu },		/* the profile's */
		{ 0x00000000u, 0x9C00u },		/* the four pins alone */
		{ 0x140FAAAAu, 0x9C0Fu },		/* others in other functions */
	};

	CHECK(FR_FLASH_BUS_PINS == 0x9C00u);
	for (unsigned i = 0; i < 16u; i++)
		CHECK(((FR_FLASH_BUS_MUX >> (2u * i)) & 3u) == ((FR_FLASH_BUS_PINS >> i) & 1u ? 3u : 0u));
	for (unsigned i = 0; i < ARRAY_LEN(st); i++) {
		bus_preset(st[i][0], st[i][1]);
		CHECK(!fr_hw_flash_bus());
		CHECK(all_writes() == 0u);
		CHECK(fake_mmr_get(PORT_MUX(WB_PORT_B)) == st[i][0]);
		CHECK(fake_mmr_get(PORT_FER(WB_PORT_B)) == st[i][1]);
	}
}

/* The reset state: MUX, then FER set, PORTB alone; fr_hw_pins() then
 * writes PORTB no more. */
static void test_flash_bus_reset(void)
{
	static const struct fake_mmr_access want[] = {
		FAKE_W32(PORT_MUX(WB_PORT_B), 0x1400FFFFu),
		FAKE_W32(PORT_FER_SET(WB_PORT_B), 0xFFFFu),
	};

	for (unsigned k = 0; k < 2u; k++) {
		bus_preset(0u, 0u);
		CHECK(fr_hw_flash_bus());
		CHECK(fake_mmr_expect_writes("flash bus from reset", want, ARRAY_LEN(want)));
		CHECK(fake_mmr_get(PORT_FER(WB_PORT_B)) == 0xFFFFu);
		fake_mmr_clear_log();
		fr_hw_pins(k == 0u ? &fr_dev_opxy : &fr_dev_op1f);
		CHECK(writes_to(PORT_MUX(WB_PORT_B)) == 0u && writes_to(PORT_FER_SET(WB_PORT_B)) == 0u &&
		      writes_to(PORT_FER_CLR(WB_PORT_B)) == 0u);
	}
}

/* A bus pin in another function: its FER bit off before the MUX write,
 * FER set after it; or a bus pin's FER bit alone missing: FER set alone. */
static void test_flash_bus_repaired(void)
{
	size_t clr, mux, set;

	bus_preset(0x00100000u, 0xFFFFu);	/* PB10 in position 1 */
	CHECK(fr_hw_flash_bus());
	clr = first_write(PORT_FER_CLR(WB_PORT_B), 1u << 10);
	mux = first_write(PORT_MUX(WB_PORT_B), 0x1400FFFFu);
	set = first_write(PORT_FER_SET(WB_PORT_B), 1u << 10);
	CHECK(clr != 0u && clr < mux && mux < set);
	CHECK(writes_to(PORT_MUX(WB_PORT_B)) == 1u && other_port_writes() == 0u);
	CHECK(fake_mmr_get(PORT_MUX(WB_PORT_B)) == 0x1400FFFFu);
	CHECK(fake_mmr_get(PORT_FER(WB_PORT_B)) == 0xFFFFu);

	bus_preset(0x1400FFFFu, 0x1C00u);	/* PB15's FER bit clear */
	CHECK(fr_hw_flash_bus());
	CHECK(writes_to(PORT_MUX(WB_PORT_B)) == 0u && writes_to(PORT_FER_CLR(WB_PORT_B)) == 0u);
	CHECK(writes_to(PORT_FER_SET(WB_PORT_B)) == 1u && other_port_writes() == 0u);
	CHECK(fake_mmr_get(PORT_FER(WB_PORT_B)) == 0xFFFFu);
}

int main(void)
{
	RUN(test_inputs_first);
	RUN(test_opxy_unchanged);
	RUN(test_op1f);
	RUN(test_flash_bus_as_found);
	RUN(test_flash_bus_reset);
	RUN(test_flash_bus_repaired);
	return check_summary("test_front_pins");
}
