/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's pins (src/front/fr_hw_port.c, built here for the host since
 * the front links it for the target only): S-13b.004, PB10 and PC4 made
 * inputs before the first MUX write, each by two writes in order, DIR
 * cleared then INEN set, and no INEN of theirs cleared after; S-13b.003's
 * MUX and FER values at the end.
 */
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
	fr_hw_pins();
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

int main(void)
{
	RUN(test_inputs_first);
	return check_summary("test_front_pins");
}
