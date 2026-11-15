/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_MSI_H
#define WAFFLE_DRIVERS_BF70X_MSI_H

/*
 * The MSI controller and an eMMC on it (ADSP-BF70x Hardware
 * Reference chapter "Mobile Storage Interface (MSI)", S-13b.050-.058).
 *
 * Data moves only through the controller's internal DMA (IDMAC) with
 * chained descriptors: the HRM gives no address for the FIFO data region.
 * The caller provides the descriptors (WB_MSI_DESC_BLOCKS blocks each)
 * and data buffers on a WB_MSI_BUF_ALIGN boundary; an entry given one off
 * it fails with WB_MSI_ERR_ARG and sends nothing. Commands and transfers
 * are polled; every wait is bounded (S-13b.057).
 *
 * The eMMC operations read, write, erase, select and switch_byte have
 * struct s03_emmc_ops's shapes, with the struct wb_msi as ctx.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x_time.h"

/* S-13b.054: a data buffer starts on a data-cache line (WB_DCACHE_LINE). */
#define WB_MSI_BUF_ALIGN 32u

#define MSI0_BASE          0x20076000u
#define MSI_CTL            (MSI0_BASE + 0x000u)
#define MSI_CLKDIV         (MSI0_BASE + 0x008u)
#define MSI_CLKEN          (MSI0_BASE + 0x010u)
#define MSI_TMOUT          (MSI0_BASE + 0x014u)
#define MSI_CTYPE          (MSI0_BASE + 0x018u)
#define MSI_BLKSIZ         (MSI0_BASE + 0x01Cu)
#define MSI_BYTCNT         (MSI0_BASE + 0x020u)
#define MSI_IMSK           (MSI0_BASE + 0x024u)
#define MSI_CMDARG         (MSI0_BASE + 0x028u)
#define MSI_CMD            (MSI0_BASE + 0x02Cu)
#define MSI_RESP0          (MSI0_BASE + 0x030u)
#define MSI_RESP1          (MSI0_BASE + 0x034u)
#define MSI_RESP2          (MSI0_BASE + 0x038u)
#define MSI_RESP3          (MSI0_BASE + 0x03Cu)
#define MSI_ISTAT          (MSI0_BASE + 0x044u)
#define MSI_STAT           (MSI0_BASE + 0x048u)
#define MSI_FIFOTH         (MSI0_BASE + 0x04Cu)
#define MSI_TCBCNT         (MSI0_BASE + 0x05Cu)
#define MSI_BUSMODE        (MSI0_BASE + 0x080u)
#define MSI_DBADDR         (MSI0_BASE + 0x088u)
#define MSI_IDSTS          (MSI0_BASE + 0x08Cu)
#define MSI_IDINTEN        (MSI0_BASE + 0x090u)
#define MSI_CDTHRCTL       (MSI0_BASE + 0x100u)
#define MSI_UHS_EXT        (MSI0_BASE + 0x108u)

/* MSI_CTL. */
#define MSI_CTL_CTLRST     (1u << 0)
#define MSI_CTL_FIFORST    (1u << 1)
#define MSI_CTL_DMARST     (1u << 2)
#define MSI_CTL_INTEN      (1u << 4)
#define MSI_CTL_INTDMAC    (1u << 25)
#define MSI_CTL_RESETS     (MSI_CTL_CTLRST | MSI_CTL_FIFORST | MSI_CTL_DMARST)

/* MSI_CMD. */
#define MSI_CMD_RXPECT     (1u << 6)
#define MSI_CMD_RLEN       (1u << 7)
#define MSI_CMD_CHKRESPCRC (1u << 8)
#define MSI_CMD_DXPECT     (1u << 9)
#define MSI_CMD_RDWR       (1u << 10)
#define MSI_CMD_WTPRIVDATA (1u << 13)
#define MSI_CMD_STPABORT   (1u << 14)
#define MSI_CMD_SENDINIT   (1u << 15)
#define MSI_CMD_UCLKREGS   (1u << 21)
#define MSI_CMD_USEHOLDREG (1u << 29)
#define MSI_CMD_STARTCMD   (1u << 31)

