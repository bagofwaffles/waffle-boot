/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Held mode on the master (S-01.016): the entry steps of boot_held_enter()
 * and the service loop over the links. The menus and console (S-15), the
 * log (S-14), USB and the host commands (S-10a, S-10c) are opxy_services.c's
 * and opxy_usb.c's; S-11's resume is opxy_install.c's.
 */
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_bridge.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_sec.h"
#include "waffle/board/opxy_rtc_board.h"
#include "waffle/wire_crc.h"

static uint8_t nordic_rx[OPXY_UART_BUF], nordic_tx[OPXY_UART_BUF];
/* S-17.021: the PSoC's info reply, for S-17's revision checks. */
static uint8_t psoc_info[S17_REPLY_MAX];
static int psoc_info_len = -1;

/* --- the PSoC link --- */

/*
 * A read takes at most PSOC_CHUNK bytes, and only while the inbox has room
 * for every frame they can complete: one under way, then one per 7 bytes,
 * the shortest frame on the wire (S-06.004). A backlog then waits in the
 * ring instead of overflowing the inbox, which would keep a stale bitmap
 * (S-07.031). A read for an awaited reply is not held back.
 */
#define PSOC_CHUNK 32u
#define PSOC_CHUNK_FRAMES (1u + (PSOC_CHUNK - 1u) / 7u)
/* Enough passes to empty a full ring. */
#define SETTLE_PASSES (OPXY_PSOC_RING / PSOC_CHUNK + S06_INBOX)

static volatile uint32_t psoc_irqs;	/* for the log line */

/* The status interrupt: the transmit side; DMA11 receives (S-13b.070). */
static void psoc_isr(void *ctx, uint32_t sid)
{
	psoc_irqs++;
	wb_uart_isr(ctx, sid);
}

static bool reply_awaited(const struct s06_link *l)
{
	for (unsigned i = 0; i < S06_SLOTS; i++)
		if (l->slot[i].state == S06_SLOT_PENDING)
			return true;
	return false;
}

static size_t psoc_read(void *ctx, uint8_t *buf, size_t max)
{
	struct opxy_master *m = ctx;
	const struct s06_link *l = &m->link[S06_LINK_PSOC];

	if (!l->raw && !reply_awaited(l)) {
		if (l->in_count + PSOC_CHUNK_FRAMES > S06_INBOX)
			return 0;
		if (max > PSOC_CHUNK)
			max = PSOC_CHUNK;
	}
	return wb_uart_read(&m->hw.uart[OPXY_UART_PSOC], buf, max);
}

static size_t psoc_room(void *ctx)
{
	struct opxy_master *m = ctx;

	return wb_uart_room(&m->hw.uart[OPXY_UART_PSOC]);
}

static void psoc_write(void *ctx, const uint8_t *data, size_t n)
{
	struct opxy_master *m = ctx;

	wb_uart_write(&m->hw.uart[OPXY_UART_PSOC], data, n);
}

static const struct s06_port_ops psoc_port = { psoc_read, psoc_room, psoc_write };

/* S-07.031 to S-15, with a long operation's hold; the time first, so a
 * hold that has reached its time runs before the event (S-15.015). */
static void psoc_key(void *ctx, uint8_t key, bool down)
{
	struct opxy_master *m = ctx;

	opxy_menu_tick(m);
	s15_menu_event(&m->sv.menu, key, down);
}

void opxy_held_psoc(struct opxy_master *m)
{
	struct wb_uart *u = &m->hw.uart[OPXY_UART_PSOC];

	m->psoc_ops.key_event = psoc_key;
	m->link[S06_LINK_PSOC].ops = &psoc_port;
	m->link[S06_LINK_PSOC].ctx = m;
	(void)wb_sec_attach(WB_SID_UART0_STAT, OPXY_PSOC_IRQ_PRIO, false,
			    psoc_isr, u);
	(void)wb_sec_enable(WB_SID_UART0_STAT);
	wb_uart_use_irq(u, true);
}

int opxy_held_settle(struct opxy_master *m)
{
	struct wb_uart *u = &m->hw.uart[OPXY_UART_PSOC];
	const struct s06_link *l = &m->link[S06_LINK_PSOC];
	struct s14_text t;

	opxy_psoc_rx_report(m);
	if (!s15_menu_held(&m->sv.menu))
		return S07_OK;
	for (unsigned i = 0; i < SETTLE_PASSES; i++) {
		int rc = s07_poll(&m->psoc);

		if (rc != S07_OK)
			return rc;
		if (wb_uart_rx_ready(u) == 0u && l->in_count == 0u)
			break;
	}
	s15_menu_release(&m->sv.menu);
	s14_text_init(&t);
	s14_text_str(&t, "long operation over: interrupts ");
	s14_text_dec(&t, psoc_irqs);
	s14_text_str(&t, ", line errors ");
	s14_text_dec(&t, u->line_errors);
	s14_log_text(&m->sv.log, S14_INFO, S14_TAG("psoc"), &t);
	return S07_OK;
}

