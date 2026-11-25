/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_USB_H
#define WAFFLE_DRIVERS_BF70X_USB_H

/*
 * USB device port: the ADSP-BF70x USB controller (HRM chapter 28, "Universal
 * Serial Bus (USB)", and Table A-77) as a TinyUSB device controller driver,
 * and the small wb_usb_* API the board calls.
 *
 * The controller is used in peripheral mode only, by the processor core (no
 * USB DMA). Every register access goes through waffle/drivers/mmr.h, so
 * system MMR reads follow anomaly 19000058's workaround and host tests see
 * every access. The endpoint registers are used through their directly
 * mapped copies (0x100 + 0x10 * n); USB_INDEX is written only to reach the
 * FIFO size and address registers, which have no direct mapping.
 *
 * FIFO RAM (16 KB, HRM "FIFO Configuration") is sized dynamically: endpoint
 * 0 has the first 64 bytes, and each endpoint opened takes the next free
 * block, double-buffered (USB_TXFIFOSZ/USB_RXFIFOSZ.DPB) for bulk. Nothing
 * is written outside the registers the HRM lists.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/drivers/bf70x/time.h"

/* Registers (HRM Table A-77). Widths in the comments. */
#define USB0_BASE           0x200D0000u
#define USB0_FADDR          (USB0_BASE + 0x000u)	/* 8 */
#define USB0_POWER          (USB0_BASE + 0x001u)	/* 8 */
#define USB0_INTRTX         (USB0_BASE + 0x002u)	/* 16, read clears */
#define USB0_INTRRX         (USB0_BASE + 0x004u)	/* 16, read clears */
#define USB0_INTRTXE        (USB0_BASE + 0x006u)	/* 16 */
#define USB0_INTRRXE        (USB0_BASE + 0x008u)	/* 16 */
#define USB0_IRQ            (USB0_BASE + 0x00Au)	/* 8, read clears */
#define USB0_IEN            (USB0_BASE + 0x00Bu)	/* 8 */
#define USB0_FRAME          (USB0_BASE + 0x00Cu)	/* 16 */
#define USB0_INDEX          (USB0_BASE + 0x00Eu)	/* 8 */
#define USB0_TESTMODE       (USB0_BASE + 0x00Fu)	/* 8 */
#define USB0_FIFO(n)        (USB0_BASE + 0x020u + 4u * (n))	/* 32, 16, 8 */
#define USB0_DEV_CTL        (USB0_BASE + 0x060u)	/* 8 */
#define USB0_TXFIFOSZ       (USB0_BASE + 0x062u)	/* 8, indexed */
#define USB0_RXFIFOSZ       (USB0_BASE + 0x063u)	/* 8, indexed */
#define USB0_TXFIFOADDR     (USB0_BASE + 0x064u)	/* 16, indexed */
#define USB0_RXFIFOADDR     (USB0_BASE + 0x066u)	/* 16, indexed */
#define USB0_SOFT_RST       (USB0_BASE + 0x07Fu)	/* 8 */
#define USB0_EP_TXMAXP(n)   (USB0_BASE + 0x100u + 0x10u * (n))	/* 16 */
#define USB0_EP_TXCSR(n)    (USB0_BASE + 0x102u + 0x10u * (n))	/* 16 */
#define USB0_EP0_CSR        USB0_EP_TXCSR(0)
#define USB0_EP_RXMAXP(n)   (USB0_BASE + 0x104u + 0x10u * (n))	/* 16 */
#define USB0_EP_RXCSR(n)    (USB0_BASE + 0x106u + 0x10u * (n))	/* 16 */
#define USB0_EP_RXCNT(n)    (USB0_BASE + 0x108u + 0x10u * (n))	/* 16 */
#define USB0_EP0_CNT        USB0_EP_RXCNT(0)
#define USB0_DMA_IRQ        (USB0_BASE + 0x200u)	/* 8, read clears */
#define USB0_VBUS_CTL       (USB0_BASE + 0x380u)	/* 8 */
#define USB0_PHY_CTL        (USB0_BASE + 0x394u)	/* 8 */
#define USB0_PLL_OSC        (USB0_BASE + 0x398u)	/* 16 */

/* USB_POWER. */
#define USB_POWER_SUSEN     (1u << 0)
#define USB_POWER_SUSPEND   (1u << 1)
#define USB_POWER_RESUME    (1u << 2)
#define USB_POWER_RESET     (1u << 3)
#define USB_POWER_HSMODE    (1u << 4)
#define USB_POWER_HSEN      (1u << 5)
#define USB_POWER_SOFTCONN  (1u << 6)

