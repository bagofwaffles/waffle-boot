/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-05.014, S-05.016 on the board (src/board/opxy_handover.c): the SEC
 * sources, the slave's MSI controller and what the OS gets at its first
 * instruction, on each CPU.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/opxy_boot.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_dmc.h"
#include "waffle/drivers/bf70x_sec.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/drivers/bf70x_usb.h"

/* Every source enabled and pending, with a priority and edge set. */
#define FOUND (0x300u | SEC_SCTL_ES | SEC_SCTL_SEN | SEC_SCTL_IEN)

/* MSI: resets finish at once, the status registers are W1C. */
static bool hook(void *ctx, enum fake_mmr_op op, unsigned width,
		 wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op != FAKE_MMR_WRITE)
		return true;
	if (addr == MSI_CTL)
		*value &= ~MSI_CTL_RESETS;
	if (addr == MSI_ISTAT || addr == MSI_IDSTS) {
		fake_mmr_set(addr, fake_mmr_get(addr) & ~*value);
		return false;
	}
	return true;
}

static void reset(void)
{
	uint32_t sid;

	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set_hook(hook, NULL);
	for (sid = 0; sid < WB_SEC_SOURCES; sid++) {
		fake_mmr_set(SEC0_SCTL(sid), FOUND);
		fake_mmr_set(SEC0_SSTAT(sid), SEC_SSTAT_PND);
	}
	fake_mmr_set(DMC0_STAT, DMC_STAT_IDLE);		/* the DDR controller idle */
	memset(&opxy_handover_os, 0, sizeof(opxy_handover_os));
}

static const struct opxy_sec_keep *kept(const struct opxy_sec_keep *k,
					unsigned n, uint32_t sid)
{
	unsigned i;

	for (i = 0; i < n; i++)
		if (k[i].sid == sid)
			return &k[i];
	return NULL;
}

static bool written(wb_mmr_addr addr, uint32_t value)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == addr &&
		    log[i].value == value)
			return true;
	return false;
}

static bool touched(wb_mmr_addr addr)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == addr)
			return true;
	return false;
}

/* The log index of the first write of value to addr, or -1. */
static int write_index(wb_mmr_addr addr, uint32_t value)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == addr &&
		    log[i].value == value)
			return (int)i;
	return -1;
}

/* USB as a held mode leaves it: the PHY on, the pull-up on. */
static void usb_on(void)
{
	fake_mmr_set(USB0_PHY_CTL, USB_PHY_CTL_EN);
	fake_mmr_set(USB0_POWER, USB_POWER_HSEN | USB_POWER_SOFTCONN);
}

/* S-05.016's USB row: the pull-up off, then the PHY off (S-10a.030). */
static void check_usb_off(void)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;
	int pullup = -1, phy = -1;

	CHECK((fake_mmr_get(USB0_POWER) & USB_POWER_SOFTCONN) == 0u);
	CHECK((fake_mmr_get(USB0_PHY_CTL) & USB_PHY_CTL_EN) == 0u);
	for (i = 0; i < fake_mmr_count(); i++) {
		if (log[i].op != FAKE_MMR_WRITE)
			continue;
		if (pullup < 0 && log[i].addr == USB0_POWER &&
		    !(log[i].value & USB_POWER_SOFTCONN))
			pullup = (int)i;
		if (phy < 0 && log[i].addr == USB0_PHY_CTL &&
		    !(log[i].value & USB_PHY_CTL_EN))
			phy = (int)i;
	}
	CHECK(pullup >= 0 && phy > pullup);
}

/* S-05.016's SEC row after an OS teardown. */
static void check_sec(const struct opxy_sec_keep *k, unsigned n)
{
	uint32_t sid;

	for (sid = 0; sid < WB_SEC_SOURCES; sid++) {
		const struct opxy_sec_keep *e = kept(k, n, sid);

		CHECK(fake_mmr_get(SEC0_SCTL(sid)) ==
		      (e != NULL ? e->sctl : FOUND & ~(SEC_SCTL_IEN | SEC_SCTL_SEN)));
		CHECK(written(SEC0_SSTAT(sid), SEC_SSTAT_PND));
		/* A kept source's pending bit is cleared before it is enabled,
		 * so it cannot latch to the core between the two writes. */
		if (e != NULL) {
			int pnd = write_index(SEC0_SSTAT(sid), SEC_SSTAT_PND);
			int ctl = write_index(SEC0_SCTL(sid), e->sctl);

			CHECK(pnd >= 0 && ctl >= 0 && pnd < ctl);
		}
	}
}

