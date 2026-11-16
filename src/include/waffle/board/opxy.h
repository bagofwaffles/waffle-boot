/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_BOARD_OPXY_H
#define WAFFLE_BOARD_OPXY_H

/*
 * The OP-XY board: the one place its values live. Each value cites the
 * requirement that fixes it. The drivers (waffle/drivers/) take these as
 * parameters; nothing here touches hardware.
 *
 * Both CPUs are ADSP-BF703 and run the same image
 * (S-01.003); the role comes from the PA2 strap at run time (S-13b.001).
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x_cgu.h"
#include "waffle/drivers/bf70x_dmc.h"
#include "waffle/drivers/bf70x_msi.h"
#include "waffle/drivers/bf70x_otp.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/drivers/bf70x_time.h"
#include "waffle/drivers/bf70x_uart.h"

enum opxy_role { OPXY_SLAVE = 0, OPXY_MASTER = 1 };

/* --- Clocks and DDR (S-13a) --- */

/* S-13a.001-.005: CLKIN 25 MHz, fVCO 800, CCLK 400, SYSCLK 200, SCLK0 100,
 * SCLK1 200, DCLK 200, OCLK 50 MHz; CLKOUT = CCLK / 16. */
extern const struct wb_cgu_config opxy_cgu;
/* The data sheet's checks for the fitted part. */
extern const struct wb_cgu_part opxy_cgu_part;
/* S-13a.010-.016: DDR2, 2 Gbit, x16, at DCLK 200 MHz. */
extern const struct wb_dmc_config opxy_dmc;

#define OPXY_CCLK_MHZ    400u		/* S-13a.001, S-13a.031 */
#define OPXY_SCLK0_HZ    100000000u	/* S-13a.001 */
/* S-13a.031: the time base divides the cycle counter by the nominal CCLK. */
extern const struct wb_time opxy_time;

/* S-13a.021: the DDR address that aliases 0x80000000 on a smaller part. */
#define OPXY_DDR_BASE    0x80000000u
#define OPXY_DDR_ALIAS   0x84000000u

/* USB: the PHY's clock is left as the boot ROM set it (S-13a.080). */
#define OPXY_USB_IRQ_PRIO 4u		/* both USB SEC sources */
/* UART0's receiver in held mode, ahead of USB: at 1 Mbaud its FIFO fills
 * within a fraction of a millisecond. */
#define OPXY_PSOC_IRQ_PRIO 3u

/* The SP the ROM entered the INIT stage with (S-01.007), and L1DM_DCTL and
 * L1DM_DCPLB_DFLT as the first stage left them (S-01.021), from the front's
 * hand-off record; 0 on the host. */
extern uint32_t opxy_init_sp;
extern uint32_t opxy_init_dctl;
extern uint32_t opxy_init_dflt;

/* --- Pins --- */

/* S-13b.001, .002, .008: PA0, PA2, PA8 sampled before any MUX or FER
 * write; at least 10 us from release to sample and 1 us from the input
 * enable (twice each here, for margin). */
extern const struct wb_strap opxy_strap;
#define OPXY_STRAP_ROLE  (1u << 2)	/* PA2: 1 master, 0 slave */
#define OPXY_STRAP_REV0  (1u << 0)	/* PA0, revision bit 0 (S-17) */
#define OPXY_STRAP_REV1  (1u << 8)	/* PA8, revision bit 1 (S-17) */

/* The PA8:PA0 revision value of a strap sample (S-13b.001, S-17). */
uint8_t opxy_strap_revision(uint16_t sample);
enum opxy_role opxy_strap_role(uint16_t sample);

/* S-13b.003, .004: the port function registers per role. */
extern const struct wb_pinmux opxy_pinmux_master;
extern const struct wb_pinmux opxy_pinmux_slave;

/* S-13b.006: the GPIO set-up after the multiplexing. Each list is applied
 * in order, one pin at a time (PA7 before PA6 on the master). */
struct opxy_gpio {
	uint8_t pin;
	uint8_t dir;		/* OPXY_IN, OPXY_OUT_LOW, OPXY_OUT_HIGH */
};
#define OPXY_IN       0u
#define OPXY_OUT_LOW  1u
#define OPXY_OUT_HIGH 2u

extern const struct opxy_gpio opxy_gpio_master[];
extern const unsigned opxy_gpio_master_n;
extern const struct opxy_gpio opxy_gpio_slave[];
extern const unsigned opxy_gpio_slave_n;

