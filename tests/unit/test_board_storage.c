/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The slave's storage at start-up: opxy_slave_storage() over the MSI
 * driver and an eMMC model.
 *
 * After a failed eMMC bring-up (S-13b.057, S-13b.059, scenario
 * SC-13b-EMMC-STUCK), the eMMC stuck in one of four ways or slow at every
 * wait: each bring-up ends in bounded time, is repeated at most
 * OPXY_EMMC_TRIES times, all within S-09.018's 5 s, and then no eMMC
 * command follows: the load fails, the master's eMMC requests get an
 * error, nothing is taken from EXT_CSD.
 *
 * With a good eMMC (S-09.018, S-START mount_on_request): no read of the
 * user area before the master's 0x08, the init answered, and a file
 * command before the 0x08 fails with ENODEV. S-03.009's switches come
 * with the first request that uses the eMMC, bounded, and only once.
 *
 * SC-03-SELECT-GUARD (S-03.006, S-03.007, S-03.063): the volume only while
 * the user area is selected; nothing after a failed select until one works.
 *
 * SC-03-EMMC-OTHER (S-03.001, S-03.002): SEC_COUNT and EXT_CSD[265] from
 * EXT_CSD, the erase group from the CSD (EXT_CSD[224] not used); another
 * eMMC gets no mount, no user-area read or write and no boot-partition
 * write, and the slave serves on.
 *
 * SC-03-SHUTDOWN-UNMOUNT on the slave (S-03.011, S-13b.056): the 0x09 of
 * a mounted volume runs the eMMC's shutdown before it answers, a failed
 * step stopping none of the later ones, and nothing follows the power-off
 * notification until a request needs the eMMC, which first brings it up
 * from CMD0 and makes S-03.009's switches again. A 0x09 with no volume
 * mounted sends the eMMC nothing.
 *
 * The yaffs2 volume's device (S-12.042, S-12.060): chunks written with
 * CMD23 (bit 31 and the count) then CMD25, no CMD24, while the bridge's
 * plain multi-sector write stays CMD25 alone; a block erased with CMD35,
 * CMD36 and CMD38, bounded retries; neither while a boot partition is
 * selected. test_board_yaffs_emmc runs yaffs2 on that device.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/opxy_boot.h"
#include "waffle/drivers/bf70x_msi.h"
#include "waffle/s09_bridge.h"
#include "waffle/s09_slave.h"

/* MSI source SCLK0 / 2, as the driver's tests. */
#define SRC_HZ 50000000u

enum stuck {
	GOOD,		/* answers every command at once */
	OCR_NEVER,	/* CMD1 never reports power-up done */
	BUSY_AFTER_CMD6,	/* DAT0 never released after a CMD6 */
	NO_RESPONSE,	/* every command times out */
	CMD8_NO_DTO,	/* CMD8 answered, its data transfer never ends */
	SLOW,		/* each wait ends just inside its bound, the last
			 * CMD8's transfer never; with cmd2_first, the first
			 * try gets no answer to CMD2 */
	FEATURE_BUSY,	/* up, then DAT0 never released after a later CMD6 */
};

#define LOG_N 256u

static struct emmc {
	enum stuck stuck;
	unsigned count[64];	/* commands taken, by index */
	unsigned n;
	bool other;		/* a command outside the bring-up's */
	unsigned busy_reads;
	bool busy_forever;
	unsigned fail_select;	/* bit p: a CMD6 select of partition p fails */
	bool cmd2_first;	/* SLOW: no answer to the first try's CMD2 */
	uint64_t busy_until, ready_at, ocr_at, dto_at;	/* SLOW, in us */
	unsigned cmd8s;		/* CMD8s of this try */
	unsigned cmd6_at_read;	/* CMD6s before the first CMD17 or CMD18 */
	bool read;
	unsigned sw[256];	/* CMD6s, by EXT_CSD index */
	uint32_t clkdiv, clken;
	bool fail_erase;	/* CMD38 answers with an R1 error */
	uint8_t fail_switch;	/* a CMD6 to this EXT_CSD index (not 0) answers with one */
	uint8_t ext_csd[512];	/* what CMD8 returns */
	uint32_t csd[4];	/* what CMD9 returns, as MSI_RESP0-3 */
	uint8_t *ext_csd_to;	/* where the driver takes it (s.ext_csd) */
	unsigned log_n;		/* commands since log_start(), the first LOG_N kept */
	uint8_t log_idx[LOG_N];
	uint32_t log_arg[LOG_N];
} emmc;

/* 1 ms of the fake clock on every cycle-counter read. */
#define CCLK_MHZ 400u
#define STEP (CCLK_MHZ * 1000u)

/* SLOW: each wait this much inside opxy_msi_bringup's bound. */
#define SLOW_BUSY_US 240000u
#define SLOW_OCR_US 1000000u
#define SLOW_DTO_US 90000u

static uint64_t now_us(void)
{
	return fake_core_peek_cycles() / CCLK_MHZ;
}

/* SLOW: DAT0 busy, then CMD13 not ready, each for SLOW_BUSY_US. */
static void slow_busy(void)
{
	emmc.busy_until = now_us() + SLOW_BUSY_US;
	emmc.ready_at = emmc.busy_until + SLOW_BUSY_US;
}