/* --- boot_held_ops --- */

static void greeting(void *ctx)
{
	struct opxy_master *m = ctx;

	(void)s08_link_greeting(&m->nordic);
}

static void info(void *ctx)
{
	struct opxy_master *m = ctx;
	int status = 0;

	psoc_info_len = s07_info_request(&m->psoc, psoc_info,
					 sizeof(psoc_info), &status);
	opxy_copro_held(m, psoc_info, psoc_info_len);
}

static void held_state(void *ctx)
{
	struct opxy_master *m = ctx;

	s07_report(&m->psoc, S07_REPORT_HELD);
}

/* S-07.043 and the rest: one pass per loop over every link. */
void opxy_held_service(void *ctx, const struct boot_held *h)
{
	struct opxy_master *m = ctx;

	opxy_held_screen(m, h); OPXY_TESTOS(opxy_testos_frame_timing(m);)	/* the test OS: HW-FRAME's timing, once */
	for (;;) {
		/* S-02.095: alive at the start of the first pass; once per boot. */
		opxy_start_finished(m);
		s06_node_poll(&m->node);
		if (s07_service_pass(&m->psoc) == S07_POWEROFF ||
		    opxy_held_settle(m) == S07_POWEROFF ||
		    s08_poll(&m->nordic) != 0)
			break;
		if (m->slave_present) {
			s09_master_service(&m->bridge);
			opxy_bridge_end_note(m);	/* S-13b.083 */
		}
		/* S-09.046: again each time the clock's value changes */
		opxy_clock_to_slave(m);
		opxy_services_poll(m); OPXY_TIMING_HELD(m);
	}
	/* S-07.051: the PSoC removes the power. */
	opxy_powered_off(m);
}

void opxy_held(void *ctx, const struct boot_held *h)
{
	struct opxy_master *m = ctx;
	struct boot_held_ops ops;
	OPXY_CRUMB(10u); OPXY_TIME(BOOT_TIME_HELD);
	OPXY_TESTOS(opxy_testos_paint(OPXY_PAINT_LIME);)
	opxy_held_psoc(m);
	/* S-13b.071: UART1 for the Nordic link, first used now (S-08.001). */
	(void)wb_uart_init(&m->hw.uart[OPXY_UART_NORDIC], OPXY_UART_NORDIC,
			   &opxy_uart_nordic, &m->hw.time, nordic_rx,
			   sizeof(nordic_rx), nordic_tx, sizeof(nordic_tx));
	s06_link_init(&m->link[S06_LINK_NORDIC], &opxy_port_ops,
		      &m->hw.uart[OPXY_UART_NORDIC], S06_ADDR_MASTER,
		      S06_ADDR_NORDIC);
	m->node.link[S06_LINK_NORDIC] = &m->link[S06_LINK_NORDIC];
	/* S-09.045: the CPU link through the bridge's tunnel. */
	if (m->slave_present) {
		s06_link_init(&m->link[S06_LINK_CPU], &s09_tunnel_port,
			      &m->bridge, S06_ADDR_MASTER, S06_ADDR_SLAVE);
		m->node.link[S06_LINK_CPU] = &m->link[S06_LINK_CPU];
	}
	/* S-06.050 */
	m->node.routes = s06_master_routes;
	m->node.nroutes = 6u;

	opxy_nordic_ops(m, &m->nordic_ops);
	m->nordic_ops.crc16 = wire_crc16_xmodem;
	s08_init(&m->nordic, &m->node, S06_LINK_NORDIC, &m->nordic_ops,
		 (int)m->meta.state);
	/* S-08.045: the sku read at start-up; the word is 0 when the record
	 * or the chip serial (OTP item 1, S-04.047) did not read. */
	s08_set_sku(&m->nordic, m->meta.sku,
		    !m->meta_failed && m->sv.chip_ok);

	/* S-13a.071: RTC0 from held mode's start, before any service reads
	 * it (S-12.016's stamps, MTP's DateTime). */
	opxy_rtc_board_start(&m->hw.time);
	/* S-09.046: the clock message once as held mode starts */
	opxy_clock_to_slave(m);
	opxy_services_start(m);
	OPXY_TESTOS(opxy_testos_paint(OPXY_PAINT_NAVY); opxy_testos_bridge_check(m);)

	ops.ctx = m;
	ops.nordic_greeting = greeting;
	ops.psoc_info = info;
	ops.psoc_held_state = held_state;
	ops.service = opxy_held_service;
	/* S-11.080: before the Nordic's first greeting is answered, so that
	 * the answer goes by the state 0 (S-08.013). */
	if (h->interrupted)
		opxy_install_interrupted(m);
	boot_held_enter(&ops, h);
}
