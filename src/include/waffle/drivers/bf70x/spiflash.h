/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_SPIFLASH_H
#define WAFFLE_DRIVERS_BF70X_SPIFLASH_H

/*
 * A serial NOR flash on an SPI controller, with S-13b.013's command set
 * only (READ, RDSR, WREN, SE, PP, REMS, RDID) and 8-bit frames.
 *
 * read, erase and program have struct s02_flash_ops's shapes, so an
 * adapter is an initializer:
 *   { &flash, wb_spiflash_read, wb_spiflash_erase, wb_spiflash_program }
 *
 * Every SE and PP is directly preceded by WREN (S-13b.014) and followed by
 * RDSR frames until WIP = 0 (S-13b.015), bounded by busy_timeout_us
 * (S-13b.017). If that wait gives up, the flash stays marked busy and
 * every later call first waits for WIP = 0 again, sending nothing else.
 * A flash marked busy at the start makes a stage's first frame an RDSR
 * that reads WIP = 0 (S-13b.015).
 *
 * S-13b.017: a READ, a WREN, an SE with its WREN or a PP with its WREN
 * whose transfer ends with a controller error (WB_SPI_ERR_HW) is sent
 * again from its first frame, after an RDSR has read WIP = 0, up to tries
 * tries in all (0 or 1: one try). The caller sets tries while an S-02
 * writer runs and back to one try after it. The receive overrun and the
 * mode fault are the errors the controller reports in these frames: the
 * transmit underrun and collision are not signaled in master mode with
 * the transmit channel initiating, nor with the transmit channel off
 * (HRM, "SPI Error Interrupts"). wb_spiflash_read_at() serves no writer
 * and makes one try.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x/spi.h"

#define WB_SPIFLASH_PAGE   256u
#define WB_SPIFLASH_SECTOR 4096u

#define WB_SPIFLASH_READ   0x03u
#define WB_SPIFLASH_RDSR   0x05u
#define WB_SPIFLASH_WREN   0x06u
#define WB_SPIFLASH_SE     0x20u
#define WB_SPIFLASH_PP     0x02u
#define WB_SPIFLASH_REMS   0x90u
#define WB_SPIFLASH_RDID   0x9Fu
#define WB_SPIFLASH_WIP    0x01u

struct wb_spiflash {
	struct wb_spi *spi;	/* 8-bit words, select through wb_spi_select */
	uint32_t size;		/* every offset is below it */
	uint32_t busy_timeout_us;	/* >= 1 s (S-13b.017) */
	bool busy;
	/* Called between two reads of the status while the part is busy
	 * (an erase takes tens of milliseconds), or NULL. It issues no
	 * flash command. */
	void (*idle)(void);
	uint8_t tries;		/* S-13b.017: tries of a frame, 0 or 1 is one */
};

#define WB_SPIFLASH_OK        0
#define WB_SPIFLASH_ERR_ARG  (-1)
#define WB_SPIFLASH_BUSY     (-2)	/* WIP stayed set */
#define WB_SPIFLASH_ERR_IO   (-3)	/* the SPI transfer failed */
#define WB_SPIFLASH_ERR_HW   (-4)	/* the controller reported an error in a
					 * frame, on every try */

int wb_spiflash_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len);
/*
 * The same read with the bus at SCLK0 / (clk + 1) and in fast mode
 * (wb_spi_clock()) for its frames, and back at its usual rate and sampling
 * afterwards (S-13b.011 bounds the rate; the caller chooses it), for a read
 * of many bytes: READ frames (S-13b.013) of up to
 * WB_SPIFLASH_FAST_FRAME bytes, each with its own address, the data
 * clocked in by the receive channel under a word count, so that the core
 * only empties the receive FIFO. No memory-mapped read (S-13b.019).
 */
#define WB_SPIFLASH_FAST_FRAME 0x8000u
int wb_spiflash_read_at(void *ctx, uint32_t clk, uint32_t off, uint8_t *buf,
			uint32_t len);
/* wb_spiflash_read_at() with each frame's data moved by the receive DMA
 * channel (wb_spi_recv_dma()) rather than the core. */
int wb_spiflash_read_dma(void *ctx, uint32_t clk, uint32_t off, uint8_t *buf,
			 uint32_t len);
/* sector_off is the first byte of a 4 KiB sector. */
int wb_spiflash_erase(void *ctx, uint32_t sector_off);
/* 1 to 256 bytes within one page. */
int wb_spiflash_program(void *ctx, uint32_t off, const uint8_t *buf,
			uint32_t len);

int wb_spiflash_status(const struct wb_spiflash *f, uint8_t *sr);

#endif /* WAFFLE_DRIVERS_BF70X_SPIFLASH_H */
