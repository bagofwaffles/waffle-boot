/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tests for the MSI controller and eMMC (src/drivers/bf70x/msi.c). */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x/msi.h"
#include "waffle/emmc/emmc.h"

static const struct wb_time cclk400 = { 400u };

/* MSI source SCLK0 / 2 (MSI_UHS_EXT reset value), S-13b.052. */
#define SRC_HZ 50000000u

static const struct wb_msi_config cfg = {
	63u, 0xFFFFFF40u, 0x20070008u, 1000000u, 500000u, 100000u, 0u, 0u, 0u,
};

/*
 * The eMMC behind the controller. Every command the CIU takes is logged
 * with the eMMC clock and the host bus width at that moment, answered at
 * once (CMDDONE, and DTO for data). Commands that leave the eMMC busy
 * hold DAT0 for a few STAT reads. The rules of S-13b.051-.056 are
 * checked as the commands arrive.
 */
#define LOG_MAX 128
static struct emmc {
	struct {
		uint8_t idx;
		uint32_t arg, hz;
		uint8_t width;
		uint32_t bytcnt;
	} log[LOG_MAX];
	unsigned n, violations;
	unsigned ocr_tries;	/* CMD1s answered busy before ready */
	unsigned busy_reads, busy_set;
	bool selected, hs_done, off, need_status, rto;
	uint8_t bus_width, pending_width;
	uint8_t last_switch_index;
	uint8_t fail_idx;	/* this command's R1 gets OUT_OF_RANGE */
	uint8_t rto_idx;	/* this command gets a response timeout */
	/* S-13b.051: the CSD CMD9 returns, as RESP0-3; CMD9 seen since CMD3,
	 * and with a long response */
	uint32_t csd[4];
	bool csd_sent;
	uint64_t dto_delay;	/* cycles to DTO after a data command; 0: at once */
	uint64_t dto_at;
	bool dto_pending;
	/* With block_cycles set, a block moves every block_cycles (TCBCNT
	 * counts them) up to stop_after blocks (0: all), and DTO follows the
	 * last; else TCBCNT stays 0 and DTO comes after dto_delay. */
	uint64_t block_cycles, xfer_start;
	uint32_t stop_after, xfer_blocks;
	uint32_t clkdiv, clken, hz;
	/* The IDMAC: the chain it walks (the host cannot follow DES3 on a
	 * 64-bit host), whose OWN bits it clears as the data completes (HRM
	 * Table 33-5) unless idmac_stuck; fbe makes the next data command
	 * end in a fatal bus error. FIFOTH and CDTHRCTL as each data command
	 * found them, and the controller resets asked for. */
	struct wb_msi_desc *chain;
	bool idmac_stuck, fbe;
	uint32_t fifoth_at, thr_at;
	unsigned ctl_resets, ctl_reset_at;	/* count, and commands before */
	unsigned not_ready;	/* CMD13s answered busy (PRG) */
} emmc;

/* The IDMAC closes each descriptor of the chain, up to the last (LD). */
static void idmac_close(void)
{
	unsigned i;

	if (emmc.chain == NULL || emmc.idmac_stuck)
		return;
	for (i = 0; i < 64u; i++) {
		uint32_t d0 = emmc.chain[i].des0;

		emmc.chain[i].des0 = d0 & ~MSI_DES0_OWN;
		if (d0 & MSI_DES0_LD)
			break;
	}
}

static uint32_t emmc_hz(void)
{
	if (!(emmc.clken & MSI_CLKEN_EN0))
		return 0;
	return emmc.clkdiv == 0 ? SRC_HZ : SRC_HZ / (2u * emmc.clkdiv);
}

static void busy(void)
{
	emmc.busy_reads = emmc.busy_set;
}

static void take_command(uint32_t cmd)
{
	uint32_t idx = cmd & 0x3Fu, arg = fake_mmr_get(MSI_CMDARG);
	uint32_t ctype = fake_mmr_get(MSI_CTYPE);
	uint32_t r1 = WB_MMC_R1_READY | WB_MMC_STATE_TRAN << 9;
	uint32_t is = MSI_IS_CMDDONE;
	uint8_t width = (ctype & MSI_CTYPE_WIDBYTE0) ? 8 : (ctype & 1u) ? 4 : 1;

	if (emmc.n < LOG_MAX) {
		emmc.log[emmc.n].idx = (uint8_t)idx;
		emmc.log[emmc.n].arg = arg;
		emmc.log[emmc.n].hz = emmc.hz;
		emmc.log[emmc.n].width = width;
		emmc.log[emmc.n].bytcnt = fake_mmr_get(MSI_BYTCNT);
	}
	emmc.n++;
	if (emmc.off)
		emmc.violations++;			/* S-13b.056 */
	if (emmc.busy_reads != 0)
		emmc.violations++;			/* S-13b.055 */
	/* S-13b.052, .053 */
	if (emmc.hz > 50000000u || (!emmc.selected && emmc.hz > 400000u) ||
	    (!emmc.hs_done && emmc.hz > 26000000u))
		emmc.violations++;
	if (cmd & MSI_CMD_DXPECT) {
		if (width != emmc.bus_width)
			emmc.violations++;		/* S-13b.051 */
		if (emmc.need_status)
			emmc.violations++;		/* S-13b.054 */
		/* HRM "Data Transmit/Receive" step 5: the IDMAC owns the first
		 * descriptor, or it suspends (DU). */
		if (emmc.chain != NULL && !(emmc.chain[0].des0 & MSI_DES0_OWN))
			emmc.violations++;
		emmc.fifoth_at = fake_mmr_get(MSI_FIFOTH);
		emmc.thr_at = fake_mmr_get(MSI_CDTHRCTL);
		if (emmc.fbe) {
			fake_mmr_set(MSI_IDSTS, fake_mmr_get(MSI_IDSTS) |
				     MSI_IDSTS_FBE);
		} else if (emmc.block_cycles != 0) {
			emmc.dto_pending = true;
			emmc.xfer_start = fake_core_peek_cycles();
			emmc.xfer_blocks = fake_mmr_get(MSI_BYTCNT) / 512u;
		} else if (emmc.dto_delay == 0) {
			is |= MSI_IS_DTO;
			idmac_close();
		} else {
			uint64_t now = fake_core_peek_cycles();

			emmc.dto_pending = true;
			emmc.dto_at = emmc.dto_delay > UINT64_MAX - now ?
				      UINT64_MAX : now + emmc.dto_delay;
		}
		if (cmd & MSI_CMD_RDWR)
			emmc.need_status = true;
	}
	if (emmc.rto || (emmc.rto_idx != 0 && idx == emmc.rto_idx)) {
		is = MSI_IS_CMDDONE | MSI_IS_RTO;
	}
	switch (idx) {
	case 1:
		r1 = emmc.ocr_tries == 0 ? 0xC0FF8080u : 0x00FF8080u;
		if (emmc.ocr_tries)
			emmc.ocr_tries--;
		if (arg != WB_MMC_OCR_ARG)
			emmc.violations++;
		break;
	case 3:
		if (arg != (uint32_t)WB_MMC_RCA << 16)
			emmc.violations++;
		emmc.csd_sent = false;
		break;
	case 9:
		/* S-13b.051: RCA << 16, an R2, before CMD7 */
		if (arg != (uint32_t)WB_MMC_RCA << 16 || emmc.selected ||
		    !(cmd & MSI_CMD_RLEN))
			emmc.violations++;
		emmc.csd_sent = !(is & MSI_IS_RTO);
		break;
	case 7:
		/* S-13b.051: CMD9 is required between steps 5 and 6 */
		if (arg != (uint32_t)WB_MMC_RCA << 16 || !emmc.csd_sent)
			emmc.violations++;
		emmc.selected = true;
		busy();
		break;
	case 6:
		emmc.last_switch_index = (uint8_t)(arg >> 16);
		if (emmc.last_switch_index == WB_EXT_CSD_BUS_WIDTH)
			emmc.pending_width = (arg >> 8 & 0xFu) == 2u ? 8 :
					     (arg >> 8 & 0xFu) == 1u ? 4 : 1;
		if (emmc.last_switch_index == WB_EXT_CSD_POWER_OFF &&
		    (arg >> 8 & 0xFFu) == 3u)
			emmc.off = true;
		busy();
		break;
	case 12:
	case 38:
		busy();
		emmc.need_status = emmc.need_status || idx == 38;
		break;
	case 13:
		emmc.need_status = false;
		if (emmc.not_ready != 0u) {
			/* busy programming: not ready, state PRG (7) */
			emmc.not_ready--;
			r1 = 7u << 9;
		}
		break;
	default:
		break;
	}
	if (emmc.fail_idx != 0 && idx == emmc.fail_idx)
		r1 |= 1u << 31;
	if (idx == 9u) {
		r1 = emmc.csd[0];
		fake_mmr_set(MSI_RESP1, emmc.csd[1]);
		fake_mmr_set(MSI_RESP2, emmc.csd[2]);
		fake_mmr_set(MSI_RESP3, emmc.csd[3]);
	}
	fake_mmr_set(MSI_RESP0, r1);
	fake_mmr_set(MSI_ISTAT, fake_mmr_get(MSI_ISTAT) | is);
}