static void slow_command(uint32_t idx, uint32_t *r1, uint32_t *is)
{
	switch (idx) {
	case 0:
		emmc.ocr_at = now_us() + SLOW_OCR_US;
		emmc.cmd8s = 0;
		break;
	case 1:
		*r1 = now_us() >= emmc.ocr_at ? 0xC0FF8080u : 0x00FF8080u;
		break;
	case 2:
		if (emmc.cmd2_first && emmc.count[0] == 1u)
			*is = MSI_IS_CMDDONE | MSI_IS_RTO;
		break;
	case 7:
		emmc.busy_until = now_us() + SLOW_BUSY_US;
		break;
	case 6:
		slow_busy();
		break;
	case 13:
		if (now_us() < emmc.ready_at) {
			*r1 = 7u << 9;		/* programming, not ready */
		} else if (emmc.ready_at != 0u && emmc.count[6] % 2u == 0u) {
			/* after Switch(185): DAT0 busy at the clock raise */
			emmc.ready_at = 0;
			emmc.busy_until = now_us() + SLOW_BUSY_US;
		}
		break;
	case 8:
		*is &= ~MSI_IS_DTO;
		if (++emmc.cmd8s == 1u)
			emmc.dto_at = now_us() + SLOW_DTO_US;
		break;
	default:
		break;
	}
}

static void take_command(uint32_t cmd)
{
	uint32_t idx = cmd & 0x3Fu;
	uint32_t r1 = WB_MMC_R1_READY | WB_MMC_STATE_TRAN << 9;
	uint32_t is = MSI_IS_CMDDONE;

	const uint32_t arg = fake_mmr_get(MSI_CMDARG);

	emmc.count[idx]++;
	emmc.n++;
	if (emmc.log_n < LOG_N) {
		emmc.log_idx[emmc.log_n] = (uint8_t)idx;
		emmc.log_arg[emmc.log_n] = arg;
	}
	emmc.log_n++;
	if (idx == 38u && emmc.fail_erase)
		r1 |= 1u << 31;
	if (idx == 8u && emmc.ext_csd_to != NULL)
		memcpy(emmc.ext_csd_to, emmc.ext_csd, sizeof(emmc.ext_csd));
	if (idx == 6u) {
		emmc.sw[(arg >> 16) & 0xFFu]++;
		if ((arg >> 16 & 0xFFu) == 179u && (arg >> 8 & 0xFFu) < 8u &&
		    (emmc.fail_select >> (arg >> 8 & 0xFFu) & 1u))
			r1 |= 1u << 31;		/* an R1 error: the select failed */
		if (emmc.fail_switch != 0u &&
		    (arg >> 16 & 0xFFu) == emmc.fail_switch)
			r1 |= 1u << 31;
	}
	if ((idx == 17u || idx == 18u) && !emmc.read) {
		emmc.read = true;
		emmc.cmd6_at_read = emmc.count[6];
	}
	/* S-13b.051's commands, CMD13 and CMD9 */
	if (idx > 13u || !((1u << idx) & 0x23CFu))
		emmc.other = true;
	if ((cmd & MSI_CMD_DXPECT) &&
	    !(emmc.stuck == CMD8_NO_DTO && idx == 8u))
		is |= MSI_IS_DTO;
	switch (idx) {
	case 1:
		r1 = emmc.stuck == OCR_NEVER ? 0x00FF8080u : 0xC0FF8080u;
		break;
	case 7:
		emmc.busy_reads = 3;
		break;
	case 6:
		emmc.busy_reads = 3;
		emmc.busy_forever = emmc.stuck == BUSY_AFTER_CMD6 ||
				    (emmc.stuck == FEATURE_BUSY &&
				     emmc.count[6] > 2u);
		break;
	default:
		break;
	}
	if (emmc.stuck == SLOW)
		slow_command(idx, &r1, &is);
	if (emmc.stuck == NO_RESPONSE)
		is = MSI_IS_CMDDONE | MSI_IS_RTO;
	if (idx == 9u) {
		r1 = emmc.csd[0];
		fake_mmr_set(MSI_RESP1, emmc.csd[1]);
		fake_mmr_set(MSI_RESP2, emmc.csd[2]);
		fake_mmr_set(MSI_RESP3, emmc.csd[3]);
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
		if (addr == MSI_STAT) {
			*value = emmc.busy_reads || emmc.busy_forever ||
				 now_us() < emmc.busy_until ?
					 MSI_STAT_DBUSY : 0;
			if (emmc.busy_reads)
				emmc.busy_reads--;
		}
		if (addr == MSI_ISTAT && emmc.dto_at != 0u &&
		    now_us() >= emmc.dto_at) {
			emmc.dto_at = 0;
			fake_mmr_set(MSI_ISTAT,
				     fake_mmr_get(MSI_ISTAT) | MSI_IS_DTO);
			*value |= MSI_IS_DTO;
		}
		return true;
	}
	if (op != FAKE_MMR_WRITE)
		return true;
	switch (addr) {
	case MSI_CTL:
		/* SLOW: a try starts with the eMMC holding DAT0 */
		if (emmc.stuck == SLOW && (*value & MSI_CTL_RESETS))
			emmc.busy_until = now_us() + SLOW_BUSY_US;
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
		if (!(*value & MSI_CMD_STARTCMD))
			return true;
		if (*value & MSI_CMD_UCLKREGS) {
			emmc.clkdiv = fake_mmr_get(MSI_CLKDIV);
			emmc.clken = fake_mmr_get(MSI_CLKEN);
		} else {
			take_command(*value);
		}
		*value &= ~MSI_CMD_STARTCMD;
		return true;
	default:
		return true;
	}
}

static struct opxy_slave s;

/*
 * S-03.001's geometry: SEC_COUNT (EXT_CSD 212-215), OPTIMAL_WRITE_SIZE
 * (EXT_CSD 265, a number) and the CSD's ERASE_GRP_SIZE (bits 46:42) and
 * ERASE_GRP_MULT (41:37), the CSD's other bits ones. EXT_CSD[224] is set
 * to 2, an erase group of 0x800 sectors were it in force.
 */