/* USB_IRQ and USB_IEN. */
#define USB_IRQ_SUSPEND     (1u << 0)
#define USB_IRQ_RESUME      (1u << 1)
#define USB_IRQ_RSTBABBLE   (1u << 2)
#define USB_IRQ_SOF         (1u << 3)
#define USB_IRQ_DISCON      (1u << 5)

/* USB_DEV_CTL. */
#define USB_DEV_CTL_SESSION (1u << 0)
#define USB_DEV_CTL_VBUS_SHIFT 3u
#define USB_DEV_CTL_VBUS    (3u << 3)

/* USB_EP0_CSR[n]_P. */
#define USB_CSR0_RXPKTRDY   (1u << 0)
#define USB_CSR0_TXPKTRDY   (1u << 1)
#define USB_CSR0_SENTSTALL  (1u << 2)
#define USB_CSR0_DATAEND    (1u << 3)
#define USB_CSR0_SETUPEND   (1u << 4)
#define USB_CSR0_SENDSTALL  (1u << 5)
#define USB_CSR0_SPKTRDY    (1u << 6)	/* serviced RXPKTRDY */
#define USB_CSR0_SSETUPEND  (1u << 7)	/* serviced SETUPEND */
#define USB_CSR0_FLUSHFIFO  (1u << 8)

/* USB_EP[n]_TXCSR_P. */
#define USB_TXCSR_TXPKTRDY  (1u << 0)
#define USB_TXCSR_NEFIFO    (1u << 1)
#define USB_TXCSR_URUNERR   (1u << 2)
#define USB_TXCSR_FLUSHFIFO (1u << 3)
#define USB_TXCSR_SENDSTALL (1u << 4)
#define USB_TXCSR_SENTSTALL (1u << 5)
#define USB_TXCSR_CLRDATATGL (1u << 6)
#define USB_TXCSR_INCOMPTX  (1u << 7)
#define USB_TXCSR_ISO       (1u << 14)

/* USB_EP[n]_RXCSR_P. */
#define USB_RXCSR_RXPKTRDY  (1u << 0)
#define USB_RXCSR_FIFOFULL  (1u << 1)
#define USB_RXCSR_ORUNERR   (1u << 2)
#define USB_RXCSR_DATAERR   (1u << 3)
#define USB_RXCSR_FLUSHFIFO (1u << 4)
#define USB_RXCSR_SENDSTALL (1u << 5)
#define USB_RXCSR_SENTSTALL (1u << 6)
#define USB_RXCSR_CLRDATATGL (1u << 7)
#define USB_RXCSR_ISO       (1u << 14)

/* USB_TXFIFOSZ, USB_RXFIFOSZ: size 8 << SZ bytes, DPB double-buffered. */
#define USB_FIFOSZ_DPB      (1u << 4)

/* USB_PHY_CTL. */
#define USB_PHY_CTL_HIBER   (1u << 0)
#define USB_PHY_CTL_RESTORE (1u << 1)
#define USB_PHY_CTL_DIS     (1u << 4)
#define USB_PHY_CTL_EN      (1u << 7)

/* USB_PLL_OSC. */
#define USB_PLL_OSC_DIVCLKIN  (1u << 0)
#define USB_PLL_OSC_PLLM_SHIFT 1u
#define USB_PLL_OSC_PLLM      (0x3Fu << 1)
#define USB_PLL_OSC_PLLMSEL   (1u << 7)
#define USB_PLL_OSC_PLLSTABLE (1u << 14)

/* USB_TESTMODE. */
#define USB_TESTMODE_SE0NAK (1u << 0)
#define USB_TESTMODE_J      (1u << 1)
#define USB_TESTMODE_K      (1u << 2)
#define USB_TESTMODE_PACKET (1u << 3)

#define WB_USB_EP_MAX       12u	/* endpoints 0-11 */
#define WB_USB_FIFO_BYTES   16384u
#define WB_USB_EP0_SIZE     64u

/* SEC sources (HRM Table 28-3). */
#define WB_SID_USB0_STAT    77u
#define WB_SID_USB0_DATA    78u

#define WB_USB_OK           0
#define WB_USB_ERR_ARG     (-1)
#define WB_USB_ERR_SEC     (-3)
#define WB_USB_ERR_STACK   (-4)	/* the device stack did not start */

/* How long the pull-up stays off when a new set starts (S-10a.030). */
#define WB_USB_DETACH_US    5000u
/* From the PHY enable to the pull-up, on every attach (S-13a.080). */
#define WB_USB_PHY_SETTLE_US 5000u

struct wb_usb_config {
	const struct wb_time *time;
	uint8_t irq_prio;	/* SEC priority of both USB sources */
};

/*
 * What the device presents, as wb_usb_state() reports it.
 */