/* S-05.014, S-05.031: SP and FP in S-05.031's DDR range, the vectors in L1 code. */
static void check_entry(const struct opxy_os_entry *e)
{
	unsigned i;

	CHECK(e->sp % 4u == 0 && e->sp >= 0x88A65000u && e->sp < 0x88A66000u);
	CHECK(e->fp % 4u == 0 && e->fp > e->sp && e->fp < 0x88A66000u);
	for (i = 0; i < OPXY_OS_EVTS; i++)
		CHECK(e->evt[i] >= 0x11A00000u && e->evt[i] < 0x11A04000u);
	CHECK(memcmp(&opxy_handover_os, e, sizeof(*e)) == 0);
}

static void test_master(void)
{
	static struct opxy_master m;
	int dma11, uart0;
	unsigned ch;

	reset();
	/* The PSoC link open (S-13b.070): UART0 on, DMA11 running. */
	fake_mmr_set(UART_CTL(OPXY_UART_PSOC), 0x00000301u);
	fake_mmr_set(UART_IMSK(OPXY_UART_PSOC), UART_IMSK_ERBFI);
	fake_mmr_set(DMA_CFG(WB_DMA_UART0_RX), 0x00001007u);
	fake_mmr_set(SPI_CTL(OPXY_SPI_FLASH), SPI_CTL_MSTR | SPI_CTL_EN);
	usb_on();
	opxy_teardown_master(&m);
	/* S-05.016: SPI2 off with its master-mode bit set; USB off. */
	CHECK(fake_mmr_get(SPI_CTL(OPXY_SPI_FLASH)) == 0x00000002u);
	check_usb_off();
	/* S-13b.072, S-13b.082, S-05.016: both UARTs and DMA10-DMA13 off,
	 * DMA11 stopped before UART0 was disabled. */
	CHECK(fake_mmr_get(UART_CTL(OPXY_UART_PSOC)) == 0);
	CHECK(fake_mmr_get(UART_CTL(OPXY_UART_NORDIC)) == 0);
	for (ch = WB_DMA_UART0_TX; ch <= WB_DMA_UART1_RX; ch++)
		CHECK((fake_mmr_get(DMA_CFG(ch)) & DMA_CFG_EN) == 0);
	dma11 = write_index(DMA_CFG(WB_DMA_UART0_RX), 0);
	uart0 = write_index(UART_CTL(OPXY_UART_PSOC), 0);
	CHECK(dma11 >= 0 && uart0 > dma11);
	check_sec(opxy_sec_keep_master, OPXY_SEC_KEEP_MASTER);
	CHECK(fake_mmr_get(SEC0_SCTL(72)) == (FOUND & ~(SEC_SCTL_IEN | SEC_SCTL_SEN)));
	check_entry(&opxy_os_entry_master);
	CHECK(!touched(MSI_CTL));
	CHECK(fake_mmr_errors() == 0);
}

/* The log index of the last access of op to addr, or -1. */
static int last_index(enum fake_mmr_op op, wb_mmr_addr addr)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	int at = -1;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == op && log[i].addr == addr)
			at = (int)i;
	return at;
}

/*
 * S-13b.072: a log line queued for the PSoC after the last drain leaves the
 * pin in the teardown, before UART0_CTL = 0.
 */
static void test_master_uart_drain(void)
{
	static struct opxy_master m;
	static uint8_t rx[64], tx[64];
	static const uint8_t line[5] = { 'l', 'o', 'g', '\r', '\n' };
	int thr, stat, ctl;

	reset();
	memset(&m, 0, sizeof(m));
	m.hw.time.cycles_per_us = 400u;
	CHECK(wb_uart_init(&m.hw.uart[OPXY_UART_PSOC], OPXY_UART_PSOC,
			   &opxy_uart_psoc, &m.hw.time, rx, sizeof(rx), tx,
			   sizeof(tx)) == WB_UART_OK);
	fake_mmr_set(UART_STAT(OPXY_UART_PSOC), 0);	/* the transmitter busy */
	wb_uart_write(&m.hw.uart[OPXY_UART_PSOC], line, sizeof(line));
	CHECK(last_index(FAKE_MMR_WRITE, UART_THR(OPXY_UART_PSOC)) < 0);
	fake_mmr_set(UART_STAT(OPXY_UART_PSOC), UART_STAT_THRE | UART_STAT_TEMT);
	opxy_teardown_master(&m);
	thr = last_index(FAKE_MMR_WRITE, UART_THR(OPXY_UART_PSOC));
	stat = last_index(FAKE_MMR_READ, UART_STAT(OPXY_UART_PSOC));
	ctl = last_index(FAKE_MMR_WRITE, UART_CTL(OPXY_UART_PSOC));
	CHECK(m.hw.uart[OPXY_UART_PSOC].tx_head ==
	      m.hw.uart[OPXY_UART_PSOC].tx_tail);
	CHECK(thr >= 0 && stat > thr && ctl > stat);
	CHECK(fake_mmr_get(UART_CTL(OPXY_UART_PSOC)) == 0);
}