static void busy_ended(void)
{
	if (emmc.last_switch_index == WB_EXT_CSD_BUS_WIDTH &&
	    emmc.pending_width)
		emmc.bus_width = emmc.pending_width;
	if (emmc.last_switch_index == WB_EXT_CSD_HS_TIMING)
		emmc.hs_done = true;
	emmc.pending_width = 0;
	emmc.last_switch_index = 0;
}

static uint32_t blocks_moved(void)
{
	uint64_t k;

	if (emmc.block_cycles == 0)
		return 0;
	k = (fake_core_peek_cycles() - emmc.xfer_start) / emmc.block_cycles;
	if (emmc.stop_after != 0 && k > emmc.stop_after)
		k = emmc.stop_after;
	return k < emmc.xfer_blocks ? (uint32_t)k : emmc.xfer_blocks;
}

static bool emmc_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		      wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_READ) {
		if (addr == MSI_STAT) {
			*value = emmc.busy_reads ? MSI_STAT_DBUSY : 0;
			if (emmc.busy_reads && --emmc.busy_reads == 0)
				busy_ended();
		}
		if (addr == MSI_TCBCNT)
			*value = blocks_moved() * 512u;
		if (addr == MSI_ISTAT && emmc.dto_pending &&
		    (emmc.block_cycles != 0 ?
		     blocks_moved() == emmc.xfer_blocks :
		     fake_core_peek_cycles() >= emmc.dto_at)) {
			emmc.dto_pending = false;
			fake_mmr_set(MSI_ISTAT, *value | MSI_IS_DTO);
			*value |= MSI_IS_DTO;
			idmac_close();
		}
		return true;
	}
	if (op != FAKE_MMR_WRITE)
		return true;
	switch (addr) {
	case MSI_CTL:
		if (*value & MSI_CTL_CTLRST) {
			emmc.ctl_resets++;
			emmc.ctl_reset_at = emmc.n;
		}
		*value &= ~MSI_CTL_RESETS;	/* resets finish at once */
		return true;
	case MSI_BUSMODE:
		*value &= ~MSI_BUSMODE_SWR;
		return true;
	case MSI_ISTAT:
	case MSI_IDSTS:
		fake_mmr_set(addr, fake_mmr_get(addr) & ~*value);
		return false;
	case MSI_CMD:
		if (!(*value & MSI_CMD_STARTCMD))
			return true;
		if (*value & MSI_CMD_UCLKREGS) {
			if (emmc.busy_reads)
				emmc.violations++;	/* clock while busy */
			emmc.clkdiv = fake_mmr_get(MSI_CLKDIV);
			emmc.clken = fake_mmr_get(MSI_CLKEN);
			emmc.hz = emmc_hz();
		} else {
			take_command(*value);
		}
		*value &= ~MSI_CMD_STARTCMD;
		return true;
	default:
		return true;
	}
}

/*
 * A CSD (JESD84-B51) with ERASE_GRP_SIZE at bits 46:42 and ERASE_GRP_MULT
 * at 41:37, the fields around them all ones (WP_GRP_SIZE 36:32, C_SIZE_MULT
 * 49:47 and up), and bit 0 one, as the R2's end bit leaves it.
 */
static void csd_erase_group(uint32_t size, uint32_t mult)
{
	emmc.csd[3] = 0x90FF8001u;
	emmc.csd[2] = 0xFFFFFFFFu;
	emmc.csd[1] = 0xFFFF801Fu | size << 10 | mult << 5;
	emmc.csd[0] = 0x00000001u;
}

static struct wb_msi_desc desc[2];
static _Alignas(32) uint32_t ext_words[128];
static uint8_t *const ext = (uint8_t *)ext_words;

static void reset_emmc(void)
{
	fake_mmr_reset();
	fake_core_reset();
	memset(&emmc, 0, sizeof(emmc));
	csd_erase_group(31u, 31u);	/* 1024 sectors */
	emmc.bus_width = 1;
	emmc.busy_set = 3;
	emmc.ocr_tries = 2;
	emmc.chain = desc;
	fake_mmr_set_hook(emmc_hook, NULL);
	memset(ext_words, 0, sizeof(ext_words));
}

static unsigned count_idx(unsigned from, uint8_t idx)
{
	unsigned i, k = 0;

	for (i = from; i < emmc.n && i < LOG_MAX; i++)
		k += emmc.log[i].idx == idx;
	return k;
}

static void init_emmc(struct wb_msi *m, uint8_t device_type, uint8_t drv)
{
	reset_emmc();
	ext[WB_EXT_CSD_DEVICE_TYPE] = device_type;
	ext[WB_EXT_CSD_DRIVER_STR] = drv;
	ext[WB_EXT_CSD_POWER_OFF_LONG_TIME] = 5;
	CHECK(wb_msi_init(m, &cfg, &cclk400, desc, 2) == WB_MSI_OK);
	CHECK(wb_msi_emmc_init(m, ext) == WB_MSI_OK);
	CHECK(emmc.violations == 0);
}

