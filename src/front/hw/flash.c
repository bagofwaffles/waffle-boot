/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SPI flash on SPI2, polled: the front's own copy of wb_spi_init() with
 * opxy_spi_flash, of wb_spi_xfer() and of the serial NOR commands of
 * spiflash.c (S-13b.010-S-13b.017). Every wait is bounded on the core
 * cycle counter at 400 MHz (S-13a.001). Target only.
 */
#include "front/hw/hw.h"
#include "front/slots/slots.h"

#include "waffle/drivers/bf70x/core.h"
#include "waffle/drivers/bf70x/spi.h"
#include "waffle/drivers/mmr.h"

#define FLASH_SPI 2u
#define FLASH_SEL 1u		/* SPI_SEL1, under software control */
/* S-13b.010, S-13b.011: master, 8-bit, MSB first, mode 3 so that the
 * software chip select frames every command; BAUD 3 is SCLK0 / 4. */
#define FLASH_CTL  (SPI_CTL_MSTR | SPI_CTL_CPOL | SPI_CTL_CPHA | SPI_CTL_SIZE8)
#define FLASH_BAUD 3u
#define FLASH_SIZE 0x100000u	/* S-02.001 */
/* The longest one byte may take on the bus (OPXY_SPI_WORD_US). */
#define WORD_US    1000u
/* S-13b.017: a busy wait gives up after at least 1 s (OPXY_FLASH_BUSY_US). */
#define BUSY_US    2000000u

/* S-13b.013 */
#define OP_READ 0x03u
#define OP_PP   0x02u
#define OP_SE   0x20u
#define OP_RDSR 0x05u
#define OP_WREN 0x06u
#define SR_WIP  0x01u

/* An erase or program may still run: wait before the next frame. */
static bool busy;
/* The last frame ended with a controller error (S-13b.017). */
static bool ctl_err;
/*
 * S-13b.017: the front has erased or programmed. Every erase and program it
 * makes is an S-02 writer's, so from its first one on it writes a slot or
 * a record, and a READ is tried up to three times; before it, one try.
 */
static bool writing;

static uint64_t deadline(uint32_t us)
{
	return wb_core_cycles() + (uint64_t)us * FR_CCLK_MHZ;
}

/* Silicon anomaly 19000038: SPI_SLVSEL is written twice, back to back. */
static void write_slvsel(uint32_t v)
{
	wb_mmr_write32(SPI_SLVSEL(FLASH_SPI), v);
	wb_mmr_write32(SPI_SLVSEL(FLASH_SPI), v);
}

void fr_hw_flash_start(void)
{
	wb_mmr_write32(SPI_CTL(FLASH_SPI), 0u);
	write_slvsel(SPI_SLVSEL_IDLE | SPI_SLVSEL_SSE(FLASH_SEL));
	wb_mmr_write32(SPI_CLK(FLASH_SPI), FLASH_BAUD);
	wb_mmr_write32(SPI_DLY(FLASH_SPI), 0u);
	wb_mmr_write32(SPI_RXCTL(FLASH_SPI), 0u);
	wb_mmr_write32(SPI_TXCTL(FLASH_SPI), 0u);
	wb_mmr_write32(SPI_IMSK_CLR(FLASH_SPI), 0xFFFFu);
	wb_mmr_write32(SPI_STAT(FLASH_SPI), SPI_STAT_W1C);
	wb_mmr_write32(SPI_CTL(FLASH_SPI), FLASH_CTL | SPI_CTL_EN);
	wb_mmr_sync();
	/* Nothing is known of the part yet: the first frame waits on RDSR. */
	busy = true;
}

static void chip_select(bool on)
{
	uint32_t v = wb_mmr_read32(SPI_SLVSEL(FLASH_SPI));

	if (on)
		v &= ~SPI_SLVSEL_SSEL(FLASH_SEL);
	else
		v |= SPI_SLVSEL_SSEL(FLASH_SEL);
	write_slvsel(v);
	wb_mmr_sync();
}

/* A STAT bit clear within one byte's time. */
static bool wait_clear(uint32_t bit)
{
	const uint64_t end = deadline(WORD_US);

	while (wb_mmr_read32(SPI_STAT(FLASH_SPI)) & bit)
		if (wb_core_cycles() > end)
			return (wb_mmr_read32(SPI_STAT(FLASH_SPI)) & bit) == 0u;
	return true;
}

/* n bytes out of tx (zeros without) and into rx (dropped without), both
 * channels on, one byte in flight. A receive overrun or mode fault fails. */