static void test_slave(void)
{
	static struct opxy_slave s;

	reset();
	memset(&s, 0, sizeof(s));
	s.hw.time.cycles_per_us = 400u;
	s.hw.msi.c = &opxy_msi;
	s.hw.msi.t = &s.hw.time;
	s.emmc_up = true;
	fake_mmr_set(MSI_CTL, MSI_CTL_INTDMAC);
	fake_mmr_set(MSI_BUSMODE, MSI_BUSMODE_DE | MSI_BUSMODE_FB);
	fake_mmr_set(MSI_DBADDR, 0x8813267Cu);
	fake_mmr_set(MSI_FIFOTH, opxy_msi.fifoth);
	fake_mmr_set(MSI_ISTAT, MSI_IS_DTO | MSI_IS_CMDDONE);
	fake_mmr_set(MSI_IDSTS, 0x3u);
	fake_mmr_set(MSI_CLKDIV, 1u);
	fake_mmr_set(MSI_CLKEN, MSI_CLKEN_EN0 | MSI_CLKEN_LP0);
	fake_mmr_set(MSI_CTYPE, MSI_CTYPE_WIDBYTE0);
	fake_mmr_set(MSI_TMOUT, opxy_msi.tmout);
	usb_on();
	opxy_teardown_slave(&s);
	check_usb_off();

	/* The controller: S-05.016's slave MSI row; the eMMC untouched. */
	CHECK(fake_mmr_get(MSI_CTL) == 0x00000010u);
	CHECK(fake_mmr_get(MSI_BUSMODE) == 0x00000600u);
	CHECK(fake_mmr_get(MSI_IMSK) == 0x0000FFC2u);
	CHECK(fake_mmr_get(MSI_FIFOTH) == 0x607F0080u);
	CHECK(fake_mmr_get(MSI_CDTHRCTL) == 0x02000001u);
	CHECK(fake_mmr_get(MSI_DBADDR) == 0);
	CHECK(fake_mmr_get(MSI_ISTAT) == 0 && fake_mmr_get(MSI_IDSTS) == 0);
	CHECK(fake_mmr_get(MSI_CLKDIV) == 1u);
	CHECK(fake_mmr_get(MSI_CLKEN) == (MSI_CLKEN_EN0 | MSI_CLKEN_LP0));
	CHECK(fake_mmr_get(MSI_CTYPE) == MSI_CTYPE_WIDBYTE0);
	CHECK(fake_mmr_get(MSI_TMOUT) == opxy_msi.tmout);
	CHECK(!touched(MSI_CMD) && !touched(MSI_CLKDIV) && !touched(MSI_CLKEN));
	CHECK(written(MSI_CTL, MSI_CTL_FIFORST | MSI_CTL_DMARST));

	check_sec(opxy_sec_keep_slave, OPXY_SEC_KEEP_SLAVE);
	CHECK(fake_mmr_get(SEC0_SCTL(72)) == 0x00000005u);
	check_entry(&opxy_os_entry_slave);
	CHECK(fake_mmr_errors() == 0);

	/* A slave whose eMMC never came up leaves the controller alone. */
	reset();
	s.emmc_up = false;
	opxy_teardown_slave(&s);
	CHECK(!touched(MSI_CTL));
	check_sec(opxy_sec_keep_slave, OPXY_SEC_KEEP_SLAVE);
}

int main(void)
{
	RUN(test_master);
	RUN(test_master_uart_drain);
	RUN(test_slave);
	return check_summary("test_board_handover");
}