/* MSI_ISTAT (W1C). */
#define MSI_IS_RE          (1u << 1)
#define MSI_IS_CMDDONE     (1u << 2)
#define MSI_IS_DTO         (1u << 3)
#define MSI_IS_RCRC        (1u << 6)
#define MSI_IS_DCRC        (1u << 7)
#define MSI_IS_RTO         (1u << 8)
#define MSI_IS_DRTO        (1u << 9)
#define MSI_IS_HTO         (1u << 10)
#define MSI_IS_FRUN        (1u << 11)
#define MSI_IS_HLE         (1u << 12)
#define MSI_IS_SBE         (1u << 13)
#define MSI_IS_EBE         (1u << 15)
#define MSI_IS_CMD_ERR     (MSI_IS_RE | MSI_IS_RCRC | MSI_IS_RTO)
#define MSI_IS_DATA_ERR    (MSI_IS_DCRC | MSI_IS_DRTO | MSI_IS_HTO | \
			    MSI_IS_FRUN | MSI_IS_SBE | MSI_IS_EBE)

/* MSI_STAT. */
#define MSI_STAT_DBUSY     (1u << 9)

/* MSI_CLKEN, MSI_CTYPE, MSI_BUSMODE, MSI_IDSTS. */
#define MSI_CLKEN_EN0      (1u << 0)
#define MSI_CLKEN_LP0      (1u << 16)
#define MSI_CTYPE_WIDBYTE0 (1u << 16)
#define MSI_BUSMODE_SWR    (1u << 0)
#define MSI_BUSMODE_FB     (1u << 1)
#define MSI_BUSMODE_DE     (1u << 7)
#define MSI_IDSTS_FBE      (1u << 2)
#define MSI_IDSTS_DU       (1u << 4)
#define MSI_IDSTS_CES      (1u << 5)
#define MSI_IDSTS_ALL      0x337u

/* IDMAC descriptor (HRM Tables 33-5 to 33-8). */
#define MSI_DES0_OWN       (1u << 31)
#define MSI_DES0_CH        (1u << 4)
#define MSI_DES0_FS        (1u << 3)
#define MSI_DES0_LD        (1u << 2)
#define MSI_DES0_DIC       (1u << 1)

struct wb_msi_desc {
	uint32_t des0, des1, des2, des3;
};

#define WB_MSI_BLOCK        512u
#define WB_MSI_DESC_BLOCKS  8u	/* BS1 is 13 bits: 4096 bytes */

/* eMMC. */
#define WB_MMC_RCA          0x5A7Eu	/* S-13b.058 */
#define WB_MMC_OCR_ARG      0x40000080u	/* S-13b.051: sector mode, 1.8 V */
#define WB_MMC_OCR_BUSY     (1u << 31)	/* power-up done */
#define WB_MMC_R1_SWITCH_ERROR (1u << 7)
#define WB_MMC_R1_READY     (1u << 8)
#define WB_MMC_R1_STATE(r)  (((r) >> 9) & 0xFu)
#define WB_MMC_STATE_TRAN   4u
#define WB_MMC_R1_ERRORS    0xFDF90000u	/* bits 31-26, 24, 23-19, 16 */
#define WB_MMC_R1_OUT_OF_RANGE (1u << 31)	/* ADDRESS_OUT_OF_RANGE */

#define WB_EXT_CSD_FLUSH_CACHE   32u
#define WB_EXT_CSD_POWER_OFF     34u
#define WB_EXT_CSD_PART_CONFIG   179u
#define WB_EXT_CSD_BUS_WIDTH     183u
#define WB_EXT_CSD_HS_TIMING     185u
#define WB_EXT_CSD_DEVICE_TYPE   196u
#define WB_EXT_CSD_DRIVER_STR    197u
#define WB_EXT_CSD_POWER_OFF_LONG_TIME 247u

struct wb_msi_config {
	uint32_t ident_clkdiv;	/* MSI_CLKDIV until HS_TIMING (<= 400 kHz) */
	uint32_t tmout;		/* MSI_TMOUT */
	uint32_t fifoth;	/* MSI_FIFOTH */
	uint32_t cmd1_timeout_us;	/* >= 1 s (S-13b.057) */
	uint32_t busy_timeout_us;	/* DAT0 and CMD13 waits */
	uint32_t xfer_timeout_us;	/* per command, or per block of data */
	uint32_t flush_timeout_us;	/* DAT0 after a flush; 0: busy_timeout_us */
	/* A multi-block data phase, whole; never less than one block's
	 * allowance (xfer, plus busy for a write), which 0 gives. */
	uint32_t xfer_total_us;
};