/* S-13b.007: the pins the bootloader relies on. */
#define OPXY_PA6  WB_PIN(WB_PORT_A, 6)	/* master: slave ready, active low */
#define OPXY_PA7  WB_PIN(WB_PORT_A, 7)	/* master: display TE (S-13c) */
#define OPXY_PA8  WB_PIN(WB_PORT_A, 8)	/* panel reset (S-13c, S-01.006) */
#define OPXY_PC5  WB_PIN(WB_PORT_C, 5)	/* master: bridge attention, active low */
#define OPXY_PC11 WB_PIN(WB_PORT_C, 11)	/* master: bridge chip select */
#define OPXY_PA4  WB_PIN(WB_PORT_A, 4)	/* slave: bridge ready, active low */
#define OPXY_PB15 WB_PIN(WB_PORT_B, 15)	/* slave: master ready, high = ready */
#define OPXY_PB2  WB_PIN(WB_PORT_B, 2)	/* slave: attention, active low */

/* --- SPI --- */

/* SPI controllers by role. */
#define OPXY_SPI_FLASH   2u	/* master */
#define OPXY_SPI_LINK    0u	/* master: boot link and bridge */
#define OPXY_SPI_BRIDGE_SLAVE 2u	/* slave */

/* S-13b.010, .011: SPI2 master on SEL1, 8-bit, MSB first, mode 3, SCK
 * SCLK0 / 4 = 25 MHz for every command but the read of a slot's stream for
 * the slave, which is at SCLK0 / 2 = 50 MHz, S-13b.011's ceiling. */
extern const struct wb_spi_config opxy_spi_flash;
#define OPXY_SPI_FLASH_STREAM_CLK 1u
/* S-13b.020, .021: SPI0 master on SEL3, mode 1, 8-bit, SCLK0 / 3. */
extern const struct wb_spi_config opxy_spi_boot_link;
/*
 * S-13b.022: the time left between a byte's end on the boot link and the
 * sample of PA6 that decides the next byte, in microseconds (make
 * BOOT_LINK_SETTLE_US=n; 0 for none). How soon after a byte the slave's
 * boot ROM raises PA6 is not measured: 2 us keeps a
 * late raise of up to that long from letting a byte through.
 */
#ifndef OPXY_BOOT_LINK_SETTLE_US
#define OPXY_BOOT_LINK_SETTLE_US 2u
#endif
#define OPXY_BOOT_LINK_SETTLE_CYCLES ((uint64_t)(OPXY_BOOT_LINK_SETTLE_US) * OPXY_CCLK_MHZ)

/* The boot link with BOOT_LINK_FC=1 (waffle/s09_master.h): flow control on
 * PA6 for the stream's sessions (S-13b.022); the opening byte and
 * S-09.007's go without it. */
#ifndef OPXY_BOOT_LINK_FC
#define OPXY_BOOT_LINK_FC 0
#endif
#if OPXY_BOOT_LINK_FC
extern const struct wb_spi_config opxy_spi_boot_link_fc;
#endif
/* S-13b.040, .041: SPI0 master, mode 1, 32-bit, flow control on PA6,
 * single I/O at SCLK0 / 4 before the link. The chip select is PC11 as a
 * GPIO, so no SPI_SELn. */
extern const struct wb_spi_config opxy_spi_bridge_pre;
/* S-13b.042: SPI2 slave, mode 1, 32-bit, flow control, single I/O before
 * the link. */
extern const struct wb_spi_config opxy_spi_bridge_slave_pre;

/* S-02.001: the 1 MiB part; S-13b.017: a busy wait of at least 1 s. */
#define OPXY_FLASH_SIZE       0x100000u
#define OPXY_FLASH_BUSY_US    2000000u
/* Longest single SPI word on the flash and boot link buses. */
#define OPXY_SPI_WORD_US      1000u

/* --- UARTs --- */

/* S-13b.070: UART0, the PSoC link: 1 Mbaud 8N1, no flow control. */
extern const struct wb_uart_config opxy_uart_psoc;
/* Its shortest byte: 10 bits at 1 Mbaud + 1 %. */
#define OPXY_PSOC_BYTE_NS 9900u
/* S-13b.071: UART1, the Nordic link: 1 Mbaud 8N1, RTS/CTS active low. */
extern const struct wb_uart_config opxy_uart_nordic;
#define OPXY_UART_PSOC    0u
#define OPXY_UART_NORDIC  1u

/* --- eMMC (S-03) --- */