static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
	bool ok = true;
	uint32_t stat;

	wb_mmr_write32(SPI_STAT(FLASH_SPI), SPI_STAT_W1C);
	/* The non-initiating channel first (HRM "SPI Programming"). */
	wb_mmr_write32(SPI_RXCTL(FLASH_SPI), SPI_XCTL_EN);
	wb_mmr_write32(SPI_TXCTL(FLASH_SPI), SPI_XCTL_EN | SPI_XCTL_TI);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t v;

		if ((i & 15u) == 15u)
			fr_serve();	/* one byte in flight: about 1 us each */
		if (!wait_clear(SPI_STAT_TFF)) {
			ok = false;
			break;
		}
		wb_mmr_write32(SPI_TFIFO(FLASH_SPI), tx != NULL ? tx[i] : 0u);
		if (!wait_clear(SPI_STAT_RFE)) {
			ok = false;
			break;
		}
		v = wb_mmr_read32(SPI_RFIFO(FLASH_SPI));
		if (rx != NULL)
			rx[i] = (uint8_t)v;
	}
	stat = wb_mmr_read32(SPI_STAT(FLASH_SPI));
	wb_mmr_write32(SPI_RXCTL(FLASH_SPI), 0u);
	wb_mmr_write32(SPI_TXCTL(FLASH_SPI), 0u);
	wb_mmr_write32(SPI_STAT(FLASH_SPI), stat & SPI_STAT_W1C);
	wb_mmr_sync();
	ctl_err = ok && (stat & (SPI_STAT_ROR | SPI_STAT_MF)) != 0u;
	return ok && !ctl_err;
}

/* S-13b.010: the chip select low for exactly one frame: the head, then
 * len data bytes out of tx or into rx. */
static bool frame(const uint8_t *head, uint32_t nhead, const uint8_t *tx, uint8_t *rx,
		  uint32_t len)
{
	bool ok;

	chip_select(true);
	ok = xfer(head, NULL, nhead);
	if (ok && len != 0u)
		ok = xfer(tx, rx, len);
	chip_select(false);
	return ok;
}

static bool frame_addr(uint8_t op, uint32_t off, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	const uint8_t head[4] = {op, (uint8_t)(off >> 16), (uint8_t)(off >> 8), (uint8_t)off};

	return frame(head, sizeof head, tx, rx, len);
}

/* S-13b.015, S-13b.017: RDSR until WIP = 0, at least once, bounded. */
static bool wait_ready(void)
{
	static const uint8_t op = OP_RDSR;
	const uint64_t end = deadline(BUSY_US);
	bool late;

	do {
		uint8_t sr = SR_WIP;

		fr_serve();
		late = wb_core_cycles() > end;
		if (!frame(&op, 1u, NULL, &sr, 1u))
			break;
		if ((sr & SR_WIP) == 0u) {
			busy = false;
			return true;
		}
	} while (!late);
	busy = true;
	return false;
}

static bool idle(void)
{
	return !busy || wait_ready();
}

static bool in_flash(uint32_t off, uint32_t len)
{
	return off < FLASH_SIZE && len <= FLASH_SIZE - off;
}

/* S-13b.017: a READ that ends with a controller error is sent again after
 * WIP = 0, up to three tries while writing, one otherwise. FR_FLASH_BUSY
 * when the wait before it gave up. */
static int hw_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (len == 0u || !in_flash(off, len))
		return FR_FLASH_IO;
	if (!idle())
		return FR_FLASH_BUSY;
	for (unsigned t = 1u;; t++) {
		if (frame_addr(OP_READ, off, NULL, buf, len))
			return FR_FLASH_OK;
		if (!ctl_err || t >= (writing ? 3u : 1u) || !wait_ready())
			return FR_FLASH_IO;
	}
}

/* S-13b.014: WREN directly before the write frame, then the wait.
 * S-13b.017: a try that ends with a controller error is made again from
 * its WREN, after WIP = 0, up to three tries. */
static int write_op(uint8_t op, uint32_t off, const uint8_t *buf, uint32_t len)
{
	static const uint8_t wren = OP_WREN;

	writing = true;
	if (!idle())
		return FR_FLASH_IO;
	for (unsigned t = 1u;; t++) {
		if (frame(&wren, 1u, NULL, NULL, 0u) && frame_addr(op, off, buf, NULL, len))
			return wait_ready() ? FR_FLASH_OK : FR_FLASH_IO;
		busy = true;	/* the write may have started */
		if (!ctl_err || t >= 3u || !wait_ready())
			return FR_FLASH_IO;
	}
}

/* S-13b.016: a sector's first byte. */
static int hw_erase(void *ctx, uint32_t sector_off)
{
	(void)ctx;
	if (sector_off % FR_SECTOR != 0u || !in_flash(sector_off, FR_SECTOR))
		return FR_FLASH_IO;
	return write_op(OP_SE, sector_off, NULL, 0u);
}

/* S-13b.016: within one page. */
static int hw_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (len == 0u || off % FR_PAGE + len > FR_PAGE || !in_flash(off, len))
		return FR_FLASH_IO;
	return write_op(OP_PP, off, buf, len);
}

const struct fr_flash fr_hw_flash = {hw_read, hw_erase, hw_program, NULL};
