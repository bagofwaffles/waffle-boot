/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tests for the USB device port (src/drivers/bf70x/usb.c, usb_dcd.c)
 * against the register access fake: the PHY bring-up, attach and detach,
 * endpoint open and FIFO set-up, endpoint 0 control transfers and the
 * chapter 9 filter, the bulk data path and the bus events.
 *
 * TinyUSB's device core is not linked: the tud_* functions the port calls
 * are stood in for here, and dcd_event_handler records what the driver
 * reports to the stack.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x_sec.h"
#include "waffle/drivers/bf70x_usb.h"

#include "tusb_option.h"
#include "device/dcd.h"
#include "device/usbd.h"

static const struct wb_time cclk400 = { 400u };

/* --- The device stack, stood in for ------------------------------------ */

static dcd_event_t events[16];
static unsigned n_events;
static unsigned n_inits, n_deinits, n_tasks;
static uint64_t connect_cycles, disconnect_cycles;
static bool mounted, suspended;

void dcd_event_handler(const dcd_event_t *event, bool in_isr)
{
	(void)in_isr;
	if (n_events < ARRAY_LEN(events))
		events[n_events++] = *event;
}

bool tud_rhport_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
	n_inits++;
	CHECK(rh_init && rh_init->role == TUSB_ROLE_DEVICE);
	if (!dcd_init(rhport, rh_init))
		return false;
	dcd_int_enable(rhport);
	return true;
}

bool tud_deinit(uint8_t rhport)
{
	n_deinits++;
	return dcd_deinit(rhport);
}

bool tud_connect(void)
{
	connect_cycles = fake_core_peek_cycles();
	dcd_connect(0);
	return true;
}

bool tud_disconnect(void)
{
	disconnect_cycles = fake_core_peek_cycles();
	dcd_disconnect(0);
	return true;
}

void tud_task_ext(uint32_t timeout_ms, bool in_isr)
{
	(void)timeout_ms;
	(void)in_isr;
	n_tasks++;
}

bool tud_mounted(void)
{
	return mounted;
}

bool tud_suspended(void)
{
	return suspended;
}

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
	wb_usb_dcd_reset_state();
	wb_usb_set_setup_filter(NULL, NULL);
	n_events = 0;
	n_inits = n_deinits = n_tasks = 0;
	mounted = suspended = false;
}

/* Only the accesses to USB0 (the SEC's and the syncs left out). */
static size_t usb_log(struct fake_mmr_access *out, size_t max)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i, n = 0;

	for (i = 0; i < fake_mmr_count() && n < max; i++)
		if (log[i].op != FAKE_MMR_SYNC && log[i].op != FAKE_MMR_CSYNC &&
		    log[i].addr >= USB0_BASE && log[i].addr < USB0_BASE + 0x400u)
			out[n++] = log[i];
	return n;
}

static bool usb_expect(const char *name, const struct fake_mmr_access *want,
		       size_t n)
{
	struct fake_mmr_access got[64];
	size_t i, m = usb_log(got, ARRAY_LEN(got));

	for (i = 0; i < n && i < m; i++) {
		if (got[i].op != want[i].op || got[i].width != want[i].width ||
		    got[i].addr != want[i].addr ||
		    got[i].value != want[i].value) {
			printf("  %s: access %u: got %d/%u %08x=%x, want "
			       "%d/%u %08x=%x\n", name, (unsigned)i, got[i].op,
			       got[i].width, (unsigned)got[i].addr,
			       (unsigned)got[i].value, want[i].op,
			       want[i].width, (unsigned)want[i].addr,
			       (unsigned)want[i].value);
			return false;
		}
	}
	if (m != n) {
		printf("  %s: %u accesses, want %u\n", name, (unsigned)m,
		       (unsigned)n);
		return false;
	}
	return true;
}

/* The counter at the last write that set USB0_PHY_CTL.EN. */
static uint64_t phy_cycles;

static bool phy_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		     /* cppcheck-suppress constParameterCallback ; the hook's type */
		     wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_WRITE && addr == USB0_PHY_CTL &&
	    (*value & USB_PHY_CTL_EN))
		phy_cycles = fake_core_peek_cycles();
	return true;
}

static const struct wb_usb_config cfg24 = {
	.time = &cclk400, .irq_prio = 4,
};

/* --- PHY and clocks ------------------------------------------------------ */

/* USB_PLL_OSC neither written nor polled (S-13a.080). */
static bool pll_untouched(void)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].addr == USB0_PLL_OSC)
			return false;
	return true;
}

static void test_phy_init(void)
{
	static const struct fake_mmr_access want[] = {
		FAKE_W8(USB0_PHY_CTL, 0x80u),
		FAKE_SYNC,
	};
	struct wb_usb_config c = cfg24;

	/* S-13a.080: the PHY enabled, its PLL left alone, no wait. */
	reset();
	wb_usb_phy_init();
	CHECK(fake_mmr_expect("phy", want, ARRAY_LEN(want)));

	/* A bad configuration writes nothing. */
	reset();
	c.time = NULL;
	CHECK(wb_usb_init(&c) == WB_USB_ERR_ARG);
	CHECK(wb_usb_init(NULL) == WB_USB_ERR_ARG);
	CHECK(fake_mmr_count() == 0);
}

/* --- Init, attach, detach ------------------------------------------------ */

static void test_init_sequence(void)
{
	static const struct fake_mmr_access want[] = {
		/* PHY */
		FAKE_W8(USB0_PHY_CTL, 0x80u),
		/* dcd_init: reset, resume, suspend and disconnect interrupts */
		FAKE_W8(USB0_IEN, 0x27u),
		FAKE_W16(USB0_INTRTXE, 1u),
		FAKE_W16(USB0_INTRRXE, 0),
		FAKE_R8(USB0_IRQ, 0),
		FAKE_R16(USB0_INTRTX, 0),
		FAKE_R16(USB0_INTRRX, 0),
	};

	reset();
	fake_mmr_set(USB0_POWER, USB_POWER_HSEN);
	CHECK(wb_usb_init(&cfg24) == WB_USB_OK);
	CHECK(usb_expect("init", want, ARRAY_LEN(want)));
	CHECK(pll_untouched());
	/* Both sources at the given priority, level-sensitive, enabled. */
	CHECK(fake_mmr_get(SEC0_SCTL(WB_SID_USB0_STAT)) ==
	      (4u << 8 | SEC_SCTL_IEN | SEC_SCTL_SEN));
	CHECK(fake_mmr_get(SEC0_SCTL(WB_SID_USB0_DATA)) ==
	      (4u << 8 | SEC_SCTL_IEN | SEC_SCTL_SEN));
	/* Nothing on the bus yet: the pull-up is off. */
	CHECK(!(fake_mmr_get(USB0_POWER) & USB_POWER_SOFTCONN));
	CHECK(!wb_usb_attached());
	CHECK(wb_usb_state() == WB_USB_DETACHED);
	CHECK(n_inits == 1);
}