/* S-13b.051 with an HS200-capable eMMC with driver strength 3. */
static void test_bring_up_hs200(void)
{
	static const uint8_t want[] = { 0, 1, 1, 1, 2, 3, 9, 7, 6, 13, 8, 6, 13, 8 };
	struct wb_msi m;
	unsigned i;

	init_emmc(&m, 0x57u, 0x0Fu);
	CHECK(emmc.n == sizeof(want));
	for (i = 0; i < sizeof(want) && i < emmc.n; i++)
		CHECK(emmc.log[i].idx == want[i]);
	/* S-13b.052: identification at 396.8 kHz (MSI_CLKDIV 63). */
	CHECK(emmc.log[0].hz == 396825u);
	CHECK(emmc.log[0].width == 1);
	CHECK(emmc.log[6].arg == 0x5A7E0000u);	/* CMD9 at RCA << 16 */
	CHECK(emmc.log[6].hz == 396825u);
	CHECK(emmc.log[7].hz == 396825u);	/* CMD7 at the identification clock */
	CHECK(emmc.log[8].arg == 0x03B70200u);	/* Switch(183, 2) */
	CHECK(emmc.log[8].hz == 25000000u);	/* then 25 MHz until HS_TIMING */
	CHECK(emmc.log[10].hz == 25000000u && emmc.log[11].hz == 25000000u);
	CHECK(emmc.log[10].width == 8);		/* CMD8 on 8 bits */
	CHECK(emmc.log[11].arg == 0x03B93200u);	/* Switch(185, 0x32) */
	CHECK(emmc.log[13].hz == 50000000u);
	CHECK(m.hs_timing == 0x32u && m.clkdiv == 0 && m.width == 8);
	CHECK(fake_mmr_get(MSI_CLKEN) == (MSI_CLKEN_EN0 | MSI_CLKEN_LP0));
	CHECK(m.power_off_long_time == 5);
	/* the CSD kept as MSI_RESP0-3 gave it */
	CHECK(m.csd[0] == emmc.csd[0] && m.csd[1] == emmc.csd[1] &&
	      m.csd[2] == emmc.csd[2] && m.csd[3] == emmc.csd[3]);
	CHECK(wb_msi_csd_erase_group(&m) == 1024u);
}

/*
 * S-03.001: the erase group from the CSD's two fields, each in its own
 * bits; S-13b.051, S-13b.059: a CSD not received fails the bring-up, no
 * CMD7 follows, and nothing of a CSD is kept.
 */
static void test_csd(void)
{
	struct wb_msi m;
	unsigned k;

	reset_emmc();
	csd_erase_group(0u, 0u);
	CHECK(wb_msi_init(&m, &cfg, &cclk400, desc, 2) == WB_MSI_OK);
	CHECK(wb_msi_emmc_init(&m, ext) == WB_MSI_OK);
	CHECK(wb_msi_csd_erase_group(&m) == 1u);
	reset_emmc();
	csd_erase_group(3u, 7u);
	CHECK(wb_msi_init(&m, &cfg, &cclk400, desc, 2) == WB_MSI_OK);
	CHECK(wb_msi_emmc_init(&m, ext) == WB_MSI_OK);
	CHECK(wb_msi_csd_erase_group(&m) == 4u * 8u);
	reset_emmc();
	csd_erase_group(15u, 31u);
	CHECK(wb_msi_init(&m, &cfg, &cclk400, desc, 2) == WB_MSI_OK);
	CHECK(wb_msi_emmc_init(&m, ext) == WB_MSI_OK);
	CHECK(wb_msi_csd_erase_group(&m) == 16u * 32u);
	CHECK(emmc.violations == 0);

	/* no CSD: the step fails, the bring-up ends there */
	reset_emmc();
	emmc.rto_idx = 9;
	CHECK(wb_msi_init(&m, &cfg, &cclk400, desc, 2) == WB_MSI_OK);
	CHECK(wb_msi_emmc_init(&m, ext) == WB_MSI_ERR_CMD);
	CHECK(count_idx(0, 9) == 1u && count_idx(0, 7) == 0u &&
	      count_idx(0, 8) == 0u);
	for (k = 0; k < 4u; k++)
		CHECK(m.csd[k] == 0u);

	/* a CSD received, then a later step fails: none of it kept */
	reset_emmc();
	emmc.fail_idx = 7;
	CHECK(wb_msi_init(&m, &cfg, &cclk400, desc, 2) == WB_MSI_OK);
	CHECK(wb_msi_emmc_init(&m, ext) == WB_MSI_ERR_EMMC);
	for (k = 0; k < 4u; k++)
		CHECK(m.csd[k] == 0u);
}

/* S-13b.053's other rows. */
static void test_bring_up_timing_rows(void)
{
	struct wb_msi m;

	/* High speed at 26 MHz only: HS_TIMING 1 at 25 MHz. */
	init_emmc(&m, 0x01u, 0x0Fu);
	CHECK(m.hs_timing == 0x01u && m.clkdiv == 1u);
	CHECK(emmc.log[emmc.n - 1u].hz == 25000000u);

	/* Driver strength 3 without HS200: high speed at 50 MHz. */
	init_emmc(&m, 0x03u, 0x08u);
	CHECK(m.hs_timing == 0x01u && m.clkdiv == 0);

	/* HS200 without driver strength 3: high speed. */
	init_emmc(&m, 0x10u, 0x01u);
	CHECK(m.hs_timing == 0x01u && m.clkdiv == 0);

	/* No high-speed type: no switch, 25 MHz from CMD7 on (S-13b.052). */
	init_emmc(&m, 0x00u, 0x0Fu);
	CHECK(m.hs_timing == 0 && m.clkdiv == 1u);
	CHECK(emmc.log[emmc.n - 1u].hz == 25000000u);
	CHECK(count_idx(0, 6) == 1);
}

/* S-13b.057: CMD1 gives up after at least 1 s. */
static void test_cmd1_bounded(void)
{
	struct wb_msi m;

	reset_emmc();
	emmc.ocr_tries = 0xFFFFFFFFu;
	CHECK(wb_msi_init(&m, &cfg, &cclk400, desc, 2) == WB_MSI_OK);
	fake_core_set_cycle_step(4000);
	CHECK(wb_msi_emmc_init(&m, ext) == WB_MSI_TIMEOUT);
	CHECK(fake_core_peek_cycles() >= 400000000ull);
	CHECK(count_idx(0, 2) == 0);

	reset_emmc();
	emmc.rto = true;
	CHECK(wb_msi_init(&m, &cfg, &cclk400, desc, 2) == WB_MSI_OK);
	CHECK(wb_msi_cmd(&m, 0, 0, WB_MSI_R1, NULL) == WB_MSI_ERR_CMD);
}