static void geometry(uint32_t sectors, uint8_t grp_size, uint8_t grp_mult,
		     uint8_t opt_write)
{
	emmc.ext_csd[212] = (uint8_t)sectors;
	emmc.ext_csd[213] = (uint8_t)(sectors >> 8);
	emmc.ext_csd[214] = (uint8_t)(sectors >> 16);
	emmc.ext_csd[215] = (uint8_t)(sectors >> 24);
	emmc.ext_csd[224] = 2u;
	emmc.ext_csd[265] = opt_write;
	emmc.ext_csd[181] = 0u;		/* ERASED_MEM_CONT: erases to zero */
	emmc.csd[3] = 0xFFFFFFFFu;
	emmc.csd[2] = 0xFFFFFFFFu;
	emmc.csd[1] = 0xFFFF801Fu | (uint32_t)grp_size << 10 |
		      (uint32_t)grp_mult << 5;
	emmc.csd[0] = 0xFFFFFFFFu;
}

static void reset(enum stuck stuck)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(STEP);
	memset(&emmc, 0, sizeof(emmc));
	emmc.stuck = stuck;
	fake_mmr_set_hook(emmc_hook, NULL);
	memset(&s, 0, sizeof(s));
	s.hw.time.cycles_per_us = CCLK_MHZ;
	memset(s.ext_csd, 0xA5, sizeof(s.ext_csd));	/* not from the eMMC */
	/* the eMMC's own: the same filler, with S-03.001's geometry */
	memset(emmc.ext_csd, 0xA5, sizeof(emmc.ext_csd));
	geometry(S03_USER_SECTORS, 31u, 31u, 1u);
	emmc.ext_csd_to = s.ext_csd;
}

static unsigned count(uint8_t idx)
{
	return emmc.count[idx];
}

static void log_start(void)
{
	emmc.log_n = 0;
}

/* The commands logged since log_start() with index idx. */
static unsigned logged(uint8_t idx)
{
	unsigned i, n = 0;

	for (i = 0; i < emmc.log_n && i < LOG_N; i++)
		if (emmc.log_idx[i] == idx)
			n++;
	return n;
}

/* S-12.060 in the log: each CMD25 right after a CMD23 whose argument has
 * bit 31 and a count of whole chunks, at a chunk's first sector; no
 * CMD24. The number of CMD25s. */
static unsigned reliable_writes(void)
{
	unsigned i, n = 0;

	CHECK(emmc.log_n <= LOG_N);
	CHECK(logged(24) == 0u);
	for (i = 0; i < emmc.log_n && i < LOG_N; i++) {
		if (emmc.log_idx[i] != 25u)
			continue;
		n++;
		CHECK(i > 0u && emmc.log_idx[i - 1u] == 23u);
		if (i > 0u)
			CHECK((emmc.log_arg[i - 1u] & WB_MMC_CMD23_RELIABLE) != 0u &&
			      (emmc.log_arg[i - 1u] & 0xFFFFu) % 8u == 0u &&
			      (emmc.log_arg[i - 1u] & 0xFFFFu) != 0u);
		CHECK(emmc.log_arg[i] % 8u == 0u);
	}
	return n;
}

/* S-03.009's switches: POWER_OFF_NOTIFICATION, CACHE_CTRL, BKOPS_EN. */
static unsigned features(void)
{
	return emmc.sw[34] + emmc.sw[33] + emmc.sw[163];
}

static void stuck_run(enum stuck stuck, bool cmd2_first, uint64_t min_us)
{
	static _Alignas(32) uint8_t data[2 * S03_SECTOR];
	static uint8_t out[300];
	const struct s09_req mount = { S09_MOUNT, 0, 0, 0, 0 };
	uint64_t t0, us;
	unsigned n;
	uint32_t err = 0;

	reset(stuck);
	emmc.cmd2_first = cmd2_first;
	t0 = fake_core_peek_cycles();
	opxy_slave_storage(&s);
	us = (fake_core_peek_cycles() - t0) / CCLK_MHZ;

	CHECK(!s.emmc_up);

	/* S-13b.057: bounded, the CMD1 loop no sooner than its 1 s;
	 * S-09.018: ready for the init within 5 s of the stream, the slave's
	 * own start-up included. */
	CHECK(us >= min_us && us <= 4500000u);
	/* the whole bring-up at most OPXY_EMMC_TRIES times */
	CHECK(count(0) >= 1u && count(0) <= OPXY_EMMC_TRIES);
	CHECK(!emmc.other);		/* no read, write or erase */
	/* S-13b.059: nothing of the partly received EXT_CSD */
	CHECK(s.store.boot_size(s.store.ctx) == 0u);
	CHECK(s.store.erase_group(s.store.ctx) == 0u);
	CHECK(s.emmc.boot_size == 0u);

	/* No eMMC command from here on. */
	n = emmc.n;
	CHECK(s.store.os_status(s.store.ctx, 0u, 0u) == 1u);	/* S-03.046 */
	CHECK(s.staged == S05_FAIL_SELECT);
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 1u, 0u, NULL, 0u) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_READ_SECTOR, 0u, 0u, data,
			   S03_SECTOR) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_WRITE_SECTOR, 0u, 0u, data,
			   S03_SECTOR) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_ERASE, 0u, WB_MMC_ERASE, NULL,
			   0u) != 0);
	CHECK(s.store.file(s.store.ctx, &mount, "/yaffs2", "", NULL, 0u, out,
			   &err) < 0);
	CHECK(s.emmc_ops.read(s.emmc_ops.ctx, 0u, 1u, data) ==
	      OPXY_EMMC_DOWN);
	CHECK(emmc.n == n);
}

/* SC-13b-EMMC-STUCK runs 1-4. */
static void test_ocr_never(void)
{
	stuck_run(OCR_NEVER, false, 2u * 1000000u);	/* two tries of >= 1 s */
	CHECK(count(0) == OPXY_EMMC_TRIES);
	CHECK(count(2) == 0u);			/* no CMD2 */
}

/* The eMMC stays busy: the second try ends at its clock set-up, which
 * waits for DAT0, before any command reaches the eMMC. Neither waits
 * longer than the bring-up's bound. */