static void test_attach_detach(void)
{
	reset();
	fake_mmr_set_hook(phy_hook, NULL);
	fake_mmr_set(USB0_POWER, USB_POWER_HSEN);
	CHECK(wb_usb_init(&cfg24) == WB_USB_OK);
	CHECK(pll_untouched());
	n_inits = n_deinits = 0;

	/* Attach: high speed enabled, the pull-up on. */
	fake_mmr_clear_log();
	CHECK(wb_usb_attach() == WB_USB_OK);
	{
		static const struct fake_mmr_access want[] = {
			FAKE_R8(USB0_POWER, 0x20u),
			FAKE_W8(USB0_POWER, 0x60u),
		};
		CHECK(usb_expect("attach", want, ARRAY_LEN(want)));
	}
	/* S-13a.080: the first attach, 5 ms after the PHY enable. */
	CHECK(connect_cycles - phy_cycles >= 400u * WB_USB_PHY_SETTLE_US);
	CHECK(wb_usb_attached());
	CHECK(n_inits == 0);
	CHECK(wb_usb_state() == WB_USB_ATTACHED);
	fake_mmr_set(USB0_DEV_CTL, USB_DEV_CTL_SESSION);
	CHECK(wb_usb_state() == WB_USB_POWERED);
	mounted = true;
	CHECK(wb_usb_state() == WB_USB_CONFIGURED);
	suspended = true;
	CHECK(wb_usb_state() == WB_USB_SUSPENDED);
	mounted = suspended = false;

	/*
	 * A new set while attached (S-10a.030): the pull-up off for at least
	 * 5 ms, the stack restarted, the pull-up on again.
	 */
	fake_core_set_cycle_step(1000);
	CHECK(wb_usb_attach() == WB_USB_OK);
	CHECK(n_deinits == 1 && n_inits == 1);
	CHECK(connect_cycles - disconnect_cycles >= 400u * WB_USB_DETACH_US);
	CHECK(fake_mmr_get(USB0_POWER) & USB_POWER_SOFTCONN);

	wb_usb_task();
	CHECK(n_tasks == 1);

	/* Detach before the OS handover: the pull-up off, the stack stopped. */
	wb_usb_detach();
	CHECK(!(fake_mmr_get(USB0_POWER) & USB_POWER_SOFTCONN));
	CHECK(fake_mmr_get(USB0_IEN) == 0);
	CHECK(n_deinits == 2);
	CHECK(!wb_usb_attached());
	CHECK(wb_usb_state() == WB_USB_DETACHED);
	wb_usb_task();
	CHECK(n_tasks == 1);

	/* Attach again: the stack restarts without a second detach delay. */
	CHECK(wb_usb_attach() == WB_USB_OK);
	CHECK(n_inits == 2 && n_deinits == 2);
	CHECK(fake_mmr_get(USB0_POWER) & USB_POWER_SOFTCONN);
	CHECK(connect_cycles - phy_cycles >= 400u * WB_USB_PHY_SETTLE_US);

	/*
	 * S-13a.080: a new start enables the PHY again, and the time counts
	 * from that write. An attach long after it does not wait.
	 */
	reset();
	fake_mmr_set_hook(phy_hook, NULL);
	fake_core_set_cycles(1000000000u);
	CHECK(wb_usb_init(&cfg24) == WB_USB_OK);
	CHECK(wb_usb_attach() == WB_USB_OK);
	CHECK(phy_cycles >= 1000000000u);
	CHECK(connect_cycles - phy_cycles >= 400u * WB_USB_PHY_SETTLE_US);
	wb_usb_detach();
	fake_core_set_cycles(fake_core_peek_cycles() + 400u * 20000u);
	{
		const uint64_t before = fake_core_peek_cycles();

		CHECK(wb_usb_attach() == WB_USB_OK);
		CHECK(connect_cycles - before < 400u * 100u);
	}
}

/*
 * S-10a.030, S-05.016: a stop takes the pull-up off, then the PHY. Only a
 * new start brings USB back, and S-13a.080's settle counts from its PHY
 * write. With nothing up the stop makes the same two writes.
 */
static void test_stop(void)
{
	reset();
	fake_mmr_set_hook(phy_hook, NULL);
	fake_mmr_set(USB0_POWER, USB_POWER_HSEN);
	CHECK(wb_usb_init(&cfg24) == WB_USB_OK);
	CHECK(wb_usb_attach() == WB_USB_OK);
	fake_mmr_clear_log();
	wb_usb_stop();
	{
		static const struct fake_mmr_access want[] = {
			FAKE_R8(USB0_POWER, 0x60u),	/* tud_disconnect */
			FAKE_W8(USB0_POWER, 0x20u),
			FAKE_R8(USB0_POWER, 0x20u),	/* dcd_deinit */
			FAKE_W8(USB0_POWER, 0x20u),
			FAKE_W8(USB0_IEN, 0),
			FAKE_R8(USB0_POWER, 0x20u),	/* the pull-up: off */
			FAKE_R8(USB0_PHY_CTL, 0x80u),
			FAKE_W8(USB0_PHY_CTL, 0),
		};
		CHECK(usb_expect("stop", want, ARRAY_LEN(want)));
	}
	CHECK(!wb_usb_attached() && wb_usb_state() == WB_USB_DETACHED);
	CHECK(!(fake_mmr_get(SEC0_SCTL(WB_SID_USB0_STAT)) & SEC_SCTL_SEN));
	CHECK(!(fake_mmr_get(SEC0_SCTL(WB_SID_USB0_DATA)) & SEC_SCTL_SEN));
	CHECK(wb_usb_attach() == WB_USB_ERR_ARG);
	CHECK(!(fake_mmr_get(USB0_POWER) & USB_POWER_SOFTCONN));

	/* A new start: the PHY on again, the pull-up 5 ms after it. */
	fake_core_set_cycles(fake_core_peek_cycles() + 400u * 20000u);
	CHECK(wb_usb_init(&cfg24) == WB_USB_OK);
	CHECK(fake_mmr_get(USB0_PHY_CTL) & USB_PHY_CTL_EN);
	CHECK(wb_usb_attach() == WB_USB_OK);
	CHECK(connect_cycles - phy_cycles >= 400u * WB_USB_PHY_SETTLE_US);
	wb_usb_stop();
	wb_usb_stop();			/* twice: nothing new */
	CHECK(!(fake_mmr_get(USB0_PHY_CTL) & USB_PHY_CTL_EN));

	/* Nothing up (USB never started): the pull-up, then the PHY. */
	reset();
	fake_mmr_set(USB0_POWER, USB_POWER_HSEN | USB_POWER_SOFTCONN);
	fake_mmr_set(USB0_PHY_CTL, USB_PHY_CTL_EN);
	wb_usb_stop();
	{
		static const struct fake_mmr_access want[] = {
			FAKE_R8(USB0_POWER, 0x60u),
			FAKE_W8(USB0_POWER, 0x20u),
			FAKE_SYNC,
			FAKE_R8(USB0_PHY_CTL, 0x80u),
			FAKE_W8(USB0_PHY_CTL, 0),
			FAKE_SYNC,
		};
		CHECK(fake_mmr_expect("stop, nothing up", want,
				      ARRAY_LEN(want)));
	}
}