/* S-13b.054 through the s03_emmc_ops adapter. */
static void test_transfers(void)
{
	struct wb_msi m;
	struct s03_emmc_ops ops;
	static _Alignas(32) uint32_t buf[20 * 128];
	unsigned from;

	init_emmc(&m, 0x57u, 0x0Fu);
	ops = (struct s03_emmc_ops){
		&m, wb_msi_emmc_select, wb_msi_emmc_read, wb_msi_emmc_write,
		wb_msi_emmc_erase, wb_msi_emmc_switch,
	};

	/* 20 blocks with two descriptors: 16 then 4, each closed by CMD12. */
	from = emmc.n;
	CHECK(ops.read(ops.ctx, 1000, 20, (uint8_t *)buf) == 0);
	CHECK(emmc.n - from == 4);
	CHECK(emmc.log[from].idx == 18 && emmc.log[from].arg == 1000);
	CHECK(emmc.log[from].bytcnt == 16u * 512u);
	CHECK(emmc.log[from + 1u].idx == 12);
	CHECK(emmc.log[from + 2u].idx == 18 && emmc.log[from + 2u].arg == 1016);
	CHECK(emmc.log[from + 2u].bytcnt == 4u * 512u);
	/* The last chain: one descriptor, first and last, handed back by the
	 * IDMAC (OWN clear, HRM Table 33-5). */
	CHECK(desc[0].des1 == 2048u);
	CHECK((desc[0].des0 & (MSI_DES0_OWN | MSI_DES0_FS | MSI_DES0_LD |
			       MSI_DES0_CH)) ==
	      (MSI_DES0_FS | MSI_DES0_LD | MSI_DES0_CH));
	CHECK(desc[0].des2 == (uint32_t)(uintptr_t)&buf[16 * 128]);
	CHECK(fake_mmr_get(MSI_DBADDR) == (uint32_t)(uintptr_t)desc);

	/* A write, then a read: CMD13 READY_FOR_DATA comes between. */
	from = emmc.n;
	CHECK(wb_msi_emmc_write(&m, 7, 1, (const uint8_t *)buf) == 0);
	CHECK(wb_msi_emmc_read(&m, 7, 1, (uint8_t *)buf) == 0);
	CHECK(emmc.log[from].idx == 24 && emmc.log[from + 1u].idx == 13);
	CHECK(emmc.log[from + 2u].idx == 17);

	/* Erase: CMD35 and CMD36 with the group's first sector, CMD38. */
	from = emmc.n;
	CHECK(ops.erase(ops.ctx, 0x4000u) == 0);
	CHECK(emmc.log[from].idx == 35 && emmc.log[from].arg == 0x4000u);
	CHECK(emmc.log[from + 1u].idx == 36 &&
	      emmc.log[from + 1u].arg == 0x4000u);
	CHECK(emmc.log[from + 2u].idx == 38 && emmc.log[from + 2u].arg == 0);
	CHECK(emmc.log[from + 3u].idx == 13);

	/* Partition select: Switch(179, n). */
	from = emmc.n;
	CHECK(ops.select(ops.ctx, 1) == 0);
	CHECK(emmc.log[from].idx == 6 && emmc.log[from].arg == 0x03B30100u);

	CHECK(ops.read(ops.ctx, 0, 1, (uint8_t *)buf + 2) ==
	      WB_MSI_ERR_ARG);
	CHECK(ops.read(ops.ctx, 0, 0, (uint8_t *)buf) == WB_MSI_ERR_ARG);

	/* S-13b.054: a buffer 4 bytes past a 32-byte boundary is refused at
	 * every entry, with nothing sent; one on the next boundary passes. */
	from = emmc.n;
	CHECK(wb_msi_emmc_read(&m, 0, 1, (uint8_t *)buf + 4) ==
	      WB_MSI_ERR_ARG);
	CHECK(wb_msi_emmc_write(&m, 0, 1, (const uint8_t *)buf + 4) ==
	      WB_MSI_ERR_ARG);
	CHECK(wb_msi_emmc_write_reliable(&m, 0, 1, (const uint8_t *)buf + 4) ==
	      WB_MSI_ERR_ARG);
	CHECK(wb_msi_read_ext_csd(&m, (uint8_t *)buf + 4) == WB_MSI_ERR_ARG);
	CHECK(wb_msi_emmc_init(&m, (uint8_t *)buf + 4) == WB_MSI_ERR_ARG);
	CHECK(emmc.n == from);
	CHECK(wb_msi_emmc_read(&m, 0, 1, (uint8_t *)buf + 32) == 0);
	CHECK(desc[0].des2 == (uint32_t)(uintptr_t)((uint8_t *)buf + 32));
	CHECK(wb_msi_emmc_write(&m, 0, 1, (const uint8_t *)buf + 32) == 0);
	CHECK(emmc.n > from);
	CHECK(emmc.violations == 0);
}

/*
 * S-13b.057: a data phase of n blocks waits one block's allowance (0.1 s,
 * and 0.5 s busy for a write, in cfg) from the last move of MSI_TCBCNT,
 * and at most xfer_total_us (2 s here) in all; one block keeps the single
 * bound. After a multi-block phase that failed, CMD12 and a write's busy
 * end leave the eMMC in transfer state.
 */
#define US 400ull		/* cycles per microsecond (cclk400) */
#define TICK 10000ull		/* cycles per read of the counter */

static uint64_t timed_out_after(int (*op)(void *, uint32_t, uint32_t,
					   uint8_t *), struct wb_msi *m,
				uint32_t count, uint8_t *buf)
{
	uint64_t t0 = fake_core_peek_cycles();

	CHECK(op(m, 0, count, buf) == WB_MSI_TIMEOUT);
	return fake_core_peek_cycles() - t0;
}

/* cppcheck-suppress constParameterCallback ; the ops shape fixes the type */
static int write_op(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	return wb_msi_emmc_write(ctx, sector, count, buf);
}

/* cppcheck-suppress constParameterCallback ; the ops shape fixes the type */
static int reliable_op(void *ctx, uint32_t sec, uint32_t count, uint8_t *buf)
{
	return wb_msi_emmc_write_reliable(ctx, sec, count, buf);
}

static void test_data_timeout(void)
{
	static struct wb_msi_desc desc16[16];
	static _Alignas(32) uint32_t buf[128 * 128];
	static const struct wb_msi_config tcfg = {
		63u, 0xFFFFFF40u, 0x20070008u, 1000000u, 500000u, 100000u, 0u,
		2000000u, 0u,
	};
	const uint64_t xfer = tcfg.xfer_timeout_us * US;
	const uint64_t wallow = xfer + tcfg.busy_timeout_us * US;
	const uint64_t total = tcfg.xfer_total_us * US;
	const uint64_t slack = 100u * TICK;	/* a few commands' polls */
	struct wb_msi m;
	unsigned from;
	uint64_t t;

	init_emmc(&m, 0x57u, 0x0Fu);
	m.desc = desc16;
	m.ndesc = 16;
	emmc.chain = desc16;
	m.c = &tcfg;
	fake_core_set_cycle_step(TICK);

	/* Moving at a rate just inside the whole bound passes, far past
	 * one block's allowance. */
	emmc.block_cycles = (total - slack) / 128u;
	CHECK(wb_msi_emmc_write(&m, 0, 128, (const uint8_t *)buf) == WB_MSI_OK);
	CHECK(wb_msi_emmc_write_reliable(&m, 0, 128, (const uint8_t *)buf) ==
	      WB_MSI_OK);
	CHECK(wb_msi_emmc_read(&m, 0, 128, (uint8_t *)buf) == WB_MSI_OK);
	CHECK(emmc.violations == 0);

	/* An eMMC that creeps, each block inside the allowance, is cut at the
	 * whole bound with the timeout, then stopped: CMD12, CMD13. */
	emmc.block_cycles = wallow - 400u * US;
	from = emmc.n;
	t = timed_out_after(write_op, &m, 128, (uint8_t *)buf);
	CHECK(t >= total && t <= total + slack);
	CHECK(emmc.n - from == 3u && emmc.log[from + 1u].idx == 12 &&
	      emmc.log[from + 2u].idx == 13);
	t = timed_out_after(reliable_op, &m, 128, (uint8_t *)buf);
	CHECK(t >= total && t <= total + slack);
	emmc.block_cycles = xfer - 400u * US;
	t = timed_out_after(wb_msi_emmc_read, &m, 128, (uint8_t *)buf);
	CHECK(t >= total && t <= total + slack);

	/* One that stops fails one allowance after it stopped. */
	emmc.block_cycles = (total - slack) / 128u;
	emmc.stop_after = 10;
	t = timed_out_after(write_op, &m, 128, (uint8_t *)buf);
	CHECK(t >= 10u * emmc.block_cycles + wallow &&
	      t <= 10u * emmc.block_cycles + wallow + slack);
	t = timed_out_after(wb_msi_emmc_read, &m, 128, (uint8_t *)buf);
	CHECK(t >= 10u * emmc.block_cycles + xfer &&
	      t <= 10u * emmc.block_cycles + xfer + slack);
	emmc.stop_after = 0;
	emmc.block_cycles = 0;

	/* A count that never moves: one allowance from the command. */
	emmc.dto_delay = wallow - slack;
	CHECK(wb_msi_emmc_write(&m, 0, 128, (const uint8_t *)buf) == WB_MSI_OK);
	emmc.dto_delay = xfer - slack;
	CHECK(wb_msi_emmc_read(&m, 0, 128, (uint8_t *)buf) == WB_MSI_OK);
	emmc.dto_delay = UINT64_MAX;
	t = timed_out_after(write_op, &m, 128, (uint8_t *)buf);
	CHECK(t >= wallow && t <= wallow + slack);
	t = timed_out_after(wb_msi_emmc_read, &m, 128, (uint8_t *)buf);
	CHECK(t >= xfer && t <= xfer + slack);
	CHECK(emmc.violations == 0);

	/* One block: the single bound and no CMD12, as before. */
	from = emmc.n;
	t = timed_out_after(write_op, &m, 1, (uint8_t *)buf);
	CHECK(t >= xfer && t <= xfer + slack);
	CHECK(emmc.n - from == 1u);
	t = timed_out_after(wb_msi_emmc_read, &m, 1, (uint8_t *)buf);
	CHECK(t >= xfer && t <= xfer + slack);
	emmc.dto_delay = xfer + slack;
	t = timed_out_after(write_op, &m, 1, (uint8_t *)buf);
	CHECK(t >= xfer && t <= xfer + slack);
	emmc.need_status = false;	/* the eMMC model's, after the timeouts */
	emmc.dto_delay = xfer - slack;
	CHECK(wb_msi_emmc_write(&m, 0, 1, (const uint8_t *)buf) == WB_MSI_OK);
	emmc.dto_delay = 0;
	fake_core_set_cycle_step(1);
}

