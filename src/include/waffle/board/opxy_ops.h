/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_BOARD_OPXY_OPS_H
#define WAFFLE_BOARD_OPXY_OPS_H

/*
 * The OP-XY's driver-level operation structs: the S-xx logic's hardware
 * ops (S-02 flash, S-03 eMMC, S-06 ports and clock, S-09 buses, S-11 OTP
 * keys, S-14 clock) filled from the bf70x drivers and the board values of
 * waffle/board/opxy.h. Protocol-level ops (S-04, S-05, S-07, S-08, S-10c,
 * S-11 runs, S-14 links, S-15) are built by the boot flow over these.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/board/opxy.h"
#include "waffle/drivers/bf70x_msi.h"
#include "waffle/drivers/bf70x_otp.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/drivers/bf70x_spiflash.h"
#include "waffle/drivers/bf70x_time.h"
#include "waffle/drivers/bf70x_uart.h"
#include "waffle/s02_flash.h"
#include "waffle/s03_emmc.h"
#include "waffle/s06_clock.h"
#include "waffle/s06_link.h"
#include "waffle/s09_fast.h"
#include "waffle/s09_master.h"
#include "waffle/s11_package.h"
#include "waffle/s14_log.h"
#include "waffle/testos.h"

/* The drivers' state. One instance; each role uses its own part of it. */
struct opxy_hw {
	struct wb_time time;		/* opxy_time, writable for the ops' ctx */
	struct wb_spi flash_spi;	/* master: SPI2 to the flash */
	struct wb_spiflash flash;
	struct wb_spi link_spi;		/* master: SPI0 */
	struct wb_spi_slave link_slave;	/* slave: SPI2, PA4 ready, PB15 */
	struct wb_msi msi;		/* both roles' eMMC */
	struct wb_uart uart[2];		/* master: PSoC, Nordic */
};

/* S-02 over the serial flash driver. */
struct s02_flash_ops opxy_flash_ops(struct opxy_hw *hw);
/* S-13b.017: on while one of S-02's writers runs, a READ, WREN, SE or PP
 * that ends with a controller error is sent up to OPXY_FLASH_WRITER_TRIES
 * times in all; off, once. */
#define OPXY_FLASH_WRITER_TRIES 3u
void opxy_flash_writer(struct opxy_hw *hw, bool on);
/* A read of many bytes at S-13b.011's highest rate, for the slot's stream
 * the slave is booted with (an s02_read_fn; ctx is the hw's flash). */
int opxy_flash_read_stream(void *ctx, uint32_t off, uint8_t *buf, uint32_t len);
/* S-03 over the MSI driver. */
struct s03_emmc_ops opxy_emmc_ops(struct opxy_hw *hw);
/* S-13b.059: an eMMC whose bring-up failed. Every operation returns
 * OPXY_EMMC_DOWN at once and sends nothing. */
#define OPXY_EMMC_DOWN (-100)
extern const struct s03_emmc_ops opxy_emmc_down_ops;
/* S-06: one UART as a link's port (ctx: the struct wb_uart). */
extern const struct s06_port_ops opxy_port_ops;
/* S-06's and S-14's time base: the cycle counter at the nominal CCLK. */
struct s06_clock opxy_clock(struct opxy_hw *hw);
/* wire_crc32() over a long buffer, with USB serviced as it goes. */
uint32_t opxy_crc32_long(uint32_t crc, const uint8_t *buf, uint32_t len);
/* S-02.016's slot check and S-03.044's OS check on the CRC peripheral
 * (opxy_crc32_fast()): crc32_fast of the master's flash and eMMC, with USB
 * serviced as it goes, and of the slave's eMMC, quiet. */
uint32_t opxy_crc32_check(uint32_t crc, const uint8_t *buf, uint32_t len);
uint32_t opxy_crc32_check_quiet(uint32_t crc, const uint8_t *buf, uint32_t len);
struct s14_clock_ops opxy_log_clock(struct opxy_hw *hw);
/* S-09.002 boot link (SPI0 SEL3, PA6 ready) and S-09 bridge (SPI0, PC11
 * select, PA6 ready, PC5 attention) on the master. opxy_bus_ops() also
 * sets the bridge controller's idle hook to opxy_usb_breathe(). The bus's
 * configure(0) and configure(1) set SPI0 to the pre-link set-up; after any
 * failed operation of the session, or with DMA4 or DMA5 still enabled, as
 * a full restart (wb_spi_restart()). */
