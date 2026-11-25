/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MSI controller and eMMC (see waffle/drivers/bf70x/msi.h). */
#include "waffle/drivers/bf70x/msi.h"

#include <stddef.h>

#include "waffle/drivers/bf70x/cplb.h"
#include "waffle/drivers/mmr.h"

#define CMD_BASE  (MSI_CMD_STARTCMD | MSI_CMD_USEHOLDREG)
#define HLE_TRIES 4u

_Static_assert(WB_MSI_BUF_ALIGN == WB_DCACHE_LINE,
	       "a data buffer owns the cache lines it covers");

/*
 * S-13b.054: every transfer goes by the internal DMA, round the data
 * cache, so a buffer starts on a cache line; being whole blocks long, it
 * then shares no line with other data, and the write-back before the DMA
 * and the invalidate after a read touch nothing else.
 */
static bool buf_ok(const uint8_t *buf)
{
	return ((uintptr_t)buf & (WB_MSI_BUF_ALIGN - 1u)) == 0u;
}

/* The fault record is kept for the test OS's report and the host's tests;
 * the bootloader itself does without it. */
#if defined(OPXY_TEST_OS) || !defined(__bfin__)
#define FAULTS 1
#else
#define FAULTS 0
#endif

/* MSI_ISTAT as the last command's end found it, before its bits were
 * cleared: one controller, so one value. */
static uint32_t cmd_istat;

void wb_msi_fault_clear(struct wb_msi *m)
{
	m->fault = (struct wb_msi_fault){ 0 };
	m->fault.state = 0xFFu;
	m->cmd13_seen = false;
	m->busy_from = 0u;
}

/* The operation's first fault, recorded; rc returned. */
static int fault_is(struct wb_msi *m, enum wb_msi_step step, uint8_t at, int rc,
		    uint32_t is)
{
	struct wb_msi_fault *f = &m->fault;

	if (!FAULTS || f->step != WB_MSI_STEP_NONE)
		return rc;
	f->step = (uint8_t)step;
	f->cmd = m->cmd_idx;
	f->at = at;
	f->rc = (int8_t)rc;
	f->state = m->cmd13_seen ? (uint8_t)WB_MMC_R1_STATE(m->cmd13_r1) : 0xFFu;
	f->arg = m->cmd_arg;
	f->r1 = m->last_r1;
	f->istat = is;
	f->idsts = wb_mmr_read32(MSI_IDSTS);
	f->busy_us = m->busy_from != 0u && m->t != NULL ?
		     (uint32_t)(wb_time_us(m->t) - m->busy_from) : 0u;
	return rc;
}

static int fault(struct wb_msi *m, enum wb_msi_step step, uint8_t at, int rc)
{
	return FAULTS ? fault_is(m, step, at, rc, wb_mmr_read32(MSI_ISTAT)) : rc;
}

/* The command an operation is about; CMD12 and CMD13 serve it. */
static void begin(struct wb_msi *m, uint32_t idx, uint32_t arg)
{
	if (!FAULTS)
		return;
	m->cmd_idx = (uint8_t)idx;
	m->cmd_arg = arg;
	m->busy_from = 0u;
}

/* A buffer refused: nothing sent. */
static int refuse_buf(struct wb_msi *m, uint32_t idx, const uint8_t *buf)
{
	begin(m, idx, (uint32_t)(uintptr_t)buf);
	return fault(m, WB_MSI_STEP_ARG, (uint8_t)idx, WB_MSI_ERR_ARG);
}

static int poll(const struct wb_msi *m, wb_mmr_addr addr, uint32_t mask,
		uint32_t value, uint32_t timeout_us)
{
	return wb_poll32(m->t, addr, mask, value, timeout_us) == WB_TIME_OK ?
	       WB_MSI_OK : WB_MSI_TIMEOUT;
}

/* The CIU took the command (STARTCMD clear), or refused it (HLE). */
static int wait_taken(const struct wb_msi *m)
{
	uint64_t end = wb_time_deadline(m->t, m->c->xfer_timeout_us);
	bool late;

	do {
		late = wb_time_passed(end);
		if (wb_mmr_read32(MSI_ISTAT) & MSI_IS_HLE)
			return WB_MSI_ERR_CMD;
		if (!(wb_mmr_read32(MSI_CMD) & MSI_CMD_STARTCMD))
			return WB_MSI_OK;
	} while (!late);
	return WB_MSI_TIMEOUT;
}

/* Write CMD; on a hardware locked error, write it again. */
static int start(const struct wb_msi *m, uint32_t cmd)
{
	unsigned i;
	int rc = WB_MSI_ERR_CMD;

	for (i = 0; i < HLE_TRIES && rc == WB_MSI_ERR_CMD; i++) {
		wb_mmr_write32(MSI_ISTAT, MSI_IS_HLE);
		wb_mmr_write32(MSI_CMD, cmd);
		rc = wait_taken(m);
	}
	return rc;
}