/* --- FIFO set-up and endpoint open --------------------------------------- */

static void test_fifo_alloc(void)
{
	static const struct fake_mmr_access want[] = {
		FAKE_W8(USB0_INDEX, 2u),
		FAKE_W8(USB0_TXFIFOSZ, 0x16u),		/* 512, double */
		FAKE_W16(USB0_TXFIFOADDR, 64u / 8u),
		FAKE_W8(USB0_INDEX, 2u),
		FAKE_W8(USB0_RXFIFOSZ, 0x03u),		/* 64, single */
		FAKE_W16(USB0_RXFIFOADDR, (64u + 1024u) / 8u),
		FAKE_W8(USB0_INDEX, 11u),
		FAKE_W8(USB0_RXFIFOSZ, 0x00u),		/* 1 rounds up to 8 */
		FAKE_W16(USB0_RXFIFOADDR, (64u + 1024u + 64u) / 8u),
	};
	unsigned i;

	reset();
	CHECK(wb_usb_fifo_used() == 64u);
	CHECK(wb_usb_fifo_alloc(2, false, 512, true));
	CHECK(wb_usb_fifo_alloc(2, true, 64, false));
	CHECK(wb_usb_fifo_alloc(11, true, 1, false));
	CHECK(fake_mmr_expect("fifo", want, ARRAY_LEN(want)));
	CHECK(wb_usb_fifo_used() == 64u + 1024u + 64u + 8u);

	/* The 16 KB fill up: the request that does not fit writes nothing. */
	reset();
	for (i = 0; i < 15u; i++)
		CHECK(wb_usb_fifo_alloc(1u + i % 11u, i & 1u, 512, true));
	CHECK(wb_usb_fifo_used() == 64u + 15u * 1024u);
	fake_mmr_clear_log();
	CHECK(!wb_usb_fifo_alloc(3, false, 512, true));
	CHECK(fake_mmr_count() == 0);
	CHECK(wb_usb_fifo_alloc(3, false, 512, false));	/* 512 still fit */
	CHECK(!wb_usb_fifo_alloc(3, false, 512, false));

	/* Endpoint 0 and out-of-range endpoints and sizes are refused. */
	reset();
	CHECK(!wb_usb_fifo_alloc(0, false, 64, false));
	CHECK(!wb_usb_fifo_alloc(12, false, 64, false));
	CHECK(!wb_usb_fifo_alloc(1, false, 0, false));
	CHECK(!wb_usb_fifo_alloc(1, false, 8193, false));
	CHECK(fake_mmr_count() == 0);

	wb_usb_fifo_alloc(1, false, 64, false);
	wb_usb_fifo_reset();
	CHECK(wb_usb_fifo_used() == 64u);
}

static const tusb_desc_endpoint_t ep_bulk_in = {
	.bLength = 7, .bDescriptorType = TUSB_DESC_ENDPOINT,
	.bEndpointAddress = 0x87, .bmAttributes = { .xfer = TUSB_XFER_BULK },
	.wMaxPacketSize = 512, .bInterval = 0,
};
static const tusb_desc_endpoint_t ep_bulk_out = {
	.bLength = 7, .bDescriptorType = TUSB_DESC_ENDPOINT,
	.bEndpointAddress = 0x08, .bmAttributes = { .xfer = TUSB_XFER_BULK },
	.wMaxPacketSize = 512, .bInterval = 0,
};
static const tusb_desc_endpoint_t ep_int_in = {
	.bLength = 7, .bDescriptorType = TUSB_DESC_ENDPOINT,
	.bEndpointAddress = 0x81,
	.bmAttributes = { .xfer = TUSB_XFER_INTERRUPT },
	.wMaxPacketSize = 16, .bInterval = 8,
};

static void test_endpoint_open(void)
{
	static const struct fake_mmr_access want[] = {
		/* bulk IN 0x87 */
		FAKE_W8(USB0_INDEX, 7u),
		FAKE_W8(USB0_TXFIFOSZ, 0x16u),
		FAKE_W16(USB0_TXFIFOADDR, 8u),
		FAKE_W16(USB0_EP_TXMAXP(7), 512u),
		FAKE_R16(USB0_EP_TXCSR(7), 0),
		FAKE_R16(USB0_EP_TXCSR(7), 0),
		FAKE_W16(USB0_EP_TXCSR(7), 0x40u),	/* DATA0 */
		FAKE_W16(USB0_INTRTXE, 0x81u),
		/* bulk OUT 0x08: a stale packet flushed */
		FAKE_W8(USB0_INDEX, 8u),
		FAKE_W8(USB0_RXFIFOSZ, 0x16u),
		FAKE_W16(USB0_RXFIFOADDR, (64u + 1024u) / 8u),
		FAKE_W16(USB0_EP_RXMAXP(8), 512u),
		FAKE_R16(USB0_EP_RXCSR(8), 1u),
		FAKE_W16(USB0_EP_RXCSR(8), 0x10u),
		FAKE_R16(USB0_EP_RXCSR(8), 0),
		FAKE_W16(USB0_EP_RXCSR(8), 0x80u),
		FAKE_W16(USB0_INTRRXE, 0x100u),
		/* interrupt IN 0x81: single-buffered */
		FAKE_W8(USB0_INDEX, 1u),
		FAKE_W8(USB0_TXFIFOSZ, 0x01u),
		FAKE_W16(USB0_TXFIFOADDR, (64u + 2048u) / 8u),
		FAKE_W16(USB0_EP_TXMAXP(1), 16u),
		FAKE_R16(USB0_EP_TXCSR(1), 0),
		FAKE_R16(USB0_EP_TXCSR(1), 0),
		FAKE_W16(USB0_EP_TXCSR(1), 0x40u),
		FAKE_W16(USB0_INTRTXE, 0x83u),
	};

	reset();
	fake_mmr_queue_read(USB0_EP_RXCSR(8), 1u);
	fake_mmr_queue_read(USB0_EP_RXCSR(8), 0);
	CHECK(dcd_edpt_open(0, &ep_bulk_in));
	CHECK(dcd_edpt_open(0, &ep_bulk_out));
	CHECK(dcd_edpt_open(0, &ep_int_in));
	CHECK(fake_mmr_expect("open", want, ARRAY_LEN(want)));
	CHECK(fake_mmr_errors() == 0);

	/* Closing all: interrupts back to endpoint 0, FIFO RAM freed. */
	fake_mmr_clear_log();
	dcd_edpt_close_all(0);
	CHECK(fake_mmr_get(USB0_INTRTXE) == 1u);
	CHECK(fake_mmr_get(USB0_INTRRXE) == 0);
	CHECK(fake_mmr_get(USB0_EP_TXMAXP(7)) == 0);
	CHECK(fake_mmr_get(USB0_EP_RXMAXP(8)) == 0);
	CHECK(fake_mmr_get(USB0_EP_TXMAXP(1)) == 0);
	CHECK(wb_usb_fifo_used() == 64u);
	{
		const struct fake_mmr_access *log = fake_mmr_log();
		size_t i;

		/* Only the opened endpoints are touched. */
		for (i = 0; i < fake_mmr_count(); i++)
			CHECK(log[i].addr != USB0_EP_TXMAXP(2) &&
			      log[i].addr != USB0_EP_RXMAXP(7));
	}
}