/*
 * S-13b.054: after any failed multi-block data phase, CMD12 whatever it
 * returns, then the busy end and CMD13, so the next command is taken. A
 * reliable write of one block is multi-block too (CMD23, CMD25).
 */
/*
 * A chain of descriptors as the unit runs it: FIFOTH stays as configured
 * and CDTHRCTL is never written, and DTO ends the transfer; the IDMAC
 * hands every descriptor back (OWN clear, HRM Table 33-5), and every
 * descriptor's buffer is on a 32-byte boundary (S-13b.054). A fatal bus
 * error gets a controller reset before CMD12 (HRM ch. 33, "Fatal Bus
 * Errors (FBE)").
 */
static void test_chain(void)
{
	static _Alignas(1024) uint8_t mem[1024u + 128u * 512u + 64u];
	static struct wb_msi_desc desc16[16];
	struct wb_msi m;
	unsigned from, i;

	init_emmc(&m, 0x57u, 0x0Fu);
	m.desc = desc16;
	m.ndesc = 16;
	emmc.chain = desc16;

	/* 128 blocks at 32 bytes past a 1 KB boundary, and on it. */
	CHECK(wb_msi_emmc_read(&m, 0, 128, mem + 32) == WB_MSI_OK);
	CHECK(emmc.fifoth_at == cfg.fifoth && emmc.thr_at == 0u);
	for (i = 0; i < 16u; i++) {
		CHECK(!(desc16[i].des0 & MSI_DES0_OWN));
		CHECK(desc16[i].des2 % 32u == 0u);
	}
	CHECK(wb_msi_emmc_write(&m, 0, 128, mem) == WB_MSI_OK);
	CHECK(emmc.fifoth_at == cfg.fifoth && emmc.thr_at == 0u);
	CHECK(wb_msi_emmc_read(&m, 0, 128, mem + 32) == WB_MSI_OK);
	CHECK(emmc.fifoth_at == cfg.fifoth && emmc.thr_at == 0u);
	CHECK(fake_mmr_get(MSI_FIFOTH) == cfg.fifoth);
	CHECK(fake_mmr_get(MSI_CDTHRCTL) == 0u);
	CHECK(emmc.violations == 0);

	/* DTO alone ends a chain: the descriptors are not read back. */
	emmc.idmac_stuck = true;
	CHECK(wb_msi_emmc_read(&m, 0, 128, mem + 32) == WB_MSI_OK);
	emmc.idmac_stuck = false;

	/* A fatal bus error: a controller reset before CMD12. */
	emmc.fbe = true;
	emmc.ctl_resets = 0;
	from = emmc.n;
	CHECK(wb_msi_emmc_read(&m, 0, 128, mem + 32) == WB_MSI_ERR_DATA);
	CHECK(emmc.ctl_resets == 1u && emmc.ctl_reset_at == from + 1u);
	CHECK(emmc.n - from == 3u && emmc.log[from + 1u].idx == 12);
	emmc.fbe = false;
	fake_mmr_set(MSI_IDSTS, 0);
	CHECK(wb_msi_emmc_read(&m, 0, 128, mem + 32) == WB_MSI_OK);
	CHECK(emmc.violations == 0);
}

static void test_failed_multi_stops(void)
{
	static _Alignas(32) uint32_t buf[4 * 128];
	struct wb_msi m;
	unsigned from;

	init_emmc(&m, 0x57u, 0x0Fu);
	fake_core_set_cycle_step(TICK);
	emmc.dto_delay = UINT64_MAX;

	/* CMD12's R1 reports an error: the wait still follows. */
	emmc.fail_idx = 12;
	from = emmc.n;
	CHECK(wb_msi_emmc_write(&m, 0, 4, (const uint8_t *)buf) ==
	      WB_MSI_TIMEOUT);
	CHECK(emmc.n - from == 3u && emmc.log[from].idx == 25 &&
	      emmc.log[from + 1u].idx == 12 && emmc.log[from + 2u].idx == 13);
	emmc.fail_idx = 0;

	/* A read is stopped and waited for too. */
	from = emmc.n;
	CHECK(wb_msi_emmc_read(&m, 0, 4, (uint8_t *)buf) == WB_MSI_TIMEOUT);
	CHECK(emmc.n - from == 3u && emmc.log[from].idx == 18 &&
	      emmc.log[from + 1u].idx == 12 && emmc.log[from + 2u].idx == 13);

	/* One block, reliable: CMD23, CMD25, CMD12, CMD13. */
	from = emmc.n;
	CHECK(wb_msi_emmc_write_reliable(&m, 0, 1, (const uint8_t *)buf) ==
	      WB_MSI_TIMEOUT);
	CHECK(emmc.n - from == 4u && emmc.log[from + 1u].idx == 25 &&
	      emmc.log[from + 2u].idx == 12 && emmc.log[from + 3u].idx == 13);

	/* The next transfer finds the eMMC ready. */
	emmc.dto_delay = 0;
	CHECK(wb_msi_emmc_write(&m, 0, 4, (const uint8_t *)buf) == WB_MSI_OK);
	CHECK(emmc.violations == 0);
	fake_core_set_cycle_step(1);
}

/*
 * JESD84-B51's read-ahead: a multiple-block read that ends at a partition's
 * last block may draw ADDRESS_OUT_OF_RANGE in CMD12's response, which the
 * host ignores once every block arrived.
 */
