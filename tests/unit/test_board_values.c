/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The OP-XY board values against the drivers' own checks (the data
 * sheet's limits), the strap decoding, the OTP key adapters, and the
 * slave's eMMC commands.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/values.h"
#include "waffle/board/boot.h"
#include "waffle/board/emmc_bridge.h"
#include "waffle/board/ops.h"
#include "waffle/board/services.h"
#include "waffle/drivers/bf70x/lcd.h"
#include "waffle/bridge/records.h"

static void test_clocks(void)
{
	CHECK(wb_cgu_check_part(&opxy_cgu, &opxy_cgu_part) == WB_CGU_OK);
	CHECK(wb_dmc_check(&opxy_dmc) == WB_DMC_OK);
	CHECK(opxy_time.cycles_per_us == OPXY_CCLK_MHZ);
}

/*
 * S-13b.022: the boot link's own ops (opxy_boot_ops()). The sample of PA6
 * that decides a byte comes the settle time after the byte before it was
 * clocked in full, and the wait reads nothing but the cycle counter.
 */
static struct {
	uint64_t byte_end, sample_at;
	unsigned bytes;
	unsigned dead_at;	/* SPI0 stops clocking at this byte (1-based; 0: never) */
} link;

static bool link_hook(void *ctx, enum fake_mmr_op op, unsigned width, wb_mmr_addr addr,
		      uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_WRITE && addr == SPI_TFIFO(OPXY_SPI_LINK)) {
		/* the first byte is answered FF, as a slave that opens answers 03 */
		fake_mmr_set(SPI_RFIFO(OPXY_SPI_LINK), link.bytes == 0u ? 0xFFu : ~*value & 0xFFu);
		link.bytes++;
	}
	if (op == FAKE_MMR_READ && addr == SPI_STAT(OPXY_SPI_LINK))
		*value = link.dead_at != 0u && link.bytes >= link.dead_at ?
				 SPI_STAT_RFE : SPI_STAT_TF;	/* nothing comes in, or the byte is in */
	if (op == FAKE_MMR_READ && addr == SPI_RFIFO(OPXY_SPI_LINK))
		link.byte_end = fake_core_peek_cycles();
	if (op == FAKE_MMR_READ && addr == PORT_DATA(WB_PORT_A))
		link.sample_at = fake_core_peek_cycles();
	return true;
}

static void test_boot_link_settle(void)
{
	static struct opxy_hw hw;
	struct s09_boot_ops ops;
	uint64_t t0;
	unsigned i;

	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(40);		/* a tenth of a microsecond a look */
	fake_mmr_set_hook(link_hook, NULL);
	memset(&link, 0, sizeof link);
	hw.time = opxy_time;
	CHECK(wb_spi_init(&hw.link_spi, OPXY_SPI_LINK, &opxy_spi_boot_link, &hw.time,
			  OPXY_SPI_WORD_US) == WB_SPI_OK);
	ops = opxy_boot_ops(&hw);
	CHECK(ops.send == NULL);		/* never the controller's pacing by default */
	/* the conservative reading while the line is unmeasured: 2 us */
	CHECK(OPXY_BOOT_LINK_SETTLE_US == 2u);
	CHECK(OPXY_BOOT_LINK_SETTLE_CYCLES == 2u * OPXY_CCLK_MHZ);

	ops.select(ops.ctx, true);
	for (i = 0; i < 200u; i++) {
		CHECK(ops.ready(ops.ctx));
		CHECK(ops.xfer(ops.ctx, (uint8_t)i) == (uint8_t)~i);
		CHECK(ops.ready(ops.ctx));	/* the sample that decides the next */
		CHECK(link.sample_at - link.byte_end >= OPXY_BOOT_LINK_SETTLE_CYCLES);
		CHECK(link.sample_at - link.byte_end <= OPXY_BOOT_LINK_SETTLE_CYCLES + 80u);
	}
	CHECK(link.bytes == 200u);
	/* the byte alone, as the timing build takes it: no settle, no clock */
	t0 = fake_core_peek_cycles();
	CHECK(opxy_boot_byte(ops.ctx, 0x5A) == 0xA5);
	CHECK(fake_core_peek_cycles() == t0);
	/* PA6 high after the settle: not ready, and no byte by itself */
	fake_mmr_set(PORT_DATA(WB_PORT_A), 1u << 6);
	CHECK(!ops.ready(ops.ctx));
	CHECK(link.bytes == 201u);
	ops.select(ops.ctx, false);
}

/* A slot for the stream: a block of 600 bytes, then FINAL. */
static uint8_t stall_slot[16 + 600 + 16];

static void stall_idle(void *ctx, uint32_t until_us)
{
	(void)ctx;
	(void)until_us;
}

