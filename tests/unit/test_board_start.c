/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The main image's start end to end on each device, over the fakes, with
 * boot_master() (src/core/boot/flow.c) and s05_master_handoff() driving the
 * real modules at each step:
 *
 *   the device from the front's version 3 record (S-18.005,
 *   opxy_device_pick()); the device's pins and GPIOs on the fake MMRs
 *   (S-13b.003, opxy_pinmux_main(), opxy_gpio_main()); the panel the
 *   front started, taken on (S-13c.013); the PSoC link resumed from the
 *   record's greeting and power state, the battery's rest and the
 *   record's key status, 8 bytes on the OP-1F (S-07.032, S-18.030), on
 *   the S-07 module over a simulated link; on the OP-1F the main CPU's
 *   own eMMC (S-13b.060) through the bridge client on the same CPU
 *   (s09_master_local()) over an eMMC model, read by the image's staging;
 *   the boot decision (S-05.001); the OS hand-off's order, S-05.036 on
 *   the OP-1F (no release, no wait for a slave) and S-05.018 on the OP-XY,
 *   whose slave's stream and release are faked.
 *
 * Not reached on the host: opxy_main() and opxy_master_main() themselves,
 * which take the record from memory and bring up the UART, the SPI flash
 * and the panel's DMA; the board's own steps between are in
 * src/board/boot/master.c, and this test calls the modules they call, in
 * their order.
 */
#include <string.h>

#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "s07_test.h"

#include "waffle/board/values.h"
#include "waffle/board/boot.h"
#include "waffle/board/emmc_bridge.h"
#include "waffle/boot/flow.h"
#include "waffle/drivers/bf70x/msi.h"
#include "waffle/drivers/bf70x/spi.h"
#include "waffle/display/panel.h"
#include "waffle/bridge/master.h"

#define CCLK_MHZ 400u

/* Each step appends its letter, so a run's order reads as a string. */
static char trace[64];
static size_t ntrace;

static void step(char c)
{
	if (ntrace + 1u < sizeof(trace))
		trace[ntrace++] = c;
	trace[ntrace] = '\0';
}

/* --- the eMMC on the main CPU's MSI0, as test_board_storage_1f's --- */

static struct {
	uint8_t ext_csd[512];
	uint32_t csd[4];
	uint8_t *ext_csd_to;
	unsigned reads;			/* CMD17 and CMD18 */
	unsigned n;			/* commands */
} emmc;

static void take_command(uint32_t cmd)
{
	const uint32_t idx = cmd & 0x3Fu;
	uint32_t r1 = WB_MMC_R1_READY | WB_MMC_STATE_TRAN << 9;
	uint32_t is = MSI_IS_CMDDONE;

	emmc.n++;
	if (cmd & MSI_CMD_DXPECT)
		is |= MSI_IS_DTO;
	switch (idx) {
	case 1:
		r1 = 0xC0FF8080u;
		break;
	case 8:
		if (emmc.ext_csd_to != NULL)
			memcpy(emmc.ext_csd_to, emmc.ext_csd, sizeof(emmc.ext_csd));
		break;
	case 9:
		r1 = emmc.csd[0];
		fake_mmr_set(MSI_RESP1, emmc.csd[1]);
		fake_mmr_set(MSI_RESP2, emmc.csd[2]);
		fake_mmr_set(MSI_RESP3, emmc.csd[3]);
		break;
	case 17:
	case 18:
		emmc.reads++;
		break;
	default:
		break;
	}
	fake_mmr_set(MSI_RESP0, r1);
	fake_mmr_set(MSI_ISTAT, fake_mmr_get(MSI_ISTAT) | is);
}

static bool emmc_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		      wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_READ) {
		if (addr == MSI_STAT)
			*value = 0u;
		return true;
	}
	if (op != FAKE_MMR_WRITE)
		return true;
	switch (addr) {
	case MSI_CTL:
		*value &= ~MSI_CTL_RESETS;
		return true;
	case MSI_BUSMODE:
		*value &= ~MSI_BUSMODE_SWR;
		return true;
	case MSI_ISTAT:
	case MSI_IDSTS:
		fake_mmr_set(addr, fake_mmr_get(addr) & ~*value);
		return false;
	case MSI_CMD:
		if ((*value & MSI_CMD_STARTCMD) && !(*value & MSI_CMD_UCLKREGS))
			take_command(*value);
		*value &= ~MSI_CMD_STARTCMD;
		return true;
	default:
		return true;
	}
}