struct wb_msi {
	const struct wb_msi_config *c;
	const struct wb_time *t;
	struct wb_msi_desc *desc;
	unsigned ndesc;
	uint32_t rca;
	uint8_t width;		/* host and eMMC bus width, 1 or 8 */
	uint8_t hs_timing;	/* written to EXT_CSD[185], or 0 */
	uint32_t clkdiv;	/* MSI_CLKDIV now; 0 is the bypass */
	uint8_t power_off_long_time;	/* EXT_CSD[247], from emmc_init */
	/* CMD9's R2 as MSI_RESP0-3 gave it: csd[k] is bits 32k+31 to 32k of
	 * the response, which are the CSD's bits of the same numbers (HRM
	 * "MSI_RESP0" to "MSI_RESP3"). All zero unless a bring-up ended well. */
	uint32_t csd[4];
	bool powered_off;	/* Switch(34, 3) sent: no command since */
	uint32_t last_r1;
};

#define WB_MSI_OK          0
#define WB_MSI_ERR_ARG    (-1)
#define WB_MSI_TIMEOUT    (-2)
#define WB_MSI_ERR_CMD    (-3)	/* response timeout, CRC or error */
#define WB_MSI_ERR_DATA   (-4)	/* data CRC, timeout, FIFO or bus error */
#define WB_MSI_ERR_EMMC   (-5)	/* R1 error bits or wrong state */
#define WB_MSI_ERR_OFF    (-6)	/* after the power-off notification */

/* Controller reset and set-up; the eMMC clock off. */
int wb_msi_init(struct wb_msi *m, const struct wb_msi_config *c,
		const struct wb_time *t, struct wb_msi_desc *desc,
		unsigned ndesc);

/* HRM "Programming Card Clocks": CLKEN off, CLKDIV, CLKEN on. */
int wb_msi_set_clock(struct wb_msi *m, uint32_t clkdiv, bool low_power);

/* One command; resp gets RESP0 (short) or RESP0-3 (long, R2). */
#define WB_MSI_R_NONE  0u
#define WB_MSI_R1      (MSI_CMD_RXPECT | MSI_CMD_CHKRESPCRC)
#define WB_MSI_R2      (MSI_CMD_RXPECT | MSI_CMD_RLEN | MSI_CMD_CHKRESPCRC)
#define WB_MSI_R3      MSI_CMD_RXPECT
int wb_msi_cmd(const struct wb_msi *m, uint32_t idx, uint32_t arg,
	       uint32_t flags, uint32_t resp[4]);

/*
 * S-13b.051 steps 1-9 at RCA 0x5A7E, with CMD9 between steps 5 and 6,
 * leaving the eMMC selected, on the 8-bit bus, with HS_TIMING and the
 * clock of S-13b.053. ext_csd (512 bytes, WB_MSI_BUF_ALIGN) holds the last
 * EXT_CSD read, and m->csd the CSD. A failure clears m->csd (S-13b.059).
 */
int wb_msi_emmc_init(struct wb_msi *m, uint8_t *ext_csd);

/*
 * S-03.001: the CSD's erase group, (ERASE_GRP_SIZE + 1) x (ERASE_GRP_MULT
 * + 1) sectors; JESD84-B51 puts the two at CSD bits 46:42 and 41:37.
 */
uint32_t wb_msi_csd_erase_group(const struct wb_msi *m);

int wb_msi_read_ext_csd(struct wb_msi *m, uint8_t *ext_csd);

/*
 * Switch(34, 3), then only DAT0 is watched, bounded by the eMMC's
 * POWER_OFF_LONG_TIME (EXT_CSD[247] x 10 ms); no command follows until
 * wb_msi_init() (S-13b.056).
 */
int wb_msi_power_off(struct wb_msi *m, uint8_t power_off_long_time);