/*
 * S-09.008, S-09.060: SPI0 stops clocking part-way through the stream, at
 * the opening byte or later. The board's ops under s09_boot_slave(): the
 * stream ends at that byte, the slave absent, in about one word's timeout
 * and not one for every byte left; the select is released and both channels
 * are off, as the bridge's set-up expects to find the controller.
 */
static void test_boot_link_stalls(void)
{
	static const unsigned at[] = { 1u, 2u, 18u, 300u };
	static struct opxy_hw hw;
	unsigned i;

	memset(stall_slot, 0x5A, sizeof stall_slot);
	memset(stall_slot, 0, 16);
	stall_slot[3] = 0xAD;
	stall_slot[8] = (uint8_t)(600u & 0xFFu);
	stall_slot[9] = (uint8_t)(600u >> 8);
	memset(stall_slot + 616, 0, 16);
	stall_slot[616 + 1] = 0x80;		/* FINAL */
	stall_slot[616 + 3] = 0xAD;

	for (i = 0; i < sizeof at / sizeof at[0]; i++) {
		const struct s06_clock clk = { wb_time_now_us, stall_idle, &hw.time };
		struct s09_boot_ops ops;
		uint64_t t0;
		int end = -1;

		fake_mmr_reset();
		fake_core_reset();
		fake_core_set_cycle_step(40);
		fake_mmr_set_hook(link_hook, NULL);
		memset(&link, 0, sizeof link);
		hw.time = opxy_time;
		CHECK(wb_spi_init(&hw.link_spi, OPXY_SPI_LINK, &opxy_spi_boot_link, &hw.time,
				  OPXY_SPI_WORD_US) == WB_SPI_OK);
		ops = opxy_boot_ops(&hw);
		link.dead_at = at[i];
		t0 = fake_core_peek_cycles();
		CHECK(s09_boot_slave(&ops, &clk, stall_slot, sizeof stall_slot, true, &end) ==
		      S09_BOOT_STUCK);
		CHECK(link.bytes == at[i]);	/* the one that never came in, and no more */
		/* one word's timeout, and the settle time of the bytes before it */
		CHECK(fake_core_peek_cycles() - t0 <=
		      ((uint64_t)OPXY_SPI_WORD_US + 100u + 3u * at[i]) * OPXY_CCLK_MHZ);
		CHECK(fake_mmr_get(SPI_TXCTL(OPXY_SPI_LINK)) == 0u);
		CHECK(fake_mmr_get(SPI_RXCTL(OPXY_SPI_LINK)) == 0u);
		CHECK(fake_mmr_get(SPI_SLVSEL(OPXY_SPI_LINK)) == (0xFE00u | 8u));
	}
	/* a link that works streams the slot whole */
	{
		const struct s06_clock clk = { wb_time_now_us, stall_idle, &hw.time };
		struct s09_boot_ops ops;
		int end = -1;

		fake_mmr_reset();
		fake_core_reset();
		fake_core_set_cycle_step(40);
		fake_mmr_set_hook(link_hook, NULL);
		memset(&link, 0, sizeof link);
		CHECK(wb_spi_init(&hw.link_spi, OPXY_SPI_LINK, &opxy_spi_boot_link, &hw.time,
				  OPXY_SPI_WORD_US) == WB_SPI_OK);
		ops = opxy_boot_ops(&hw);
		CHECK(s09_boot_slave(&ops, &clk, stall_slot, sizeof stall_slot, true, &end) ==
		      S09_BOOT_STREAMED);
		CHECK(end == S09_END_FINAL);
		CHECK(link.bytes == 1u + sizeof stall_slot);
	}
}

/* BOOT_LINK_FC's own ops: a session the controller gives up leaves nothing
 * behind in SPI0, the transmit word count included, for the bridge's set-up. */
static bool fc_held, fc_dead;
static unsigned fc_bytes, fc_hold_at;

static bool fc_hook(void *ctx, enum fake_mmr_op op, unsigned width, wb_mmr_addr addr,
		    uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_WRITE && addr == SPI_TFIFO(OPXY_SPI_LINK)) {
		/* the opening byte is answered FF, as a slave that opens does */
		fake_mmr_set(SPI_RFIFO(OPXY_SPI_LINK), fc_bytes == 0u ? 0xFFu : ~*value & 0xFFu);
		fc_bytes++;
		if (fc_hold_at != 0u && fc_bytes >= fc_hold_at)
			fc_held = true;
	}
	if (op == FAKE_MMR_READ && addr == SPI_STAT(OPXY_SPI_LINK)) {
		*value = fc_held ? SPI_STAT_FCS | SPI_STAT_TFF : SPI_STAT_TF | SPI_STAT_TFS_EMPTY;
		if (fc_dead)
			*value = SPI_STAT_TFF | SPI_STAT_RFE;	/* nothing moves at all */
	}
	return true;
}