/* --- the panel's bus: the commands sent, as text --- */

static char panel_log[256];

static void p_log(const char *s)
{
	const size_t n = strlen(panel_log);

	if (n + strlen(s) + 1u < sizeof panel_log)
		strcat(panel_log, s);
}

static void p_reset(void *ctx, bool high) { (void)ctx; (void)high; }
static void p_select(void *ctx, bool on) { (void)ctx; if (on) p_log("S "); }

static void p_cmd(void *ctx, uint8_t c)
{
	static const char hex[] = "0123456789ABCDEF";
	const char t[4] = { hex[c >> 4], hex[c & 15u], ' ', '\0' };

	(void)ctx;
	p_log(t);
}

static void p_data(void *ctx, const uint8_t *p, size_t n) { (void)ctx; (void)p; (void)n; }

static void p_repeat(void *ctx, const uint8_t *g, size_t len, uint32_t count)
{
	(void)ctx;
	(void)g;
	(void)len;
	(void)count;
}

static int p_read(void *ctx, uint8_t cmd, uint8_t *out, size_t n)
{
	(void)ctx;
	(void)cmd;
	memset(out, 0, n);
	return 0;
}

static int p_frame_start(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	(void)buf;
	(void)n;
	return 0;
}

static bool p_frame_busy(void *ctx) { (void)ctx; return false; }
static void p_frame_stop(void *ctx) { (void)ctx; }
static int p_te_wait(void *ctx, uint32_t timeout_us) { (void)ctx; (void)timeout_us; return 0; }

/* The panel's time: well past its reset. */
static uint64_t p_now(void *ctx) { (void)ctx; return 10000000u; }
static void p_delay(void *ctx, uint32_t us) { (void)ctx; (void)us; }

static void black_row(void *ctx, unsigned r, uint8_t *out)
{
	(void)ctx;
	(void)r;
	memset(out, 0, S13C_ROW_BYTES);
}

/* --- an LDR stream the hand-off accepts, as test_boot_flow's --- */

static uint8_t staging[64];

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void ldr_block(uint8_t *h, uint16_t flags, uint32_t target, uint32_t count, uint32_t arg)
{
	uint8_t x = 0;

	put32(h, 0xAD000000u | flags | 1u);
	put32(h + 4, target);
	put32(h + 8, count);
	put32(h + 12, arg);
	for (unsigned i = 0; i < 16u; i++)
		x ^= h[i];
	h[2] = x;
}

/* --- one start --- */

static struct {
	const struct wb_device *dev;
	struct fr_handoff ho;
	struct opxy_hw hw;
	struct opxy_storage st;
	struct s09_master client;
	struct s13c_panel panel;
	struct s07_key_status ks;
	unsigned spi0_writes;		/* SPI0 register writes, the whole start */
	size_t psoc_from;		/* the rig's first item of this start */
	bool held;
	struct boot_held h;
} run;

static const struct s13c_bus panel_bus = {
	.reset = p_reset, .select = p_select, .cmd = p_cmd, .data = p_data, .repeat = p_repeat,
	.read = p_read, .frame_start = p_frame_start, .frame_busy = p_frame_busy,
	.frame_stop = p_frame_stop, .te_wait = p_te_wait, .now_us = p_now, .delay_us = p_delay,
};
static const struct s09_bus_ops no_bus;		/* the local client uses none */
static const struct s06_clock no_clock;

static unsigned spi0_writes(void)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	const wb_mmr_addr base = SPI_BASE(OPXY_SPI_LINK);
	unsigned n = 0;

	for (size_t i = 0; i < fake_mmr_count(); i++)
		n += log[i].op == FAKE_MMR_WRITE && log[i].addr >= base && log[i].addr < base + 0x1000u;
	fake_mmr_clear_log();
	return n;
}

/* S-13b.003, .004, .006 as bringup() applies them; the panel the front
 * started, taken on (S-01.023, S-13c.013). */