static void test_busy_after_switch(void)
{
	stuck_run(BUSY_AFTER_CMD6, false, 0u);
	CHECK(count(0) == 1u && count(6) == 1u && count(8) == 0u);
	CHECK(fake_core_peek_cycles() / CCLK_MHZ < 1000000u);
}

static void test_no_response(void)
{
	stuck_run(NO_RESPONSE, false, 0u);
	CHECK(count(0) == OPXY_EMMC_TRIES && emmc.n == OPXY_EMMC_TRIES);
}

static void test_cmd8_no_dto(void)
{
	stuck_run(CMD8_NO_DTO, false, 0u);
	CHECK(count(0) == OPXY_EMMC_TRIES && count(8) == OPXY_EMMC_TRIES);
	/* S-13b.059: no HS_TIMING switch after the failed CMD8 */
	CHECK(count(6) == OPXY_EMMC_TRIES);
}

/* S-09.018: the longest the bring-up takes. The first try fails late
 * enough to need its 1 s CMD1 loop and still starts the repeat; the
 * repeat waits just inside every bound, then fails at the last CMD8. */
static void test_slow_emmc(void)
{
	stuck_run(SLOW, true, 3500000u);
	CHECK(count(0) == 2u && count(2) == 2u);
	CHECK(count(6) == 2u && count(8) == 2u);	/* both switches */
}

/* A try that fails after OPXY_EMMC_REPEAT_US is not repeated. */
static void test_no_late_repeat(void)
{
	stuck_run(SLOW, false, 2500000u);
	CHECK(count(0) == 1u && count(8) == 2u);
}

/* A file request; its result. */
static int32_t file_req(uint8_t cmd, const char *path, uint32_t *err)
{
	const struct s09_req r = { cmd, 0, 0, 0, 0 };
	static uint8_t out[300];

	*err = 0xFFFFu;
	return s.store.file(s.store.ctx, &r, path, "", NULL, 0u, out, err);
}

/* The number of reads, writes and erases the eMMC has seen. */
static unsigned data_cmds(void)
{
	return count(17) + count(18) + count(24) + count(25) + count(35) +
	       count(36) + count(38);
}

static void select_guard(void)
{
	static _Alignas(32) uint8_t data[S03_SECTOR];
	uint32_t err;
	unsigned n;

	CHECK(file_req(S09_STAT, "/yaffs2", &err) == 0);

	/* BP1 selected: the volume is not used, and the eMMC sees nothing. */
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 1u, 0u, NULL, 0u) == 0);
	n = emmc.n;
	CHECK(file_req(S09_STAT, "/yaffs2", &err) == -1 && err == S09_ENODEV);
	CHECK(file_req(S09_OPENDIR, "/yaffs2", &err) == 0 && err == S09_ENODEV);
	CHECK(file_req(S09_UNMOUNT, "", &err) == -1 && err == S09_ENODEV);
	CHECK(file_req(S09_MOUNT, "", &err) == -1 && err == S09_ENODEV);
	CHECK(s.store.read(s.store.ctx, 3u, data, 16u, &err) == -1 &&
	      err == S09_ENODEV);
	CHECK(emmc.n == n);
	/* The boot partition's own sectors still go through. */
	CHECK(s.store.emmc(s.store.ctx, S09_READ_SECTOR, 0u, 0u, data,
			   S03_SECTOR) == 0);

	/* Back on the user area: the volume again. */
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 0u, 0u, NULL, 0u) == 0);
	CHECK(file_req(S09_STAT, "/yaffs2", &err) == 0 && err == 0);

	/* S-03.006: a select of 3 or more sends no command and fails. */
	n = emmc.n;
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 3u, 0u, NULL, 0u) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 0x41u, 0u, NULL, 0u) != 0);
	CHECK(emmc.n == n);
	CHECK(file_req(S09_STAT, "/yaffs2", &err) == 0);

	/* S-03.063: a failed select; nothing reaches the eMMC but a select. */
	emmc.fail_select = 1u;
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 0u, 0u, NULL, 0u) != 0);
	n = data_cmds();
	CHECK(s.store.emmc(s.store.ctx, S09_READ_SECTOR, 0u, 0u, data,
			   S03_SECTOR) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_WRITE_SECTOR, 0u, 0u, data,
			   S03_SECTOR) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_ERASE, 0u, WB_MMC_ERASE, NULL,
			   0u) != 0);
	CHECK(file_req(S09_STAT, "/yaffs2", &err) == -1 && err == S09_ENODEV);
	CHECK(data_cmds() == n);
	emmc.fail_select = 0;
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 0u, 0u, NULL, 0u) == 0);
	CHECK(file_req(S09_STAT, "/yaffs2", &err) == 0 && err == 0);

	/* The slave's own load: BP1's select fails, and its select 0 after it
	 * (S-03.046); no image is read. */
	emmc.fail_select = 3u;
	CHECK(s.store.os_status(s.store.ctx, 0u, 0u) == 1u);
	n = data_cmds();
	CHECK(file_req(S09_STAT, "/yaffs2", &err) == -1 && err == S09_ENODEV);
	CHECK(s.store.emmc(s.store.ctx, S09_READ_SECTOR, 0u, 0u, data,
			   S03_SECTOR) != 0);
	CHECK(data_cmds() == n);
	emmc.fail_select = 0;
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 0u, 0u, NULL, 0u) == 0);
	CHECK(file_req(S09_STAT, "/yaffs2", &err) == 0 && err == 0);
}

/* S-09.018, S-09.041: the init is answered before any read of the user
 * area; file commands fail with ENODEV until the 0x08, which mounts. */