static void test_boot_link_fc_given_up(void)
{
	static struct opxy_hw hw;
	static const uint8_t block[32] = { 1, 2, 3 };
	struct s09_boot_ops ops;

	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(4000);
	fake_mmr_set_hook(fc_hook, NULL);
	fc_held = fc_dead = false;
	fc_bytes = fc_hold_at = 0;
	hw.time = opxy_time;
	CHECK(wb_spi_init(&hw.link_spi, OPXY_SPI_LINK, &opxy_spi_boot_link, &hw.time,
			  OPXY_BOOT_LINK_WORD_US) == WB_SPI_OK);
	CHECK(OPXY_BOOT_LINK_WORD_US == S09_T_STREAM_US);
	ops = opxy_boot_ops_fc(&hw);
	ops.select(ops.ctx, true);
	CHECK(ops.xfer(ops.ctx, 0x03) == 0xFF);
	CHECK(!ops.failed(ops.ctx));
	ops.select(ops.ctx, false);
	ops.select(ops.ctx, true);
	CHECK(fake_mmr_get(SPI_CTL(OPXY_SPI_LINK)) == (opxy_spi_boot_link_fc.ctl | SPI_CTL_EN));
	CHECK(ops.send(ops.ctx, block, sizeof block));
	ops.select(ops.ctx, false);
	/* PA6 held high for good */
	ops.select(ops.ctx, true);
	fc_held = true;
	CHECK(!ops.send(ops.ctx, block, sizeof block));
	ops.select(ops.ctx, false);
	CHECK(fake_mmr_get(SPI_TWC(OPXY_SPI_LINK)) == 0u);
	CHECK(fake_mmr_get(SPI_TXCTL(OPXY_SPI_LINK)) == 0u);
	CHECK(fake_mmr_get(SPI_RXCTL(OPXY_SPI_LINK)) == 0u);
	CHECK(fake_mmr_get(SPI_SLVSEL(OPXY_SPI_LINK)) == (0xFE00u | 8u));	/* released */

	/* under s09_boot_slave(): SPI0 dead at the opening byte, and PA6 held
	 * for good part-way through the stream. Each ends the stream, the
	 * slave absent, within T_STREAM (S-09.060), and leaves the same. */
	for (unsigned run = 0; run < 2u; run++) {
		const struct s06_clock clk = { wb_time_now_us, stall_idle, &hw.time };
		uint64_t t0;
		int end = -1;

		fake_mmr_reset();
		fake_core_reset();
		fake_core_set_cycle_step(4000);
		fake_mmr_set_hook(fc_hook, NULL);
		fc_held = false;
		fc_dead = run == 0u;
		fc_bytes = 0;
		fc_hold_at = run == 0u ? 0u : 40u;
		CHECK(wb_spi_init(&hw.link_spi, OPXY_SPI_LINK, &opxy_spi_boot_link, &hw.time,
				  OPXY_BOOT_LINK_WORD_US) == WB_SPI_OK);
		ops = opxy_boot_ops_fc(&hw);
		t0 = fake_core_peek_cycles();
		CHECK(s09_boot_slave(&ops, &clk, stall_slot, sizeof stall_slot, true, &end) ==
		      S09_BOOT_STUCK);
		CHECK(fake_core_peek_cycles() - t0 <=
		      ((uint64_t)S09_T_STREAM_US + 100000u) * OPXY_CCLK_MHZ);
		CHECK(run == 0u ? fc_bytes <= 1u : fc_bytes >= 40u && fc_bytes < 60u);
		CHECK(fake_mmr_get(SPI_TWC(OPXY_SPI_LINK)) == 0u || run == 0u);
		CHECK(fake_mmr_get(SPI_TXCTL(OPXY_SPI_LINK)) == 0u);
		CHECK(fake_mmr_get(SPI_RXCTL(OPXY_SPI_LINK)) == 0u);
		CHECK(fake_mmr_get(SPI_SLVSEL(OPXY_SPI_LINK)) == (0xFE00u | 8u));
	}
}