static void m_bringup(void *ctx)
{
	unsigned n;
	const struct opxy_gpio *g = opxy_gpio_main(run.dev, &n);

	(void)ctx;
	step('B');
	CHECK(wb_pinmux_apply(opxy_pinmux_main(run.dev)) == 0);
	for (unsigned i = 0; i < n; i++) {
		if (g[i].dir == OPXY_IN)
			(void)wb_gpio_input(g[i].pin);
		else if (g[i].dir == OPXY_OUT_KEEP)
			(void)wb_gpio_output_keep(g[i].pin);
		else
			(void)wb_gpio_output(g[i].pin, g[i].dir == OPXY_OUT_HIGH);
	}
	CHECK((run.ho.flags & FR_HO_PANEL_UP) != 0u);
	s13c_adopt(&run.panel, &panel_bus, run.dev, run.ho.panel_reset_ms);
	run.spi0_writes += spi0_writes();
}

static bool m_ddr(void *ctx) { (void)ctx; step('D'); return true; }
static void m_fatal(void *ctx) { (void)ctx; step('F'); }

static void m_meta(void *ctx, uint32_t *state, bool *nonzero)
{
	(void)ctx;
	step('M');
	*state = 0u;
	*nonzero = true;
}

static uint16_t m_reason(void *ctx) { (void)ctx; step('R'); return BOOT_REASON_NONE; }

/* psoc_start()'s path with the front's record: the link resumed, the
 * battery's rest, the record's key status. */
static enum s05_keys m_psoc(void *ctx)
{
	const uint32_t f = run.ho.flags;

	(void)ctx;
	step('K');
	t_rig_init_dev(S06_MS(100000), run.dev);
	run.psoc_from = rig.nitems;
	s07_resume(&rig.psoc, (f & FR_HO_GREETED) != 0u, (f & FR_HO_POWER_KNOWN) != 0u,
		   run.ho.power_state, run.ho.battery, run.ho.battery_len);
	CHECK(s07_battery_rest(&rig.psoc) == S07_OK);
	s07_key_status_given(&rig.psoc, (f & FR_HO_KEYS_REPLIED) != 0u ? run.ho.keys : NULL,
			     run.ho.key_len, &run.ks);
	if (!run.ks.replied)
		return S05_KEYS_SILENT;
	return run.ks.hold ? S05_KEYS_HELD : S05_KEYS_NOT_HELD;
}

/* The OP-XY's slave: its stream and bridge start, faked. */
static bool m_slave(void *ctx) { (void)ctx; step('S'); return true; }

/* storage_start()'s storage: the main CPU's own eMMC, served to the bridge
 * client on the same CPU. */
static void m_storage(void *ctx)
{
	const uint32_t sectors = S03_USER_SECTORS + 0x2000u;

	(void)ctx;
	step('E');
	fake_mmr_set_hook(emmc_hook, NULL);
	memset(&emmc, 0, sizeof(emmc));
	emmc.ext_csd[212] = (uint8_t)sectors;
	emmc.ext_csd[213] = (uint8_t)(sectors >> 8);
	emmc.ext_csd[214] = (uint8_t)(sectors >> 16);
	emmc.ext_csd[215] = (uint8_t)(sectors >> 24);
	emmc.ext_csd[226] = 0x20u;		/* BOOT_SIZE_MULT: 4 MiB */
	emmc.ext_csd[247] = 5u;			/* POWER_OFF_LONG_TIME */
	emmc.ext_csd[249] = 1u;			/* a cache */
	emmc.ext_csd[265] = 1u;
	emmc.ext_csd[179] = 0x48u;
	emmc.csd[3] = 0xFFFFFFFFu;
	emmc.csd[2] = 0xFFFFFFFFu;
	emmc.csd[1] = 0xFFFF801Fu | 31u << 10 | 31u << 5;
	emmc.csd[0] = 0xFFFFFFFFu;
	emmc.ext_csd_to = run.st.ext_csd;
	opxy_storage_init(&run.st, &run.hw, run.dev, true);
	s09_master_init(&run.client, &no_bus, &no_clock);
	s09_master_local(&run.client, &run.st.store);
	CHECK(run.st.emmc_up && !run.st.emmc_other);
	run.spi0_writes += spi0_writes();
}

static void m_held(void *ctx, const struct boot_held *h)
{
	(void)ctx;
	step('H');
	run.held = true;
	run.h = *h;
}

