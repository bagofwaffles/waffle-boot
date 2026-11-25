/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Serial NOR flash commands (see waffle/drivers/bf70x/spiflash.h). */
#include "waffle/drivers/bf70x/spiflash.h"

#include <stddef.h>

/*
 * One frame: select, send head, then move len data bytes (out of tx or
 * into rx; neither for a head-only frame), deselect. WB_SPIFLASH_ERR_HW
 * when a transfer ended with a controller error (S-13b.017).
 */
static int frame(const struct wb_spiflash *f, const uint8_t *head, size_t nhead,
		 const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	int rc;

	if (wb_spi_word_bytes(f->spi) != 1u)
		return WB_SPIFLASH_ERR_ARG;
	wb_spi_select(f->spi, true);
	rc = wb_spi_xfer(f->spi, head, NULL, nhead);
	if (rc == WB_SPI_OK && len != 0)
		rc = wb_spi_xfer(f->spi, tx, rx, len);
	wb_spi_select(f->spi, false);
	if (rc == WB_SPI_OK)
		return WB_SPIFLASH_OK;
	return rc == WB_SPI_ERR_HW ? WB_SPIFLASH_ERR_HW : WB_SPIFLASH_ERR_IO;
}

static int frame_addr(const struct wb_spiflash *f, uint8_t op, uint32_t off,
		      const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	const uint8_t head[4] = {
		op, (uint8_t)(off >> 16), (uint8_t)(off >> 8), (uint8_t)off,
	};

	return frame(f, head, sizeof(head), tx, rx, len);
}

int wb_spiflash_status(const struct wb_spiflash *f, uint8_t *sr)
{
	static const uint8_t op = WB_SPIFLASH_RDSR;

	return frame(f, &op, 1, NULL, sr, 1);
}

/* RDSR until WIP = 0, at least one, bounded (S-13b.015, .017). */
static int wait_ready(struct wb_spiflash *f)
{
	uint64_t end = wb_time_deadline(f->spi->t, f->busy_timeout_us);
	uint8_t sr = WB_SPIFLASH_WIP;
	bool late;
	int rc;

	do {
		late = wb_time_passed(end);
		rc = wb_spiflash_status(f, &sr);
		if (rc != WB_SPIFLASH_OK)
			break;
		if ((sr & WB_SPIFLASH_WIP) == 0) {
			f->busy = false;
			return WB_SPIFLASH_OK;
		}
		if (f->idle != NULL)
			f->idle();
	} while (!late);
	f->busy = true;
	return rc != WB_SPIFLASH_OK ? rc : WB_SPIFLASH_BUSY;
}

static int idle(struct wb_spiflash *f)
{
	if (!f->busy)
		return WB_SPIFLASH_OK;
	return wait_ready(f);
}

/*
 * S-13b.017: whether try n (from 1) that ended with rc is followed by
 * another, after an RDSR has read WIP = 0 (S-13b.015). *rc becomes the
 * wait's failure when that wait fails.
 */
static bool again(struct wb_spiflash *f, unsigned n, int *rc)
{
	int w;

	if (*rc != WB_SPIFLASH_ERR_HW || n >= (f->tries > 1u ? f->tries : 1u))
		return false;
	w = wait_ready(f);
	if (w != WB_SPIFLASH_OK) {
		*rc = w;
		return false;
	}
	return true;
}

static bool in_flash(const struct wb_spiflash *f, uint32_t off, uint32_t len)
{
	return off < f->size && len <= f->size - off;
}

int wb_spiflash_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	struct wb_spiflash *f = ctx;
	unsigned n = 1;
	int rc;

	if (len == 0 || !in_flash(f, off, len))
		return WB_SPIFLASH_ERR_ARG;
	rc = idle(f);
	if (rc != WB_SPIFLASH_OK)
		return rc;
	do
		rc = frame_addr(f, WB_SPIFLASH_READ, off, NULL, buf, len);
	while (again(f, n++, &rc));
	return rc;
}

