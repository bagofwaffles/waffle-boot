/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The OP-1F's main CPU and its own eMMC (S-13b.060): the storage of
 * src/board/opxy_storage.c on wb_device_op1f, reached by the bridge client
 * on the same CPU (s09_master_local()), over the MSI driver and an eMMC
 * model. The volume is added once in a process, so this is a test of its
 * own (test_board_storage covers the OP-XY slave and the OP-1F's bring-up,
 * select and footprint).
 *
 * SC-03-POWER-OFF-1F, SC-03-POWER-OFF-ERR-1F (S-03.013, S-04.066's step 1):
 * the unmount of a mounted volume runs the flush, the cache off, the
 * unmount and the power-off notification with CMD13 polled; BKOPS_EN is
 * not written and no CMD8 follows; a failed step stops none of the later
 * ones; an unmounted volume gets nothing.
 *
 * The client's side: the mount, a stat and a sector read reach the store
 * with no bus, the link's own operations do nothing, nothing is sent to
 * any CPU, the release is not made.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_bridge.h"
#include "waffle/drivers/bf70x_msi.h"
#include "waffle/s09_bridge.h"
#include "waffle/s09_files.h"
#include "waffle/s09_master.h"

#define LOG_N 512u
#define CCLK_MHZ 400u

static struct {
	uint8_t ext_csd[512];		/* what CMD8 returns */
	uint32_t csd[4];
	uint8_t *ext_csd_to;		/* where the driver takes it */
	uint8_t fail_switch;		/* a CMD6 to this index answers an error */
	bool refuse_179;		/* a CMD6 to 179 not taken: SWITCH_ERROR */
	bool switch_error;		/* the next CMD13 reports SWITCH_ERROR */
	uint8_t data_err;		/* this data command ends with a CRC error */
	uint8_t *read_to;		/* where a read's data lands, NULL none */
	const uint8_t *write_from;	/* where a write's data comes from */
	uint8_t written[2u * 512u];	/* the last write's first sectors */
	unsigned not_ready;		/* CMD13s answered busy after a CMD6 */
	unsigned busy;			/* CMD13s still to answer busy */
	unsigned log_n;
	uint8_t log_idx[LOG_N];
	uint32_t log_arg[LOG_N];
	unsigned n;
} emmc;

static void take_command(uint32_t cmd)
{
	const uint32_t idx = cmd & 0x3Fu, arg = fake_mmr_get(MSI_CMDARG);
	uint32_t r1 = WB_MMC_R1_READY | WB_MMC_STATE_TRAN << 9;
	uint32_t is = MSI_IS_CMDDONE;

	emmc.n++;
	if (emmc.log_n < LOG_N) {
		emmc.log_idx[emmc.log_n] = (uint8_t)idx;
		emmc.log_arg[emmc.log_n] = arg;
	}
	emmc.log_n++;
	if (cmd & MSI_CMD_DXPECT)
		is |= emmc.data_err == idx ? MSI_IS_DCRC : MSI_IS_DTO;
	if ((idx == 17u || idx == 18u) && emmc.read_to != NULL) {
		const uint32_t n = fake_mmr_get(MSI_BYTCNT);

		for (uint32_t i = 0; i < n; i++)
			emmc.read_to[i] = (uint8_t)(arg + i / 512u + i);
	}
	if ((idx == 24u || idx == 25u) && emmc.write_from != NULL)
		memcpy(emmc.written, emmc.write_from, sizeof(emmc.written));
	switch (idx) {
	case 1:
		r1 = 0xC0FF8080u;
		break;
	case 6:
		if (emmc.fail_switch != 0u && (arg >> 16 & 0xFFu) == emmc.fail_switch)
			r1 |= 1u << 31;
		else if ((arg >> 16 & 0xFFu) == 179u && emmc.refuse_179)
			emmc.switch_error = true;	/* JESD84-B51: shown by CMD13 */
		else if ((arg >> 16 & 0xFFu) == 179u)
			emmc.ext_csd[179] = (uint8_t)(arg >> 8);
		emmc.busy = emmc.not_ready;
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
	case 13:
		if (emmc.switch_error) {
			emmc.switch_error = false;
			r1 |= WB_MMC_R1_SWITCH_ERROR;
		}
		if (emmc.busy != 0u) {
			emmc.busy--;
			r1 = 7u << 9;		/* programming, not ready */
		}
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

static struct opxy_hw hw;
static struct opxy_storage st;
static struct s09_master client;
static struct s09_files files;
static const struct s09_bus_ops no_bus;	/* the local client uses none */
static const struct s06_clock no_clock;

static uint32_t switch_arg(uint8_t index, uint8_t value)
{
	return 3u << 24 | (uint32_t)index << 16 | (uint32_t)value << 8;
}

static unsigned find_switch(uint32_t arg, unsigned from)
{
	for (unsigned i = from; i < emmc.log_n && i < LOG_N; i++)
		if (emmc.log_idx[i] == 6u && emmc.log_arg[i] == arg)
			return i;
	return LOG_N;
}

static unsigned switches_of(uint32_t arg)
{
	unsigned n = 0;

	for (unsigned i = 0; i < emmc.log_n && i < LOG_N; i++)
		n += emmc.log_idx[i] == 6u && emmc.log_arg[i] == arg;
	return n;
}

static unsigned logged(uint8_t idx)
{
	unsigned n = 0;

	for (unsigned i = 0; i < emmc.log_n && i < LOG_N; i++)
		n += emmc.log_idx[i] == idx;
	return n;
}

static void setup(void)
{
	const uint32_t sectors = S03_USER_SECTORS + 0x2000u;

	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(CCLK_MHZ);	/* 1 us a read of the counter */
	memset(&emmc, 0, sizeof(emmc));
	fake_mmr_set_hook(emmc_hook, NULL);
	hw.time.cycles_per_us = CCLK_MHZ;
	emmc.ext_csd[212] = (uint8_t)sectors;
	emmc.ext_csd[213] = (uint8_t)(sectors >> 8);
	emmc.ext_csd[214] = (uint8_t)(sectors >> 16);
	emmc.ext_csd[215] = (uint8_t)(sectors >> 24);
	emmc.ext_csd[226] = 0x20u;		/* BOOT_SIZE_MULT: 4 MiB */
	emmc.ext_csd[247] = 5u;			/* POWER_OFF_LONG_TIME: 50 ms */
	emmc.ext_csd[249] = 1u;			/* a cache to flush */
	emmc.ext_csd[265] = 1u;
	emmc.ext_csd[179] = 0x48u;
	emmc.csd[3] = 0xFFFFFFFFu;
	emmc.csd[2] = 0xFFFFFFFFu;
	emmc.csd[1] = 0xFFFF801Fu | 31u << 10 | 31u << 5;	/* 1024 sectors */
	emmc.csd[0] = 0xFFFFFFFFu;
	emmc.ext_csd_to = st.ext_csd;
	opxy_storage_init(&st, &hw, &wb_device_op1f, true);
	s09_master_init(&client, &no_bus, &no_clock);
	s09_master_local(&client, &st.store);
	s09_files_init(&files, &client);
}

/* The client reaches its own store: immediates, a select, a sector read;
 * the link's operations; nothing for another CPU. */
static void test_client(void)
{
	static _Alignas(32) uint8_t data[S03_SECTOR];
	const struct s09_req init = { S09_INIT, 0, 0, 0, 0 };
	const struct s09_req group = { S09_ERASE_GROUP, 0, 0, 0, 0 };
	const struct s09_req push = { S09_PUSH0, 0, 4, 0, 0 };
	const struct s09_req peek = { OPXY_DEV_PEEK_REQ, 0, 0, 0x20000000u, 0 };
	struct s03_emmc_ops ops = opxy_bridge_emmc(&client);
	uint32_t err = 0, got = 7;
	struct s06_node node;

	CHECK(st.emmc_up && !st.emmc_other);
	CHECK(s09_master_start(&client) && s09_master_link_fast(&client, 4u) &&
	      s09_master_link_plain(&client) && s09_master_settled(&client));
	CHECK((uint32_t)s09_master_call(&client, &init, NULL, 0, NULL, 0, &got, &err) ==
	      S09_INIT_VALUE && err == 0u && got == 0u);
	CHECK(s09_master_call(&client, &group, NULL, 0, NULL, 0, NULL, &err) == 1024);
	CHECK(opxy_bridge_boot_size(&client) == 0x20u * 0x20000u);
	CHECK(s09_master_call(&client, &push, data, 4, NULL, 0, NULL, &err) == 0);
	CHECK((uint32_t)s09_master_call(&client, &peek, NULL, 0, NULL, 0, NULL, &err) ==
	      0xDEADBEEFu);			/* no other CPU's memory */
	CHECK(s09_tunnel_port.room(&client) == 0u);
	memset(&node, 0, sizeof node);
	CHECK(!s09_power_off(&client, &node, 0u));
	CHECK(s09_master_release(&client) == -1);
	CHECK(!client.absent);
	/* S-03.012 through the bridge's ops: CMD8, then the kept bits */
	emmc.log_n = 0;
	CHECK(ops.select(ops.ctx, S03_PART_BP2) == 0);
	CHECK(logged(8) == 1u && find_switch(switch_arg(179, 0x4Au), 0) < LOG_N);
	CHECK(ops.select(ops.ctx, S03_PART_USER) == 0);
	CHECK(find_switch(switch_arg(179, 0x48u), 0) < LOG_N);
	emmc.log_n = 0;
	CHECK(ops.read(ops.ctx, 100u, 1u, data) == 0 && logged(17) == 1u);
	/* S-03.075 through the client as well */
	CHECK(ops.read(ops.ctx, 0x480000u, 1u, data) != 0 && logged(17) == 1u);
}

/* The volume mounted through the client, a stat on it; then the 0x09:
 * S-03.013 in order, with each failing step tried and the rest made. */
static void test_power_off(void)
{
	static const uint8_t failing[] = { 0u, 32u, 33u, 34u };
	const uint32_t flush = switch_arg(32, 1), cache = switch_arg(33, 0);
	const uint32_t notify = switch_arg(34, 3);
	struct s09_stat sb;

	for (unsigned k = 0; k < sizeof(failing); k++) {
		unsigned a, b, d, n;

		CHECK(s09_files_mount(&files) >= 0);
		CHECK(files.mounted);
		CHECK(s09_files_stat(&files, "/yaffs2", &sb) == 0);
		emmc.log_n = 0;
		emmc.not_ready = 2u;		/* the notification's busy, by CMD13 */
		emmc.fail_switch = failing[k];
		CHECK(s09_files_unmount(&files) || failing[k] != 0u);
		emmc.fail_switch = 0u;
		emmc.not_ready = 0u;
		CHECK(st.emmc_notified);
		a = find_switch(flush, 0);
		b = find_switch(cache, a);
		d = find_switch(notify, b);
		CHECK(a < b && b < d && d < LOG_N);
		CHECK(switches_of(flush) == (failing[k] == 32u ? S03_RETRIES : 1u));
		CHECK(switches_of(cache) == (failing[k] == 33u ? S03_RETRIES : 1u));
		/* retried only on a command error; nothing reaches the eMMC
		 * once the notification was taken */
		CHECK(switches_of(notify) == (failing[k] == 34u ? S03_RETRIES : 1u));
		CHECK(find_switch(switch_arg(163, 0), 0) == LOG_N);	/* BKOPS_EN kept */
		for (unsigned i = d + 1u; i < emmc.log_n && i < LOG_N; i++)
			CHECK(emmc.log_idx[i] == 13u || (failing[k] == 34u && emmc.log_idx[i] == 6u));
		CHECK(failing[k] == 34u || logged(13) >= 3u);
		/* an unmounted volume: nothing sent */
		n = emmc.n;
		CHECK(s09_files_unmount(&files));
		CHECK(emmc.n == n);
	}
	/* the next mount wakes the eMMC from CMD0, on four lines */
	emmc.log_n = 0;
	CHECK(s09_files_mount(&files) >= 0 && files.mounted);
	CHECK(emmc.log_idx[0] == 0u && find_switch(switch_arg(183, 1), 0) < LOG_N);
	CHECK(find_switch(switch_arg(183, 2), 0) == LOG_N);
}

/*
 * The fault an OP-1F showed: PART_CONFIG 00 at start, the check suite's
 * buffers on 4 bytes only. Over the bridge the slave's buffers are its own;
 * the local client hands the caller's to the store, whose driver takes only
 * a cache line's (S-13b.054): every boot-partition read then failed with no
 * command sent. Now the store copies through its own buffer: the same
 * commands, and the data as the eMMC gave it.
 */
static void test_part_config_zero_off_line(void)
{
	static _Alignas(32) uint8_t raw[3u * S03_SECTOR + 32u];
	uint8_t *const buf = raw + 4;		/* on 4 bytes, not on 32 */
	struct s03_emmc_ops ops = opxy_bridge_emmc(&client);
	uint8_t *const bounce = opxy_storage_bounce();
	uint8_t want[2u * S03_SECTOR];

	memset(&st.note_select, 0, sizeof st.note_select);
	memset(&st.note_read, 0, sizeof st.note_read);
	emmc.ext_csd[179] = 0x00u;
	st.sel.known = false;
	emmc.log_n = 0;
	CHECK(ops.select(ops.ctx, S03_PART_BP1) == 0);
	CHECK(logged(8) == 1u && switches_of(switch_arg(179, 0x01u)) == 1u);
	CHECK(emmc.ext_csd[179] == 0x01u);
	emmc.read_to = bounce;
	CHECK(ops.read(ops.ctx, 0u, 1u, buf) == 0 && logged(17) == 1u);
	CHECK(buf[0] == 0u && buf[1] == 1u && buf[511] == 0xFFu);
	CHECK(ops.read(ops.ctx, 16u, 2u, buf) == 0 && logged(18) == 1u);
	for (uint32_t i = 0; i < 2u * S03_SECTOR; i++)
		want[i] = (uint8_t)(16u + i / 512u + i);
	CHECK(memcmp(buf, want, sizeof want) == 0);
	emmc.read_to = NULL;
	/* a write the same way: its bytes reach the eMMC */
	for (uint32_t i = 0; i < 2u * S03_SECTOR; i++)
		buf[i] = (uint8_t)(i * 7u);
	emmc.write_from = bounce;
	CHECK(ops.write(ops.ctx, 0u, 1u, buf) == 0 && logged(24) == 1u);
	CHECK(memcmp(emmc.written, buf, S03_SECTOR) == 0);
	emmc.write_from = NULL;
	CHECK(!st.note_read.seen && !st.note_select.seen);
	CHECK(ops.select(ops.ctx, S03_PART_USER) == 0);
	CHECK(emmc.ext_csd[179] == 0x00u);
}

/* A select the eMMC refuses, and a read that fails: each first one noted
 * with the step, the command, the response and EXT_CSD[179] around it. */
static void test_fault_notes(void)
{
	static _Alignas(32) uint8_t data[2u * S03_SECTOR];
	struct s03_emmc_ops ops = opxy_bridge_emmc(&client);
	const struct opxy_emmc_note *n = &st.note_select;

	memset(&st.note_select, 0, sizeof st.note_select);
	memset(&st.note_read, 0, sizeof st.note_read);
	emmc.ext_csd[179] = 0x00u;
	st.sel.known = false;
	emmc.refuse_179 = true;
	emmc.log_n = 0;
	CHECK(ops.select(ops.ctx, S03_PART_BP2) != 0);
	/* S-03.012: CMD8, the CMD6, and after the failure CMD8 again first */
	CHECK(logged(8) == 1u && switches_of(switch_arg(179, 0x02u)) == 1u);
	CHECK(n->seen && n->part == 2u && n->why == OPXY_NOTE_DRIVER);
	CHECK(n->f.step == WB_MSI_STEP_SWITCH && n->f.cmd == 6u && n->f.at == 13u);
	CHECK(n->f.arg == switch_arg(179, 0x02u) && n->f.rc == WB_MSI_ERR_EMMC);
	CHECK((n->f.r1 & WB_MMC_R1_SWITCH_ERROR) != 0u && n->f.state == WB_MMC_STATE_TRAN);
	CHECK(n->pc_flags == OPXY_NOTE_PC_BEFORE && n->pc_before == 0x00u);
	emmc.log_n = 0;
	CHECK(ops.select(ops.ctx, S03_PART_BP2) != 0);
	CHECK(logged(8) == 1u && emmc.log_idx[0] == 8u);	/* read first, never blind */
	CHECK(n->pc_flags == (OPXY_NOTE_PC_BEFORE | OPXY_NOTE_PC_AFTER) && n->pc_after == 0x00u);
	CHECK(n->f.arg == switch_arg(179, 0x02u));	/* the first one kept */
	emmc.refuse_179 = false;
	/* a read in the unknown partition is refused by the store */
	CHECK(ops.read(ops.ctx, 0u, 1u, data) != 0);
	CHECK(st.note_read.seen && st.note_read.why == OPXY_NOTE_UNKNOWN);
	memset(&st.note_read, 0, sizeof st.note_read);
	CHECK(ops.select(ops.ctx, S03_PART_BP1) == 0);
	emmc.data_err = 18u;
	CHECK(ops.read(ops.ctx, 8u, 2u, data) != 0);
	emmc.data_err = 0u;
	CHECK(st.note_read.seen && st.note_read.why == OPXY_NOTE_DRIVER &&
	      st.note_read.part == 1u && st.note_read.sector == 8u && st.note_read.count == 2u);
	CHECK(st.note_read.f.step == WB_MSI_STEP_DATA && st.note_read.f.cmd == 18u &&
	      st.note_read.f.arg == 8u && (st.note_read.f.istat & MSI_IS_DCRC) != 0u);
	CHECK(ops.select(ops.ctx, S03_PART_USER) == 0);
}

/* S-03.075 on the OP-1F through its own client, a raw request as a host or
 * a developer sends it: refused, nothing reaches the eMMC, and an erase of
 * the FAT region does nothing. */
static void test_raw_footprint(void)
{
	static _Alignas(32) uint8_t data[S03_SECTOR];
	const struct s09_req erase = { S09_ERASE, 0, 0, 0x480000u, 0 };
	const struct s09_req fat = { S09_ERASE, 0, 0, 0x500000u, 0 };
	const struct s09_req wr = { S09_WRITE_SECTOR, 0, S03_SECTOR, 0x480000u, 0 };
	uint32_t err = 0;
	unsigned n;

	CHECK(st.part == S03_PART_USER);
	n = emmc.n;
	CHECK(s09_master_call(&client, &erase, NULL, 0, NULL, 0, NULL, &err) != 0);
	CHECK(s09_master_call(&client, &fat, NULL, 0, NULL, 0, NULL, &err) != 0);
	CHECK(s09_master_call(&client, &wr, data, S03_SECTOR, NULL, 0, NULL, &err) != 0);
	CHECK(emmc.n == n);
}

/* S-05.037, S-13b.062: the controller closed with no command. */
static void test_close(void)
{
	const unsigned n = emmc.n;

	fake_mmr_set(MSI_CTL, MSI_CTL_INTEN | MSI_CTL_INTDMAC);
	fake_mmr_set(MSI_IMSK, 0xFFC2u);
	fake_mmr_set(MSI_ISTAT, 0x4u);
	CHECK(opxy_storage_close(&st) == WB_MSI_OK);
	CHECK((fake_mmr_get(MSI_CTL) & MSI_CTL_INTEN) == 0u);
	CHECK(fake_mmr_get(MSI_IMSK) == 0u && fake_mmr_get(MSI_ISTAT) == 0u);
	CHECK(emmc.n == n);
}

int main(void)
{
	setup();
	RUN(test_client);
	RUN(test_part_config_zero_off_line);
	RUN(test_fault_notes);
	RUN(test_raw_footprint);
	RUN(test_power_off);
	RUN(test_close);
	return check_summary("test_board_storage_1f");
}