/* S-13b.052, .057: identification at 396.8 kHz (SCLK0 / 2 / 126), CMD1
 * for up to 1.5 s, every other wait bounded. */
extern const struct wb_msi_config opxy_msi;
/* S-09.018: the same for the bring-up alone, with shorter waits (CMD1 still
 * at least 1 s); opxy_msi takes over once the eMMC is up. */
extern const struct wb_msi_config opxy_msi_bringup;
/* S-05.016's slave MSI row: the controller in FIFO mode at the hand-off. */
extern const struct wb_msi_leave opxy_msi_leave;

/* --- The hand-over (S-05.014, S-05.016) --- */

/* S-05.016's SEC row: a source left enabled, with its SEC0_SCTL. */
struct opxy_sec_keep {
	uint32_t sid;
	uint32_t sctl;
};
#define OPXY_SEC_KEEP_MASTER 4u
#define OPXY_SEC_KEEP_SLAVE  5u
extern const struct opxy_sec_keep opxy_sec_keep_master[OPXY_SEC_KEEP_MASTER];
extern const struct opxy_sec_keep opxy_sec_keep_slave[OPXY_SEC_KEEP_SLAVE];

/* S-05.014: SP, FP and EVT2 to EVT15 at the OS's first instruction.
 * opxy_handover_entry.S reads the fields at these offsets. */
#define OPXY_OS_EVTS 14u
struct opxy_os_entry {
	uint32_t sp;			/* +0 */
	uint32_t fp;			/* +4 */
	uint32_t evt[OPXY_OS_EVTS];	/* +8: EVT2 first */
};
extern const struct opxy_os_entry opxy_os_entry_master;
extern const struct opxy_os_entry opxy_os_entry_slave;

/* --- OTP (S-16.095, S-11, S-10c.057) --- */

/* S-01.004: the boot ROM's OTP read entry (adi_rom_otp_get, HRM 36). */
#define OPXY_ROM_OTP_GET  0x04000048u

/*
 * The board's wb_otp_get_fn: a call of the ROM entry. Target only; on the
 * host it fails every read.
 */
bool opxy_otp_get(uint32_t item, uint32_t *data);

/* S-16.095, S-01.004: the ROM's ADI-area read, which gives the FSN: the
 * entry called with the command and a buffer of four words; nonzero is a
 * read. */
#define OPXY_ROM_OTP_ADI  0x0400004Cu
#define OPXY_OTP_ADI_FSN  0x13u

/* The FSN's four words through the ADI-area read (S-10c.057). Target only;
 * on the host it fails. */
bool opxy_otp_fsn(uint32_t words[4]);

/* The PSoC unlock, six bytes (S-07.081): false, this board supplies none. */
bool opxy_psoc_unlock(uint8_t unlock[6]);

/* S-16.095: the items the bootloader reads. */
#define OPXY_OTP_SERIAL   WB_OTP_HUK		/* item 1: the TE serial */
/* The TE serial's length: S-10c.055's 18 characters (the chip serial of
 * S-12.017, S-17.030). */
#define OPXY_CHIP_SERIAL_MAX 18u
#define OPXY_OTP_K0_LO    WB_OTP_PVT_128KEY0	/* items 3, 4: AES key 0 */
#define OPXY_OTP_K0_HI    WB_OTP_PVT_128KEY1
#define OPXY_OTP_K1_LO    WB_OTP_PVT_128KEY2	/* items 5, 6: AES key 1 */
#define OPXY_OTP_K1_HI    WB_OTP_PVT_128KEY3
#define OPXY_OTP_Q0       WB_OTP_PUBLIC_KEY0	/* item 9: P-224 key 0 */
#define OPXY_OTP_GP1      WB_OTP_GP1		/* item 0xE */

/* --- Memory (S-01) --- */

/*
 * DDR (S-01.012: 0x80000000-0x90000000, 256 MiB). S-01.016 makes the held-
 * mode buffers hold the whole RAM disk of S-12.001, 222 MiB, outside the
 * handover reserve (S-01.013, 0x8803D000-0x88A65000) and the bootloader's
 * own ranges, so the disk is in two parts, one each side of the reserve:
 *
 *   0x80000000  disk part 0, up to the reserve
 *   0x8803D000  the DDR handover reserve (to 0x88A65000), which the
 *               handover path does not use: the main image, its data and
 *               its stack (bf703.ld), at most 3 MiB, then
 *   0x8833D000  the heap (picolibc's malloc, used by yaffs2), on through
 *               the rest of the reserve up to the staging area (S-01.014);
 *   0x8A000000  disk part 1, over the staging area (S-01.016 allows it);
 *   0x8FE00000  the display's frame buffers (S-13c), below the display
 *               record (S-01.019).
 *
 * tools/layout.py declares these to S-01's check_layout.py.
 */