static int read_fast(void *ctx, uint32_t clk, uint32_t off, uint8_t *buf, uint32_t len,
		     bool dma)
{
	struct wb_spiflash *f = ctx;
	int rc;

	if (len == 0 || !in_flash(f, off, len) || wb_spi_word_bytes(f->spi) != 1u)
		return WB_SPIFLASH_ERR_ARG;
	rc = idle(f);
	if (rc != WB_SPIFLASH_OK)
		return rc;
	/*
	 * The controller is disabled and enabled again here, after each frame
	 * and at the end: always between frames, with the chip select high, as
	 * wb_spi_init() and wb_spi_clock() are used. These frames run in fast
	 * mode, the full-cycle receive timing the HRM asks for at this rate;
	 * every other frame keeps the configured control value and clock.
	 * The last bit of a word is then latched by an edge the controller
	 * makes itself, not one on SCK; the select stays low until the
	 * receive has returned its last byte, so the part still drives it.
	 */
	if (wb_spi_clock(f->spi, clk, true) != WB_SPI_OK)
		return WB_SPIFLASH_ERR_ARG;
	while (len != 0u && rc == WB_SPI_OK) {
		const uint32_t k = len < WB_SPIFLASH_FAST_FRAME ? len : WB_SPIFLASH_FAST_FRAME;
		const uint8_t head[4] = {
			WB_SPIFLASH_READ, (uint8_t)(off >> 16), (uint8_t)(off >> 8), (uint8_t)off,
		};

		wb_spi_select(f->spi, true);
		rc = wb_spi_xfer(f->spi, head, NULL, sizeof(head));
		if (rc == WB_SPI_OK)
			rc = dma ? wb_spi_recv_dma(f->spi, buf, k) : wb_spi_recv(f->spi, buf, k);
		wb_spi_select(f->spi, false);
		off += k;
		buf += k;
		len -= k;
		/* nothing of a receive is left for the next frame's opcode; after
		 * the last frame, or a failed one, the usual rate and sampling */
		(void)wb_spi_clock(f->spi, len != 0u && rc == WB_SPI_OK ? clk : f->spi->clk,
				   len != 0u && rc == WB_SPI_OK);
	}
	return rc == WB_SPI_OK ? WB_SPIFLASH_OK : WB_SPIFLASH_ERR_IO;
}

int wb_spiflash_read_at(void *ctx, uint32_t clk, uint32_t off, uint8_t *buf,
			uint32_t len)
{
	return read_fast(ctx, clk, off, buf, len, false);
}

int wb_spiflash_read_dma(void *ctx, uint32_t clk, uint32_t off, uint8_t *buf,
			 uint32_t len)
{
	return read_fast(ctx, clk, off, buf, len, true);
}

/* WREN, the write frame, then wait (S-13b.014, .015); a failed try is
 * repeated from its WREN (S-13b.017). */
static int write_op(struct wb_spiflash *f, uint8_t op, uint32_t off,
		    const uint8_t *buf, uint32_t len)
{
	static const uint8_t wren = WB_SPIFLASH_WREN;
	unsigned n = 1;
	int rc;

	rc = idle(f);
	if (rc != WB_SPIFLASH_OK)
		return rc;
	do {
		rc = frame(f, &wren, 1, NULL, NULL, 0);
		if (rc == WB_SPIFLASH_OK)
			rc = frame_addr(f, op, off, buf, NULL, len);
		if (rc == WB_SPIFLASH_OK)
			return wait_ready(f);
		/* The write may have started. */
		f->busy = true;
	} while (again(f, n++, &rc));
	return rc;
}

int wb_spiflash_erase(void *ctx, uint32_t sector_off)
{
	struct wb_spiflash *f = ctx;

	if (sector_off % WB_SPIFLASH_SECTOR != 0 ||
	    !in_flash(f, sector_off, WB_SPIFLASH_SECTOR))
		return WB_SPIFLASH_ERR_ARG;
	return write_op(f, WB_SPIFLASH_SE, sector_off, NULL, 0);
}

int wb_spiflash_program(void *ctx, uint32_t off, const uint8_t *buf,
			uint32_t len)
{
	struct wb_spiflash *f = ctx;

	if (len == 0 || off % WB_SPIFLASH_PAGE + len > WB_SPIFLASH_PAGE ||
	    !in_flash(f, off, len))
		return WB_SPIFLASH_ERR_ARG;
	return write_op(f, WB_SPIFLASH_PP, off, buf, len);
}