/* --- Endpoint 0 ---------------------------------------------------------- */

/* Make the controller hold a SETUP for the next interrupt. */
static void post_setup(const uint8_t s[8])
{
	fake_mmr_queue_read(USB0_IRQ, 0);
	fake_mmr_queue_read(USB0_INTRTX, 1u);
	fake_mmr_queue_read(USB0_INTRRX, 0);
	fake_mmr_queue_read(USB0_EP0_CSR, USB_CSR0_RXPKTRDY);
	fake_mmr_queue_read(USB0_EP0_CNT, 8u);
	fake_mmr_queue_read(USB0_FIFO(0), (uint32_t)s[0] | (uint32_t)s[1] << 8 |
			    (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24);
	fake_mmr_queue_read(USB0_FIFO(0), (uint32_t)s[4] | (uint32_t)s[5] << 8 |
			    (uint32_t)s[6] << 16 | (uint32_t)s[7] << 24);
}

/* An endpoint 0 interrupt with CSR0 reading csr. */
static void post_ep0(uint16_t csr)
{
	fake_mmr_queue_read(USB0_IRQ, 0);
	fake_mmr_queue_read(USB0_INTRTX, 1u);
	fake_mmr_queue_read(USB0_INTRRX, 0);
	fake_mmr_queue_read(USB0_EP0_CSR, csr);
}

static void dcd_ready(void)
{
	reset();
	(void)dcd_init(0, NULL);
	fake_mmr_clear_log();
}

static void test_ep0_get_descriptor(void)
{
	static const uint8_t get_dev[8] = { 0x80, 6, 0, 1, 0, 0, 0x40, 0 };
	static const uint8_t desc[18] = { 18, 1, 0, 2, 0, 0, 0, 64, 0x67, 0x23,
		0x21, 0, 0x57, 2, 1, 2, 3, 1 };
	uint8_t buf[18];

	dcd_ready();
	post_setup(get_dev);
	dcd_int_handler(0);
	CHECK(n_events == 1);
	CHECK(events[0].event_id == DCD_EVENT_SETUP_RECEIVED);
	CHECK(memcmp(&events[0].setup_received, get_dev, 8) == 0);
	/* A data IN request: RXPKTRDY serviced at once. */
	CHECK(fake_mmr_get(USB0_EP0_CSR) == USB_CSR0_SPKTRDY);

	/* The stack answers 18 bytes: one short packet with DATAEND. */
	fake_mmr_clear_log();
	memcpy(buf, desc, sizeof(buf));
	CHECK(dcd_edpt_xfer(0, 0x80, buf, 18, false));
	{
		static const struct fake_mmr_access want[] = {
			FAKE_W32(USB0_FIFO(0), 0x02000112u),
			FAKE_W32(USB0_FIFO(0), 0x40000000u),
			FAKE_W32(USB0_FIFO(0), 0x00212367u),
			FAKE_W32(USB0_FIFO(0), 0x02010257u),
			FAKE_W8(USB0_FIFO(0), 3u),
			FAKE_W8(USB0_FIFO(0), 1u),
			FAKE_W16(USB0_EP0_CSR, 0x0Au),
		};
		CHECK(fake_mmr_expect("ep0 in", want, ARRAY_LEN(want)));
	}

	/* Sent: the data stage completes. Then the status stage. */
	post_ep0(0);
	dcd_int_handler(0);
	CHECK(n_events == 2);
	CHECK(events[1].event_id == DCD_EVENT_XFER_COMPLETE);
	CHECK(events[1].xfer_complete.ep_addr == 0x80);
	CHECK(events[1].xfer_complete.len == 18);
	CHECK(dcd_edpt_xfer(0, 0x00, NULL, 0, false));
	post_ep0(0);
	dcd_int_handler(0);
	CHECK(n_events == 3);
	CHECK(events[2].xfer_complete.ep_addr == 0x00);
	CHECK(events[2].xfer_complete.len == 0);
}

static void test_ep0_set_address(void)
{
	static const uint8_t set_addr[8] = { 0x00, 5, 9, 0, 0, 0, 0, 0 };

	dcd_ready();
	post_setup(set_addr);
	dcd_int_handler(0);
	CHECK(n_events == 1);
	/* No data stage: RXPKTRDY stays until the status stage is armed. */
	CHECK(fake_mmr_get(USB0_EP0_CSR) == 0);
	dcd_set_address(0, 9);
	CHECK(fake_mmr_get(USB0_EP0_CSR) ==
	      (USB_CSR0_SPKTRDY | USB_CSR0_DATAEND));
	/* FADDR is written only after the status stage. */
	CHECK(fake_mmr_get(USB0_FADDR) == 0);
	post_ep0(0);
	dcd_int_handler(0);
	CHECK(fake_mmr_get(USB0_FADDR) == 9u);
	CHECK(n_events == 2 && events[1].xfer_complete.ep_addr == 0x80);
}

static void test_ep0_stall(void)
{
	static const uint8_t vendor[8] = { 0x40, 1, 0, 0, 0, 0, 0, 0 };

	/* The stack refuses a no-data request: the parked SETUP is serviced. */
	dcd_ready();
	post_setup(vendor);
	dcd_int_handler(0);
	dcd_edpt_stall(0, 0x80);	/* EP0 IN: nothing */
	CHECK(fake_mmr_get(USB0_EP0_CSR) == 0);
	dcd_edpt_stall(0, 0x00);
	CHECK(fake_mmr_get(USB0_EP0_CSR) ==
	      (USB_CSR0_SENDSTALL | USB_CSR0_SPKTRDY));
	/* SENTSTALL is cleared and the next SETUP is answered. */
	post_ep0(USB_CSR0_SENTSTALL);
	dcd_int_handler(0);
	CHECK(fake_mmr_get(USB0_EP0_CSR) == 0);
	post_setup(vendor);
	dcd_int_handler(0);
	CHECK(n_events == 2);
}

static unsigned filter_calls;

static enum wb_usb_setup_action filter(void *ctx, const uint8_t s[8],
				       uint8_t *reply, uint16_t *len)
{
	(void)ctx;
	filter_calls++;
	switch (s[1]) {
	case 0x00:		/* GET_STATUS: answered here */
		reply[0] = 0;
		reply[1] = 0;
		*len = 2;
		return WB_USB_SETUP_REPLY;
	case 0x03:		/* SET_FEATURE: accepted here */
		return WB_USB_SETUP_ACCEPT;
	case 0x07:		/* SET_DESCRIPTOR: refused */
		return WB_USB_SETUP_STALL;
	case 0x0A:		/* GET_INTERFACE: a reply too long for EP0 */
		*len = WB_USB_EP0_SIZE;
		return WB_USB_SETUP_REPLY;
	default:
		return WB_USB_SETUP_FORWARD;
	}
}

static void test_ep0_filter(void)
{
	static const uint8_t get_status[8] = { 0x81, 0, 0, 0, 1, 0, 64, 0 };
	static const uint8_t test_mode[8] = { 0x00, 3, 2, 0, 0, 4, 0, 0 };
	static const uint8_t set_desc[8] = { 0x00, 7, 0, 1, 0, 0, 18, 0 };
	static const uint8_t get_itf[8] = { 0x81, 0x0A, 0, 0, 0, 0, 1, 0 };
	static const uint8_t get_cfg[8] = { 0x80, 8, 0, 0, 0, 0, 1, 0 };

	dcd_ready();
	filter_calls = 0;
	wb_usb_set_setup_filter(filter, NULL);

	/* REPLY: two bytes cut to nothing more, DATAEND, no event. */
	post_setup(get_status);
	dcd_int_handler(0);
	{
		static const struct fake_mmr_access want[] = {
			FAKE_W16(USB0_EP0_CSR, USB_CSR0_SPKTRDY),
			FAKE_W8(USB0_FIFO(0), 0),
			FAKE_W8(USB0_FIFO(0), 0),
			FAKE_W16(USB0_EP0_CSR,
				 USB_CSR0_TXPKTRDY | USB_CSR0_DATAEND),
		};
		struct fake_mmr_access w[8];

		CHECK(fake_mmr_writes(w, ARRAY_LEN(w)) == ARRAY_LEN(want));
		CHECK(memcmp(w, want, sizeof(want)) == 0);
	}
	CHECK(n_events == 0);
	/* Its packet sent, then its status stage: still no event. */
	post_ep0(0);
	dcd_int_handler(0);
	post_ep0(0);
	dcd_int_handler(0);
	CHECK(n_events == 0);

	/* ACCEPT: the status stage only. */
	fake_mmr_clear_log();
	post_setup(test_mode);
	dcd_int_handler(0);
	CHECK(fake_mmr_get(USB0_EP0_CSR) ==
	      (USB_CSR0_SPKTRDY | USB_CSR0_DATAEND));
	post_ep0(0);
	dcd_int_handler(0);
	CHECK(n_events == 0);

	/* STALL. */
	post_setup(set_desc);
	dcd_int_handler(0);
	CHECK(fake_mmr_get(USB0_EP0_CSR) ==
	      (USB_CSR0_SPKTRDY | USB_CSR0_SENDSTALL));
	post_ep0(USB_CSR0_SENTSTALL);
	dcd_int_handler(0);

	/* A reply that would need a ZLP is not sent: STALL instead. */
	post_setup(get_itf);
	dcd_int_handler(0);
	CHECK(fake_mmr_get(USB0_EP0_CSR) ==
	      (USB_CSR0_SPKTRDY | USB_CSR0_SENDSTALL));
	post_ep0(USB_CSR0_SENTSTALL);
	dcd_int_handler(0);
	CHECK(n_events == 0);

	/* FORWARD: the stack sees it. */
	post_setup(get_cfg);
	dcd_int_handler(0);
	CHECK(n_events == 1 && events[0].event_id == DCD_EVENT_SETUP_RECEIVED);
	CHECK(filter_calls == 5);

	/* A SETUP right after our own reply's data: answered, not lost. */
	dcd_ready();
	wb_usb_set_setup_filter(filter, NULL);
	post_setup(get_status);
	dcd_int_handler(0);
	post_setup(get_cfg);
	dcd_int_handler(0);
	CHECK(n_events == 1 && events[0].event_id == DCD_EVENT_SETUP_RECEIVED);
	wb_usb_set_setup_filter(NULL, NULL);
}

/*
 * A SETUP that arrives with the tail of the transfer before it (one
 * interrupt, RXPKTRDY set) is held, then offered to the filter like any
 * other: a SET_CONFIGURATION that skips it leaves S-10a.031 to S-10a.034's
 * state behind the stack's.
 */
static void test_ep0_coalesced_filter(void)
{
	static const uint8_t get_dev[8] = { 0x80, 6, 0, 1, 0, 0, 0x40, 0 };
	static const uint8_t set_desc[8] = { 0x00, 7, 0, 1, 0, 0, 18, 0 };
	static const uint8_t set_cfg[8] = { 0x00, 9, 1, 0, 0, 0, 0, 0 };
	static const uint8_t get_status[8] = { 0x81, 0, 0, 0, 1, 0, 64, 0 };
	uint8_t buf[18] = { 18, 1 };

	dcd_ready();
	filter_calls = 0;
	wb_usb_set_setup_filter(filter, NULL);

	/* After a data IN stage: held until the stack's status OUT. */
	post_setup(get_dev);
	dcd_int_handler(0);
	CHECK(n_events == 1);
	CHECK(dcd_edpt_xfer(0, 0x80, buf, sizeof(buf), false));
	post_setup(set_desc);
	dcd_int_handler(0);
	CHECK(n_events == 2 && events[1].xfer_complete.ep_addr == 0x80);
	CHECK(filter_calls == 1);
	CHECK(dcd_edpt_xfer(0, 0x00, NULL, 0, false));
	CHECK(n_events == 3 && events[2].xfer_complete.ep_addr == 0x00);
	CHECK(filter_calls == 2);
	CHECK(fake_mmr_get(USB0_EP0_CSR) ==
	      (USB_CSR0_SPKTRDY | USB_CSR0_SENDSTALL));
	post_ep0(USB_CSR0_SENTSTALL);
	dcd_int_handler(0);

	/* After a status IN stage: the filter's own reply. */
	post_setup(set_cfg);
	dcd_int_handler(0);
	CHECK(n_events == 4 && events[3].event_id == DCD_EVENT_SETUP_RECEIVED);
	CHECK(filter_calls == 3);
	CHECK(dcd_edpt_xfer(0, 0x80, NULL, 0, false));
	post_setup(get_status);
	dcd_int_handler(0);
	CHECK(n_events == 5 && events[4].xfer_complete.ep_addr == 0x80);
	CHECK(filter_calls == 4);
	CHECK(fake_mmr_get(USB0_EP0_CSR) ==
	      (USB_CSR0_TXPKTRDY | USB_CSR0_DATAEND));

	/* Forwarded: the stack sees it as before. */
	post_ep0(0);
	dcd_int_handler(0);
	post_ep0(0);
	dcd_int_handler(0);
	post_setup(set_cfg);
	dcd_int_handler(0);
	CHECK(dcd_edpt_xfer(0, 0x80, NULL, 0, false));
	post_setup(set_cfg);
	dcd_int_handler(0);
	CHECK(filter_calls == 6);
	CHECK(n_events == 8 && events[7].event_id == DCD_EVENT_SETUP_RECEIVED);
	CHECK(events[7].setup_received.bRequest == 9);
	wb_usb_set_setup_filter(NULL, NULL);
}

static enum wb_usb_setup_action reselect_filter(void *ctx, const uint8_t s[8],
						uint8_t *reply, uint16_t *len)
{
	(void)ctx;
	(void)reply;
	(void)len;
	return s[1] == 9 ? WB_USB_SETUP_RESELECT : WB_USB_SETUP_FORWARD;
}

/* RESELECT: TinyUSB answers SET_CONFIGURATION(0), then the request. */
static void test_ep0_reselect(void)
{
	static const uint8_t set_cfg[8] = { 0x00, 9, 1, 0, 0, 0, 0, 0 };
	static const uint8_t get_cfg[8] = { 0x80, 8, 0, 0, 0, 0, 1, 0 };

	dcd_ready();
	wb_usb_set_setup_filter(reselect_filter, NULL);
	fake_mmr_clear_log();
	post_setup(set_cfg);
	dcd_int_handler(0);
	CHECK(n_events == 1);
	CHECK(events[0].event_id == DCD_EVENT_SETUP_RECEIVED);
	CHECK(events[0].setup_received.bRequest == 9);
	CHECK(events[0].setup_received.wValue == 0);
	/* Nothing answered on the bus yet. */
	CHECK(fake_mmr_get(USB0_EP0_CSR) == 0);
	/* usbd closes the configuration's endpoints in between. */
	dcd_edpt_close_all(0);
	/* SET_CONFIGURATION(0)'s status stage: the request itself, no packet. */
	fake_mmr_clear_log();
	CHECK(dcd_edpt_xfer(0, 0x80, NULL, 0, false));
	{
		struct fake_mmr_access w[4];

		CHECK(fake_mmr_writes(w, ARRAY_LEN(w)) == 0);
	}
	CHECK(n_events == 2);
	CHECK(events[1].setup_received.bRequest == 9);
	CHECK(events[1].setup_received.wValue == 1);
	/* Its status stage goes on the bus. */
	CHECK(dcd_edpt_xfer(0, 0x80, NULL, 0, false));
	CHECK(fake_mmr_get(USB0_EP0_CSR) ==
	      (USB_CSR0_SPKTRDY | USB_CSR0_DATAEND));
	post_ep0(0);
	dcd_int_handler(0);
	CHECK(n_events == 3 && events[2].event_id == DCD_EVENT_XFER_COMPLETE);

	/* A host that gives up first (SETUPEND): the request is dropped. */
	post_setup(set_cfg);
	dcd_int_handler(0);
	CHECK(n_events == 4);
	post_ep0(USB_CSR0_SETUPEND);
	dcd_int_handler(0);
	post_setup(get_cfg);
	dcd_int_handler(0);
	CHECK(n_events == 5 && events[4].setup_received.bRequest == 8);

	/* RESELECT for anything but SET_CONFIGURATION is a STALL. */
	dcd_ready();
	wb_usb_set_setup_filter(reselect_filter, NULL);
	{
		static const uint8_t bad[8] = { 0x80, 9, 1, 0, 0, 0, 1, 0 };

		post_setup(bad);
		dcd_int_handler(0);
		CHECK(n_events == 0);
		CHECK(fake_mmr_get(USB0_EP0_CSR) ==
		      (USB_CSR0_SPKTRDY | USB_CSR0_SENDSTALL));
	}
	wb_usb_set_setup_filter(NULL, NULL);
}

/* --- Bulk data ----------------------------------------------------------- */

static void test_bulk_in(void)
{
	static uint8_t data[600];
	unsigned i;

	dcd_ready();
	CHECK(dcd_edpt_open(0, &ep_bulk_in));
	for (i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)i;
	fake_mmr_clear_log();
	CHECK(dcd_edpt_xfer(0, 0x87, data, 600, false));
	/* First packet: 512 bytes (128 words), TXPKTRDY. */
	CHECK(fake_mmr_count() == 129u);
	CHECK(fake_mmr_log()[0].value == 0x03020100u);
	CHECK(fake_mmr_get(USB0_EP_TXCSR(7)) == USB_TXCSR_TXPKTRDY);

	/* Its interrupt: the 88-byte rest (22 words). */
	fake_mmr_clear_log();
	fake_mmr_queue_read(USB0_IRQ, 0);
	fake_mmr_queue_read(USB0_INTRTX, 0x80u);
	fake_mmr_queue_read(USB0_INTRRX, 0);
	fake_mmr_queue_read(USB0_EP_TXCSR(7), 0);
	dcd_int_handler(0);
	CHECK(n_events == 0);
	CHECK(fake_mmr_writes(NULL, 0) == 0);
	{
		struct fake_mmr_access w[32];

		CHECK(fake_mmr_writes(w, ARRAY_LEN(w)) == 23u);
		CHECK(w[0].value == 0x03020100u);	/* byte 512 */
		CHECK(w[22].addr == USB0_EP_TXCSR(7));
	}

	/* Next interrupt: the FIFO still holds a packet, so not done yet. */
	fake_mmr_queue_read(USB0_IRQ, 0);
	fake_mmr_queue_read(USB0_INTRTX, 0x80u);
	fake_mmr_queue_read(USB0_INTRRX, 0);
	fake_mmr_queue_read(USB0_EP_TXCSR(7), USB_TXCSR_NEFIFO);
	dcd_int_handler(0);
	CHECK(n_events == 0);
	fake_mmr_queue_read(USB0_IRQ, 0);
	fake_mmr_queue_read(USB0_INTRTX, 0x80u);
	fake_mmr_queue_read(USB0_INTRRX, 0);
	fake_mmr_queue_read(USB0_EP_TXCSR(7), 0);
	dcd_int_handler(0);
	CHECK(n_events == 1);
	CHECK(events[0].xfer_complete.ep_addr == 0x87);
	CHECK(events[0].xfer_complete.len == 600);

	/* A spurious interrupt afterwards completes nothing. */
	fake_mmr_queue_read(USB0_IRQ, 0);
	fake_mmr_queue_read(USB0_INTRTX, 0x80u);
	fake_mmr_queue_read(USB0_INTRRX, 0);
	fake_mmr_queue_read(USB0_EP_TXCSR(7), 0);
	dcd_int_handler(0);
	CHECK(n_events == 1);

	/* A zero-length packet: TXPKTRDY with nothing written. */
	fake_mmr_clear_log();
	CHECK(dcd_edpt_xfer(0, 0x87, data, 0, false));
	CHECK(fake_mmr_count() == 1u);
	CHECK(fake_mmr_get(USB0_EP_TXCSR(7)) == USB_TXCSR_TXPKTRDY);
}

static void test_bulk_out(void)
{
	static uint8_t buf[31];

	dcd_ready();
	CHECK(dcd_edpt_open(0, &ep_bulk_out));

	/* A packet before any transfer: left in the FIFO, endpoint masked. */
	fake_mmr_queue_read(USB0_IRQ, 0);
	fake_mmr_queue_read(USB0_INTRTX, 0);
	fake_mmr_queue_read(USB0_INTRRX, 0x100u);
	fake_mmr_queue_read(USB0_EP_RXCSR(8), USB_RXCSR_RXPKTRDY);
	dcd_int_handler(0);
	CHECK(n_events == 0);
	CHECK(fake_mmr_get(USB0_INTRRXE) == 0);

	/* Arming drains it: a 31-byte (short) packet completes the transfer. */
	fake_mmr_clear_log();
	fake_mmr_queue_read(USB0_EP_RXCSR(8), USB_RXCSR_RXPKTRDY);
	fake_mmr_queue_read(USB0_EP_RXCSR(8), USB_RXCSR_RXPKTRDY);
	fake_mmr_set(USB0_EP_RXCNT(8), 31u);
	fake_mmr_set(USB0_FIFO(8), 0x43425355u);	/* "USBC" */
	CHECK(dcd_edpt_xfer(0, 0x08, buf, 31, false));
	CHECK(fake_mmr_get(USB0_INTRRXE) == 0x100u);
	CHECK(fake_mmr_get(USB0_EP_RXCSR(8)) == 0);	/* RXPKTRDY cleared */
	CHECK(n_events == 1);
	CHECK(events[0].xfer_complete.ep_addr == 0x08);
	CHECK(events[0].xfer_complete.len == 31);
	CHECK(memcmp(buf, "USBC", 4) == 0);
}

static void test_stall_clear(void)
{
	dcd_ready();
	CHECK(dcd_edpt_open(0, &ep_bulk_in));
	CHECK(dcd_edpt_open(0, &ep_bulk_out));
	fake_mmr_clear_log();
	dcd_edpt_stall(0, 0x87);
	dcd_edpt_stall(0, 0x08);
	dcd_edpt_clear_stall(0, 0x87);
	dcd_edpt_clear_stall(0, 0x08);
	{
		static const struct fake_mmr_access want[] = {
			FAKE_W16(USB0_EP_TXCSR(7), USB_TXCSR_SENDSTALL),
			FAKE_W16(USB0_EP_RXCSR(8), USB_RXCSR_SENDSTALL),
			/* Nothing in either FIFO to flush. */
			FAKE_R16(USB0_EP_TXCSR(7), USB_TXCSR_SENDSTALL),
			FAKE_R16(USB0_EP_TXCSR(7), USB_TXCSR_SENDSTALL),
			FAKE_W16(USB0_EP_TXCSR(7), USB_TXCSR_CLRDATATGL),
			FAKE_R16(USB0_EP_RXCSR(8), USB_RXCSR_SENDSTALL),
			FAKE_R16(USB0_EP_RXCSR(8), USB_RXCSR_SENDSTALL),
			FAKE_W16(USB0_EP_RXCSR(8), USB_RXCSR_CLRDATATGL),
		};
		CHECK(fake_mmr_expect("stall", want, ARRAY_LEN(want)));
	}

	/* SENTSTALL is cleared and SENDSTALL kept while halted. */
	fake_mmr_clear_log();
	fake_mmr_queue_read(USB0_IRQ, 0);
	fake_mmr_queue_read(USB0_INTRTX, 0x80u);
	fake_mmr_queue_read(USB0_INTRRX, 0);
	fake_mmr_queue_read(USB0_EP_TXCSR(7), USB_TXCSR_SENDSTALL |
			    USB_TXCSR_SENTSTALL);
	dcd_int_handler(0);
	CHECK(fake_mmr_get(USB0_EP_TXCSR(7)) == USB_TXCSR_SENDSTALL);
	CHECK(n_events == 0);
}

/* A clear (or SET_INTERFACE) ends the transfer in progress. */
static void test_clear_ends_transfer(void)
{
	static uint8_t data[600], buf[64];

	dcd_ready();
	CHECK(dcd_edpt_open(0, &ep_bulk_in));
	CHECK(dcd_edpt_open(0, &ep_bulk_out));
	CHECK(dcd_edpt_xfer(0, 0x87, data, 600, false));
	CHECK(dcd_edpt_xfer(0, 0x08, buf, 64, false));

	/* Its packet leaves the FIFO and the toggle is reset. */
	fake_mmr_clear_log();
	fake_mmr_queue_read(USB0_EP_TXCSR(7), USB_TXCSR_NEFIFO);
	fake_mmr_queue_read(USB0_EP_TXCSR(7), 0);
	dcd_edpt_clear_stall(0, 0x87);
	{
		static const struct fake_mmr_access want[] = {
			FAKE_R16(USB0_EP_TXCSR(7), USB_TXCSR_NEFIFO),
			FAKE_W16(USB0_EP_TXCSR(7), USB_TXCSR_FLUSHFIFO),
			FAKE_R16(USB0_EP_TXCSR(7), 0),
			FAKE_W16(USB0_EP_TXCSR(7), USB_TXCSR_CLRDATATGL),
		};
		CHECK(fake_mmr_expect("clear in", want, ARRAY_LEN(want)));
	}
	dcd_edpt_clear_stall(0, 0x08);

	/* The old transfers complete nothing. */
	fake_mmr_queue_read(USB0_IRQ, 0);
	fake_mmr_queue_read(USB0_INTRTX, 0x80u);
	fake_mmr_queue_read(USB0_INTRRX, 0x100u);
	fake_mmr_queue_read(USB0_EP_TXCSR(7), 0);
	fake_mmr_queue_read(USB0_EP_RXCSR(8), USB_RXCSR_RXPKTRDY);
	dcd_int_handler(0);
	CHECK(n_events == 0);
	CHECK(fake_mmr_get(USB0_INTRRXE) == 0);	/* waits for a transfer */

	/* A new transfer starts afresh: 4 bytes, one packet. */
	fake_mmr_clear_log();
	CHECK(dcd_edpt_xfer(0, 0x87, data, 4, false));
	CHECK(fake_mmr_count() == 2u);
	fake_mmr_queue_read(USB0_EP_RXCSR(8), USB_RXCSR_RXPKTRDY);
	fake_mmr_queue_read(USB0_EP_RXCSR(8), USB_RXCSR_RXPKTRDY);
	fake_mmr_set(USB0_EP_RXCNT(8), 31u);
	CHECK(dcd_edpt_xfer(0, 0x08, buf, 64, false));
	CHECK(n_events == 1 && events[0].xfer_complete.len == 31);
}

/* --- Bus events ---------------------------------------------------------- */

static void test_bus_events(void)
{
	dcd_ready();
	CHECK(dcd_edpt_open(0, &ep_bulk_in));
	CHECK(wb_usb_fifo_used() > 64u);

	/* A reset at high speed. */
	fake_mmr_queue_read(USB0_IRQ, USB_IRQ_RSTBABBLE);
	fake_mmr_set(USB0_POWER, USB_POWER_HSEN | USB_POWER_HSMODE |
		     USB_POWER_SOFTCONN);
	dcd_int_handler(0);
	CHECK(n_events == 1);
	CHECK(events[0].event_id == DCD_EVENT_BUS_RESET);
	CHECK(events[0].bus_reset.speed == TUSB_SPEED_HIGH);
	CHECK(fake_mmr_get(USB0_INTRTXE) == 1u);
	CHECK(fake_mmr_get(USB0_INTRRXE) == 0);
	CHECK(wb_usb_fifo_used() == 64u);

	/* At full speed. */
	fake_mmr_queue_read(USB0_IRQ, USB_IRQ_RSTBABBLE);
	fake_mmr_set(USB0_POWER, USB_POWER_HSEN | USB_POWER_SOFTCONN);
	dcd_int_handler(0);
	CHECK(events[1].bus_reset.speed == TUSB_SPEED_FULL);

	/* Suspend, resume, the session's end. */
	fake_mmr_queue_read(USB0_IRQ, USB_IRQ_SUSPEND);
	dcd_int_handler(0);
	fake_mmr_queue_read(USB0_IRQ, USB_IRQ_RESUME);
	dcd_int_handler(0);
	fake_mmr_queue_read(USB0_IRQ, USB_IRQ_DISCON);
	dcd_int_handler(0);
	CHECK(n_events == 5);
	CHECK(events[2].event_id == DCD_EVENT_SUSPEND);
	CHECK(events[3].event_id == DCD_EVENT_RESUME);
	CHECK(events[4].event_id == DCD_EVENT_UNPLUGGED);

	/* SOF is reported only once enabled. */
	fake_mmr_queue_read(USB0_IRQ, USB_IRQ_SOF);
	dcd_int_handler(0);
	CHECK(n_events == 5);
	dcd_sof_enable(0, true);
	CHECK(fake_mmr_get(USB0_IEN) == 0x2Fu);
	fake_mmr_queue_read(USB0_IRQ, USB_IRQ_SOF);
	dcd_int_handler(0);
	CHECK(n_events == 6 && events[5].event_id == DCD_EVENT_SOF);

	/* The DMA source only acknowledges. */
	fake_mmr_clear_log();
	wb_usb_irq_data(NULL, WB_SID_USB0_DATA);
	CHECK(fake_mmr_count() == 1u && fake_mmr_log()[0].addr == USB0_DMA_IRQ);
}

/* No access ever reaches the offsets the HRM does not list (0x340-0x343). */
static void test_no_unlisted_offsets(void)
{
	const struct fake_mmr_access *log;
	size_t i;

	dcd_ready();
	CHECK(dcd_edpt_open(0, &ep_bulk_in));
	CHECK(dcd_edpt_open(0, &ep_bulk_out));
	CHECK(dcd_edpt_open(0, &ep_int_in));
	dcd_edpt_close_all(0);
	log = fake_mmr_log();
	for (i = 0; i < fake_mmr_count(); i++)
		CHECK(log[i].addr < USB0_BASE + 0x340u ||
		      log[i].addr >= USB0_BASE + 0x344u);
}

int main(void)
{
	RUN(test_phy_init);
	RUN(test_init_sequence);
	RUN(test_attach_detach);
	RUN(test_stop);
	RUN(test_fifo_alloc);
	RUN(test_endpoint_open);
	RUN(test_ep0_get_descriptor);
	RUN(test_ep0_set_address);
	RUN(test_ep0_stall);
	RUN(test_ep0_filter);
	RUN(test_ep0_coalesced_filter);
	RUN(test_ep0_reselect);
	RUN(test_bulk_in);
	RUN(test_bulk_out);
	RUN(test_stall_clear);
	RUN(test_clear_ends_transfer);
	RUN(test_bus_events);
	RUN(test_no_unlisted_offsets);
	return check_summary("test_drv_usb");
}