static void test_mount_on_request(void)
{
	const struct s09_req init = { S09_INIT, 0, 0, 0, 0 };
	const struct s09_req stat = { S09_STAT, 0, 0, 0, 0 };
	const struct s09_req mount = { S09_MOUNT, 0, 0, 0, 0 };
	static uint8_t out[300];
	uint8_t hdr[S09_HDR];
	const uint8_t *d;
	size_t n;
	uint32_t err = 0;

	reset(GOOD);
	opxy_slave_storage(&s);
	CHECK(s.emmc_up);
	/* S-03.001: the geometry the eMMC's EXT_CSD and CSD give, accepted */
	CHECK(!s.emmc_other);
	CHECK(count(9) == 1u);
	CHECK(s.ext_csd[212] == (uint8_t)S03_USER_SECTORS && s.ext_csd[265] == 1u);
	CHECK(s.store.erase_group(s.store.ctx) == S03_GROUP_SECTORS);
	CHECK(s.hw.msi.c == &opxy_msi);		/* the runtime waits */
	CHECK(count(17) == 0u && count(18) == 0u);
	CHECK(count(6) == 2u && features() == 0u);	/* no S-03.009 yet */

	s09_req_pack(&init, hdr);
	s09_slave_request(&s.server, hdr, sizeof(hdr));
	CHECK(s09_slave_reply(&s.server, &d, &n) && n == 8u);
	CHECK(n == 8u && ((uint32_t)d[4] | (uint32_t)d[5] << 8 |
	      (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24) == S09_INIT_VALUE);
	CHECK(count(17) == 0u && count(18) == 0u);

	CHECK(s.store.file(s.store.ctx, &stat, "/yaffs2/logs", "", NULL, 0u,
			   out, &err) == -1 && err == S09_ENODEV);
	CHECK(count(17) == 0u && count(18) == 0u);

	/* The 0x08 reads the volume and mounts it, the checkpoint tried first
	 * (S-12.061): this volume has none; a repeat answers the same. */
	CHECK(S09_MOUNT_WAY(s.store.file(s.store.ctx, &mount, "", "", NULL, 0u, out,
					 &err)) == 2u && err == 0);
	CHECK(count(17) + count(18) > 0u);
	CHECK(emmc.cmd6_at_read == 5u && features() == 3u);	/* S-03.009 first */
	CHECK(s.hw.msi.c == &opxy_msi);
	CHECK(count(24) == 0u && count(25) == 0u && count(38) == 0u);
	n = emmc.n;
	CHECK(S09_MOUNT_WAY(s.store.file(s.store.ctx, &mount, "", "", NULL, 0u, out,
					 &err)) == 2u && err == 0);
	CHECK(emmc.n == n);
	CHECK(s.store.file(s.store.ctx, &stat, "/yaffs2", "", NULL, 0u, out,
			   &err) == 0 && err == 0);
	/* SC-03-SELECT-GUARD on the volume just mounted: the slave's one
	 * volume is mounted once in a process. */
	select_guard();
}

/* A CMD6 in write-byte mode to EXT_CSD[index] with value. */
static uint32_t switch_arg(uint8_t index, uint8_t value)
{
	return 3u << 24 | (uint32_t)index << 16 | (uint32_t)value << 8;
}

/* The place in the log of the CMD6 with arg from `from` on; LOG_N if none. */
static unsigned find_switch(uint32_t arg, unsigned from)
{
	unsigned i;

	for (i = from; i < emmc.log_n && i < LOG_N; i++)
		if (emmc.log_idx[i] == 6u && emmc.log_arg[i] == arg)
			return i;
	return LOG_N;
}

/* The CMD6s logged with arg. */
static unsigned switches_of(uint32_t arg)
{
	unsigned i, n = 0;

	for (i = 0; i < emmc.log_n && i < LOG_N; i++)
		if (emmc.log_idx[i] == 6u && emmc.log_arg[i] == arg)
			n++;
	return n;
}

/*
 * The 0x09 of the mounted volume, with the CMD6 to EXT_CSD[failing] (0:
 * none) answering an error on every try: S-03.011's five steps in order
 * before the answer (1, errno 0), each failing one tried S03_RETRIES
 * times, Switch(34, 3) once, and no command after it (S-13b.056). Between
 * the cache's switch-off and BKOPS_EN only the unmount's writes.
 */
static void shutdown_run(uint8_t failing)
{
	const uint32_t flush = switch_arg(32, 1), cache = switch_arg(33, 0);
	const uint32_t bkops = switch_arg(163, 0), notify = switch_arg(34, 3);
	unsigned a, b, c, d, i;
	uint32_t err;

	emmc.fail_switch = failing;
	log_start();
	CHECK(file_req(S09_UNMOUNT, "", &err) == 1 && err == 0u);
	emmc.fail_switch = 0;
	CHECK(s.emmc_notified);
	CHECK(emmc.log_n > 0u && emmc.log_n <= LOG_N);
	a = find_switch(flush, 0);
	b = find_switch(cache, a);
	c = find_switch(bkops, b);
	d = find_switch(notify, c);
	CHECK(a < b && b < c && c < d && d < LOG_N);
	CHECK(switches_of(flush) == (failing == 32u ? S03_RETRIES : 1u));
	CHECK(switches_of(cache) == (failing == 33u ? S03_RETRIES : 1u));
	CHECK(switches_of(bkops) == (failing == 163u ? S03_RETRIES : 1u));
	CHECK(switches_of(notify) == 1u);
	CHECK(logged(6) == switches_of(flush) + switches_of(cache) +
			   switches_of(bkops) + 1u);
	CHECK(d == emmc.log_n - 1u);	/* the last command of all */
	for (i = b; i < c; i++)
		CHECK(emmc.log_idx[i] == 6u || emmc.log_idx[i] == 13u ||
		      emmc.log_idx[i] == 23u || emmc.log_idx[i] == 25u ||
		      emmc.log_idx[i] == 12u);
	CHECK(logged(0) == 0u && logged(17) == 0u && logged(18) == 0u);
}

static unsigned first_logged(uint8_t idx);

/* After the notification: nothing reaches the eMMC for what needs no
 * command; what needs one first brings the eMMC up again (S-13b.051 from
 * CMD0, CMD9 between CMD3 and CMD7, EXT_CSD read), then makes S-03.009's
 * switches, before its own. */
static void woken(unsigned first_own)
{
	CHECK(first_own < LOG_N && first_own < emmc.log_n);
	CHECK(emmc.log_idx[0] == 0u && logged(8) >= 1u);
	CHECK(logged(9) == 1u && first_logged(3) < first_logged(9) &&
	      first_logged(9) < first_logged(7));
	CHECK(find_switch(switch_arg(34, 1), 0) < first_own &&
	      find_switch(switch_arg(33, 1), 0) < first_own &&
	      find_switch(switch_arg(163, 2), 0) < first_own);
	CHECK(!s.emmc_notified && s.emmc_up && !s.emmc_other && s.features);
}

/* The first logged command with index idx; LOG_N if none. */
static unsigned first_logged(uint8_t idx)
{
	unsigned i;

	for (i = 0; i < emmc.log_n && i < LOG_N; i++)
		if (emmc.log_idx[i] == idx)
			return i;
	return LOG_N;
}

/* SC-03-SHUTDOWN-UNMOUNT's slave half, on the volume test_mount_on_request
 * mounted. */
static void test_shutdown_unmount(void)
{
	static const uint8_t failing[] = { 0u, 32u, 33u, 163u, 34u };
	uint32_t err;
	unsigned i;

	CHECK(file_req(S09_STAT, "/yaffs2", &err) == 0);
	for (i = 0; i < sizeof(failing); i++) {
		unsigned n;

		shutdown_run(failing[i]);

		/* the volume is gone; nothing reaches the eMMC */
		n = emmc.n;
		CHECK(file_req(S09_STAT, "/yaffs2", &err) == -1 && err == S09_ENODEV);
		CHECK(file_req(S09_UNMOUNT, "", &err) == 1 && err == 0u);
		CHECK(s.store.boot_size(s.store.ctx) != 0u);
		CHECK(emmc.n == n);

		/* the mount brings the eMMC up again and mounts */
		log_start();
		CHECK(file_req(S09_MOUNT, "", &err) >= 0 && err == 0u);
		woken(first_logged(17) < first_logged(18) ? first_logged(17) :
							    first_logged(18));
		CHECK(file_req(S09_STAT, "/yaffs2", &err) == 0);
	}

	/* An eMMC request after the notification: the bring-up, then the
	 * select itself. */
	shutdown_run(0u);
	log_start();
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 0u, 0u, NULL, 0u) == 0);
	woken(find_switch(switch_arg(179, 0), 0));
	CHECK(find_switch(switch_arg(179, 0), 0) < LOG_N);
	/* mounted again, as test_mount_on_request left it */
	CHECK(file_req(S09_MOUNT, "", &err) >= 0 && s.features);
}