static void test_buses(void)
{
	CHECK(wb_spi_check(OPXY_SPI_FLASH, &opxy_spi_flash) == WB_SPI_OK);
	CHECK(wb_spi_check(OPXY_SPI_LINK, &opxy_spi_boot_link) == WB_SPI_OK);
	CHECK(wb_spi_check(OPXY_SPI_LINK, &opxy_spi_bridge_pre) == WB_SPI_OK);
	CHECK(wb_spi_check(OPXY_SPI_BRIDGE_SLAVE, &opxy_spi_bridge_slave_pre) ==
	      WB_SPI_OK);
	/* wb_spi_slave_init(): a slave's ctl with flow control, no select */
	CHECK(!(opxy_spi_bridge_slave_pre.ctl & SPI_CTL_MSTR));
	CHECK(opxy_spi_bridge_slave_pre.ctl & SPI_CTL_FCEN);
	CHECK(opxy_spi_bridge_slave_pre.sel == 0u);
	CHECK(wb_uart_check(OPXY_UART_PSOC, &opxy_uart_psoc) == WB_UART_OK);
	CHECK(wb_uart_check(OPXY_UART_NORDIC, &opxy_uart_nordic) ==
	      WB_UART_OK);
	/* BOOT_LINK_FC: S-13b.020 and .021's link, and with it SPI0's ready
	 * input alone, low for ready, as on the bridge (S-13b.040). */
	CHECK(wb_spi_check(OPXY_SPI_LINK, &opxy_spi_boot_link_fc) == WB_SPI_OK);
	CHECK(opxy_spi_boot_link_fc.ctl == (opxy_spi_boot_link.ctl | SPI_CTL_FCEN));
	CHECK(!(opxy_spi_boot_link_fc.ctl & SPI_CTL_FCPL));
	CHECK((opxy_spi_bridge_pre.ctl & (SPI_CTL_FCEN | SPI_CTL_FCPL)) == SPI_CTL_FCEN);
	CHECK(opxy_spi_boot_link_fc.clk == opxy_spi_boot_link.clk);
	CHECK(opxy_spi_boot_link_fc.sel == opxy_spi_boot_link.sel);
	CHECK(OPXY_SCLK0_HZ / (opxy_spi_boot_link.clk + 1u) <= 33400000u);
	/* S-13b.011: a slot's stream is read at SCLK0 / 2, 50 MHz, and no faster. */
	CHECK(OPXY_SPI_FLASH_STREAM_CLK >= 1u);
	CHECK(OPXY_SCLK0_HZ / (OPXY_SPI_FLASH_STREAM_CLK + 1u) <= 50000000u);
	CHECK(OPXY_SCLK0_HZ / (opxy_spi_flash.clk + 1u) <= 50000000u);
	/* S-13b.017: the flash's busy bound is at least 1 s. */
	CHECK(OPXY_FLASH_BUSY_US >= 1000000u);
	/* S-13b.057: CMD1 for at least 1 s. */
	CHECK(opxy_msi.cmd1_timeout_us >= 1000000u);
	CHECK(opxy_msi_bringup.cmd1_timeout_us >= 1000000u);
	/* S-09.018: a try's waits (CMD1, seven DAT0 or CMD13 waits, two CMD8
	 * transfers) after a repeat's latest start, within 4.5 s. */
	CHECK(OPXY_EMMC_REPEAT_US + opxy_msi_bringup.cmd1_timeout_us +
	      7u * opxy_msi_bringup.busy_timeout_us +
	      2u * opxy_msi_bringup.xfer_timeout_us <= 4500000u);
	CHECK(opxy_msi_bringup.ident_clkdiv == opxy_msi.ident_clkdiv);
	/* S-03.009's switches inside a request's T_EXEC (10 s): three, each
	 * tried S03_RETRIES times, each try CMD6, DAT0, CMD13s. */
	CHECK(3u * S03_RETRIES * (2u * opxy_msi_bringup.busy_timeout_us +
				  2u * opxy_msi_bringup.xfer_timeout_us) <=
	      8000000u);
	/* One try of S-03.068's flush inside a request's T_EXEC: CMD6, its own
	 * DAT0 wait, then the busy wait's DAT0 and CMD13s. */
	CHECK(opxy_msi.flush_timeout_us > opxy_msi.busy_timeout_us);
	CHECK(opxy_msi.flush_timeout_us + opxy_msi.busy_timeout_us +
	      2u * opxy_msi.xfer_timeout_us <= 10000000u);
	/* S-13b.057: a multi-block write, its data phase whole, then DAT0
	 * and CMD13, inside a request's T_EXEC (10 s) with 2 s to spare; a
	 * stall fails on one block's allowance before the whole runs out. */
	CHECK(opxy_msi.xfer_total_us + 2u * opxy_msi.busy_timeout_us <=
	      8000000u);
	CHECK(opxy_msi.xfer_timeout_us + opxy_msi.busy_timeout_us <
	      opxy_msi.xfer_total_us);
}

static void test_strap(void)
{
	CHECK(opxy_strap_role(OPXY_STRAP_ROLE) == OPXY_MASTER);
	CHECK(opxy_strap_role(0) == OPXY_SLAVE);
	CHECK(opxy_strap_revision(0) == 0u);
	CHECK(opxy_strap_revision(OPXY_STRAP_REV0) == 1u);
	CHECK(opxy_strap_revision(OPXY_STRAP_REV1) == 2u);
	CHECK(opxy_strap_revision(OPXY_STRAP_REV0 | OPXY_STRAP_REV1 |
				  OPXY_STRAP_ROLE) == 3u);
	CHECK((opxy_strap.pins & 0x105u) == 0x105u);
}