/* S-05.003, S-05.004: the hand-over frame on the panel taken on. */
static void h_pre(void *ctx)
{
	(void)ctx;
	step('P');
	CHECK(s13c_boot_frame(&run.panel, black_row, NULL) == S13C_OK);
}

/* S-05.005: the image staged; on the OP-1F a sector read reaches the
 * main CPU's own eMMC through the client, with no bus. */
static enum s05_fail h_stage(void *ctx, uint32_t *length)
{
	(void)ctx;
	step('G');
	if (!wb_has(run.dev, WB_F_SECOND_CPU)) {
		static _Alignas(32) uint8_t sector[S03_SECTOR];
		struct s03_emmc_ops ops = opxy_bridge_emmc(&run.client);
		const unsigned before = emmc.reads;

		CHECK(ops.select(ops.ctx, S03_PART_USER) == 0);
		CHECK(ops.read(ops.ctx, 100u, 1u, sector) == 0);
		CHECK(emmc.reads == before + 1u);
		CHECK(s09_master_release(&run.client) == -1);	/* no other CPU */
	}
	memset(staging, 0, sizeof(staging));
	ldr_block(staging, (uint16_t)(S05_FLAG_FIRST | S05_FLAG_IGNORE), 0x11A00000u, 0u, 32u);
	ldr_block(staging + 16, (uint16_t)S05_FLAG_FINAL, 0x11A00000u, 0u, 0u);
	*length = 32u;
	return S05_FAIL_NONE;
}

static int h_release(void *ctx, uint8_t *reply)
{
	(void)ctx;
	step('L');
	*reply = 0u;
	return 0;
}

/* S-05.016, S-13c.022: the panel's hand-over record; on the OP-1F the MSI
 * controller closed with no command (S-05.037). */
static void h_teardown(void *ctx)
{
	uint8_t rec[S13C_RECORD_SIZE];

	(void)ctx;
	step('T');
	CHECK(s13c_handover(&run.panel, rec) == S13C_OK);
	if (!wb_has(run.dev, WB_F_SECOND_CPU)) {
		const unsigned n = emmc.n;

		CHECK(opxy_storage_close(&run.st) == WB_MSI_OK);
		CHECK(emmc.n == n);
	}
	run.spi0_writes += spi0_writes();
}

static void h_kernel(void *ctx, uint32_t a) { (void)ctx; (void)a; step('k'); }
static uint32_t h_svect(void *ctx) { (void)ctx; step('V'); return 0x11A00000u; }
static void h_jump(void *ctx, uint32_t e) { (void)ctx; (void)e; step('J'); }
static void h_clear(void *ctx) { (void)ctx; step('Z'); }
static void h_failed(void *ctx, enum s05_fail cause) { (void)ctx; (void)cause; step('X'); }
static void h_accepted(void *ctx) { (void)ctx; step('A'); }

static const struct s05_master_ops handoff_ops = {
	.pre_load = h_pre, .stage = h_stage, .release = h_release, .teardown = h_teardown,
	.kernel_call = h_kernel, .read_svect0 = h_svect, .jump = h_jump,
	.clear_sw_version = h_clear, .held = h_failed, .accepted = h_accepted,
};

/* The record the front leaves (S-01.023, S-18.005): greeted, power on, a
 * usable battery, the panel up and lit, the key status of the device's
 * length, with the device's hold key down when held. */
static void record(const struct wb_device *dev, bool held)
{
	static const uint8_t usable[7] = { 0x01, 50, 25, 0, 0, 0x0F, 0xA0 };

	memset(&run, 0, sizeof run);
	run.dev = dev;
	run.ho.magic = FR_HO_MAGIC;
	run.ho.version = FR_HO_VERSION;
	run.ho.size = FR_HO_SIZE;
	run.ho.role_master = 1u;
	run.ho.flags = FR_HO_GREETED | FR_HO_POWER_KNOWN | FR_HO_KEYS_REPLIED | FR_HO_PANEL_UP |
		       FR_HO_PANEL_LIT | FR_HO_SUBSCRIBED;
	run.ho.power_state = S07_POWER_ON;
	run.ho.panel_reset_ms = 40u;
	memcpy(run.ho.battery, usable, sizeof usable);
	run.ho.battery_len = sizeof usable;
	run.ho.key_len = (uint8_t)dev->keys.bitmap_bytes;
	if (held && dev->id == WB_DEV_OP1F)
		run.ho.keys[7] = 0x02u;			/* key 0x39 (S-18.030) */
	else if (held)
		run.ho.keys[3] = 0x01u;			/* the OP-XY's hold bit */
	run.ho.device = dev->id == WB_DEV_OP1F ? FR_HO_DEV_OP1F : FR_HO_DEV_OPXY;
	run.ho.device_how = FR_HO_HOW_TABLE;
	run.hw.time.cycles_per_us = CCLK_MHZ;
}