int wb_msi_init(struct wb_msi *m, const struct wb_msi_config *c,
		const struct wb_time *t, struct wb_msi_desc *desc,
		unsigned ndesc)
{
	int rc;

	if (desc == NULL || ndesc == 0 || c->ident_clkdiv > 0xFFu)
		return WB_MSI_ERR_ARG;
	*m = (struct wb_msi){ 0 };
	m->c = c;
	m->t = t;
	m->desc = desc;
	m->ndesc = ndesc;
	m->width = 1;

	wb_mmr_write32(MSI_CTL, MSI_CTL_RESETS);
	rc = poll(m, MSI_CTL, MSI_CTL_RESETS, 0, c->xfer_timeout_us);
	if (rc != WB_MSI_OK)
		return rc;
	wb_mmr_write32(MSI_BUSMODE, MSI_BUSMODE_SWR);
	rc = poll(m, MSI_BUSMODE, MSI_BUSMODE_SWR, 0, c->xfer_timeout_us);
	if (rc != WB_MSI_OK)
		return rc;
	wb_mmr_write32(MSI_IMSK, 0);
	wb_mmr_write32(MSI_ISTAT, 0xFFFFFFFFu);
	wb_mmr_write32(MSI_IDINTEN, 0);
	wb_mmr_write32(MSI_IDSTS, MSI_IDSTS_ALL);
	wb_mmr_write32(MSI_TMOUT, c->tmout);
	wb_mmr_write32(MSI_FIFOTH, c->fifoth);
	wb_mmr_write32(MSI_CTYPE, 0);
	wb_mmr_write32(MSI_BUSMODE, MSI_BUSMODE_DE | MSI_BUSMODE_FB);
	wb_mmr_write32(MSI_CTL, MSI_CTL_INTDMAC);
	wb_mmr_sync();
	return WB_MSI_OK;
}

static int update_clock(const struct wb_msi *m)
{
	return start(m, MSI_CMD_STARTCMD | MSI_CMD_UCLKREGS |
		     MSI_CMD_WTPRIVDATA);
}

int wb_msi_set_clock(struct wb_msi *m, uint32_t clkdiv, bool low_power)
{
	uint32_t en;
	int rc;

	if (m->powered_off)
		return WB_MSI_ERR_OFF;
	if (clkdiv > 0xFFu)
		return WB_MSI_ERR_ARG;
	rc = poll(m, MSI_STAT, MSI_STAT_DBUSY, 0, m->c->busy_timeout_us);
	if (rc != WB_MSI_OK)
		return rc;
	wb_mmr_write32(MSI_CLKEN, 0);
	rc = update_clock(m);
	if (rc != WB_MSI_OK)
		return rc;
	wb_mmr_write32(MSI_CLKDIV, clkdiv);
	rc = update_clock(m);
	if (rc != WB_MSI_OK)
		return rc;
	en = MSI_CLKEN_EN0;
	if (low_power)
		en |= MSI_CLKEN_LP0;
	wb_mmr_write32(MSI_CLKEN, en);
	rc = update_clock(m);
	if (rc == WB_MSI_OK)
		m->clkdiv = clkdiv;
	return rc;
}

/* Send a command and wait for its response (not for its data). */
static int send(const struct wb_msi *m, uint32_t idx, uint32_t arg,
		uint32_t flags, uint32_t resp[4])
{
	uint32_t is;
	int rc;

	if (m->powered_off)
		return WB_MSI_ERR_OFF;
	wb_mmr_write32(MSI_ISTAT, MSI_IS_CMDDONE | MSI_IS_CMD_ERR);
	wb_mmr_write32(MSI_CMDARG, arg);
	if (!(flags & MSI_CMD_STPABORT))
		flags |= MSI_CMD_WTPRIVDATA;
	rc = start(m, CMD_BASE | flags | (idx & 0x3Fu));
	if (FAULTS)
		cmd_istat = wb_mmr_read32(MSI_ISTAT);
	if (rc != WB_MSI_OK)
		return rc;
	rc = poll(m, MSI_ISTAT, MSI_IS_CMDDONE, MSI_IS_CMDDONE,
		  m->c->xfer_timeout_us);
	if (FAULTS)
		cmd_istat = wb_mmr_read32(MSI_ISTAT);
	if (rc != WB_MSI_OK)
		return rc;
	is = wb_mmr_read32(MSI_ISTAT);
	if (FAULTS)
		cmd_istat = is;
	wb_mmr_write32(MSI_ISTAT, is & (MSI_IS_CMDDONE | MSI_IS_CMD_ERR));
	if (is & MSI_IS_CMD_ERR)
		return WB_MSI_ERR_CMD;
	if (resp != NULL) {
		resp[0] = wb_mmr_read32(MSI_RESP0);
		if (flags & MSI_CMD_RLEN) {
			resp[1] = wb_mmr_read32(MSI_RESP1);
			resp[2] = wb_mmr_read32(MSI_RESP2);
			resp[3] = wb_mmr_read32(MSI_RESP3);
		}
	}
	return WB_MSI_OK;
}