static uint32_t fail_item;

/* Each item's word i reads as item << 24 | i, so the order shows. */
static bool fake_get(uint32_t item, uint32_t *data)
{
	unsigned i;

	if (item == fail_item)
		return false;
	for (i = 0; i < wb_otp_words(item); i++)
		data[i] = item << 24 | i;
	return true;
}

static void test_otp(void)
{
	uint8_t key[32], q[56];

	fail_item = 0xFFu;
	CHECK(opxy_otp_key(fake_get, OPXY_OTP_K0_LO, OPXY_OTP_K0_HI, key));
	CHECK(key[0] == 0x00u && key[3] == 0x03u);	/* item 3, word 0 */
	CHECK(key[4] == 0x01u);
	CHECK(key[16] == 0x00u && key[19] == 0x04u);	/* item 4, word 0 */
	CHECK(opxy_otp_q0(fake_get, q));
	CHECK(q[3] == OPXY_OTP_Q0 && q[52] == 13u);

	fail_item = OPXY_OTP_K1_HI;
	CHECK(!opxy_otp_key(fake_get, OPXY_OTP_K1_LO, OPXY_OTP_K1_HI, key));
	CHECK(key[0] == 0u && key[3] == 0u && key[16] == 0u);

	/* On the host the ROM entry is absent: every read fails. */
	CHECK(!opxy_unit_ops.read_k0(NULL, key));
	CHECK(!opxy_unit_ops.read_q0(NULL, q));
}

/* The slave's eMMC: which op each command reached, with what. */
static char emmc_op;
static uint32_t emmc_a, emmc_b, emmc_c;

static int fake_write(void *ctx, uint32_t sector, uint32_t count,
		      const uint8_t *buf)
{
	(void)ctx;
	(void)buf;
	emmc_op = 'w';
	emmc_a = sector;
	emmc_b = count;
	return 0;
}

static int fake_reliable(void *ctx, uint32_t sector, uint32_t count,
			 const uint8_t *buf)
{
	(void)ctx;
	(void)buf;
	emmc_op = 'r';
	emmc_a = sector;
	emmc_b = count;
	return 0;
}

static int fake_erase(void *ctx, uint32_t first_sector)
{
	(void)ctx;
	emmc_op = 'e';
	emmc_a = first_sector;
	return 0;
}

static int fake_select(void *ctx, unsigned part)
{
	(void)ctx;
	emmc_op = 's';
	emmc_a = part;
	return 0;
}

static int fake_erase_arg(void *ctx, uint32_t first_sector, uint32_t arg)
{
	(void)ctx;
	emmc_op = 'E';
	emmc_a = first_sector;
	emmc_c = arg;
	return 0;
}

/* S-09.040: 0x13 is the reliable write and 0x14 the erase (an argument
 * other than 0 fails with no eMMC command); 0x16 stays a plain
 * multi-block write. */
static void test_slave_emmc(void)
{
	static uint8_t data[4 * S03_SECTOR];
	struct s03_emmc_ops emmc = { 0 };
	const struct opxy_emmc_more more = { fake_erase_arg, fake_reliable };

	emmc.write = fake_write;
	emmc.erase = fake_erase;
	CHECK(opxy_slave_emmc(&emmc, &more, S09_WRITE_RELIABLE, 100u, 4u, data,
			      sizeof(data)) == 0);
	CHECK(emmc_op == 'r' && emmc_a == 100u && emmc_b == 4u);
	CHECK(opxy_slave_emmc(&emmc, &more, S09_WRITE_SECTORS, 200u, 3u, data,
			      sizeof(data)) == 0);
	CHECK(emmc_op == 'w' && emmc_a == 200u && emmc_b == 3u);
	emmc_c = 0xFFu;
	CHECK(opxy_slave_emmc(&emmc, &more, S09_ERASE, 300u, WB_MMC_ERASE, NULL,
			      0u) == 0);
	CHECK(emmc_op == 'E' && emmc_a == 300u && emmc_c == WB_MMC_ERASE);
	emmc_op = 0;
	CHECK(opxy_slave_emmc(&emmc, &more, S09_ERASE, 300u, WB_MMC_TRIM, NULL,
			      0u) != 0);
	CHECK(emmc_op == 0);
	/* S-03.006: a select past BP2 sends nothing */
	emmc_op = 0;
	emmc.select = fake_select;
	CHECK(opxy_slave_emmc(&emmc, &more, S09_SELECT, 2u, 0u, NULL, 0u) == 0);
	CHECK(emmc_op == 's' && emmc_a == 2u);
	emmc_op = 0;
	CHECK(opxy_slave_emmc(&emmc, &more, S09_SELECT, 3u, 0u, NULL, 0u) != 0);
	CHECK(opxy_slave_emmc(&emmc, &more, S09_SELECT, 0x41u, 0u, NULL, 0u) != 0);
	CHECK(emmc_op == 0);
	/* a count past the payload sends nothing */
	emmc_op = 0;
	CHECK(opxy_slave_emmc(&emmc, &more, S09_WRITE_RELIABLE, 0u, 5u, data,
			      sizeof(data)) != 0);
	CHECK(emmc_op == 0);
}