static int no_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	(void)off;
	(void)buf;
	(void)len;
	t_failures++;			/* the record decides: no flash read */
	return -1;
}

/* A whole start, held with the device's hold key down. */
static void start(const struct wb_device *dev, bool held)
{
	struct opxy_device_pick pick;
	struct boot_master_ops ops;

	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(CCLK_MHZ);
	record(dev, held);
	ntrace = 0;
	trace[0] = '\0';
	panel_log[0] = '\0';
	/* S-18.005: the record's device; the main image decides nothing */
	opxy_device_pick(&run.ho, WB_BOOT_MODE_MAIN, NULL, NULL, no_read, NULL, &pick);
	CHECK(pick.dev == dev && pick.how == WB_DEV_BY_TABLE && pick.master && !pick.decided);
	memset(&ops, 0, sizeof ops);
	ops.bringup = m_bringup;
	ops.ddr_check = m_ddr;
	ops.ddr_fatal = m_fatal;
	ops.metadata = m_meta;
	ops.reboot_reason = m_reason;
	ops.psoc_start = m_psoc;
	ops.slave_start = m_slave;
	ops.storage_start = m_storage;
	ops.dev = pick.dev;
	ops.handoff = &handoff_ops;
	ops.staging = staging;
	ops.config_addr = 0x0803D000u;
	ops.held = m_held;
	boot_master(&ops);
	fake_mmr_set_hook(NULL, NULL);
}

static bool sent(uint8_t id)
{
	for (size_t j = run.psoc_from; j < rig.nitems; j++)
		if (!rig.item[j].raw && rig.item[j].id == id)
			return true;
	return false;
}

/* The OP-1F: the storage in the slave's place, S-05.036's order, no SPI0,
 * no PMIC message, the panel taken on with no gamma and the 8-byte key
 * status read; held with its hold key. */
static void test_op1f(void)
{
	t_case = "OP-1F start";
	start(&wb_device_op1f, false);
	CHECK(strcmp(trace, "BDMRKEPGATkVJ") == 0);
	CHECK(run.ks.replied && !run.ks.hold);
	CHECK(run.ho.key_len == 8u && rig.psoc.dev == &wb_device_op1f);
	CHECK(run.spi0_writes == 0u);				/* no_spi0 */
	CHECK(!sent(0x70) && !sent(0x71) && !sent(0x73) && !sent(0x01));	/* no_pmic_ids */
	CHECK(strstr(panel_log, "E2") == NULL && strstr(panel_log, "E3") == NULL);
	CHECK(!run.held);

	t_case = "OP-1F start, held";
	start(&wb_device_op1f, true);
	CHECK(run.ks.hold);
	CHECK(strcmp(trace, "BDMRKEH") == 0);
	CHECK(run.held && !run.h.slave_absent && !run.h.load_attempted);
}

/* The OP-XY, unchanged: the slave's stream, S-05.018's release, no
 * storage of its own. */
static void test_opxy(void)
{
	t_case = "OP-XY start";
	start(&wb_device_opxy, false);
	CHECK(strcmp(trace, "BDMRKSPGLTkVJ") == 0);
	CHECK(run.ks.replied && !run.ks.hold);
	CHECK(run.ho.key_len == 9u && rig.psoc.dev == &wb_device_opxy);
	CHECK(!run.held);

	t_case = "OP-XY start, held";
	start(&wb_device_opxy, true);
	CHECK(run.ks.hold);
	CHECK(strcmp(trace, "BDMRKSH") == 0);
	CHECK(run.held && !run.h.slave_absent);
}

int main(void)
{
	test_op1f();
	test_opxy();
	return t_finish("test_board_start");
}