int wb_msi_cmd(const struct wb_msi *m, uint32_t idx, uint32_t arg,
	       uint32_t flags, uint32_t resp[4])
{
	return send(m, idx, arg, flags & ~(uint32_t)MSI_CMD_DXPECT, resp);
}

/* A command with an R1 response, its error bits checked. */
static int r1(struct wb_msi *m, uint32_t idx, uint32_t arg, uint32_t flags)
{
	uint32_t resp[4] = { 0 };
	int rc;

	if (idx != 12u && idx != 13u)
		begin(m, idx, arg);
	rc = send(m, idx, arg, WB_MSI_R1 | flags, resp);
	if (rc != WB_MSI_OK)
		return fault_is(m, rc == WB_MSI_ERR_OFF ? WB_MSI_STEP_OFF : WB_MSI_STEP_CMD,
				(uint8_t)idx, rc, cmd_istat);
	m->last_r1 = resp[0];
	if (FAULTS && idx == 13u) {
		m->cmd13_r1 = resp[0];
		m->cmd13_seen = true;
	}
	if (resp[0] & WB_MMC_R1_ERRORS)
		return fault_is(m, WB_MSI_STEP_R1, (uint8_t)idx, WB_MSI_ERR_EMMC, cmd_istat);
	return WB_MSI_OK;
}

/* The busy wait's start: the response's end. */
static void busy_start(struct wb_msi *m)
{
	if (FAULTS && m->t != NULL)
		m->busy_from = wb_time_us(m->t);
}

static int wait_dat0(struct wb_msi *m, uint32_t timeout_us)
{
	int rc = poll(m, MSI_STAT, MSI_STAT_DBUSY, 0, timeout_us);

	return rc != WB_MSI_OK ? fault(m, WB_MSI_STEP_BUSY, m->cmd_idx, rc) : rc;
}

/* CMD13 until READY_FOR_DATA in transfer state (S-13b.054, .055). */
static int wait_ready(struct wb_msi *m)
{
	uint64_t end = wb_time_deadline(m->t, m->c->busy_timeout_us);
	bool late;

	do {
		int rc;

		late = wb_time_passed(end);
		rc = r1(m, 13, (uint32_t)m->rca << 16, 0);
		if (rc != WB_MSI_OK)
			return rc;
		if (m->last_r1 & WB_MMC_R1_SWITCH_ERROR)
			return fault(m, WB_MSI_STEP_SWITCH, 13u, WB_MSI_ERR_EMMC);
		if ((m->last_r1 & WB_MMC_R1_READY) &&
		    WB_MMC_R1_STATE(m->last_r1) == WB_MMC_STATE_TRAN)
			return WB_MSI_OK;
	} while (!late);
	return fault(m, WB_MSI_STEP_STATE, 13u, WB_MSI_TIMEOUT);
}

/* After an R1b command or a write: DAT0 released, then CMD13. */
static int wait_busy_end(struct wb_msi *m)
{
	int rc = wait_dat0(m, m->c->busy_timeout_us);

	return rc != WB_MSI_OK ? rc : wait_ready(m);
}

static void stop_dma(void)
{
	wb_mmr_write32(MSI_CTL, wb_mmr_read32(MSI_CTL) | MSI_CTL_FIFORST |
		       MSI_CTL_DMARST);
	wb_mmr_sync();
}