/* S-18.030's pins of the OP-1F's main CPU, the board's tables against the
 * profile (S-13b.003, S-13b.004, S-13b.006, S-13c.001). */
static void test_op1f_pins(void)
{
	const struct wb_device *d = &wb_device_op1f;
	const struct wb_pinmux *pm = opxy_pinmux_main(d);
	const struct opxy_gpio *g;
	unsigned n, i, p;

	CHECK(pm == &opxy_pinmux_op1f && opxy_pinmux_main(&wb_device_opxy) == &opxy_pinmux_master);
	for (p = 0; p < 3u; p++)
		CHECK(pm->port[p].mux == d->pins.mux[p] && pm->port[p].fer == d->pins.fer[p]);
	CHECK(pm->n_inputs_first == 0u);	/* no DIR or INEN before the MUX */
	g = opxy_gpio_main(d, &n);
	CHECK(g == opxy_gpio_op1f && n == d->pins.gpio_n);
	for (i = 0; i < n && i < d->pins.gpio_n; i++) {
		CHECK(g[i].pin == d->pins.gpio[i].pin);
		CHECK((g[i].dir == OPXY_IN && d->pins.gpio[i].kind == WB_GPIO_IN) ||
		      (g[i].dir == OPXY_OUT_KEEP && d->pins.gpio[i].kind == WB_GPIO_OUT_KEEP));
		CHECK(g[i].pin != OPXY_PA8);	/* PA8 is never an input */
	}
	/* the OP-XY master's values are the profile's too */
	pm = opxy_pinmux_main(&wb_device_opxy);
	for (p = 0; p < 3u; p++)
		CHECK(pm->port[p].mux == wb_device_opxy.pins.mux[p] &&
		      pm->port[p].fer == wb_device_opxy.pins.fer[p]);
	CHECK(wb_lcd_op1f.dc == d->pins.panel_dc && wb_lcd_opxy.dc == wb_device_opxy.pins.panel_dc);
	CHECK(wb_lcd_op1f.reset == wb_lcd_opxy.reset && wb_lcd_op1f.te == wb_lcd_opxy.te);
}

/* The profile's kind of a board GPIO direction. */
static unsigned kind_of(uint8_t dir)
{
	return dir == OPXY_IN ? WB_GPIO_IN : dir == OPXY_OUT_LOW ? WB_GPIO_OUT_LOW :
	       dir == OPXY_OUT_HIGH ? WB_GPIO_OUT_HIGH : WB_GPIO_OUT_KEEP;
}

/*
 * S-18.030's pins of the OP-XY's main CPU against the board's: the
 * profile's list holds what the board sets at the start (PA7 before PA6,
 * the display's TE first) and the bridge's two, PC11 a high output and
 * PC5 an input, which the bridge sets (S-13b.040); each pin once, of the
 * profile's kind, the master's inputs-first pins apart (S-13b.004).
 */
static void test_opxy_pins(void)
{
	static const struct opxy_gpio bridge[] = {
		{ OPXY_PC11, OPXY_OUT_HIGH }, { OPXY_PC5, OPXY_IN },
	};
	const struct wb_device *d = &wb_device_opxy;
	const struct opxy_gpio *g;
	unsigned n, i, j, at6 = 99, at7 = 99;

	g = opxy_gpio_main(d, &n);
	CHECK(g == opxy_gpio_master && n + ARRAY_LEN(bridge) == d->pins.gpio_n);
	for (i = 0; i < d->pins.gpio_n; i++) {
		const struct wb_dev_gpio *p = &d->pins.gpio[i];
		unsigned found = 0;

		for (j = 0; j < n; j++)
			if (g[j].pin == p->pin) {
				found++;
				CHECK(kind_of(g[j].dir) == p->kind);
				if (p->pin == WB_PIN(WB_PORT_A, 6))
					at6 = j;
				if (p->pin == WB_PIN(WB_PORT_A, 7))
					at7 = j;
			}
		for (j = 0; j < ARRAY_LEN(bridge); j++)
			if (bridge[j].pin == p->pin) {
				found++;
				CHECK(kind_of(bridge[j].dir) == p->kind);
			}
		CHECK(found == 1u);
	}
	CHECK(at7 < at6 && at6 < n);
	for (i = 0; i < opxy_pinmux_master.n_inputs_first; i++)
		for (j = 0; j < d->pins.gpio_n; j++)
			CHECK(opxy_pinmux_master.inputs_first[i] != d->pins.gpio[j].pin);
}