static void test_read_ahead_out_of_range(void)
{
	static _Alignas(32) uint8_t buf[128u * 512u];
	struct wb_msi m;
	unsigned from;

	init_emmc(&m, 0x57u, 0x0Fu);
	emmc.dto_delay = 0;
	emmc.fail_idx = 12;
	from = emmc.n;
	CHECK(wb_msi_emmc_read(&m, 0x3F80u, 128, buf) == WB_MSI_OK);
	CHECK(emmc.log[from].idx == 18 && emmc.log[from + 1u].idx == 12);
	/* a single block has no CMD12 */
	CHECK(wb_msi_emmc_read(&m, 0x3FFFu, 1, buf) == WB_MSI_OK);
	emmc.fail_idx = 0;
	CHECK(wb_msi_emmc_read(&m, 0, 4, buf) == WB_MSI_OK);
	CHECK(emmc.violations == 0);
}

/* No whole bound given: the allowance alone, which saturates where
 * 2^31 + 2^31 us would wrap to 0. */
static void test_data_timeout_saturates(void)
{
	static const struct wb_msi_config big = {
		63u, 0xFFFFFF40u, 0x20070008u, 1000000u, 0x80000000u,
		0x80000000u, 0u, 0u, 0u,
	};
	static struct wb_msi_desc desc16[16];
	static _Alignas(32) uint32_t buf[128 * 128];
	struct wb_msi m;
	uint64_t t;

	init_emmc(&m, 0x57u, 0x0Fu);
	m.desc = desc16;
	m.ndesc = 16;
	emmc.chain = desc16;
	m.c = &big;
	fake_core_set_cycle_step(1ull << 24);
	emmc.dto_delay = UINT64_MAX;
	t = timed_out_after(write_op, &m, 128, (uint8_t *)buf);
	CHECK(t >= 0xFFFFFFFFull * US && t <= 0xFFFFFFFFull * US +
	      100ull * (1ull << 24));
	emmc.dto_delay = 0;
	fake_core_set_cycle_step(1);
}

/* S-09.040 0x13, S-13b.054: CMD23 with bit 31, CMD25, no CMD12. */
static void test_write_reliable(void)
{
	static _Alignas(32) uint32_t buf[17 * 128];
	struct wb_msi m;
	unsigned from;

	init_emmc(&m, 0x57u, 0x0Fu);
	from = emmc.n;
	CHECK(wb_msi_emmc_write_reliable(&m, 0x2000u, 16,
					 (const uint8_t *)buf) == 0);
	CHECK(emmc.n - from == 3);
	CHECK(emmc.log[from].idx == 23 && emmc.log[from].arg == 0x80000010u);
	CHECK(emmc.log[from + 1u].idx == 25 &&
	      emmc.log[from + 1u].arg == 0x2000u);
	CHECK(emmc.log[from + 1u].bytcnt == 16u * 512u);
	CHECK(emmc.log[from + 2u].idx == 13);
	CHECK(count_idx(from, 12) == 0);

	from = emmc.n;
	CHECK(wb_msi_emmc_write_reliable(&m, 5, 1, (const uint8_t *)buf) == 0);
	CHECK(emmc.log[from].arg == 0x80000001u);

	/* Never split: 17 blocks need three descriptors. */
	from = emmc.n;
	CHECK(wb_msi_emmc_write_reliable(&m, 0, 17, (const uint8_t *)buf) ==
	      WB_MSI_ERR_ARG);
	CHECK(wb_msi_emmc_write_reliable(&m, 0, 0, (const uint8_t *)buf) ==
	      WB_MSI_ERR_ARG);
	CHECK(wb_msi_emmc_write_reliable(&m, 0, 1, (const uint8_t *)buf + 2) ==
	      WB_MSI_ERR_ARG);
	CHECK(emmc.n == from);

	/* A refused CMD23 sends no CMD25. */
	emmc.fail_idx = 23;
	CHECK(wb_msi_emmc_write_reliable(&m, 0, 4, (const uint8_t *)buf) ==
	      WB_MSI_ERR_EMMC);
	CHECK(emmc.n == from + 1u);
	CHECK(emmc.violations == 0);
}

/* S-09.040 0x14, S-16.093: the CMD38 argument as given. */
static void test_erase_arg(void)
{
	static const uint32_t ok[] = {
		WB_MMC_ERASE, WB_MMC_TRIM, WB_MMC_DISCARD, WB_MMC_SECURE_ERASE,
		WB_MMC_SECURE_TRIM1, WB_MMC_SECURE_TRIM2,
	};
	static const uint32_t bad[] = { 2u, 0x80000003u, 0x00008000u, 1u << 30 };
	struct wb_msi m;
	unsigned from, i;

	init_emmc(&m, 0x57u, 0x0Fu);
	for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
		from = emmc.n;
		CHECK(wb_msi_emmc_erase_arg(&m, 0x4400u, ok[i]) == 0);
		CHECK(emmc.n - from == 4);
		CHECK(emmc.log[from].idx == 35 &&
		      emmc.log[from].arg == 0x4400u);
		CHECK(emmc.log[from + 1u].idx == 36 &&
		      emmc.log[from + 1u].arg == 0x4400u);
		CHECK(emmc.log[from + 2u].idx == 38 &&
		      emmc.log[from + 2u].arg == ok[i]);
		CHECK(emmc.log[from + 3u].idx == 13);
	}
	from = emmc.n;
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(wb_msi_emmc_erase_arg(&m, 0, bad[i]) == WB_MSI_ERR_ARG);
	CHECK(emmc.n == from);

	/* A refused CMD36 sends no CMD38. */
	emmc.fail_idx = 36;
	CHECK(wb_msi_emmc_erase_arg(&m, 0, WB_MMC_TRIM) == WB_MSI_ERR_EMMC);
	CHECK(emmc.n == from + 2u);
	CHECK(emmc.violations == 0);
}

/* S-13b.055: only the listed EXT_CSD bytes. */
static void test_switch_list(void)
{
	struct wb_msi m;
	unsigned from;

	init_emmc(&m, 0x57u, 0x0Fu);
	from = emmc.n;
	CHECK(wb_msi_emmc_switch(&m, 162, 1) == WB_MSI_ERR_ARG);
	CHECK(wb_msi_emmc_switch(&m, 0, 0) == WB_MSI_ERR_ARG);
	CHECK(emmc.n == from);
	CHECK(wb_msi_emmc_switch(&m, 33, 1) == WB_MSI_OK);
	CHECK(wb_msi_emmc_switch(&m, 34, 1) == WB_MSI_OK);	/* POWERED_ON */
	CHECK(emmc.violations == 0);
}

/* S-03.068, S-13b.057: a flush may keep DAT0 low past the busy bound of every
 * other switch, and no longer than its own. */
