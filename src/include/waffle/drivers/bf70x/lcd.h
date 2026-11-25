/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_LCD_H
#define WAFFLE_DRIVERS_BF70X_LCD_H

/*
 * The display panel's 8-bit 8080-I bus (S-13c.001 - S-13c.003) on the
 * BF70x: every byte the panel latches is a write cycle of SMC0 bank 0,
 * frames by memory DMA to one fixed bank address; D/C, CS, RD, RESET and
 * TE are GPIO, and the identification reads take the data pins back from
 * the SMC for their strobes.
 *
 * Why the SMC (data sheet Tables 8 and 9, HRM chapter "Static Memory
 * Controller"): S-13b.003's master MUX and FER values already put PB0-PB7
 * on function 3, SMC0_D07-SMC0_D00, and PA14 on function 3, SMC0_AWE, so
 * the write strobe is the SMC's and no pin changes function. PBn is
 * SMC0_D(7-n) and the panel's D(7-n), so a byte written to the bank
 * reaches D[7:0] unchanged; through PORTB_DATA it is bit-reversed. The
 * bank's timing registers fix the strobe's set-up, width and hold, and a
 * DMA to the bank sends a 215 040-byte frame in one TE period (S-13c.018),
 * which bit-banged GPIO cannot. SMC0_AMS0 (PA15) and SMC0_A07 (PA5) stay
 * GPIO: CS and D/C are driven by the port, so no access to the bank
 * reaches the panel without its CS.
 *
 * Writes to the bank are posted: every change of D/C, CS or the data
 * pins' function follows wb_mmr_sync(), so the earlier write cycle has
 * ended at the pins.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/drivers/bf70x/time.h"

/* SMC0 (HRM Table A-52). */
#define SMC0_BASE        0x2005D000u
#define SMC0_B0CTL       (SMC0_BASE + 0x0Cu)
#define SMC0_B0TIM       (SMC0_BASE + 0x10u)
#define SMC0_B0ETIM      (SMC0_BASE + 0x14u)

/* SMC_B0CTL (HRM Table 10-3): async SRAM protocol, AMS0 only. */
#define SMC_CTL_EN       (1u << 0)

/* SMC_B0TIM (HRM Table 10-5), in SCLK0 cycles. */
#define SMC_TIM_WST(n)   ((uint32_t)(n) << 0)	/* 1-7, 0 is 8 */
#define SMC_TIM_WHT(n)   ((uint32_t)(n) << 4)	/* 0-7 */
#define SMC_TIM_WAT(n)   ((uint32_t)(n) << 8)	/* 1-63 */
#define SMC_TIM_RST(n)   ((uint32_t)(n) << 16)
#define SMC_TIM_RHT(n)   ((uint32_t)(n) << 20)
#define SMC_TIM_RAT(n)   ((uint32_t)(n) << 24)

/* SMC_B0ETIM (HRM Table 10-4). */
#define SMC_ETIM_TT(n)   ((uint32_t)(n) << 8)	/* 1-7 */
#define SMC_ETIM_IT(n)   ((uint32_t)(n) << 12)	/* 0-7 */

/* SMC bank 0 (data sheet memory map; the CPLB map's 64 MB I/O page). */
#define WB_LCD_SMC_BANK0 0x70000000u

/*
 * The write cycle at SCLK0 = 100 MHz (S-13a.001): set-up 2, strobe 3,
 * hold 2, no idle: 70 ns per byte, WR low 30 ns, D[7:0] valid 50 ns
 * before and 20 ns after WR's rising edge (S-13c.003: 66, 15, 15, 15 ns).
 * Reads never go through the bank; their fields are the reset values'
 * order of magnitude.
 */
#define WB_LCD_B0TIM  (SMC_TIM_WST(2) | SMC_TIM_WAT(3) | SMC_TIM_WHT(2) | \
		       SMC_TIM_RST(2) | SMC_TIM_RAT(5) | SMC_TIM_RHT(2))
#define WB_LCD_B0ETIM (SMC_ETIM_TT(2) | SMC_ETIM_IT(0))

struct wb_lcd_config {
	uint8_t dc, rd, cs, reset, te;	/* GPIO pins (WB_PIN) */
	uint8_t wr;			/* SMC0_AWE's pin */
	uint32_t data_port;		/* D[7:0] on pins 0-7, PBn = D(7-n) */
	uint32_t bank;			/* the bank address the bytes go to */
	uint32_t b0tim, b0etim;
	unsigned mdma;			/* memory DMA stream for frames, 0-2 */
	uint32_t read_strobe_us;	/* RD low, and high between strobes */
};

/* The board pins of S-13c.001 and the timing above. */
extern const struct wb_lcd_config wb_lcd_opxy;
/* The same with the OP-1F's D/C pin, PA11 (S-18.030 device.panel_dc). */
extern const struct wb_lcd_config wb_lcd_op1f;

struct wb_lcd {
	const struct wb_lcd_config *cfg;
	const struct wb_time *time;
	int dc;			/* D/C level last driven, -1 unknown */
	bool selected;		/* CS low */
	bool frame;		/* a frame DMA was started and not reaped */
};

#define WB_LCD_OK        0
#define WB_LCD_ERR_ARG  (-1)
#define WB_LCD_BUSY     (-2)
#define WB_LCD_TIMEOUT  (-3)
#define WB_LCD_ERR_HW   (-4)

/*
 * Bank 0's control and timing, then the pins: D/C high, RD high and CS
 * high as outputs, TE an input, D[7:0] and WR on the SMC. RESET is left
 * alone (S-13c.004: its level follows the strap until the reset step).
 */
int wb_lcd_init(struct wb_lcd *l, const struct wb_lcd_config *cfg,
		const struct wb_time *t);

/* Drive RESET (S-13c.005). */
void wb_lcd_reset(const struct wb_lcd *l, bool high);
/* CS low (on) or high. */
void wb_lcd_select(struct wb_lcd *l, bool on);

/* A command byte (D/C low), parameter bytes (D/C high), and count copies
 * of a group of parameter bytes. */
void wb_lcd_cmd(struct wb_lcd *l, uint8_t c);
void wb_lcd_data(struct wb_lcd *l, const uint8_t *p, size_t n);
void wb_lcd_repeat(struct wb_lcd *l, const uint8_t *group, size_t len,
		   uint32_t count);

/*
 * A register read (S-13c.011): CS high then low, the command byte, n read
 * strobes with D/C high (the first returns the dummy byte), CS high. The
 * data pins are GPIO inputs for the strobes and go back to the SMC after.
 */
int wb_lcd_read(struct wb_lcd *l, uint8_t cmd, uint8_t *out, size_t n);

/* Wait for a rising edge of TE: TE low, then high, within timeout_us. */
int wb_lcd_te_wait(const struct wb_lcd *l, uint32_t timeout_us);

/*
 * A frame's parameter bytes, after its RAMWR (S-13c.016): memory DMA from
 * buf to the bank, one byte per write cycle, the destination address
 * fixed. It returns once the transfer runs; frame_busy() polls it and
 * reaps it when done.
 */
int wb_lcd_frame_start(struct wb_lcd *l, const uint8_t *buf, uint32_t n);
bool wb_lcd_frame_busy(struct wb_lcd *l);
/* The frame's transfer stopped, the source first: buf is not read after
 * the return. Nothing when no frame runs. */
void wb_lcd_frame_stop(struct wb_lcd *l);

#endif /* WAFFLE_DRIVERS_BF70X_LCD_H */