/* S-13b.061, S-05.038, S-12.008: the OP-1F's other board values. */
static void test_op1f_values(void)
{
	static const uint32_t evt[OPXY_OS_EVTS] = {
		0x11A00C18u, 0x11A00B78u, 0x11A00664u, 0x11A00664u, 0x11A00664u,
		0x11A00664u, 0x11A00664u, 0x11A00664u, 0x11A00664u, 0x11A01438u,
		0x11A00664u, 0x11A00664u, 0x11A00664u, 0x11A000CAu,
	};
	struct opxy_disk_part part[OPXY_DISK_PARTS];
	unsigned i;

	CHECK(opxy_msi_1f.emmc_width == wb_device_op1f.mem.emmc_width &&
	      opxy_msi_bringup_1f.emmc_width == 4u);
	CHECK(opxy_msi.emmc_width == 0u && opxy_msi_bringup.emmc_width == 0u);	/* 8 */
	CHECK(opxy_msi_1f.cmd1_timeout_us == opxy_msi.cmd1_timeout_us &&
	      opxy_msi_bringup_1f.busy_timeout_us == opxy_msi_bringup.busy_timeout_us);
	for (i = 0; i < OPXY_OS_EVTS; i++)
		CHECK(opxy_os_entry_op1f.evt[i] == evt[i]);
	CHECK(opxy_os_entry_op1f.sp == opxy_os_entry_master.sp &&
	      opxy_os_entry_op1f.fp == opxy_os_entry_master.fp);
	opxy_disk_ddr_dev(&wb_device_op1f, part);
	CHECK((uintptr_t)part[0].mem == wb_device_op1f.mem.ramdisk_base &&
	      part[0].sectors == wb_device_op1f.mem.ramdisk_blocks &&
	      (uint64_t)part[0].sectors * 512u == wb_device_op1f.mem.ramdisk_len);
	CHECK(part[0].sectors == OPXY_1F_DISK_SECTORS && part[1].sectors == 0u);
	opxy_disk_ddr_dev(&wb_device_opxy, part);
	CHECK(part[0].sectors + part[1].sectors == wb_device_opxy.mem.ramdisk_blocks);
}

/* S-18.001 to S-18.005 on the main image: the record's device, or the
 * boot mode and the first stage when there is none. */
static uint8_t stage[0x9000];

static int stage_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	unsigned *reads = ctx;

	(*reads)++;
	if (off + len > sizeof stage)
		memset(buf, 0xFF, len);
	else
		memcpy(buf, stage + off, len);
	return 0;
}

/* The strap sample the decision asks for: its level, and how often. */
struct strap_fake {
	uint16_t level;
	unsigned samples;
};

static uint16_t strap_sample(void *ctx)
{
	struct strap_fake *f = ctx;

	f->samples++;
	return f->level;
}

/* A first stage whose reads fail a number of times, then succeed. */
struct flaky {
	unsigned reads, fail, busy;
};

static int flaky_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	struct flaky *f = ctx;

	f->reads++;
	if (f->busy != 0u)
		return WB_FS_READ_BUSY;
	if (f->fail != 0u) {
		f->fail--;
		return -1;
	}
	memset(buf, 0xFF, len);
	(void)off;
	return 0;
}