static void test_flush_wait(void)
{
	static const struct wb_msi_config flush_cfg = {
		63u, 0xFFFFFF40u, 0x20070008u, 1000000u, 500000u, 100000u, 5000000u,
		0u, 0u,
	};
	struct wb_msi m;

	init_emmc(&m, 0x57u, 0x0Fu);
	fake_core_set_cycle_step(4000);		/* 10 us a read of the counter */
	emmc.busy_set = 120000u;		/* at least 1.2 s */
	CHECK(wb_msi_emmc_switch(&m, 32, 1) == WB_MSI_TIMEOUT);	/* cfg names no flush wait */

	init_emmc(&m, 0x57u, 0x0Fu);
	m.c = &flush_cfg;
	fake_core_set_cycle_step(4000);
	emmc.busy_set = 120000u;
	CHECK(wb_msi_emmc_switch(&m, 32, 1) == WB_MSI_OK);
	CHECK(emmc.busy_reads == 0);
	CHECK(emmc.violations == 0);

	/* A switch of another byte keeps the busy bound. */
	emmc.busy_set = 120000u;
	CHECK(wb_msi_emmc_switch(&m, 33, 1) == WB_MSI_TIMEOUT);

	/* A flush that never ends fails within its bound. */
	init_emmc(&m, 0x57u, 0x0Fu);
	m.c = &flush_cfg;
	fake_core_set_cycle_step(4000);
	emmc.busy_set = 0xFFFFFFFFu;
	CHECK(wb_msi_emmc_switch(&m, 32, 1) == WB_MSI_TIMEOUT);
}

/* S-13b.056: after Switch(34, 3) nothing at all is sent. */
static void test_power_off(void)
{
	static _Alignas(32) uint32_t buf[128];
	struct wb_msi m;
	unsigned from;

	init_emmc(&m, 0x57u, 0x0Fu);
	from = emmc.n;
	emmc.busy_set = 50;
	CHECK(wb_msi_emmc_switch(&m, 34, 3) == WB_MSI_OK);
	CHECK(emmc.n == from + 1u);
	CHECK(emmc.log[from].arg == 0x03220300u);
	CHECK(emmc.busy_reads == 0);	/* DAT0 was watched to its end */
	CHECK(wb_msi_emmc_read(&m, 0, 1, (uint8_t *)buf) == WB_MSI_ERR_OFF);
	CHECK(wb_msi_cmd(&m, 13, 0, WB_MSI_R1, NULL) == WB_MSI_ERR_OFF);
	CHECK(wb_msi_set_clock(&m, 1, false) == WB_MSI_ERR_OFF);
	CHECK(emmc.n == from + 1u);
	CHECK(emmc.violations == 0);
}

/* S-13b.058, S-05.016: the controller left in FIFO mode, the eMMC untouched. */
static void test_leave(void)
{
	static const struct wb_msi_leave v = {
		0x00000010u, 0x00000600u, 0x0000FFC2u, 0x607F0080u,
		0x02000001u,
	};
	struct wb_msi m;
	unsigned from;

	init_emmc(&m, 0x57u, 0x0Fu);
	fake_mmr_set(MSI_DBADDR, 0x8813267Cu);
	fake_mmr_set(MSI_ISTAT, 0x21u);
	fake_mmr_set(MSI_IDSTS, 0x2u);
	fake_mmr_set(MSI_IDINTEN, 0x2u);
	from = emmc.n;
	emmc.busy_set = 3;
	busy();
	CHECK(wb_msi_leave(&m, &v) == WB_MSI_OK);
	CHECK(emmc.busy_reads == 0);		/* DAT0 waited for */
	CHECK(emmc.n == from);			/* no command */
	CHECK(fake_mmr_get(MSI_CTL) == 0x10u);
	CHECK(fake_mmr_get(MSI_BUSMODE) == 0x600u);
	CHECK(fake_mmr_get(MSI_IMSK) == 0xFFC2u);
	CHECK(fake_mmr_get(MSI_FIFOTH) == 0x607F0080u);
	CHECK(fake_mmr_get(MSI_CDTHRCTL) == 0x02000001u);
	CHECK(fake_mmr_get(MSI_DBADDR) == 0);
	CHECK(fake_mmr_get(MSI_ISTAT) == 0);
	CHECK(fake_mmr_get(MSI_IDSTS) == 0);
	CHECK(fake_mmr_get(MSI_IDINTEN) == 0);
	CHECK(fake_mmr_get(MSI_CTYPE) == MSI_CTYPE_WIDBYTE0);
	CHECK(fake_mmr_get(MSI_TMOUT) == cfg.tmout);
	CHECK(fake_mmr_get(MSI_CLKEN) == emmc.clken && emmc.clken != 0);
	CHECK(emmc.violations == 0);

	/* DAT0 held: a timeout, and the controller still left as above. */
	init_emmc(&m, 0x57u, 0x0Fu);
	fake_core_set_cycle_step(400u * 1000u);
	emmc.busy_set = 0xFFFFFFFFu;
	busy();
	CHECK(wb_msi_leave(&m, &v) == WB_MSI_TIMEOUT);
	CHECK(fake_mmr_get(MSI_CTL) == 0x10u);
	CHECK(fake_mmr_get(MSI_BUSMODE) == 0x600u);
	CHECK(fake_mmr_get(MSI_IMSK) == 0xFFC2u);
	fake_core_set_cycle_step(1);
}

/* S-13b.061: the OP-1F's bus, Switch(183, 1) and four lines. */
static void test_bring_up_four(void)
{
	static const struct wb_msi_config cfg4 = {
		63u, 0xFFFFFF40u, 0x20070008u, 1000000u, 500000u, 100000u, 0u, 0u, 4u,
	};
	struct wb_msi m;
	unsigned i, w4 = 0, w8 = 0;

	reset_emmc();
	ext[WB_EXT_CSD_DEVICE_TYPE] = 0x57u;
	ext[WB_EXT_CSD_DRIVER_STR] = 0x0Fu;
	CHECK(wb_msi_init(&m, &cfg4, &cclk400, desc, 2) == WB_MSI_OK);
	CHECK(wb_msi_emmc_init(&m, ext) == WB_MSI_OK);
	CHECK(emmc.violations == 0);	/* every data command at the eMMC's width */
	for (i = 0; i < emmc.n && i < LOG_MAX; i++) {
		w4 += emmc.log[i].idx == 6u && emmc.log[i].arg == 0x03B70100u;
		w8 += emmc.log[i].idx == 6u && emmc.log[i].arg == 0x03B70200u;
		if (emmc.log[i].idx == 8u && i > 0u)
			CHECK(emmc.log[i].width == 4u);	/* the CMD8s after the switch */
	}
	CHECK(w4 == 1u && w8 == 0u);
	CHECK(m.width == 4u && emmc.bus_width == 4u);
	CHECK(fake_mmr_get(MSI_CTYPE) == 1u);
}

/* S-03.013: the notification's busy end by CMD13 alone, bounded. */
static void test_switch_poll(void)
{
	struct wb_msi m;
	unsigned from;

	init_emmc(&m, 0x57u, 0x0Fu);
	from = emmc.n;
	emmc.busy_set = 0;
	emmc.not_ready = 3u;
	CHECK(wb_msi_switch_poll(&m, 34, 3, 100u) == 0);
	CHECK(emmc.log[from].idx == 6u && emmc.log[from].arg == 0x03220300u);
	CHECK(count_idx(from, 13u) == 4u);	/* three busy, then ready */
	CHECK(emmc.n == from + 5u);
	/* nothing after it */
	CHECK(wb_msi_cmd(&m, 13, 0, WB_MSI_R1, NULL) == WB_MSI_ERR_OFF);
	CHECK(emmc.n == from + 5u);

	/* a wait that runs out: 1, and still nothing after */
	init_emmc(&m, 0x57u, 0x0Fu);
	fake_core_set_cycle_step(400u * 1000u);	/* 1 ms a read of the counter */
	from = emmc.n;
	emmc.busy_set = 0;
	emmc.not_ready = 0xFFFFFFFFu;
	CHECK(wb_msi_switch_poll(&m, 34, 3, 20u) == 1);
	CHECK(count_idx(from, 13u) >= 1u && count_idx(from, 13u) < 40u);
	CHECK(wb_msi_switch_poll(&m, 34, 3, 20u) == -1);	/* no command */
	fake_core_set_cycle_step(1);

	/* S-03.013: at least one CMD13, however short the bound */
	init_emmc(&m, 0x57u, 0x0Fu);
	from = emmc.n;
	emmc.busy_set = 0;
	CHECK(wb_msi_switch_poll(&m, 33, 0, 0u) == 0);
	CHECK(count_idx(from, 13u) == 1u);

	/* the flush by the same path; an index the driver does not switch */
	init_emmc(&m, 0x57u, 0x0Fu);
	from = emmc.n;
	emmc.busy_set = 0;
	CHECK(wb_msi_switch_poll(&m, 32, 1, 100u) == 0);
	CHECK(emmc.log[from].arg == 0x03200100u);
	CHECK(wb_msi_switch_poll(&m, 162, 1, 100u) == -1);
	CHECK(emmc.n == from + 2u);
	CHECK(emmc.violations == 0);
}