#define OPXY_IMAGE_START  0x8803D000u	/* S01_RESERVE_DDR_START */
#define OPXY_IMAGE_END    0x8833D000u	/* bf703.ld's ddr region */
/*
 * The test OS (wiki/Test-OS.md, waffle/testos.h) is loaded by another
 * bootloader, so it may not load into the handover reserve (S-01.017,
 * S-01.013: DDR 0x8803D000-0x88A65000) and, under another bootloader,
 * not right above it either: that bootloader's hand-over may use working
 * memory past S-01.013's end, so the image keeps a margin of several MiB.
 * So the image sits at 0x89000000, 16 MiB into DDR and 5.6 MiB above the
 * reserve's end (bf703_testos.ld; everything of it in DDR, nothing in L1
 * or L2), and its heap runs from the image's end on to the staging area,
 * which it leaves alone too (S-05.024). The held-mode buffers are the
 * same as the bootloader's (S-01.016). tools/testos_layout.py checks it.
 */
#define OPXY_TESTOS_IMAGE_START 0x89000000u
#define OPXY_TESTOS_IMAGE_END   0x89300000u
#define OPXY_TESTOS_HEAP_START  0x89300000u
#define OPXY_TESTOS_HEAP_END    0x8A000000u
#if defined(OPXY_TEST_OS)
#define OPXY_HEAP_START   OPXY_TESTOS_HEAP_START
#define OPXY_HEAP_END     OPXY_TESTOS_HEAP_END
#else
#define OPXY_HEAP_START   0x8833D000u
#define OPXY_HEAP_END     0x8A000000u
#endif
#define OPXY_DISK_SECTORS 454656u	/* S-12.001 */
#define OPXY_DISK0_START  0x80000000u
#define OPXY_DISK0_END    0x8803D000u
#define OPXY_DISK1_START  0x8A000000u
#define OPXY_DISK1_END    0x8FDC3000u
#define OPXY_FRAMES_START 0x8FE00000u
#define OPXY_FRAMES_END   0x8FF00000u
/* S-05.031: where the display DMA reads a frame from at the hand-over.
 * The boot path leaves the logo there (opxy_display.c, S-05.016): an OS draws
 * its first frame over what it finds there. Inside DISK1, which only held mode uses. */
#define OPXY_OS_FRAME_START 0x8C000000u
#define OPXY_OS_FRAME_END   0x8C080000u	/* 320 x 480 at 3 bytes, rounded up */
/* How an OS reads that memory (S-05.016): rows this many
 * bytes apart, 2 bytes a pixel, low byte first. */
#define OPXY_OS_FRAME_ROW   512u

_Static_assert(OPXY_DISK0_END - OPXY_DISK0_START +
	       (OPXY_DISK1_END - OPXY_DISK1_START) ==
	       OPXY_DISK_SECTORS * 512u, "the disk parts hold S-12.001's disk");
_Static_assert((OPXY_DISK0_END - OPXY_DISK0_START) % 512u == 0u &&
	       (OPXY_DISK1_END - OPXY_DISK1_START) % 512u == 0u,
	       "each disk part holds whole sectors");
_Static_assert(OPXY_DISK0_END <= OPXY_IMAGE_START &&
	       OPXY_IMAGE_END <= OPXY_HEAP_START &&
	       OPXY_HEAP_END <= OPXY_DISK1_START &&
	       OPXY_DISK1_END <= OPXY_FRAMES_START &&
	       OPXY_FRAMES_END <= 0x8FFFFF00u,
	       "the DDR ranges are in order and apart");
_Static_assert(OPXY_OS_FRAME_START >= OPXY_DISK1_START &&
	       OPXY_OS_FRAME_END <= OPXY_DISK1_END,
	       "the OS's frame lies in held mode's disk");
_Static_assert(OPXY_TESTOS_IMAGE_START >= 0x88A65000u &&
	       OPXY_TESTOS_IMAGE_END <= OPXY_TESTOS_HEAP_START &&
	       OPXY_TESTOS_HEAP_END <= OPXY_DISK1_START,
	       "the test OS lies between the DDR reserve and the staging area");

#endif /* WAFFLE_BOARD_OPXY_H */