/* S-03.009 with the first eMMC request; not again for the load. */
static void test_features_once(void)
{
	static _Alignas(32) uint8_t data[S03_SECTOR];
	uint32_t err;
	unsigned n;

	reset(GOOD);
	opxy_slave_storage(&s);
	/* SC-03-SHUTDOWN-UNMOUNT's not_mounted: a 0x09 with no volume
	 * mounted answers and sends the eMMC nothing. */
	n = emmc.n;
	CHECK(file_req(S09_UNMOUNT, "", &err) == 1 && err == 0u);
	CHECK(!s.emmc_notified && emmc.n == n);
	CHECK(features() == 0u);
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 0u, 0u, NULL, 0u) == 0);
	CHECK(emmc.sw[34] == 1u && emmc.sw[33] == 1u && emmc.sw[163] == 1u);
	CHECK(s.store.emmc(s.store.ctx, S09_READ_SECTOR, 0u, 0u, data,
			   S03_SECTOR) == 0);
	(void)s.store.os_status(s.store.ctx, 0u, 0u);
	CHECK(features() == 3u);
}

/* S-09.082, S-05.035: a file start's release with no volume mounted is
 * "no OS", with no boot partition selected and no sector read; one with
 * the arguments 0 is the boot partition's load. */
static void test_file_release(void)
{
	unsigned n;

	reset(GOOD);
	opxy_slave_storage(&s);
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 0u, 0u, NULL, 0u) == 0);
	n = data_cmds();
	emmc.fail_select = 6u;		/* a boot partition's select would fail */
	CHECK(s.store.os_status(s.store.ctx, 742764u, 0x1234ABCDu) == 1u);
	CHECK(s.staged == S05_FAIL_NO_OS && s.staged_len == 0u);
	CHECK(data_cmds() == n && s.part == S03_PART_USER);
	CHECK(s.store.os_status(s.store.ctx, 0u, 0u) == 1u);	/* S-03.046 */
	CHECK(s.staged == S05_FAIL_SELECT);
	emmc.fail_select = 0;
}

/* A switch that fails after its retries fails the request, within its
 * T_EXEC, before any read; the next request tries again. */
static void test_features_fail(void)
{
	static _Alignas(32) uint8_t data[S03_SECTOR];
	uint64_t t0;

	reset(FEATURE_BUSY);
	opxy_slave_storage(&s);
	CHECK(s.emmc_up && count(6) == 2u && features() == 0u);

	t0 = fake_core_peek_cycles();
	CHECK(s.store.emmc(s.store.ctx, S09_READ_SECTOR, 0u, 0u, data,
			   S03_SECTOR) != 0);
	CHECK((fake_core_peek_cycles() - t0) / CCLK_MHZ <= 8000000u);
	CHECK(emmc.sw[34] == S03_RETRIES && features() == S03_RETRIES);
	CHECK(count(17) == 0u && count(18) == 0u);
	CHECK(s.hw.msi.c == &opxy_msi);

	t0 = fake_core_peek_cycles();
	CHECK(s.store.os_status(s.store.ctx, 0u, 0u) == 1u);	/* S-03.046 */
	CHECK(s.staged == S05_FAIL_SELECT);
	CHECK((fake_core_peek_cycles() - t0) / CCLK_MHZ <= 8000000u);
	CHECK(emmc.sw[34] == 2u * S03_RETRIES && features() == emmc.sw[34]);
	CHECK(count(17) == 0u && count(18) == 0u);
}