/* S-03.012: EXT_CSD[179] by one CMD8. */
static void test_part_config(void)
{
	static _Alignas(32) uint8_t buf[512];
	struct wb_msi m;
	unsigned from;
	uint8_t v = 0;

	init_emmc(&m, 0x57u, 0x0Fu);
	memcpy(buf, ext, sizeof buf);
	buf[WB_EXT_CSD_PART_CONFIG] = 0x48u;
	from = emmc.n;
	CHECK(wb_msi_read_part_config(&m, buf, &v) == WB_MSI_OK);
	CHECK(v == 0x48u);
	CHECK(emmc.n == from + 1u && emmc.log[from].idx == 8u);
	CHECK(emmc.violations == 0);
}

/* The first fault of an operation, for the test OS's report. */
static void test_fault_record(void)
{
	static _Alignas(32) uint8_t buf[2u * 512u + 32u];
	struct wb_msi m;

	init_emmc(&m, 0x57u, 0x0Fu);
	wb_msi_fault_clear(&m);
	CHECK(m.fault.step == WB_MSI_STEP_NONE && m.fault.state == 0xFFu);
	/* a buffer off a cache line: refused, nothing sent, its address kept */
	CHECK(wb_msi_emmc_read(&m, 0, 2, buf + 4) == WB_MSI_ERR_ARG);
	CHECK(m.fault.step == WB_MSI_STEP_ARG && m.fault.cmd == 18u && m.fault.at == 18u);
	CHECK(m.fault.arg == (uint32_t)(uintptr_t)(buf + 4) && m.fault.rc == WB_MSI_ERR_ARG);
	/* a later fault does not overwrite the first */
	CHECK(wb_msi_emmc_read(&m, 0, 1, buf + 8) == WB_MSI_ERR_ARG);
	CHECK(m.fault.cmd == 18u);

	/* the select's CMD6 not answered: the controller's status kept */
	wb_msi_fault_clear(&m);
	emmc.rto_idx = 6;
	CHECK(wb_msi_emmc_select(&m, 1) == WB_MSI_ERR_CMD);
	emmc.rto_idx = 0;
	emmc.busy_reads = 0;	/* the fake's busy after a CMD6 the eMMC did not take */
	CHECK(m.fault.step == WB_MSI_STEP_CMD && m.fault.cmd == 6u && m.fault.at == 6u);
	CHECK(m.fault.arg == 0x03B30100u && (m.fault.istat & MSI_IS_RTO) != 0u);

	/* R1 error bits in the response */
	wb_msi_fault_clear(&m);
	emmc.fail_idx = 6;
	CHECK(wb_msi_emmc_select(&m, 2) == WB_MSI_ERR_EMMC);
	emmc.fail_idx = 0;
	emmc.busy_reads = 0;
	CHECK(m.fault.step == WB_MSI_STEP_R1 && (m.fault.r1 & (1u << 31)) != 0u);

	/* never ready in tran: the state CMD13 showed, and the wait's length */
	wb_msi_fault_clear(&m);
	fake_core_set_cycle_step(400u * 1000u);	/* 1 ms a read of the counter */
	emmc.not_ready = 0xFFFFFFFFu;
	CHECK(wb_msi_emmc_select(&m, 1) == WB_MSI_TIMEOUT);
	emmc.not_ready = 0;
	fake_core_set_cycle_step(1);
	CHECK(m.fault.step == WB_MSI_STEP_STATE && m.fault.cmd == 6u && m.fault.at == 13u);
	CHECK(m.fault.state == 7u && m.fault.busy_us >= cfg.busy_timeout_us);

	/* the read-ahead's out-of-range, which a read ignores, is no fault */
	wb_msi_fault_clear(&m);
	emmc.fail_idx = 12;
	CHECK(wb_msi_emmc_read(&m, 0x3F80u, 2, buf) == WB_MSI_OK);
	emmc.fail_idx = 0;
	CHECK(m.fault.step == WB_MSI_STEP_NONE);
	CHECK(emmc.violations == 0);
}

/* S-05.037, S-13b.062: closed and idle, the eMMC sent nothing. */
static void test_close(void)
{
	struct wb_msi m;
	unsigned from;

	init_emmc(&m, 0x57u, 0x0Fu);
	fake_mmr_set(MSI_CTL, MSI_CTL_INTEN | MSI_CTL_INTDMAC);
	fake_mmr_set(MSI_IMSK, 0xFFC2u);
	fake_mmr_set(MSI_ISTAT, 0x21u);
	fake_mmr_set(MSI_IDINTEN, 0x2u);
	fake_mmr_set(MSI_IDSTS, 0x2u);
	from = emmc.n;
	busy();
	CHECK(wb_msi_close(&m) == WB_MSI_OK);
	CHECK(emmc.busy_reads == 0);		/* DAT0 waited for */
	CHECK(emmc.n == from);			/* no command */
	CHECK((fake_mmr_get(MSI_CTL) & (MSI_CTL_INTEN | MSI_CTL_INTDMAC)) == 0u);
	CHECK(fake_mmr_get(MSI_IMSK) == 0u);
	CHECK(fake_mmr_get(MSI_ISTAT) == 0u);
	CHECK((fake_mmr_get(MSI_BUSMODE) & MSI_BUSMODE_DE) == 0u);
	CHECK(fake_mmr_get(MSI_IDINTEN) == 0u && fake_mmr_get(MSI_IDSTS) == 0u);
	CHECK((fake_mmr_get(MSI_CMD) & MSI_CMD_STARTCMD) == 0u);
	CHECK(emmc.violations == 0);
}

int main(void)
{
	RUN(test_bring_up_hs200);
	RUN(test_bring_up_timing_rows);
	RUN(test_csd);
	RUN(test_cmd1_bounded);
	RUN(test_transfers);
	RUN(test_write_reliable);
	RUN(test_data_timeout);
	RUN(test_data_timeout_saturates);
	RUN(test_failed_multi_stops);
	RUN(test_read_ahead_out_of_range);
	RUN(test_chain);
	RUN(test_erase_arg);
	RUN(test_switch_list);
	RUN(test_flush_wait);
	RUN(test_power_off);
	RUN(test_leave);
	RUN(test_bring_up_four);
	RUN(test_switch_poll);
	RUN(test_part_config);
	RUN(test_fault_record);
	RUN(test_close);
	return check_summary("test_drv_msi");
}