/* s03_emmc_ops shapes (ctx is the struct wb_msi). */
int wb_msi_emmc_select(void *ctx, unsigned part);
int wb_msi_emmc_read(void *ctx, uint32_t sector, uint32_t count,
		     uint8_t *buf);
int wb_msi_emmc_write(void *ctx, uint32_t sector, uint32_t count,
		      const uint8_t *buf);
int wb_msi_emmc_erase(void *ctx, uint32_t first_sector);

/*
 * S-09.040 0x14, S-16.093: CMD35 and CMD36 with first_sector, CMD38 with
 * arg, then the ready wait. wb_msi_emmc_erase() is arg 0. arg is one of
 * the CMD38 arguments below, else WB_MSI_ERR_ARG with nothing sent.
 * The list is the eMMC standard's; S-09.040 lets a slave pass on only
 * the erase, 0 (opxy_slave_emmc() refuses the others). With a
 * trim or discard argument the eMMC removes write blocks, not a group,
 * so start = end removes only first_sector. The secure forms may take
 * longer than busy_timeout_us (EXT_CSD SEC_ERASE_MULT, SEC_TRIM_MULT).
 */
#define WB_MMC_ERASE             0x00000000u
#define WB_MMC_TRIM              0x00000001u
#define WB_MMC_DISCARD           0x00000003u
#define WB_MMC_SECURE_ERASE      0x80000000u
#define WB_MMC_SECURE_TRIM1      0x80000001u
#define WB_MMC_SECURE_TRIM2      0x80008000u
int wb_msi_emmc_erase_arg(void *ctx, uint32_t first_sector, uint32_t arg);

/*
 * S-09.040 0x13: count sectors as one reliable write, CMD23 with bit 31
 * and the block count, then CMD25 and no CMD12 (S-13b.054), then the
 * ready wait. The write is never split: count is 1 to the descriptors'
 * capacity (ndesc x 8 blocks; 16 descriptors take S-09's 128), else
 * WB_MSI_ERR_ARG with nothing sent. An R1 error on CMD23 sends no CMD25.
 * The commands are S-13b.054's. The driver neither reads nor sets
 * WR_REL_PARAM or WR_REL_SET (EXT_CSD 166, 167; the latter is set once,
 * with the partitioning), so the eMMC's own reliable-write mode applies;
 * an eMMC in the legacy mode may refuse a count above REL_WR_SEC_C
 * (EXT_CSD 222), which comes back as WB_MSI_ERR_EMMC and so as S-09.040's
 * failed result.
 */
#define WB_MMC_CMD23_RELIABLE (1u << 31)
int wb_msi_emmc_write_reliable(void *ctx, uint32_t sector, uint32_t count,
			       const uint8_t *buf);
/*
 * Only the EXT_CSD bytes of S-13b.055. Switch(34, 3) goes through
 * wb_msi_power_off() with the eMMC's POWER_OFF_LONG_TIME.
 */
int wb_msi_emmc_switch(void *ctx, uint8_t index, uint8_t value);

/* The controller's registers at the hand-off (S-13b.058, S-05.016). */
struct wb_msi_leave {
	uint32_t ctl;		/* MSI_CTL, written last */
	uint32_t busmode;	/* MSI_BUSMODE: DE clear */
	uint32_t imsk;		/* MSI_IMSK */
	uint32_t fifoth;	/* MSI_FIFOTH */
	uint32_t cdthrctl;	/* MSI_CDTHRCTL */
};

/*
 * The controller handed over in FIFO mode, the eMMC untouched: a bounded
 * wait for DAT0 and the CIU, the internal DMA off, the DMA interface and
 * FIFO reset, BUSMODE, the DMA status cleared, DBADDR 0, FIFOTH, CDTHRCTL
 * and IMSK, ISTAT cleared, then CTL. Every write is made even after a wait
 * that timed out; the first timeout is returned. The clock, bus width and
 * timeout registers stay as they are. CDTHRCTL is written here alone: the
 * driver's own transfers leave it as reset sets it.
 */
int wb_msi_leave(const struct wb_msi *m, const struct wb_msi_leave *v);

#endif /* WAFFLE_DRIVERS_BF70X_MSI_H */