/*
 * S-12.042, S-13b.054: the volume's device empties a block with CMD35 and
 * CMD36 at the block's first sector, then CMD38 with the erase argument,
 * and writes nothing; one that fails is tried S03_RETRIES times
 * (S-03.010); none while a boot partition is selected (S-03.007).
 */
static void test_user_bd_erase(void)
{
	struct wb_blockdev bd;

	reset(GOOD);
	opxy_slave_storage(&s);
	CHECK(s.emmc_up);
	opxy_slave_user_bd(&s, &bd);
	CHECK(bd.erase != NULL);
	if (bd.erase == NULL)
		return;

	log_start();
	CHECK(bd.erase(bd.ctx, 5u * S03_GROUP_SECTORS, S03_GROUP_SECTORS) == 0);
	CHECK(emmc.log_n >= 4u && emmc.log_n <= LOG_N);
	CHECK(emmc.log_idx[0] == 35u && emmc.log_arg[0] == 5u * S03_GROUP_SECTORS);
	CHECK(emmc.log_idx[1] == 36u && emmc.log_arg[1] == 5u * S03_GROUP_SECTORS);
	CHECK(emmc.log_idx[2] == 38u && emmc.log_arg[2] == WB_MMC_ERASE);
	CHECK(logged(35) == 1u && logged(36) == 1u && logged(38) == 1u);
	CHECK(logged(13) == emmc.log_n - 3u);	/* then the busy end */
	CHECK(logged(24) == 0u && logged(25) == 0u);

	log_start();
	emmc.fail_erase = true;
	CHECK(bd.erase(bd.ctx, 5u * S03_GROUP_SECTORS, S03_GROUP_SECTORS) != 0);
	CHECK(logged(35) == S03_RETRIES && logged(38) == S03_RETRIES);
	emmc.fail_erase = false;

	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 1u, 0u, NULL, 0u) == 0);
	log_start();
	CHECK(bd.erase(bd.ctx, 0u, S03_GROUP_SECTORS) != 0);
	CHECK(emmc.log_n == 0u);

	/* S-03.003: what an erase leaves, from EXT_CSD[181] as the bring-up
	 * read it; with 0xFF the glue writes zeros in place of the erase */
	CHECK(bd.erase_zeroes != NULL);
	if (bd.erase_zeroes == NULL)
		return;
	CHECK(bd.erase_zeroes(bd.ctx));
	s.ext_csd[181] = 1u;
	CHECK(!bd.erase_zeroes(bd.ctx));
	s.ext_csd[181] = 0xFEu;
	CHECK(bd.erase_zeroes(bd.ctx));
}

/* S-03.068: a flush reaches the eMMC only where EXT_CSD shows a cache. */
static void test_flush_needs_cache(void)
{
	reset(GOOD);
	opxy_slave_storage(&s);
	CHECK(s.emmc_up);
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 0u, 0u, NULL, 0u) == 0);
	log_start();
	CHECK(s.store.emmc(s.store.ctx, S09_FLUSH, 0u, 0u, NULL, 0u) == 0);
	CHECK(emmc.sw[32] == 1u);			/* CACHE_SIZE 0xA5A5A5A5 */
	memset(s.ext_csd + 249, 0, 4u);		/* no cache */
	CHECK(s.store.emmc(s.store.ctx, S09_FLUSH, 0u, 0u, NULL, 0u) == 0);
	CHECK(emmc.sw[32] == 1u);
	s.ext_csd[252] = 1u;
	CHECK(s.store.emmc(s.store.ctx, S09_FLUSH, 0u, 0u, NULL, 0u) == 0);
	CHECK(emmc.sw[32] == 2u);
}

/* S-12.060, S-13b.054: the volume's device writes chunks with CMD23,
 * bit 31 and the count, then CMD25 at the chunk's first sector. */
static void test_user_bd_reliable(void)
{
	static _Alignas(32) uint8_t buf[16u * S03_SECTOR];
	struct wb_blockdev bd;

	reset(GOOD);
	opxy_slave_storage(&s);
	CHECK(s.emmc_up);
	opxy_slave_user_bd(&s, &bd);
	CHECK(bd.write_reliable != NULL);
	if (bd.write_reliable == NULL)
		return;

	log_start();
	CHECK(bd.write_reliable(bd.ctx, 80u, buf, 8u) == 0);
	CHECK(bd.write_reliable(bd.ctx, 160u, buf, 16u) == 0);
	CHECK(reliable_writes() == 2u);
	CHECK(emmc.log_idx[0] == 23u &&
	      emmc.log_arg[0] == (WB_MMC_CMD23_RELIABLE | 8u));
	CHECK(emmc.log_idx[1] == 25u && emmc.log_arg[1] == 80u);
	CHECK(logged(12) == 0u);	/* CMD23 ends the transfer, not CMD12 */

	/* the bridge's plain write (0x16) is not routed there */
	log_start();
	CHECK(s.store.emmc(s.store.ctx, S09_WRITE_SECTORS, 0x1000000u, 8u, buf,
			   8u * S03_SECTOR) == 0);
	CHECK(logged(25) == 1u && logged(23) == 0u && logged(12) == 1u);

	/* none while a boot partition is selected */
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 2u, 0u, NULL, 0u) == 0);
	log_start();
	CHECK(bd.write_reliable(bd.ctx, 80u, buf, 8u) != 0);
	CHECK(emmc.log_n == 0u);
}