struct s09_boot_ops opxy_boot_ops(struct opxy_hw *hw);
/* A byte of opxy_boot_ops()'s link inside a session, without the settle
 * time its xfer() adds (ctx is the ops'). */
uint8_t opxy_boot_byte(void *ctx, uint8_t tx);
#if OPXY_BOOT_LINK_FC
/* BOOT_LINK_FC=1: the stream's sessions paced by SPI0's own flow control on
 * PA6; the controller's word timeout must be T_STREAM (S-09.060). */
struct s09_boot_ops opxy_boot_ops_fc(struct opxy_hw *hw);
/*
 * BOOT_LINK_FC=2 (S-09.009, S-13b.023): the same fed by DMA4, in parts with
 * a guard before each, and the fall-back to one byte at a time after a part
 * the controller gives up with PA6 low. Both are built whenever either is.
 */
struct s09_boot_ops opxy_boot_ops_dma(struct opxy_hw *hw);
/* The DMA-fed stream's state and account (waffle/s09_fast.h), and the
 * driver's four fault words of the part that started a fall-back. */
const struct s09_fast *opxy_boot_fast(void);
void opxy_boot_fast_fault(uint32_t out[4]);
/* SPI0_STAT.FCS in the DMA-fed parts (S-13b.023): the status reads taken
 * while they ran, those with FCS set, the parts that saw it set, and the
 * slot's bytes clocked before the first such part, plus one (0: none). */
struct opxy_boot_fcs {
	uint32_t samples, held, parts, first;
};
void opxy_boot_fcs_stats(struct opxy_boot_fcs *out);
/* Which of the two this start's stream took: 1 core-fed, 2 DMA-fed, 0 neither. */
unsigned opxy_boot_way(void);
#if OPXY_BOOT_LINK_FC == 2
#define OPXY_BOOT_OPS(hw) opxy_boot_ops_dma(hw)
#else
#define OPXY_BOOT_OPS(hw) opxy_boot_ops_fc(hw)
#endif
#define OPXY_BOOT_LINK_WORD_US S09_T_STREAM_US
#else
#define OPXY_BOOT_OPS(hw) opxy_boot_ops(hw)
#define OPXY_BOOT_LINK_WORD_US OPXY_SPI_WORD_US
#endif
struct s09_bus_ops opxy_bus_ops(struct opxy_hw *hw);
/* The bridge controller's last failure as {rc, stat, left, stage}
 * (struct wb_spi_fault). */
void opxy_bus_fault(struct opxy_hw *hw, uint32_t out[4]);
/* With it, as read at the failure: DMA4's DMA_STAT and DMA_CFG, SPI0_TXCTL,
 * SPI0_TWC (wb_spi_last_fault_more()). */
void opxy_bus_fault_more(struct opxy_hw *hw, uint32_t out[4]);
/* S-09.080, S-13b.084: the quad reply test's reply session on SPI0
 * (s09_quad_recv_fn), ctx the bridge's controller. */
bool opxy_bus_quad_recv(void *ctx, uint8_t *d, size_t n, unsigned clock,
			uint32_t out[4]);
/* S-16.095 Q0, K0 and K1 through the ROM's OTP read. S-11 passes its
 * package ctx, which these do not use. */
extern const struct s11_unit_ops opxy_unit_ops;

/* The OTP reads behind opxy_unit_ops, on a given getter (tests). */
bool opxy_otp_q0(wb_otp_get_fn get, uint8_t q[56]);
/* S-16 (the OTP item map): provisioned when more than 14 of GP1's first 28
 * bytes are nonzero; an unreadable GP1 counts as not provisioned. */
bool opxy_otp_provisioned(wb_otp_get_fn get);
bool opxy_otp_key(wb_otp_get_fn get, uint32_t lo, uint32_t hi,
		  uint8_t key[32]);
/*
 * S-16.095, S-10c.055: the TE serial (the chip serial) from item 1, up to
 * its first 00 byte and at most OPXY_CHIP_SERIAL_MAX characters, into s
 * NUL-terminated. False when the item cannot be read (s is then empty).
 */
bool opxy_otp_serial(wb_otp_get_fn get,
		     char s[OPXY_CHIP_SERIAL_MAX + 1u]);

#endif /* WAFFLE_BOARD_OPXY_OPS_H */