static uint32_t sat_us(uint64_t us)
{
	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

/*
 * One block's allowance in a multi-block data phase: its transfer and, in
 * a write, the busy period the eMMC signals on DAT0 between blocks.
 */
static uint32_t block_allowance_us(const struct wb_msi_config *c, bool write)
{
	return sat_us((uint64_t)c->xfer_timeout_us +
		      (write ? c->busy_timeout_us : 0u));
}

/* A multi-block data phase, whole: xfer_total_us, at least one allowance. */
static uint32_t data_total_us(const struct wb_msi_config *c, uint32_t allow)
{
	return c->xfer_total_us > allow ? c->xfer_total_us : allow;
}

/* One data command over the IDMAC; count blocks fit the descriptors. */
static int data(struct wb_msi *m, uint32_t idx, uint32_t arg,
		const uint8_t *buf, uint32_t count, bool write)
{
	uint32_t addr = (uint32_t)(uintptr_t)buf, len = count * WB_MSI_BLOCK;
	uint32_t i, n = (count + WB_MSI_DESC_BLOCKS - 1u) / WB_MSI_DESC_BLOCKS;
	uint32_t flags, allow, moved, ids = 0;
	uint64_t cap, stall;
	bool late;
	int rc;

	begin(m, idx, arg);
	for (i = 0; i < n; i++) {
		struct wb_msi_desc *d = &m->desc[i];
		uint32_t left = len - i * WB_MSI_DESC_BLOCKS * WB_MSI_BLOCK;
		uint32_t size = left < WB_MSI_DESC_BLOCKS * WB_MSI_BLOCK ?
				left : WB_MSI_DESC_BLOCKS * WB_MSI_BLOCK;

		d->des0 = MSI_DES0_OWN | MSI_DES0_CH | MSI_DES0_DIC;
		if (i == 0)
			d->des0 |= MSI_DES0_FS;
		if (i + 1u == n)
			d->des0 |= MSI_DES0_LD;
		d->des1 = size;
		d->des2 = addr + i * WB_MSI_DESC_BLOCKS * WB_MSI_BLOCK;
		d->des3 = i + 1u == n ? 0 :
			  (uint32_t)(uintptr_t)&m->desc[i + 1u];
	}
	wb_dcache_flush_range((uint32_t)(uintptr_t)m->desc,
			      n * (uint32_t)sizeof(*m->desc));
	if (write)
		wb_dcache_flush_range(addr, len);
	else
		wb_dcache_invalidate_range(addr, len);
	WB_MSI_PHASE(WB_MSI_PH_CACHE);

	wb_mmr_write32(MSI_CTL, wb_mmr_read32(MSI_CTL) | MSI_CTL_FIFORST);
	rc = poll(m, MSI_CTL, MSI_CTL_FIFORST, 0, m->c->xfer_timeout_us);
	if (rc != WB_MSI_OK)
		return fault(m, WB_MSI_STEP_CMD, (uint8_t)idx, rc);
	wb_mmr_write32(MSI_IDSTS, MSI_IDSTS_ALL);
	wb_mmr_write32(MSI_ISTAT, MSI_IS_DTO | MSI_IS_DATA_ERR);
	wb_mmr_write32(MSI_BYTCNT, len);
	wb_mmr_write32(MSI_BLKSIZ, WB_MSI_BLOCK);
	wb_mmr_write32(MSI_DBADDR, (uint32_t)(uintptr_t)m->desc);
	flags = WB_MSI_R1 | MSI_CMD_DXPECT;
	if (write)
		flags |= MSI_CMD_RDWR;
	WB_MSI_PHASE(WB_MSI_PH_SETUP);
	rc = send(m, idx, arg, flags, NULL);
	if (rc != WB_MSI_OK)
		(void)fault_is(m, rc == WB_MSI_ERR_OFF ? WB_MSI_STEP_OFF : WB_MSI_STEP_CMD,
			       (uint8_t)idx, rc, cmd_istat);
	if (rc == WB_MSI_OK)
		m->last_r1 = wb_mmr_read32(MSI_RESP0);
	if (rc == WB_MSI_OK && (m->last_r1 & WB_MMC_R1_ERRORS))
		rc = fault_is(m, WB_MSI_STEP_R1, (uint8_t)idx, WB_MSI_ERR_EMMC, cmd_istat);
	WB_MSI_PHASE(WB_MSI_PH_CMD);
	if (rc != WB_MSI_OK) {
		stop_dma();
		return rc;
	}

	/*
	 * One block waits xfer_timeout_us. More blocks wait one block's
	 * allowance from the last time MSI_TCBCNT moved, and the whole data
	 * phase at most xfer_total_us (S-13b.057): TCBCNT counts the bytes
	 * the CIU has moved to or from the eMMC, the count software reads
	 * after a data timeout to learn how much of the transfer was made
	 * (HRM ch. 33, "Error Handling" and the MSI_TCBCNT register
	 * description). A count that never moves leaves one allowance from
	 * the command's end.
	 */
	allow = count > 1u ? block_allowance_us(m->c, write) :
		m->c->xfer_timeout_us;
	cap = wb_time_deadline(m->t, count > 1u ? data_total_us(m->c, allow) :
				     allow);
	stall = wb_time_deadline(m->t, allow);
	moved = 0;
	do {
		uint32_t is;
		bool capped = wb_time_passed(cap);

		late = capped || wb_time_passed(stall);
		if (count > 1u) {
			uint32_t now = wb_mmr_read32(MSI_TCBCNT);

			if (now != moved) {
				moved = now;
				stall = wb_time_deadline(m->t, allow);
				late = capped;
			}
		}
		is = wb_mmr_read32(MSI_ISTAT);
		ids = wb_mmr_read32(MSI_IDSTS);
		if ((is & MSI_IS_DATA_ERR) ||
		    (ids & (MSI_IDSTS_FBE | MSI_IDSTS_DU | MSI_IDSTS_CES))) {
			rc = fault_is(m, WB_MSI_STEP_DATA, (uint8_t)idx, WB_MSI_ERR_DATA, is);
			break;
		}
		if (is & MSI_IS_DTO) {
			wb_mmr_write32(MSI_ISTAT, MSI_IS_DTO);
			WB_MSI_PHASE(WB_MSI_PH_DATA);
			if (!write)
				wb_dcache_invalidate_range(addr, len);
			WB_MSI_PHASE(WB_MSI_PH_CACHE);
			return WB_MSI_OK;
		}
	} while (!late);
	if (rc == WB_MSI_OK)
		rc = fault(m, WB_MSI_STEP_DATA_TIME, (uint8_t)idx, WB_MSI_TIMEOUT);
	stop_dma();
	/* After a fatal bus error only a controller reset recovers the MSI
	 * (HRM ch. 33, "Fatal Bus Errors (FBE)"; MSI_IDSTS.FBE). */
	if (ids & MSI_IDSTS_FBE) {
		wb_mmr_write32(MSI_CTL, wb_mmr_read32(MSI_CTL) | MSI_CTL_RESETS);
		(void)poll(m, MSI_CTL, MSI_CTL_RESETS, 0, m->c->xfer_timeout_us);
	}
	/* After a multi-block data phase that failed, CMD12 takes the eMMC
	 * out of its data state, as the HRM advises on a data error (ch. 33,
	 * "Error Handling"), and the busy end and CMD13 follow whatever CMD12
	 * returned, so that the next command finds the eMMC in transfer state
	 * (S-13b.054). CMD25 after CMD23 is multi-block even for one block. */
	if (idx == 18u || idx == 25u) {
		(void)r1(m, 12, 0, MSI_CMD_STPABORT);
		/* The first CMD13 may carry CMD12's own error bits. */
		if (wait_dat0(m, m->c->busy_timeout_us) == WB_MSI_OK &&
		    wait_ready(m) == WB_MSI_ERR_EMMC)
			(void)wait_ready(m);
	}
	return rc;
}

static int stop_transmission(struct wb_msi *m)
{
	int rc = r1(m, 12, 0, MSI_CMD_STPABORT);

	return rc != WB_MSI_OK ? rc : wait_dat0(m, m->c->busy_timeout_us);
}

/*
 * CMD12 after a CMD18 whose every block arrived. The eMMC may read ahead of
 * the host in a multiple-block read, and when the read ends at the last
 * block of a partition it may already have tried the address past it:
 * ADDRESS_OUT_OF_RANGE then shows in CMD12's response, and the host is to
 * ignore it (JESD84-B51, "Read ahead in multiple block read operation").
 * Any other error bit still fails the read.
 */
static int stop_read(struct wb_msi *m)
{
	const bool had = !FAULTS || m->fault.step != WB_MSI_STEP_NONE;
	int rc = r1(m, 12, 0, MSI_CMD_STPABORT);

	if (rc == WB_MSI_ERR_EMMC &&
	    (m->last_r1 & WB_MMC_R1_ERRORS) == WB_MMC_R1_OUT_OF_RANGE) {
		rc = WB_MSI_OK;
		if (!had)
			wb_msi_fault_clear(m);
	}
	return rc != WB_MSI_OK ? rc : wait_dat0(m, m->c->busy_timeout_us);
}

/* cppcheck-suppress constParameterPointer ; the IDMAC writes ext_csd */
int wb_msi_read_ext_csd(struct wb_msi *m, uint8_t *ext_csd)
{
	if (!buf_ok(ext_csd))
		return refuse_buf(m, 8u, ext_csd);
	return data(m, 8, 0, ext_csd, 1, false);
}

/* Switch in write-byte mode, then its busy end (S-13b.055). */
static int switch_byte(struct wb_msi *m, uint8_t index, uint8_t value)
{
	uint32_t arg = 3u << 24 | (uint32_t)index << 16 |
		       (uint32_t)value << 8;
	int rc = r1(m, 6, arg, 0);

	busy_start(m);
	/* A flush keeps the eMMC busy for as long as its cache takes (S-03.068). */
	if (rc == WB_MSI_OK && index == WB_EXT_CSD_FLUSH_CACHE &&
	    m->c->flush_timeout_us != 0u)
		rc = wait_dat0(m, m->c->flush_timeout_us);
	return rc != WB_MSI_OK ? rc : wait_busy_end(m);
}

int wb_msi_power_off(struct wb_msi *m, uint8_t power_off_long_time)
{
	uint32_t timeout_us = power_off_long_time != 0 ?
			      power_off_long_time * 10000u :
			      m->c->busy_timeout_us;
	int rc = r1(m, 6, 3u << 24 | WB_EXT_CSD_POWER_OFF << 16 | 3u << 8,
		    0);

	/* Whatever happened, nothing more is sent (S-13b.056). */
	m->powered_off = true;
	return rc != WB_MSI_OK ? rc : wait_dat0(m, timeout_us);
}

static bool switch_allowed(uint8_t index);

/* S-03.013: the switch's busy end read by CMD13 alone. */
int wb_msi_switch_poll(void *ctx, uint8_t index, uint8_t value, uint32_t limit_ms)
{
	struct wb_msi *m = ctx;
	int rc, out = 1;

	if (!switch_allowed(index))
		return -1;
	rc = r1(m, 6, 3u << 24 | (uint32_t)index << 16 | (uint32_t)value << 8, 0);
	if (rc != WB_MSI_OK) {
		out = -1;
	} else {
		const uint64_t end = wb_time_deadline(m->t, limit_ms * 1000u);
		bool late;

		busy_start(m);
		do {
			late = wb_time_passed(end);
			if (r1(m, 13, (uint32_t)m->rca << 16, 0) != WB_MSI_OK) {
				out = -1;
				break;
			}
			if ((m->last_r1 & WB_MMC_R1_SWITCH_ERROR) != 0u) {
				(void)fault(m, WB_MSI_STEP_SWITCH, 13u, WB_MSI_ERR_EMMC);
				out = -1;
				break;
			}
			if ((m->last_r1 & WB_MMC_R1_READY) &&
			    WB_MMC_R1_STATE(m->last_r1) == WB_MMC_STATE_TRAN) {
				out = 0;
				break;
			}
		} while (!late);
		if (out == 1)
			(void)fault(m, WB_MSI_STEP_STATE, 13u, WB_MSI_TIMEOUT);
	}
	/* S-03.013: nothing reaches the eMMC after a notification it took */
	if (index == WB_EXT_CSD_POWER_OFF && rc == WB_MSI_OK)
		m->powered_off = true;
	return out;
}

int wb_msi_read_part_config(struct wb_msi *m, uint8_t *buf, uint8_t *value)
{
	int rc = wb_msi_read_ext_csd(m, buf);

	if (rc == WB_MSI_OK)
		*value = buf[WB_EXT_CSD_PART_CONFIG];
	return rc;
}

static bool switch_allowed(uint8_t index)
{
	static const uint8_t ok[] = { 32, 33, 34, 163, 179, 183, 185 };
	unsigned i;

	for (i = 0; i < sizeof(ok); i++)
		if (ok[i] == index)
			return true;
	return false;
}

int wb_msi_emmc_switch(void *ctx, uint8_t index, uint8_t value)
{
	struct wb_msi *m = ctx;
	int rc;

	if (!switch_allowed(index)) {
		begin(m, 6u, 3u << 24 | (uint32_t)index << 16 | (uint32_t)value << 8);
		return fault(m, WB_MSI_STEP_ARG, 6u, WB_MSI_ERR_ARG);
	}
	if (index == WB_EXT_CSD_POWER_OFF && value == 3u)
		return wb_msi_power_off(m, m->power_off_long_time);
	rc = switch_byte(m, index, value);
	if (rc == WB_MSI_OK && index == WB_EXT_CSD_BUS_WIDTH) {
		/* The host follows once the busy period has ended. */
		uint32_t w = value & 0xFu;

		m->width = w == 2u || w == 6u ? 8u : w == 1u || w == 5u ? 4u :
			   1u;
		wb_mmr_write32(MSI_CTYPE, m->width == 8u ? MSI_CTYPE_WIDBYTE0 :
			       m->width == 4u ? 1u : 0);
		wb_mmr_sync();
	}
	return rc;
}

int wb_msi_emmc_select(void *ctx, unsigned part)
{
	if (part > 0xFFu)
		return WB_MSI_ERR_ARG;
	return wb_msi_emmc_switch(ctx, WB_EXT_CSD_PART_CONFIG, (uint8_t)part);
}

static uint32_t max_blocks(const struct wb_msi *m)
{
	return m->ndesc * WB_MSI_DESC_BLOCKS;
}

int wb_msi_emmc_read(void *ctx, uint32_t sector, uint32_t count,
		     uint8_t *buf)
{
	struct wb_msi *m = ctx;
	int rc = WB_MSI_OK;

	if (count == 0 || !buf_ok(buf))
		return refuse_buf(m, count == 1u ? 17u : 18u, buf);
	while (count != 0 && rc == WB_MSI_OK) {
		uint32_t n = count < max_blocks(m) ? count : max_blocks(m);

		if (n == 1) {
			rc = data(m, 17, sector, buf, 1, false);
		} else {
			rc = data(m, 18, sector, buf, n, false);
			if (rc == WB_MSI_OK) {
				rc = stop_read(m);
				WB_MSI_PHASE(WB_MSI_PH_STOP);
			}
		}
		sector += n;
		buf += n * WB_MSI_BLOCK;
		count -= n;
	}
	return rc;
}

int wb_msi_emmc_write(void *ctx, uint32_t sector, uint32_t count,
		      const uint8_t *buf)
{
	struct wb_msi *m = ctx;
	int rc = WB_MSI_OK;

	if (count == 0 || !buf_ok(buf))
		return refuse_buf(m, count == 1u ? 24u : 25u, buf);
	while (count != 0 && rc == WB_MSI_OK) {
		uint32_t n = count < max_blocks(m) ? count : max_blocks(m);

		rc = data(m, n == 1 ? 24u : 25u, sector, buf, n, true);
		if (rc == WB_MSI_OK)
			busy_start(m);
		if (rc == WB_MSI_OK && n > 1)
			rc = stop_transmission(m);
		/* No data command until the eMMC is ready (S-13b.054). */
		if (rc == WB_MSI_OK)
			rc = wait_busy_end(m);
		sector += n;
		buf += n * WB_MSI_BLOCK;
		count -= n;
	}
	return rc;
}

int wb_msi_emmc_write_reliable(void *ctx, uint32_t sector, uint32_t count,
			       const uint8_t *buf)
{
	struct wb_msi *m = ctx;
	int rc;

	if (count == 0 || count > max_blocks(m) || count > 0xFFFFu ||
	    !buf_ok(buf))
		return refuse_buf(m, 25u, buf);
	rc = r1(m, 23, WB_MMC_CMD23_RELIABLE | count, 0);
	if (rc == WB_MSI_OK)
		rc = data(m, 25, sector, buf, count, true);
	if (rc == WB_MSI_OK)
		rc = wait_busy_end(m);
	return rc;
}

static bool erase_arg_ok(uint32_t arg)
{
	return arg == WB_MMC_ERASE || arg == WB_MMC_TRIM ||
	       arg == WB_MMC_DISCARD || arg == WB_MMC_SECURE_ERASE ||
	       arg == WB_MMC_SECURE_TRIM1 || arg == WB_MMC_SECURE_TRIM2;
}

int wb_msi_emmc_erase_arg(void *ctx, uint32_t first_sector, uint32_t arg)
{
	struct wb_msi *m = ctx;
	int rc;

	if (!erase_arg_ok(arg))
		return WB_MSI_ERR_ARG;
	rc = r1(m, 35, first_sector, 0);
	if (rc == WB_MSI_OK)
		rc = r1(m, 36, first_sector, 0);
	if (rc == WB_MSI_OK)
		rc = r1(m, 38, arg, 0);
	if (rc == WB_MSI_OK)
		rc = wait_busy_end(m);
	return rc;
}

int wb_msi_emmc_erase(void *ctx, uint32_t first_sector)
{
	return wb_msi_emmc_erase_arg(ctx, first_sector, WB_MMC_ERASE);
}

/* S-13b.051 step 3: CMD1 until power-up is done, at least 1 s. A CMD1 and
 * its R3 are about 100 clocks, some 250 us at 396.8 kHz; the pause between
 * tries is shorter still, so that the end of power-up is seen soon. */
#define CMD1_PAUSE_US 100u

static int op_cond(const struct wb_msi *m)
{
	uint64_t end = wb_time_deadline(m->t, m->c->cmd1_timeout_us);
	uint32_t resp[4] = { 0 };
	bool late;

	do {
		int rc;

		late = wb_time_passed(end);
		rc = wb_msi_cmd(m, 1, WB_MMC_OCR_ARG, WB_MSI_R3, resp);
		if (rc == WB_MSI_OK && (resp[0] & WB_MMC_OCR_BUSY))
			return WB_MSI_OK;
		if (rc != WB_MSI_OK && rc != WB_MSI_ERR_CMD)
			return rc;
		wb_delay_us(m->t, CMD1_PAUSE_US);
	} while (!late);
	return WB_MSI_TIMEOUT;
}

static int bring_up(struct wb_msi *m, uint8_t *ext_csd)
{
	uint8_t type, timing;
	int rc;

	m->rca = WB_MMC_RCA;
	m->width = 1;
	m->hs_timing = 0;
	wb_mmr_write32(MSI_CTYPE, 0);
	rc = wb_msi_set_clock(m, m->c->ident_clkdiv, false);
	if (rc == WB_MSI_OK)
		rc = wb_msi_cmd(m, 0, 0, WB_MSI_R_NONE | MSI_CMD_SENDINIT,
				NULL);
	if (rc == WB_MSI_OK)
		rc = op_cond(m);
	if (rc == WB_MSI_OK)
		rc = wb_msi_cmd(m, 2, 0, WB_MSI_R2, NULL);
	if (rc == WB_MSI_OK)
		rc = r1(m, 3, (uint32_t)m->rca << 16, 0);
	/* S-13b.051: CMD9 SEND_CSD, RCA << 16, an R2. A response with a
	 * timeout, a CRC or an end-bit error fails the step. */
	if (rc == WB_MSI_OK)
		rc = wb_msi_cmd(m, 9, (uint32_t)m->rca << 16, WB_MSI_R2, m->csd);
	if (rc == WB_MSI_OK)
		rc = r1(m, 7, (uint32_t)m->rca << 16, 0);
	if (rc == WB_MSI_OK)
		rc = wait_dat0(m, m->c->busy_timeout_us);
	/* S-13b.052: from CMD7 until HS_TIMING at most 26 MHz; 25 MHz
	 * (MSI_CLKDIV 1) for the bus width's switch and the CMD8s, and to the
	 * end for an eMMC with no high-speed type. Its DAT0 wait follows the
	 * one above with no command between, so it ends at its first read. */
	if (rc == WB_MSI_OK)
		rc = wb_msi_set_clock(m, 1u, false);
	/* S-13b.051 step 7: 8 bits (2), or 4 (1) on a 4-line bus (S-13b.061) */
	if (rc == WB_MSI_OK)
		rc = wb_msi_emmc_switch(m, WB_EXT_CSD_BUS_WIDTH,
					m->c->emmc_width == 4u ? 1u : 2u);
	if (rc == WB_MSI_OK)
		rc = wb_msi_read_ext_csd(m, ext_csd);
	if (rc != WB_MSI_OK)
		return rc;
	m->power_off_long_time = ext_csd[WB_EXT_CSD_POWER_OFF_LONG_TIME];

	/* S-13b.053: DEVICE_TYPE bits 0, 1, 4 and DRIVER_STRENGTH bit 3. */
	type = ext_csd[WB_EXT_CSD_DEVICE_TYPE] & 0x13u;
	if (type == 0)
		return WB_MSI_OK;
	timing = (type & 0x10u) && (ext_csd[WB_EXT_CSD_DRIVER_STR] & 0x08u) ?
		 0x32u : 0x01u;
	rc = wb_msi_emmc_switch(m, WB_EXT_CSD_HS_TIMING, timing);
	if (rc != WB_MSI_OK)
		return rc;
	m->hs_timing = timing;
	/* 50 MHz (bypass) for bit 1 or 4, else 25 MHz. */
	rc = wb_msi_set_clock(m, (type & 0x12u) ? 0u : 1u, true);
	if (rc == WB_MSI_OK)
		rc = wb_msi_read_ext_csd(m, ext_csd);
	return rc;
}

int wb_msi_emmc_init(struct wb_msi *m, uint8_t *ext_csd)
{
	unsigned i;
	int rc;

	if (!buf_ok(ext_csd))
		return WB_MSI_ERR_ARG;
	for (i = 0; i < 4u; i++)
		m->csd[i] = 0;
	rc = bring_up(m, ext_csd);
	/* S-13b.059: nothing of a CSD from a bring-up that failed */
	if (rc != WB_MSI_OK)
		for (i = 0; i < 4u; i++)
			m->csd[i] = 0;
	return rc;
}

uint32_t wb_msi_csd_erase_group(const struct wb_msi *m)
{
	const uint32_t w = m->csd[1];	/* CSD bits 63:32 */

	return (((w >> 10) & 0x1Fu) + 1u) * (((w >> 5) & 0x1Fu) + 1u);
}

/* DAT0 released, then the CIU idle: each wait bounded, the second made
 * whatever the first gave; the first timeout. */
static int idle_wait(const struct wb_msi *m)
{
	int rc = wb_poll32(m->t, MSI_STAT, MSI_STAT_DBUSY, 0,
			   m->c->busy_timeout_us);

	if (wb_poll32(m->t, MSI_CMD, MSI_CMD_STARTCMD, 0,
		      m->c->xfer_timeout_us) != WB_TIME_OK)
		rc = WB_TIME_TIMEOUT;
	return rc;
}

int wb_msi_leave(const struct wb_msi *m, const struct wb_msi_leave *v)
{
	const uint32_t resets = MSI_CTL_FIFORST | MSI_CTL_DMARST;
	int rc = idle_wait(m);

	wb_mmr_write32(MSI_CTL, resets);
	if (wb_poll32(m->t, MSI_CTL, resets, 0, m->c->xfer_timeout_us) !=
	    WB_TIME_OK)
		rc = WB_TIME_TIMEOUT;
	wb_mmr_write32(MSI_BUSMODE, v->busmode);
	wb_mmr_write32(MSI_IDINTEN, 0);
	wb_mmr_write32(MSI_IDSTS, MSI_IDSTS_ALL);
	wb_mmr_write32(MSI_DBADDR, 0);
	wb_mmr_write32(MSI_FIFOTH, v->fifoth);
	wb_mmr_write32(MSI_CDTHRCTL, v->cdthrctl);
	wb_mmr_write32(MSI_IMSK, v->imsk);
	wb_mmr_write32(MSI_ISTAT, 0xFFFFFFFFu);
	wb_mmr_write32(MSI_CTL, v->ctl);
	wb_mmr_sync();
	return rc == WB_TIME_OK ? WB_MSI_OK : WB_MSI_TIMEOUT;
}

int wb_msi_close(const struct wb_msi *m)
{
	const uint32_t resets = MSI_CTL_FIFORST | MSI_CTL_DMARST;
	int rc = idle_wait(m);

	wb_mmr_write32(MSI_BUSMODE, wb_mmr_read32(MSI_BUSMODE) & ~(uint32_t)MSI_BUSMODE_DE);
	wb_mmr_write32(MSI_IDINTEN, 0);
	wb_mmr_write32(MSI_IDSTS, MSI_IDSTS_ALL);
	wb_mmr_write32(MSI_CTL, (wb_mmr_read32(MSI_CTL) &
				 ~(uint32_t)(MSI_CTL_INTEN | MSI_CTL_INTDMAC)) | resets);
	if (wb_poll32(m->t, MSI_CTL, resets, 0, m->c->xfer_timeout_us) != WB_TIME_OK)
		rc = WB_TIME_TIMEOUT;
	wb_mmr_write32(MSI_IMSK, 0);
	wb_mmr_write32(MSI_ISTAT, 0xFFFFFFFFu);
	wb_mmr_write32(MSI_CTL, wb_mmr_read32(MSI_CTL) &
				~(uint32_t)(MSI_CTL_INTEN | MSI_CTL_INTDMAC | resets));
	wb_mmr_sync();
	return rc == WB_TIME_OK ? WB_MSI_OK : WB_MSI_TIMEOUT;
}