/*
 * SC-03-EMMC-OTHER (S-03.001, S-03.002) on the slave's start-up: an eMMC
 * with another sector count, erase group or optimal write size. No
 * user-area operation and no boot-partition write reaches it, the mount
 * fails as S-09.041's does, and the slave keeps answering the master.
 */
static void other_run(uint32_t sectors, uint8_t grp_size, uint8_t grp_mult,
		      uint8_t opt_write)
{
	const struct s09_req init = { S09_INIT, 0, 0, 0, 0 };
	const struct s09_req mount = { S09_MOUNT, 0, 0, 0, 0 };
	static _Alignas(32) uint8_t data[8u * S03_SECTOR];
	static uint8_t out[300];
	uint8_t hdr[S09_HDR];
	const uint8_t *d;
	size_t n;
	uint32_t err = 0xFFFFu;

	reset(GOOD);
	geometry(sectors, grp_size, grp_mult, opt_write);
	opxy_slave_storage(&s);
	CHECK(s.emmc_up && s.emmc_other);

	/* the volume: no mount, no read of it */
	CHECK(s.store.file(s.store.ctx, &mount, "", "", NULL, 0u, out,
			   &err) < 0 && err == 0u);
	CHECK(file_req(S09_STAT, "/yaffs2", &err) == -1);
	/* the user area over the bridge: nothing reaches the eMMC */
	CHECK(s.store.emmc(s.store.ctx, S09_READ_SECTOR, 0u, 0u, data,
			   S03_SECTOR) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_READ_SECTORS, 0x1000000u, 8u, data,
			   sizeof(data)) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_WRITE_SECTOR, 0u, 0u, data,
			   S03_SECTOR) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_WRITE_SECTORS, 0u, 8u, data,
			   sizeof(data)) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_WRITE_RELIABLE, 0u, 8u, data,
			   sizeof(data)) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_ERASE, 0x4000u * S03_GROUP_SECTORS,
			   WB_MMC_ERASE, NULL, 0u) != 0);
	CHECK(data_cmds() == 0u);
	/* a boot partition: selected and read, never written or erased */
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 1u, 0u, NULL, 0u) == 0);
	CHECK(s.store.emmc(s.store.ctx, S09_READ_SECTOR, 0u, 0u, data,
			   S03_SECTOR) == 0);
	CHECK(s.store.emmc(s.store.ctx, S09_WRITE_SECTOR, 0u, 0u, data,
			   S03_SECTOR) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_WRITE_SECTORS, 1u, 8u, data,
			   sizeof(data)) != 0);
	CHECK(s.store.emmc(s.store.ctx, S09_ERASE, 0u, WB_MMC_ERASE, NULL,
			   0u) != 0);
	CHECK(count(24) == 0u && count(25) == 0u && count(35) == 0u &&
	      count(36) == 0u && count(38) == 0u);
	CHECK(s.store.emmc(s.store.ctx, S09_SELECT, 0u, 0u, NULL, 0u) == 0);
	CHECK(s.store.emmc(s.store.ctx, S09_READ_SECTOR, 0u, 0u, data,
			   S03_SECTOR) != 0);
	CHECK(count(17) == 1u && count(18) == 0u);	/* BP1's sector alone */

	/* the slave answers on */
	s09_req_pack(&init, hdr);
	s09_slave_request(&s.server, hdr, sizeof(hdr));
	CHECK(s09_slave_reply(&s.server, &d, &n) && n == 8u);
}

static void test_other_emmc(void)
{
	other_run(0x1D2F000u, 31u, 31u, 1u);		/* another sector count */
	other_run(S03_USER_SECTORS, 31u, 15u, 1u);	/* a group of 0x200 */
	other_run(S03_USER_SECTORS, 15u, 31u, 1u);	/* a group of 0x200 */
	other_run(S03_USER_SECTORS, 0u, 0u, 1u);	/* a group of one */
	other_run(S03_USER_SECTORS, 31u, 31u, 16u);	/* does not divide 8 */
	other_run(S03_USER_SECTORS, 31u, 31u, 3u);
	other_run(S03_USER_SECTORS, 31u, 31u, 0u);	/* none reported */
}

/*
 * S-03.001: EXT_CSD[265] of 1, 2, 4 and 8 pass as the numbers they are,
 * with the CSD's group of 0x400 and EXT_CSD[224] at 2 (not in force). 0x03
 * reports the CSD's group (S-09.030).
 */
static void test_geometry_accepted(void)
{
	static const uint8_t ok[] = { 1u, 2u, 4u, 8u };
	unsigned i;

	for (i = 0; i < sizeof(ok); i++) {
		reset(GOOD);
		geometry(S03_USER_SECTORS, 31u, 31u, ok[i]);
		opxy_slave_storage(&s);
		CHECK(s.emmc_up && !s.emmc_other);
		CHECK(s.store.erase_group(s.store.ctx) == S03_GROUP_SECTORS);
	}
	/* another group from the CSD: refused, and 0x03 reports it */
	reset(GOOD);
	geometry(S03_USER_SECTORS, 31u, 30u, 1u);
	opxy_slave_storage(&s);
	CHECK(s.emmc_up && s.emmc_other);
	CHECK(s.store.erase_group(s.store.ctx) == 32u * 31u);
}

int main(void)
{
	RUN(test_ocr_never);
	RUN(test_busy_after_switch);
	RUN(test_no_response);
	RUN(test_cmd8_no_dto);
	RUN(test_slow_emmc);
	RUN(test_no_late_repeat);
	/* before the one mount of the process (test_mount_on_request) */
	RUN(test_other_emmc);
	RUN(test_mount_on_request);
	RUN(test_shutdown_unmount);	/* on that volume */
	RUN(test_features_once);
	RUN(test_file_release);
	RUN(test_features_fail);
	RUN(test_user_bd_erase);
	RUN(test_flush_needs_cache);
	RUN(test_user_bd_reliable);
	RUN(test_geometry_accepted);
	return check_summary("test_board_storage");
}