enum wb_usb_state {
	WB_USB_DETACHED,	/* pull-up off */
	WB_USB_ATTACHED,	/* pull-up on, no VBUS (no session) */
	WB_USB_POWERED,		/* VBUS present, not configured */
	WB_USB_CONFIGURED,	/* a configuration selected */
	WB_USB_SUSPENDED,	/* the bus suspended */
};

/*
 * Bring the controller up for device mode with the pull-up off: the PHY
 * (USB_PHY_CTL.EN; USB_PLL_OSC stays at its reset value and PLLSTABLE is not
 * waited on, S-13a.080), the SEC sources, and the TinyUSB device stack.
 * Nothing is visible on the bus until wb_usb_attach().
 */
int wb_usb_init(const struct wb_usb_config *cfg);

/*
 * Start presenting the descriptor set the class layer has selected
 * (S-10a.030). If the device is already attached it is first detached for
 * at least WB_USB_DETACH_US and the device stack is restarted, so nothing
 * of the old set's state carries over. The pull-up goes on no sooner than
 * WB_USB_PHY_SETTLE_US after wb_usb_init() enabled the PHY (S-13a.080).
 */
int wb_usb_attach(void);

/* Turn the pull-up off and stop the device stack (a change of set). */
void wb_usb_detach(void);

/*
 * Stop USB before a hand-over, a reset or a power-off (S-10a.030,
 * S-05.016): wb_usb_detach(), the pull-up off whatever the stack's state,
 * then USB0_PHY_CTL.EN clear. Only wb_usb_init() starts it again.
 */
void wb_usb_stop(void);

/* Run the device stack's deferred work; never blocks. */
void wb_usb_task(void);

enum wb_usb_state wb_usb_state(void);

/* True between wb_usb_attach() and wb_usb_detach(). */
bool wb_usb_attached(void);

/*
 * Chapter 9 front end. The device controller driver offers every SETUP it
 * receives to this filter (in interrupt context) before TinyUSB sees it.
 * The filter answers:
 *   FORWARD  hand the request to TinyUSB as usual;
 *   STALL    a Request Error: the data or status stage gets a STALL;
 *   ACCEPT   (wLength 0 only) complete the status stage, nothing else;
 *   REPLY    (device to host only) send reply[0..len), cut to wLength,
 *            then complete the status stage.
 *   RESELECT (SET_CONFIGURATION only) hand TinyUSB a SET_CONFIGURATION(0)
 *            first, then the request once that one has been answered, so
 *            that the current configuration is selected afresh (TinyUSB
 *            skips a SET_CONFIGURATION of the current value). The host sees
 *            one request and one status stage.
 * The driver answers STALL, ACCEPT and REPLY itself; TinyUSB never sees
 * those requests. reply holds up to WB_USB_EP0_SIZE bytes.
 */
enum wb_usb_setup_action {
	WB_USB_SETUP_FORWARD,
	WB_USB_SETUP_STALL,
	WB_USB_SETUP_ACCEPT,
	WB_USB_SETUP_REPLY,
	WB_USB_SETUP_RESELECT,
};

typedef enum wb_usb_setup_action (*wb_usb_setup_filter)(void *ctx,
		const uint8_t setup[8], uint8_t *reply, uint16_t *len);

void wb_usb_set_setup_filter(wb_usb_setup_filter fn, void *ctx);

/*
 * Driver internals shared by usb.c and usb_dcd.c, and used by the tests.
 */

/* The clock behind tusb_time_millis_api() (usb_time.c). */
void wb_usb_set_clock(const struct wb_time *t);

/* Enable the PHY; its PLL is left as reset sets it (S-13a.080). */
void wb_usb_phy_init(void);

/*
 * Give endpoint epn's FIFO in one direction its block: size the smallest
 * power of two >= mps (at least 8), doubled when double-buffered. Returns
 * false when the FIFO RAM is used up (nothing is written then).
 */
bool wb_usb_fifo_alloc(unsigned epn, bool rx, unsigned mps, bool dbl);

/* Forget every endpoint's FIFO block (bus reset, closing all endpoints). */
void wb_usb_fifo_reset(void);

/* Next free FIFO byte (64 after a reset): for the tests. */
unsigned wb_usb_fifo_used(void);

/* SEC handlers of WB_SID_USB0_STAT and WB_SID_USB0_DATA. */
void wb_usb_irq_stat(void *ctx, uint32_t sid);
void wb_usb_irq_data(void *ctx, uint32_t sid);

/* Clear the driver's endpoint 0 state; for the tests. */
void wb_usb_dcd_reset_state(void);

#endif /* WAFFLE_DRIVERS_BF70X_USB_H */