static void test_device_pick(void)
{
	struct fr_handoff ho;
	struct opxy_device_pick p;
	struct strap_fake sf = { 0u, 0u };
	struct flaky fl = { 0u, 0u, 0u };
	unsigned reads = 0;

	memset(&ho, 0, sizeof ho);
	ho.role_master = 1u;
	ho.device = FR_HO_DEV_OP1F;
	ho.device_how = FR_HO_HOW_TABLE;
	opxy_device_pick(&ho, 0u, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(p.dev == &wb_device_op1f && p.how == WB_DEV_BY_TABLE && p.master &&
	      !p.decided && reads == 0u && sf.samples == 0u && !p.fallback);
	/* the record's flags bit 12: the front's fallback */
	ho.flags = FR_HO_ROLE_BY_STRAP;
	opxy_device_pick(&ho, 0u, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(p.fallback && p.master && !p.decided && sf.samples == 0u);
	ho.flags = 0u;
	ho.role_master = 0u;
	ho.device = FR_HO_DEV_OPXY;
	ho.device_how = FR_HO_HOW_SECOND;
	opxy_device_pick(&ho, 1u, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(p.dev == &wb_device_opxy && !p.master && reads == 0u && sf.samples == 0u);
	/* S-18.005: a code that names nothing: the role the record's; its
	 * slave decides as without a record, PA2 = 0 the second CPU, no read */
	ho.device = 7u;
	opxy_device_pick(&ho, 2u, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(p.decided && !p.master && p.dev == &wb_device_opxy && p.how == WB_DEV_SECOND_CPU);
	CHECK(reads == 0u && sf.samples == 1u);
	sf.samples = 0u;
	/* the OP-1F on a CPU that is not the master cannot hold: decided as
	 * without a record, as the slave above */
	ho.device = FR_HO_DEV_OP1F;
	ho.device_how = FR_HO_HOW_TABLE;
	opxy_device_pick(&ho, 2u, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(p.decided && !p.master && p.dev == &wb_device_opxy && p.how == WB_DEV_SECOND_CPU);
	CHECK(reads == 0u && sf.samples == 1u);
	sf.samples = 0u;
	/* nor the OP-1F decided as the second CPU, on the master: the
	 * master decides from the first stage */
	ho.role_master = 1u;
	ho.device_how = FR_HO_HOW_SECOND;
	memset(stage, 0xFF, sizeof stage);
	opxy_device_pick(&ho, 2u, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(p.decided && p.master && reads != 0u && sf.samples == 0u && p.how == WB_DEV_UNKNOWN);
	reads = 0u;
	ho.role_master = 0u;
	ho.device = 7u;
	/* its master: the device from the first stage, whatever the boot mode */
	ho.role_master = 1u;
	memset(stage, 0xFF, sizeof stage);
	opxy_device_pick(&ho, 2u, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(p.decided && p.master && reads != 0u && sf.samples == 0u && p.how == WB_DEV_UNKNOWN);
	CHECK(p.n == 0xFFFFFFFFu);
	ho.device_how = 9u;
	ho.device = FR_HO_DEV_OP1F;
	opxy_device_pick(&ho, 1u, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(p.decided && p.master && p.dev == &wb_device_opxy && p.how == WB_DEV_UNKNOWN);
	/* no record: boot mode 2 with PA2 = 0 is the OP-XY's second CPU, the
	 * fallback sampled once and nothing read */
	sf.samples = 0u;
	reads = 0u;
	opxy_device_pick(NULL, 2u, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(!p.master && p.how == WB_DEV_SECOND_CPU && reads == 0u && p.n == 0u);
	CHECK(sf.samples == 1u && !p.fallback);
	/* boot mode 2 with PA2 = 1: a main CPU all the same, the first stage
	 * read after the sample */
	sf.samples = 0u;
	sf.level = WB_STRAP_PA2;
	opxy_device_pick(NULL, 2u, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(p.master && p.decided && reads != 0u && sf.samples == 1u);
	CHECK(p.fallback && p.how == WB_DEV_UNKNOWN);
	/* a main CPU with an unlisted (erased) first stage: the default, no
	 * sample */
	sf.samples = 0u;
	reads = 0u;
	opxy_device_pick(NULL, WB_BOOT_MODE_MAIN, strap_sample, &sf, stage_read, &reads, &p);
	CHECK(p.master && p.decided && p.dev == &wb_device_opxy && p.how == WB_DEV_UNKNOWN);
	CHECK(reads == WB_FS_TRIES && p.n == 0xFFFFFFFFu && p.c == 0u);
	CHECK(sf.samples == 0u && !p.fallback);
	/* S-18.003's tries: a failed read is made again, up to three times */
	fl.fail = 2u;
	opxy_device_pick(NULL, WB_BOOT_MODE_MAIN, strap_sample, &sf, flaky_read, &fl, &p);
	CHECK(p.n == 0xFFFFFFFFu && fl.fail == 0u);
	fl.fail = 5u;
	opxy_device_pick(NULL, WB_BOOT_MODE_MAIN, strap_sample, &sf, flaky_read, &fl, &p);
	CHECK(fl.fail == 2u && p.how == WB_DEV_UNKNOWN && p.c == 0u);
	/* a busy timeout is not tried again */
	fl.reads = 0u;
	fl.busy = 1u;
	opxy_device_pick(NULL, WB_BOOT_MODE_MAIN, strap_sample, &sf, flaky_read, &fl, &p);
	CHECK(fl.reads == 1u && p.how == WB_DEV_UNKNOWN);
}

int main(void)
{
	RUN(test_clocks);
	RUN(test_buses);
	RUN(test_boot_link_settle);
	RUN(test_boot_link_stalls);
	RUN(test_boot_link_fc_given_up);
	RUN(test_strap);
	RUN(test_otp);
	RUN(test_slave_emmc);
	RUN(test_op1f_pins);
	RUN(test_opxy_pins);
	RUN(test_op1f_values);
	RUN(test_device_pick);
	return check_summary("test_board_values");
}
