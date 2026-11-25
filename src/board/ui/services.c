/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The master's services: see waffle/board/services.h. */
#include "waffle/board/services.h"

#include <stdlib.h>
#include <string.h>

#include "waffle/board/boot.h"
#include "waffle/board/emmc_bridge.h"
#include "waffle/drivers/bf70x_core.h"

/* --- S-06: dropped frames go to the log (S-16.020) --- */

static void node_dropped(void *ctx, unsigned link, int why,
			 const uint8_t *data, size_t n)
{
	struct opxy_master *m = ctx;

	(void)link;
	/* The raw frame: route, seq, id, len (S-06.003). */
	if (why == S06_DROP_CRC && data != NULL && n >= 3u)
		s14_log_bad_crc(&m->sv.log, data[0], data[1], data[2]);
}

/* --- S-14 --- */

/* S-14.027: whole or not at all, a sequence number only when written. */
static bool log_psoc_send(void *ctx, uint8_t route, uint8_t id,
			  const uint8_t *payload, size_t len)
{
	struct opxy_master *m = ctx;

	(void)route;	/* S14_ROUTE_PSOC: the link's own route */
	return s06_send_log(&m->node, S06_LINK_PSOC, id, payload, len);
}

/* The CDC link is U1's (S-10c.031): taken only while the port is open. */
static bool log_cdc_send(void *ctx, uint8_t route, uint8_t id,
			 const uint8_t *payload, size_t len)
{
	struct opxy_master *m = ctx;

	(void)route;	/* S14_ROUTE_CDC: the link's own route */
	if (m->sv.host_ops.cdc_open == NULL ||
	    !m->sv.host_ops.cdc_open(m->sv.host.ctx))
		return false;
	return s06_send_log(&m->node, S06_LINK_CDC, id, payload, len);
}

static void host_boot_start(struct opxy_master *m);

void opxy_log_start(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;

	sv->node_ops.ctx = m;
	sv->node_ops.dropped = node_dropped;
	m->node.obs = sv->node_ops;
	sv->log_clock = opxy_log_clock(&m->hw);
	sv->log_psoc.ctx = m;
	sv->log_psoc.send = log_psoc_send;
	sv->log_cdc.ctx = m;
	sv->log_cdc.send = log_cdc_send;
	sv->log_screen.ctx = &sv->menu.con;
	sv->log_screen.show = s15_console_sink;
	s14_log_init(&sv->log, S14_MASTER, &sv->log_clock, &sv->log_psoc,
		     &sv->log_cdc, &sv->log_screen);
	/* Before any key event (S-15.013): sv->boot is filled and the env
	 * built when held mode starts, and nothing reads them before. */
	s15_menu_init(&sv->menu, opxy_dev(m), &sv->boot, &sv->env, &sv->log);
	/* S-07.076, S-10c.045: the common set from the PSoC link's start */
	host_boot_start(m);
}

/* --- S-15: the draw ops that record --- */

static void rec_fill(void *ctx, uint16_t x, uint16_t y, uint16_t w,
		     uint16_t h, uint16_t rgb565)
{
	struct opxy_draw_rec *r = ctx;

	r->fills++;
	r->x = x;
	r->y = y;
	r->w = w;
	r->h = h;
	r->color = rgb565;
}

static void rec_text(void *ctx, uint16_t x, uint16_t y, const char *s,
		     size_t n, uint16_t fg, uint16_t bg)
{
	struct opxy_draw_rec *r = ctx;
	size_t k = n < OPXY_DRAW_TEXT_MAX ? n : OPXY_DRAW_TEXT_MAX;

	r->texts++;
	r->x = x;
	r->y = y;
	r->color = fg;
	(void)bg;
	memcpy(r->text, s, k);
	r->text_len = (uint8_t)k;
}

static void rec_ink(void *ctx, uint16_t x, uint16_t y, const char *s,
		    size_t n, uint16_t fg)
{
	rec_text(ctx, x, y, s, n, fg, 0);
}

void opxy_draw_record(struct opxy_draw_rec *r, struct s15_draw_ops *ops)
{
	memset(r, 0, sizeof(*r));
	ops->ctx = r;
	ops->fill = rec_fill;
	ops->text = rec_text;
	ops->ink = rec_ink;
	ops->bold = rec_ink;
}

/* --- S-17: the info exchange over S-06 --- */

static int info_exchange(void *ctx, enum s17_peer peer, uint32_t timeout_ms,
			 uint8_t *reply, size_t cap)
{
	struct opxy_master *m = ctx;
	struct s06_frame f;
	const unsigned link = peer == S17_PEER_NORDIC ? S06_LINK_NORDIC
						      : S06_LINK_PSOC;
	size_t n;

	if (s06_transact(&m->node, link, OPXY_INFO_REQUEST, NULL, 0u,
			 timeout_ms * 1000u, &f) != 1 ||
	    f.id != OPXY_INFO_REPLY)
		return -1;
	n = f.len < cap ? f.len : cap;
	memcpy(reply, f.payload, n);
	return (int)n;
}

/* --- S-08: the Nordic link's events --- */

/* S-08's events as log lines; a DFU error reply verbatim (S-16.099). */
static void nordic_log(void *ctx, int ev, const uint8_t *data, size_t n,
		       uint32_t a)
{
	struct opxy_master *m = ctx;
	struct s14_text t;

	if (ev == S08_LOG_DFU_ERROR) {
		s14_log_dfu_error(&m->sv.log, "nordic", data, n);
		return;
	}
	s14_text_init(&t);
	if (ev == S08_LOG_EXIT_LEFT_OUT) {	/* S-08.048 */
		s14_text_str(&t, "exit left out: no application");
		s14_log_text(&m->sv.log, S14_WARNING, S14_TAG("nordic"), &t);
		return;
	}
	s14_text_str(&t, "nordic ");
	s14_text_hex(&t, (uint32_t)ev, 1u);
	s14_text_str(&t, " ");
	s14_text_hex(&t, a, 1u);
	s14_log_text(&m->sv.log, S14_INFO, S14_TAG("nordic"), &t);
}

static int nordic_power(void *ctx, const struct s06_frame *f)
{
	struct opxy_master *m = ctx;

	return s07_power_message(&m->psoc, f);
}

/*
 * S-07.076, S-08.004: a coprocessor's message its own document does not
 * claim, handled by S-10c's common set (S-10c.045) whenever S-07 and S-08
 * handle the link's messages: from the PSoC link's start with the boot
 * ops (host_boot_start), from held mode's start with the held ones.
 */
void opxy_copro_common(struct opxy_master *m, enum s10c_link link,
		       const struct s06_frame *f)
{
	struct s14_text t;

	if (s14_log_relay(&m->sv.log, (uint8_t)(f->route >> 4), f->id,
			  f->payload, f->len))
		return;
	if (m->sv.host.ops != NULL &&
	    s10c_host_frame(&m->sv.host, link, f->route, f->id, f->payload,
			    f->len))
		return;
	s14_text_init(&t);
	s14_text_str(&t, "not served ");
	s14_text_hex(&t, f->id, 2u);
	s14_log_text(&m->sv.log, S14_INFO,
		     S14_TAG(link == S10C_LINK_PSOC ? "psoc" : "nordic"), &t);
}

/* S-08.004: the Nordic's other messages are S-10c's common command set. */
static void nordic_common(void *ctx, const struct s06_frame *f)
{
	opxy_copro_common(ctx, S10C_LINK_NORDIC, f);
}

/* S-17.011, S-17.014: a greeting's revision byte, logged as two hex
 * digits when it sets the reported revision. */
static void nordic_revision(void *ctx, uint8_t rev)
{
	struct opxy_master *m = ctx;
	const uint8_t greeting[2] = { 0u, rev };
	uint8_t byte;
	struct s14_text t;

	if (!s17_rev_greeting(&m->sv.rev, greeting, sizeof(greeting), &byte))
		return;
	s14_text_init(&t);
	s14_text_str(&t, "hw_rev ");
	s14_text_hex(&t, byte, 2u);
	s14_log_text(&m->sv.log, S14_INFO, S14_TAG("rev"), &t);
}

void opxy_nordic_ops(struct opxy_master *m, struct s08_ops *ops)
{
	ops->ctx = m;
	ops->log = nordic_log;
	ops->power = nordic_power;
	ops->common = nordic_common;
	ops->revision = nordic_revision;
}

/* --- S-04: reboots --- */

/* S-04.065 for the soft reboot (W2). S-10a.030: the device detached
 * first, since the PSoC may reset the processor at any time after it. */
static void rb_psoc_reset_request(void *ctx)
{
	struct opxy_master *m = ctx;
	static const uint8_t magic[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

	opxy_usb_stop();
	(void)s06_send(&m->node, S06_LINK_PSOC, 0u, OPXY_PSOC_RESET_ID, magic,
		       sizeof(magic));
}

/* S-04.025, S-04.032: skipped when the slave is absent (S-09.008), and
 * on a device without second-cpu (S-04.066). */
static void rb_slave_power_off(void *ctx)
{
	struct opxy_master *m = ctx;

	if (m->slave_present && wb_has(opxy_dev(m), WB_F_SECOND_CPU))
		(void)s09_power_off(&m->bridge, &m->node, S06_LINK_CPU);
}

/*
 * S-04.063: the display's power-off first, so no frame starts from the
 * shutdown's start (S-13c.022), then the slave's power-off (W9), then the
 * reports to the PSoC, which follow the last frame's end. The UART
 * moves at most a FIFO's worth of bytes each time it is serviced, so the
 * reports can sit in the ring behind queued log lines; a hard reset masks
 * the interrupts that would send them (S-04.028), and the PSoC would keep
 * the backlight lit into the next start. They are sent here.
 *
 * Ahead of the slave's power-off, a mounted volume's unmount, while which
 * the slave shuts the eMMC down (S-03.011). Its reply is waited for within
 * T_UNMOUNT (S-09.060); a failure or no reply does not stop the shutdown.
 * A wait that runs out counts as a failed unmount and leaves the slave
 * present, so the power-off request still follows (S-04.063).
 */
/* S-04.066's ops: the volume's unmount runs S-03.013 in the storage
 * (board/storage/storage.c), then the reports. */
static bool one_cpu_mounted(void *ctx)
{
	const struct opxy_master *m = ctx;

	return m->sv.files.mounted;
}

static int one_cpu_emmc_shutdown(void *ctx)
{
	struct opxy_master *m = ctx;

	return s09_files_unmount(&m->sv.files) ? 0 : -1;
}

static void one_cpu_reports(void *ctx)
{
	struct opxy_master *m = ctx;

	s07_shutdown_reports(&m->psoc);
}

void opxy_shutdown(struct opxy_master *m)
{
	opxy_display_power_off(m);
	if (!wb_has(opxy_dev(m), WB_F_SECOND_CPU)) {
		/* S-04.066: no CPU link, no power-off request, no wait */
		const struct s04_shutdown_ops ops = {
			m, one_cpu_mounted, one_cpu_emmc_shutdown, one_cpu_reports,
		};

		s04_shutdown_one_cpu(&ops);
		(void)wb_uart_flush(&m->hw.uart[OPXY_UART_PSOC], &m->hw.time,
				    OPXY_PSOC_DRAIN_US);
		return;
	}
	s09_master_shutdown(&m->bridge);
	if (m->slave_present && m->sv.files.mounted)
		(void)s09_files_unmount(&m->sv.files);
	rb_slave_power_off(m);
	s07_shutdown_reports(&m->psoc);
	(void)wb_uart_flush(&m->hw.uart[OPXY_UART_PSOC], &m->hw.time,
			    OPXY_PSOC_DRAIN_US);
}

/*
 * A reboot's shutdown: then the backlight's time to go dark (S-13c.019's
 * 00 00 02) before the record and the reset, so the next start does not
 * find it lit. It comes after the slave's power-off, which only lengthens
 * the master's delay behind the slave's reset (S-04.032).
 *
 * S-04.063, S-10a.030: then the device detached and the PHY off, before
 * the record and the reset. Not sooner: W3's reply to sub-command 05
 * (S-10c.091), queued just before the reboot, has had the settle's time
 * to reach the host. A power-off the PSoC reports does not come here
 * (rb_power_off()).
 */
static void rb_shutdown(void *ctx)
{
	struct opxy_master *m = ctx;

	opxy_shutdown(m);
	wb_delay_us(&m->hw.time, OPXY_BACKLIGHT_SETTLE_MS * 1000u);
	opxy_usb_stop();
}

/*
 * S-13b.072: each enabled UART's queue and transmitter empty, or its
 * bounded wait run out (the Nordic holding CTS off); the UARTs stay on.
 */
static void drain_uarts(struct opxy_master *m)
{
	(void)wb_uart_drain(OPXY_UART_PSOC, &m->hw.uart[OPXY_UART_PSOC],
			    &m->hw.time);
	(void)wb_uart_drain(OPXY_UART_NORDIC, &m->hw.uart[OPXY_UART_NORDIC],
			    &m->hw.time);
}

/*
 * A power-off the PSoC reports (S-07.051): after the shutdown's reports
 * the device detached and the PHY off (S-04.063, S-10a.030), then the
 * 0x32 answer, since the PSoC removes the power after it. S-13b.072: at
 * the shutdown's end both UARTs are drained, so every byte owed on either
 * link is out before the 0x32; opxy_powered_off() then sends the 0x32
 * itself out.
 */
static void rb_power_off(void *ctx)
{
	opxy_shutdown(ctx);
	opxy_usb_stop();
	drain_uarts(ctx);
}

/* The UART holds two bytes at most and is not serviced from interrupts
 * outside held mode, so the 0x32 leaves only through this drain there. */
_Noreturn void opxy_powered_off(struct opxy_master *m)
{
	drain_uarts(m);
	for (;;)
		wb_core_idle();
}

static void rb_nordic_enter_dfu(void *ctx)
{
	struct opxy_master *m = ctx;

	(void)s08_enter_dfu(&m->nordic);
}

/* S-04.027; nothing is sent after a failed application transfer
 * (S-08.048). */
static void rb_nordic_exit_dfu(void *ctx)
{
	struct opxy_master *m = ctx;

	(void)s08_exit_dfu(&m->nordic);
}

/* S-13b.072: the exit request has left the pin first, or the wait ran out
 * (the Nordic holding CTS off). */
static void rb_nordic_link_stop(void *ctx)
{
	struct opxy_master *m = ctx;

	(void)wb_uart_disable(OPXY_UART_NORDIC, &m->hw.uart[OPXY_UART_NORDIC],
			      &m->hw.time);
}

static void rb_delay_ms(void *ctx, uint32_t ms)
{
	struct opxy_master *m = ctx;

	wb_delay_us(&m->hw.time, ms * 1000u);
}

/* S-13b.072 before the hard reset's arming, and S-04.028: before the
 * record, after which nothing goes out on a link. */
static void rb_drain_links(void *ctx)
{
	drain_uarts(ctx);
}

static void rb_write_record(void *ctx, uint16_t reason)
{
	(void)ctx;
	s04_reboot_write((uint8_t *)(uintptr_t)S04_REBOOT_ADDR, reason);
}

static void rb_hard_reset(void *ctx)
{
	(void)ctx;
	opxy_hard_reset();
}

/* The soft reboot: the PSoC removes and restores the power (S-04.065). */
static void rb_wait_for_psoc(void *ctx)
{
	(void)ctx;
	for (;;)
		wb_core_idle();
}

/*
 * S-17.021 step 1 on entering held mode: the Nordic first, after S-08's
 * greeting (S-08.001); the PSoC's exchange is S-07.077's, sent once by
 * the caller and passed in (len -1: no reply).
 */
void opxy_copro_held(struct opxy_master *m, const uint8_t *psoc,
		     int psoc_len)
{
	static uint8_t reply[S17_REPLY_MAX];
	const int n = info_exchange(m, S17_PEER_NORDIC, S17_REPLY_TIMEOUT_MS,
				    reply, sizeof(reply));

	if (n >= 0)
		s17_copro_held_reply(&m->sv.copro, S17_PEER_NORDIC, reply,
				     (size_t)n);
	if (psoc_len >= 0)
		s17_copro_held_reply(&m->sv.copro, S17_PEER_PSOC, psoc,
				     (size_t)psoc_len);
}

void opxy_reboot(struct opxy_master *m, enum s04_writer w, uint16_t arg)
{
	s04_reboot(&m->sv.reboot, w, arg);
}

/* --- S-10c: the board's side --- */

static bool host_option_on(void *ctx)
{
	struct opxy_master *m = ctx;

	return m->sv.option;
}

static void version_of(const uint16_t v[4], struct s11_version *out)
{
	out->major = v[0];
	out->minor = v[1];
	out->patch = v[2];
	out->build = v[3];
}

static void host_read_info(void *ctx, struct s10c_info *v)
{
	struct opxy_master *m = ctx;
	size_t n = strlen(m->meta.serial);

	memset(v, 0, sizeof(*v));
	v->serial_len = (uint8_t)(n < sizeof(v->serial) ? n : sizeof(v->serial));
	memcpy(v->serial, m->meta.serial, v->serial_len);
	/* S-10c.055 D: the TE serial from OTP item 1 (S-16.095) */
	v->dsp_ok = m->sv.chip_ok;
	memcpy(v->dsp_otp, m->sv.chip_serial, strlen(m->sv.chip_serial));
	v->hw_rev = s17_rev_reported(&m->sv.rev);
	v->sku = m->meta.sku;
	v->factory = m->meta.factory;
	version_of(m->meta.sw_version, &v->sw);
	version_of(m->meta.os_version, &v->os);
	v->bl.major = OPXY_BL_VERSION_MAJOR;
	v->bl.minor = OPXY_BL_VERSION_MINOR;
	v->bl.patch = OPXY_BL_VERSION_PATCH;
	v->bl.build = OPXY_BL_VERSION_BUILD;
}

/* The version the image's descriptor carries (tools/mkldr.py reads it out
 * of the image): what the front shows with the early logo (S-15.132). */
const uint16_t opxy_bl_version[4] = {
	OPXY_BL_VERSION_MAJOR, OPXY_BL_VERSION_MINOR,
	OPXY_BL_VERSION_PATCH, OPXY_BL_VERSION_BUILD,
};

/* S-04.046: the keys applied to the record read at start, then M7. */
static bool host_write_metadata(void *ctx, const struct s10c_setmeta *sm)
{
	struct opxy_master *m = ctx;
	char serial[sizeof(sm->serial) + 1u], sku[11], factory[11];
	struct s04_setmeta req = { NULL, NULL, NULL };
	struct s14_text t;

	if (sm->has_serial) {
		memcpy(serial, sm->serial, sm->serial_len);
		serial[sm->serial_len] = '\0';
		req.serial = serial;
	}
	if (sm->has_sku) {
		s14_text_init(&t);
		s14_text_dec(&t, sm->sku);
		memcpy(sku, t.buf, t.len);
		sku[t.len] = '\0';
		req.sku_variant = sku;
	}
	if (sm->has_factory) {
		s14_text_init(&t);
		s14_text_dec(&t, sm->factory);
		memcpy(factory, t.buf, t.len);
		factory[t.len] = '\0';
		req.factory = factory;
	}
	if (!s04_set_metadata(opxy_dev(m), &m->meta, m->sv.host.factory, m->sv.option,
			      &req))
		return false;
	return opxy_meta_write(m);
}

static bool host_read_fsn(void *ctx, uint32_t words[4])
{
	(void)ctx;
	return opxy_read_fsn(words);
}

static void host_log_config(void *ctx, const uint8_t *payload, size_t len)
{
	struct opxy_master *m = ctx;

	s14_log_config(&m->sv.log, payload, len);
}

static void host_log_relay(void *ctx, uint8_t id, const uint8_t *payload,
			   size_t len)
{
	struct opxy_master *m = ctx;

	(void)s14_log_relay(&m->sv.log, S06_ADDR_HOST, id, payload, len);
}

static void host_reboot_os(void *ctx)
{
	opxy_reboot(ctx, S04_W2, 0u);
}

static void host_reboot_mode(void *ctx, bool has_mode, uint8_t mode)
{
	opxy_reboot(ctx, S04_W1,
		    s04_host_mode_reason(&mode, has_mode ? 1u : 0u));
}

static void host_reboot_after_dfu(void *ctx)
{
	opxy_reboot(ctx, S04_W3, 0u);
}

#if !defined(OPXY_TEST_OS)
/* S-10c.076: what S-15.190 would do now; a scan or an install the board
 * runs is busy too. */
static enum s10c_continue host_continue_check(void *ctx)
{
	const struct opxy_master *m = ctx;

	if (m->sv.working)
		return S10C_CONT_BUSY;
	return s15_menu_continue_check(&m->sv.menu) == S15_CONT_STARTED ?
	       S10C_CONT_GO : S10C_CONT_BUSY;
}

/* The reply is out (S-10c.076); the menu draws the restarting view and
 * runs env_continue_os(). */
static void host_continue(void *ctx, enum s10c_link link)
{
	struct opxy_master *m = ctx;

	(void)link;	/* the reply leaves before the shutdown's detach */
	(void)s15_menu_continue(&m->sv.menu);
}
#endif

/* --- S-10c.045 before held mode: the boot ops --- */

static void boot_not_taken(struct opxy_master *m, const char *what)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, what);
	s14_text_str(&t, " not taken before held mode");
	s14_log_text(&m->sv.log, S14_INFO, S14_TAG("host"), &t);
}

static void boot_upgrade_screen(void *ctx, uint32_t total)
{
	(void)ctx;
	(void)total;
}

static void boot_begin_refused(void *ctx, enum s11_reason reason)
{
	(void)ctx;
	(void)reason;
}

/* S-10c.045 before held mode (S-07.023 steps 1-6a): a begin is answered as
 * one whose transfer cannot start (S-10c.071's 84), no screen opens and
 * the reboots 18 and EE have no effect; none is kept for held mode. */
static bool boot_transfer_start(void *ctx)
{
	boot_not_taken(ctx, "transfer");
	return false;
}

static bool boot_transfer_append(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	(void)p;
	(void)n;
	return false;
}

static void boot_transfer_discard(void *ctx)
{
	(void)ctx;
}

static void boot_transfer_submit(void *ctx, enum s10c_source source,
				 struct s11_outcome *o)
{
	(void)ctx;
	(void)source;
	memset(o, 0, sizeof(*o));
}

static void boot_ramdisk_offer(void *ctx)
{
	(void)ctx;
}

static void boot_reboot_os(void *ctx)
{
	boot_not_taken(ctx, "reboot");
}

static void boot_reboot_mode(void *ctx, bool has_mode, uint8_t mode)
{
	(void)has_mode;
	(void)mode;
	boot_not_taken(ctx, "reboot");
}

static void boot_reboot_after_dfu(void *ctx)
{
	boot_not_taken(ctx, "reboot");
}

/* The ops that need no held-mode service are the held ones. */
static void host_boot_start(struct opxy_master *m)
{
	struct s10c_ops *o = &m->sv.host_ops;

	opxy_usb_host_ops(m, o);
	o->option_on = host_option_on;
	o->read_info = host_read_info;
	o->write_metadata = host_write_metadata;
	o->read_fsn = host_read_fsn;
	o->log_config = host_log_config;
	o->log_relay = host_log_relay;
	o->upgrade_screen = boot_upgrade_screen;
	o->begin_refused = boot_begin_refused;
	o->transfer_start = boot_transfer_start;
	o->transfer_append = boot_transfer_append;
	o->transfer_discard = boot_transfer_discard;
	o->transfer_submit = boot_transfer_submit;
	o->ramdisk_offer = boot_ramdisk_offer;
	o->reboot_os = boot_reboot_os;
	o->reboot_mode = boot_reboot_mode;
	o->reboot_after_dfu = boot_reboot_after_dfu;
	s10c_host_init(&m->sv.host, o, m, opxy_dev(m), (enum wb_dev_how)m->dev_how);
}

void opxy_startup_read(struct opxy_master *m, wb_otp_get_fn get)
{
	struct opxy_services *sv = &m->sv;
	const unsigned rev = opxy_strap_revision(m->strap);

	/* S-04.047: not read when the record could not be read */
	sv->chip_serial[0] = '\0';
	sv->chip_ok = !m->meta_failed && opxy_otp_serial(get, sv->chip_serial);
	/* S-17.012, S-17.015; without straps 0 until a greeting (S-17.016) */
	s17_rev_power_on_dev(&sv->rev, opxy_dev(m), (rev >> 1) & 1u, rev & 1u);
	s17_rev_metadata(&sv->rev, m->meta_failed ? S17_META_RECORD_FAILED
				   : sv->chip_ok  ? S17_META_READ
						  : S17_META_SERIAL_FAILED);
}

/* --- S-12.061, S-12.062: how the drive's volume was mounted --- */

/* S-09.041's way as S-15's (enum s15_files). */
static uint8_t files_way(const struct s09_files *f)
{
	if (!f->mounted)
		return S15_FILES_NONE;
	if (f->way == 0u || f->way > S15_FILES_FOR_WRITE)
		return S15_FILES_SCANNED;
	return f->way;
}

/* Each mount's way, into the log and Device Info 2 (S-15.192). */
static void files_note(struct opxy_master *m)
{
	const struct s09_files *f = &m->sv.files;
	struct s15_buf b;
	struct s14_text t;

	m->sv.boot.files_way = files_way(f);
	m->sv.boot.files_objects = f->objects;
	if (!f->mount_sent || f->mount_lost)
		return;
	s15_files_text(&b, files_way(f), f->objects);
	s14_text_init(&t);
	s14_text_str(&t, b.s);
	if (f->mounted && files_way(f) != S15_FILES_RESTORED && f->objects != 0u) {
		s14_text_str(&t, ", ");
		s14_text_dec(&t, f->objects);
		s14_text_str(&t, " objects");
	}
	s14_log_text(&m->sv.log, S14_INFO, S14_TAG("files"), &t);
}

void opxy_services_start(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;

	/* S-17.012, S-17.015 are opxy_startup_read()'s; S-17.021 step 1 is
	 * opxy_copro_held(). */
	s17_copro_init(&sv->copro);
	sv->copro_link.info_exchange = info_exchange;

	sv->reboot.ctx = m;
	sv->reboot.psoc_reset_request = rb_psoc_reset_request;
	sv->reboot.shutdown = rb_shutdown;
	sv->reboot.nordic_enter_dfu = rb_nordic_enter_dfu;
	sv->reboot.nordic_exit_dfu = rb_nordic_exit_dfu;
	sv->reboot.nordic_link_stop = rb_nordic_link_stop;
	sv->reboot.delay_ms = rb_delay_ms;
	sv->reboot.drain_links = rb_drain_links;
	sv->reboot.write_record = rb_write_record;
	sv->reboot.hard_reset = rb_hard_reset;
	sv->reboot.wait_for_psoc = rb_wait_for_psoc;
	/* S-04.063 step 1 on a PSoC power-off (S-07.051): the CPU link's
	 * tunnel is up from here, so the slave is asked to power off before
	 * the reports. */
	m->psoc_ops.shutdown = rb_power_off;

	opxy_display_draw_ops(m, &sv->draw);
	sv->menu_shown = s15_menu_changes(&sv->menu);
	sv->provisioned = opxy_otp_provisioned(opxy_otp_get);
	/* S-12.052: the mount's result says whether the volume is refused.
	 * S-09.018: the slave mounts only now, on this 0x08, within its
	 * T_EXEC; every file command of the master's comes after it. A slave
	 * the load lost answers nothing: the mount, sent to no one, records
	 * that (s09_files_slave_lost()) and is not tried again this boot. */
	s09_files_init(&sv->files, &m->bridge);
	s09_files_fs_ops(&sv->files, &sv->staging);
	sv->scan_due = false;
	sv->working = false;
	if (m->slave_present) {
		int32_t rc;

		OPXY_TIME(BOOT_TIME_MOUNT_SENT);
		rc = s09_files_mount(&sv->files);
		OPXY_TIMING_MOUNT(rc, sv->files.err);
		files_note(m);
	}
	opxy_settings_read(m, &sv->settings); OPXY_TIME(BOOT_TIME_SETTINGS);
	opxy_menu_env(m, &sv->env);
	opxy_install_ops(m, &sv->run, &sv->step);

	opxy_usb_host_ops(m, &sv->host_ops);
	sv->host_ops.option_on = host_option_on;
	sv->host_ops.read_info = host_read_info;
	sv->host_ops.write_metadata = host_write_metadata;
	sv->host_ops.read_fsn = host_read_fsn;
	sv->host_ops.log_config = host_log_config;
	sv->host_ops.log_relay = host_log_relay;
	sv->host_ops.reboot_os = host_reboot_os;
	sv->host_ops.reboot_mode = host_reboot_mode;
	sv->host_ops.reboot_after_dfu = host_reboot_after_dfu;
	/* S-10c.076; the test OS never hands over */
	sv->host_ops.continue_check = OPXY_TESTOS_OR(NULL, host_continue_check);
	sv->host_ops.continue_os = OPXY_TESTOS_OR(NULL, host_continue);
	sv->host_ops.gate = OPXY_TESTOS_OR(NULL, opxy_hostlink_gate);
	opxy_hostlink_start(m);
	s10c_host_init(&sv->host, &sv->host_ops, m, opxy_dev(m), (enum wb_dev_how)m->dev_how);
	opxy_usb_start(m); OPXY_TIME(BOOT_TIME_USB);
}

/* Rendered, then sent: what was drawn reaches the panel on this call. */
void opxy_menu_show(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;
	const struct s15_geometry *g = opxy_display_geometry();

	/* cppcheck-suppress knownConditionTrueFalse ; a weak default */
	if (g != NULL && s15_menu_changes(&sv->menu) != sv->menu_shown) {
		sv->menu_shown = s15_menu_changes(&sv->menu);
		s15_menu_render(&sv->menu, g, &sv->draw);
	}
	opxy_display_send();
}

/* Before a long call: the screen on the panel and its frame ended. */
void opxy_menu_show_now(struct opxy_master *m)
{
	opxy_menu_show(m);
	opxy_display_settle();
}

void opxy_menu_tick(struct opxy_master *m)
{
	s15_menu_tick(&m->sv.menu, (uint32_t)wb_time_ms(&m->hw.time));
}

void opxy_services_poll(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;

	opxy_usb_poll(m);
	/* A developer host's key events (waffle/host/devhost.h). */
	OPXY_TESTOS_OR((void)0, opxy_dev_keys(m));
	opxy_menu_tick(m);
	/* S-15.105: only differences of the time matter. */
	s15_up_tick(&sv->menu, (uint32_t)wb_time_ms(&m->hw.time));
	if (sv->scan_due) {
		sv->scan_due = false;
		sv->working = true;
		opxy_scan(m);
		sv->working = false;
	}
	opxy_menu_show(m);
}

/* --- S-05.006, S-15.163: a failed plain start --- */

_Static_assert(S15_AT_COUNT == S09_AT_COUNT && S15_AT_IN_POLL == S09_LOST_IN_POLL,
	       "S-15's names for where the bridge broke follow S-09's");

/* S-05.006, S-05.007: a failed select or read, the bridge's included, says
 * nothing about the image, so it is S05_FAIL_SELECT and writes no record. */
enum s05_fail opxy_stage_fail(const struct s03_load *ld)
{
	switch (ld->result) {
	case S03_LOAD_OK:
		return S05_FAIL_NONE;
	case S03_LOAD_ENCRYPTED:
		return S05_FAIL_ENCRYPTED;
	case S03_LOAD_IO:
		return S05_FAIL_SELECT;
	case S03_LOAD_NO_ACTIVE:
	case S03_LOAD_ZERO_LENGTH:
	case S03_LOAD_TOO_LONG:
	case S03_LOAD_PAST_PARTITION:
	case S03_LOAD_FORMAT:
	case S03_LOAD_CRC:
	default:
		return S05_FAIL_NO_OS;
	}
}

static enum s15_check start_check(enum s05_refusal r)
{
	switch (r) {
	case S05_REFUSE_LENGTH: return S15_CHECK_LENGTH;
	case S05_REFUSE_PARSE: return S15_CHECK_PARSE;
	case S05_REFUSE_FLAGS: return S15_CHECK_FLAGS;
	case S05_REFUSE_STAGING: return S15_CHECK_STAGING;
	case S05_REFUSE_RESERVE: return S15_CHECK_RESERVE;
	case S05_REFUSE_RECORD: return S15_CHECK_RECORD;
	case S05_REFUSE_BOOT_RESERVE: return S15_CHECK_BOOT_RESERVE;
	case S05_REFUSE_SPACE:
	case S05_ACCEPT:
	default: return S15_CHECK_SPACE;
	}
}

/* S-05.006's causes: what the header or the image said. */
static void start_no_os(const struct boot_start *st, struct s15_start *s)
{
	const struct s03_load *ld = &st->load;

	switch (ld->result) {
	case S03_LOAD_ZERO_LENGTH:
		s->why = S15_START_ZERO_LENGTH;
		break;
	case S03_LOAD_TOO_LONG:
		s->why = S15_START_TOO_LONG;
		s->value = ld->header.length;
		s->limit = S03_OS_MAX;
		break;
	case S03_LOAD_PAST_PARTITION:		/* S-16.136 */
		s->why = S15_START_PAST_PARTITION;
		s->value = s03_sectors(ld->header.length) + 1u;
		s->limit = st->boot_size / S03_SECTOR;
		break;
	case S03_LOAD_FORMAT:
		s->why = S15_START_FORMAT;
		s->value = ld->header.format;
		break;
	case S03_LOAD_CRC:
		s->why = S15_START_CRC;
		s->value = ld->crc;
		s->limit = ld->header.crc;
		break;
	case S03_LOAD_NO_ACTIVE:
	case S03_LOAD_OK:
	case S03_LOAD_ENCRYPTED:
	case S03_LOAD_IO:
	default:
		s->why = S15_START_NO_ACTIVE;
		break;
	}
}

/* S-05.007: the request that failed. */
static void start_io(const struct s03_load *ld, struct s15_start *s)
{
	s->part = ld->step_part;
	switch (ld->step) {
	case S03_STEP_HEADER:
		s->why = S15_START_HEADER_READ;
		break;
	case S03_STEP_IMAGE:
		s->why = S15_START_IMAGE_READ;
		s->value = ld->sector;
		s->limit = ld->count;
		break;
	case S03_STEP_USER:
		s->why = S15_START_USER_SELECT;
		break;
	case S03_STEP_SELECT:
	case S03_STEP_NONE:
	default:
		s->why = S15_START_SELECT;
		break;
	}
}

void opxy_start_view(enum s05_fail cause, const struct boot_start *st,
		     struct s15_start *out)
{
	memset(out, 0, sizeof(*out));
	out->why = S15_START_OK;
	out->lost = st->lost;
	out->cmd = st->why.cmd;
	out->at = st->why.at;
	switch (cause) {
	case S05_FAIL_NONE:
		out->lost = false;
		out->cmd = 0;
		out->at = 0;
		break;
	case S05_FAIL_ENCRYPTED:
		out->why = S15_START_ENCRYPTED;
		out->part = st->load.part;
		break;
	case S05_FAIL_SELECT:
		start_io(&st->load, out);
		break;
	case S05_FAIL_REFUSED:
		out->why = S15_START_REFUSED;
		out->check = start_check(st->refusal);
		break;
	case S05_FAIL_SLAVE:
		out->why = st->replied ? S15_START_SLAVE_NO_OS : S15_START_NO_REPLY;
		out->value = st->reply;
		break;
	case S05_FAIL_SLAVE_START:
		out->why = S15_START_SLAVE_START;
		break;
	case S05_FAIL_FILE:			/* S-05.035 */
		out->why = S15_START_FILE;
		out->file_why = st->os_file.why;
		out->value = st->os_file.value;
		out->limit = st->os_file.limit;
		break;
	case S05_FAIL_NO_OS:
	default:
		start_no_os(st, out);
		break;
	}
	out->file = cause != S05_FAIL_NONE && st->file;	/* S-15.195 */
	out->listed = out->file && st->listed;		/* S-05.044 */
}

/* The boot partition's load, as a failed start's line gives it. */
static void load_log(struct s14_text *t, const struct s03_load *ld)
{
	s14_text_str(t, " load ");
	s14_text_dec(t, (uint32_t)ld->result);
	s14_text_str(t, " step ");
	s14_text_dec(t, (uint32_t)ld->step);
	s14_text_str(t, " part ");
	s14_text_dec(t, ld->step != S03_STEP_NONE ? ld->step_part : ld->part);
	if (ld->step == S03_STEP_IMAGE) {
		s14_text_str(t, " sector ");
		s14_text_dec(t, ld->sector);
		s14_text_str(t, "+");
		s14_text_dec(t, ld->count);
	}
	s14_text_str(t, " length ");
	s14_text_dec(t, ld->header.length);
	s14_text_str(t, " format ");
	s14_text_dec(t, ld->header.format);
	if (ld->result == S03_LOAD_CRC) {
		s14_text_str(t, " crc ");
		s14_text_hex(t, ld->crc, 8u);
		s14_text_str(t, " header ");
		s14_text_hex(t, ld->header.crc, 8u);
	}
}

void opxy_start_log(struct opxy_master *m, enum s05_fail cause,
		    const struct boot_start *st)
{
	const struct s03_load *ld = &st->load;
	struct s15_start s;
	struct s15_buf why;
	struct s14_text t;

	opxy_bridge_end_note(m);	/* S-13b.083: a reply end changed in the load */
	opxy_start_view(cause, st, &s);
	s15_start_reason(&why, &s);
	s14_text_init(&t);
	s14_text_str(&t, st->file ? "OS not started from /os.ldr: " : "OS not started: ");
	s14_text_str(&t, why.s);
	s14_text_str(&t, "; cause ");
	s14_text_dec(&t, (uint32_t)cause);
	if (st->file) {				/* S-05.035: no partition read */
		s14_text_str(&t, " length ");
		s14_text_dec(&t, ld->length);
		s14_text_str(&t, " crc ");
		s14_text_hex(&t, st->file_crc, 8u);
	} else {
		load_log(&t, ld);
	}
	if (cause == S05_FAIL_REFUSED) {
		s14_text_str(&t, " refusal ");
		s14_text_dec(&t, (uint32_t)st->refusal);
	}
	if (cause == S05_FAIL_SLAVE) {
		s14_text_str(&t, st->replied ? " reply " : " no reply");
		if (st->replied)
			s14_text_dec(&t, st->reply);
	}
	if (st->lost) {
		s14_text_str(&t, "; bridge lost on ");
		s14_text_hex(&t, st->why.cmd, 2u);
		s14_text_str(&t, " arg ");
		s14_text_hex(&t, st->why.arg0, 1u);
		s14_text_str(&t, " at ");
		s14_text_hex(&t, st->why.at, 2u);
		s14_text_str(&t, " fault");
		for (unsigned i = 0; i < 4u; i++) {
			s14_text_str(&t, " ");
			s14_text_hex(&t, st->why.fault[i], 1u);
		}
	}
	s14_log_text(&m->sv.log, S14_ERROR, S14_TAG("boot"), &t);
}

static uint8_t *put8(uint8_t *p, uint32_t v)
{
	*p = (uint8_t)v;
	return p + 1;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
	for (unsigned i = 0; i < 4u; i++)
		p[i] = (uint8_t)(v >> (8u * i));
	return p + 4;
}

size_t opxy_start_report(const struct opxy_master *m,
			 uint8_t out[OPXY_START_REPORT])
{
	const struct boot_held *h = &m->sv.held;
	const struct boot_start *st = &h->start;
	const struct s03_load *ld = &st->load;
	uint8_t *p = out;

	memset(out, 0, OPXY_START_REPORT);
	if (!h->load_attempted)
		return OPXY_START_REPORT;
	p = put8(p, 1u);
	p = put8(p, (uint32_t)h->cause);
	p = put8(p, (uint32_t)ld->result);
	p = put8(p, (uint32_t)ld->step);
	p = put8(p, ld->step_part);
	p = put8(p, ld->part);
	p = put8(p, ld->select0_failed ? 1u : 0u);
	p = put32(p, ld->sector);
	p = put32(p, ld->count);
	p = put32(p, ld->header.format);
	p = put32(p, ld->header.length);
	p = put32(p, ld->header.crc);
	p = put32(p, ld->crc);
	p = put32(p, st->boot_size);
	p = put8(p, (uint32_t)st->refusal);
	p = put8(p, st->replied ? 1u : 0u);
	p = put8(p, st->reply);
	p = put8(p, st->lost ? 1u : 0u);
	p = put8(p, st->why.cmd);
	p = put8(p, st->why.at);
	p = put32(p, st->why.arg0);
	for (unsigned i = 0; i < 4u; i++)
		p = put32(p, st->why.fault[i]);
	p = put8(p, st->file ? 1u : 0u);		/* S-05.035 */
	p = put8(p, (uint32_t)st->os_file.why);
	p = put32(p, st->file ? ld->length : 0u);
	p = put32(p, st->os_file.value);
	p = put32(p, st->os_file.limit);
	p = put32(p, st->file_crc);
	return (size_t)(p - out);
}

/* --- S-15: held mode's first screen --- */

/* A version as s11_version_text() writes it, NUL-terminated, into
 * out[S11_VERSION_TEXT_MAX + 1]. */
static void version_str(const struct s11_version *v, char *out)
{
	out[s11_version_text(v, out, S11_VERSION_TEXT_MAX)] = '\0';
}

/*
 * What the boot gives S-15 (struct s15_boot). The screens show no build
 * date since the owner's revision, so it stays empty.
 */
static void boot_info(struct opxy_master *m, const struct boot_held *h)
{
	struct s15_boot *b = &m->sv.boot;
	struct s11_version v;

	memset(b, 0, sizeof(*b));
	v.major = OPXY_BL_VERSION_MAJOR;
	v.minor = OPXY_BL_VERSION_MINOR;
	v.patch = OPXY_BL_VERSION_PATCH;
	v.build = OPXY_BL_VERSION_BUILD;
	version_str(&v, m->sv.bl_text);
	version_of(m->meta.os_version, &v);
	version_str(&v, m->sv.os_text);
	version_of(m->meta.sw_version, &v);
	version_str(&v, m->sv.sw_text);
	/* S-17.030: the values S-17.021 step 1 read on entering held mode */
	version_str(&m->sv.copro.nordic_app, m->sv.ble_text);
	version_str(&m->sv.copro.psoc, m->sv.psoc_text);
	memcpy(m->sv.psoc_serial, m->sv.copro.psoc_serial,
	       m->sv.copro.psoc_serial_len);
	m->sv.psoc_serial[m->sv.copro.psoc_serial_len] = '\0';
	b->version = m->sv.bl_text;
	/* S-18.004: the device not identified, with the first stage's n and
	 * c (read again in bring-up when the front's record says so) */
	b->dev_how = (enum wb_dev_how)m->dev_how;
	b->first_stage_n = m->fs_n;
	b->first_stage_c = m->fs_c;
	b->reason = h->reason;
	b->settings = m->sv.settings;
	/* A release answered with a value other than 0 is "no OS"
	 * (S-05.021), one whose OS did not start "not started" (S-05.032);
	 * an absent slave is S-09.008's, and one the bridge lost after the
	 * start, the release's missing reply included, stopped answering. */
	if (h->slave_absent OPXY_TESTOS(|| m->bridge.absent))
		b->slave = S15_SLAVE_ABSENT;
	else if (h->cause == S05_FAIL_SLAVE && h->start.replied)
		b->slave = S15_SLAVE_NO_OS;
	else if (h->cause == S05_FAIL_SLAVE_START)
		b->slave = S15_SLAVE_NO_START;		/* S-05.032 */
	/* cppcheck-suppress knownConditionTrueFalse ; the test OS's build takes it above (waffle/testos.h) */
	else if ((m->slave_present && m->bridge.absent) ||
		 h->cause == S05_FAIL_SLAVE)
		b->slave = S15_SLAVE_LOST;
	else
		b->slave = S15_SLAVE_OK;
	b->running_known = m->running_established;
	b->running_slot = m->running_slot;
	opxy_start_view(h->load_attempted ? h->cause : S05_FAIL_NONE,
			&h->start, &b->start);	/* S-15.163, S-15.161 */
	b->interrupted = h->interrupted;			/* S-11.080 */
	b->unit_serial = m->meta.serial;
	b->chip_serial = m->sv.chip_serial;	/* S-17.030 */
	b->psoc_serial = m->sv.psoc_serial;
	b->os_version = m->sv.os_text;		/* S-04's record */
	b->sw_version = m->sv.sw_text;
	b->ble_version = m->sv.ble_text;
	b->psoc_version = m->sv.psoc_text;
	b->provisioned = m->sv.provisioned;
	b->files_way = files_way(&m->sv.files);			/* S-15.192 */
	b->files_objects = m->sv.files.objects;
}

void opxy_held_screen(struct opxy_master *m, const struct boot_held *h)
{
	m->sv.held = *h;
	/* S-04.014: a start from /os.ldr that held mode took before its load;
	 * the request is gone, and the reason is the screen's own */
	if (h->reason == S04_REASON_OS_FILE && !h->load_attempted)
		s14_log_str(&m->sv.log, S14_WARNING, S14_TAG("boot"),
			    "/os.ldr not started: this start opened the boot menu");
	if (S04_REASON_IS_OS_LIST(h->reason) && !h->load_attempted)	/* S-04.072 */
		s14_log_str(&m->sv.log, S14_WARNING, S14_TAG("boot"),
			    "OS file not started: this start opened the boot menu");
	boot_info(m, h);
	opxy_display_held_start(m);
	/* S-14.025: level-0 lines reach the screen once the display is up;
	 * without a display (S-13c) there is none. */
	/* cppcheck-suppress knownConditionTrueFalse ; a weak default */
	if (opxy_display_geometry() != NULL)
		s14_log_screen_start(&m->sv.log);
	s15_menu_start(&m->sv.menu);
}

/* --- S-15's environment --- */

static void env_usb_set(void *ctx, enum s15_usb_set set)
{
	opxy_usb_select(ctx, opxy_usb_state(set));
}

/* S-07.045: the key test's LEDs; the end's ends with 0x40 {01}
 * (S-07.067). */
static void env_keytest_leds(void *ctx, bool start)
{
	struct opxy_master *m = ctx;

	if (start)
		s07_key_test_start(&m->psoc);
	else
		s07_key_test_stop(&m->psoc);
}

/* --- S-15's file operations on the slave's /yaffs2 (S-09.041) --- */

/* cppcheck-suppress constParameterCallback ; s15_env_ops fixes the type */
static bool env_volume_refused(void *ctx)
{
	const struct opxy_master *m = ctx;

	return s09_files_volume_refused(&m->sv.files);
}

/* cppcheck-suppress constParameterCallback ; s15_env_ops fixes the type */
static bool env_slave_lost(void *ctx)
{
	const struct opxy_master *m = ctx;

	return s09_files_slave_lost(&m->sv.files);
}

static bool env_dir_exists(void *ctx, const char *path)
{
	struct opxy_master *m = ctx;

	return s09_files_dir_exists(&m->sv.files, path);
}

static bool env_pack_dir(void *ctx, size_t index, char name[S15_NAME_MAX + 1])
{
	struct opxy_master *m = ctx;

	if (s09_files_pack_dir(&m->sv.files, index, name))
		return true;
	name[0] = '\0';
	return false;
}

static size_t env_read_manifest(void *ctx, const char *dir, uint8_t *buf,
				size_t max)
{
	struct opxy_master *m = ctx;

	return s09_files_read_manifest(&m->sv.files, dir, buf, max);
}

/*
 * S-10a.049: held mode's long file operations run under the host hold. They
 * run on screens whose set has no disk (S-10a.010); were one offered, it is
 * withdrawn for the operation (an own write's withdrawal: no release, so no
 * scan) and offered again after it, with the medium change.
 */
static bool long_begin(struct opxy_master *m)
{
	const bool offered = opxy_usb_up() && s10a_usb_disk()->offered;

	opxy_host_hold_begin(m);
	if (offered)
		s10a_usb_disk_withdraw(S10A_WITHDRAW_OWN_WRITE);
	return offered;
}

static void long_end(struct opxy_master *m, bool offered)
{
	if (offered && m->sv.usb_state == S10A_STATE_UPGRADE &&
	    s15_up_disk_offerable(&m->sv.menu))
		s10a_usb_disk_offer();
	opxy_host_hold_end(m);
}

/* The long calls hold the menu's keys (opxy_held_settle()). */
static bool env_remove_tree(void *ctx, const char *path)
{
	struct opxy_master *m = ctx;
	const bool offered = long_begin(m);
	bool ok;

	s15_menu_hold(&m->sv.menu);
	opxy_menu_show_now(m);
	ok = s09_files_remove_tree(&m->sv.files, path);
	long_end(m, offered);
	return ok;
}

/* S-15.052: S-15 reports no failure of its own, so the log does. */
static void env_erase_os_fat(void *ctx)
{
	struct opxy_master *m = ctx;
	bool offered;

	/* S-03.075: no erase outside the volume without fat-region */
	if (!wb_has(opxy_dev(m), WB_F_FAT_REGION))
		return;
	offered = long_begin(m);
	s15_menu_hold(&m->sv.menu);
	opxy_menu_show_now(m);
	if (!s09_files_erase_os_fat(&m->sv.files))
		s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG,
			    "os fat erase failed");
	long_end(m, offered);
}

/* S-12.062: the scan before the session's first write, its line on the
 * panel first; the host waits meanwhile. */
static void env_files_scan(void *ctx)
{
	struct opxy_master *m = ctx;
	const bool offered = long_begin(m);

	s15_menu_hold(&m->sv.menu);
	opxy_menu_show_now(m);
	OPXY_TIMING_WRITE_SCAN(m, true);
	if (!s09_files_for_write(&m->sv.files) && s09_files_restored(&m->sv.files))
		s14_log_str(&m->sv.log, S14_WARNING, S14_TAG("files"),
			    "scan before a write not made: files open");
	OPXY_TIMING_WRITE_SCAN(m, false);
	files_note(m);
	long_end(m, offered);
}

/* S-15.072: s09_files_format() mounts again after every format, which
 * alone sets the refused state from then on. */
static bool env_format(void *ctx)
{
	struct opxy_master *m = ctx;
	const bool offered = long_begin(m);
	bool ok;

	s15_menu_hold(&m->sv.menu);
	opxy_menu_show_now(m);
	ok = s09_files_format(&m->sv.files);
	files_note(m);
	if (!ok)
		s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG,
			    "yaffs2 format failed");
	long_end(m, offered);
	return ok;
}

/* S-15.094: each report drawn at once, the write being one long call. */
static void osw_step(void *ctx, enum s15_osw_step step)
{
	struct opxy_master *m = ctx;

	s15_osw_step(&m->sv.menu, step);
	opxy_menu_show(m);
	opxy_hostlink_progress(m, 0u, 0u, s15_osw_label(step));
}

static void osw_progress(void *ctx, uint32_t done, uint32_t total)
{
	struct opxy_master *m = ctx;

	s15_osw_progress(&m->sv.menu, done, total);
	opxy_menu_show(m);
	opxy_hostlink_progress(m, done, total, NULL);
}

static void osw_done(void *ctx, uint32_t a, uint32_t b)
{
	struct opxy_master *m = ctx;

	s15_osw_done(&m->sv.menu, a, b);
	opxy_menu_show(m);
}

/*
 * S-15.094 with S-03's OS write on the master's eMMC (opxy_bridge_emmc()).
 * The image is read whole first, into the heap: S-01.016's staging area
 * lies under the RAM disk's second part.
 */
const char *opxy_osw_path = OPXY_OS_LDR;
unsigned opxy_os_place = OPXY_OS_PLACE_NONE;

static void os_write(struct opxy_master *m, struct s15_osw_end *end)
{
	uint8_t *image;

	s15_menu_hold(&m->sv.menu);
	opxy_menu_show_now(m);	/* the screen before the first request */
	/* The test OS (waffle/testos.h): the inactive boot partition holds the
	 * factory OS, the way back to a normal unit, so the write is refused
	 * before anything reaches the eMMC. */
	OPXY_TESTOS(end->result = S15_OSW_FAILED; end->why = S15_OSW_WHY_DISABLED; return;)
	/* cppcheck-suppress unreachableCode ; true of the test OS's build only (waffle/testos.h) */
	image = malloc(S03_OS_MAX);
	if (image == NULL) {
		s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG,
			    "os write: no memory");
		end->result = S15_OSW_FAILED;
		end->why = S15_OSW_WHY_MEMORY;
		return;
	}
	{
		const struct s15_osw_report rep = { m, osw_step, osw_progress, osw_done };

		(void)s09_files_os_write_from(&m->sv.files, opxy_osw_path, &m->emmc, image,
					      S03_OS_MAX, &rep, end);
	}
	free(image);
	if (end->select0_failed) {	/* S-03.063: reported, the result kept */
		s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG,
			    "os write: select 0 failed");
	}
}

static void env_os_write(void *ctx, struct s15_osw_end *end)
{
	struct opxy_master *m = ctx;
	const bool offered = long_begin(m);

	os_write(m, end);
	long_end(m, offered);
}

static void env_option(void *ctx, bool on)
{
	struct opxy_master *m = ctx;

	m->sv.option = on;
}


static void env_volcal(void *ctx)
{
	struct opxy_master *m = ctx;

	s07_volcal(&m->psoc);
}

static void env_system_info(void *ctx)
{
	opxy_system_info(ctx);
}

/*
 * S-11.045: the scan runs from the service pass once the menu's tick has
 * returned, not inside it; S-15 starts no other scan meanwhile (S-15.103).
 */
static void env_scan_now(void *ctx)
{
	struct opxy_master *m = ctx;

	m->sv.scan_due = true;
}

/* S-04 W7: the factory screen's reset key. The menu's last line reaches
 * the panel and stays there long enough to read before the shutdown; a
 * frame that does not end is given up after its timeout, so the restart
 * always follows. */
static void env_reset_w7(void *ctx)
{
	struct opxy_master *m = ctx;

	opxy_menu_show_now(m);
	wb_delay_us(&m->hw.time, OPXY_RESTART_HOLD_MS * 1000u);
	opxy_reboot(m, S04_W7, m->boot_reason);
}

#if !defined(OPXY_TEST_OS)
/*
 * S-15.190, S-05.033: the view on the panel as for W7, then the restart
 * whose next start boots the OS whatever start in boot menu says (W11,
 * reason 80). With the slave absent or not answering a hard reset is not
 * safe (S-04.032): the soft reboot, which restarts the slave too (W2).
 */
static void env_continue_os(void *ctx)
{
	struct opxy_master *m = ctx;
	/* with one CPU none can be left running by the hard reset (S-04.066) */
	const bool slave = !wb_has(opxy_dev(m), WB_F_SECOND_CPU) ||
			   (m->slave_present && !m->bridge.absent);

	opxy_menu_show_now(m);
	wb_delay_us(&m->hw.time, OPXY_RESTART_HOLD_MS * 1000u);
	opxy_reboot(m, slave ? S04_W11 : S04_W2, 0u);
}

void opxy_trial_continue(struct opxy_master *m)
{
	if (!m->trial_go || !m->trial_reached)
		return;
	if (m->trial_until == 0u) {
		/* a busy menu keeps the boot menu: nothing goes on */
		if (s15_menu_trial_ok(&m->sv.menu) != S15_CONT_STARTED) {
			m->trial_go = false;
			return;
		}
		s14_log_str(&m->sv.log, S14_INFO, S14_TAG("boot"),
			    "trial start: boot menu reached, going on to the OS");
		opxy_menu_show_now(m);
		m->trial_until = wb_time_deadline(&m->hw.time, OPXY_TRIAL_SHOW_MS * 1000u);
		return;
	}
	if (!wb_time_passed(m->trial_until))
		return;
	m->trial_go = false;
	env_continue_os(m);
}

/*
 * S-15.194: the slave answering, and /yaffs2/os.ldr there as the next
 * start will take it (S-05.035), by its stat alone; nothing is written. A
 * soft reboot cannot carry the request (S-04.029), so with the slave absent
 * or not answering there is none.
 */
static enum s15_once env_os_once_check(void *ctx, struct s15_os_file *file)
{
	struct opxy_master *m = ctx;
	uint32_t len;

	if (!m->slave_present || m->bridge.absent)
		return S15_ONCE_SLAVE;
	if (!s09_files_os_check_at(&m->sv.files, opxy_osw_path, S03_OS_MAX, file, &len))
		return m->bridge.absent ? S15_ONCE_SLAVE : S15_ONCE_FILE;
	return S15_ONCE_STARTED;
}

/* S-15.194, S-04.014: the view on the panel as for W7, then W12, whose
 * next start reads the OS from the file. */
static void env_os_once(void *ctx)
{
	struct opxy_master *m = ctx;

	const bool listed = opxy_os_place < S04_OS_LIST_PLACES;
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "restarting to start ");
	s14_text_str(&t, opxy_osw_path);
	s14_text_str(&t, " once, writing nothing");
	s14_log_text(&m->sv.log, S14_INFO, S14_TAG("boot"), &t);
	opxy_menu_show_now(m);
	wb_delay_us(&m->hw.time, OPXY_RESTART_HOLD_MS * 1000u);
	/* S-04.072: an OS file's start names its list place */
	if (listed)
		opxy_reboot(m, S04_W13, (uint16_t)opxy_os_place);
	else
		opxy_reboot(m, S04_W12, 0u);
}

/* S-15.203: the OS files of /yaffs2/os. */
static bool env_os_files(void *ctx, struct s15_os_files *out)
{
	struct opxy_master *m = ctx;

	return s09_files_os_files(&m->sv.files, out);
}

/* S-15.204: the file the OS write and the start once take. The path is
 * the menu's, kept while it is set. */
static void env_os_file(void *ctx, const char *path, unsigned place)
{
	(void)ctx;
	opxy_osw_path = path != NULL ? path : OPXY_OS_LDR;
	opxy_os_place = path != NULL ? place : OPXY_OS_PLACE_NONE;
}

/* S-15.172, S-02.098: the running slot's settings sector; nothing of the
 * eMMC is touched */
static enum s15_store_save env_settings_save(void *ctx, const struct s15_settings *s,
					     struct s15_settings *now)
{
	struct opxy_master *m = ctx;
	const struct s02_store want = { s->dev_mode, s->color, s->mtp_macos, s->start_menu,
					 s->host_link };
	struct s02_store after;
	const enum s15_store_save r = opxy_boot_store_save(m, &want, &after);

	m->sv.store = after;
	now->dev_mode = after.dev_mode;
	now->color = after.color;
	now->mtp_macos = after.mtp_macos;
	now->start_menu = after.start_menu;
	now->host_link = after.host_link;
	return r;
}

/* S-02.105 */
static enum s15_store_save env_settings_clear(void *ctx)
{
	return opxy_boot_store_clear(ctx);
}
#endif

/* S-15.082: Device Info's second page reads the start log again. */
static void env_start_log_read(void *ctx)
{
	opxy_start_log_reread(ctx);
}

/* The PSoC's, the option's and the USB sets are the board's; the file
 * operations are the S-09 client's. The test OS (waffle/testos.h) has no
 * settings sector and never hands over. */
void opxy_menu_env(struct opxy_master *m, struct s15_env_ops *env)
{
	env->ctx = m;
	env->usb_set = env_usb_set;
	env->volcal_reset = env_volcal;
	env->keytest_leds = env_keytest_leds;
	env->volume_refused = env_volume_refused;
	env->slave_lost = env_slave_lost;
	env->dir_exists = env_dir_exists;
	env->pack_dir = env_pack_dir;
	env->read_manifest = env_read_manifest;
	env->remove_tree = env_remove_tree;
	env->erase_os_fat = env_erase_os_fat;
	env->format = env_format;
	env->os_write = env_os_write;
	env->reset_w7 = env_reset_w7;
	env->system_info = env_system_info;
	env->scan_now = env_scan_now;
	env->option = env_option;
	env->settings_save = OPXY_TESTOS_OR(NULL, env_settings_save);
	env->settings_clear = OPXY_TESTOS_OR(NULL, env_settings_clear);
	env->continue_os = OPXY_TESTOS_OR(NULL, env_continue_os);
	env->start_log_read = env_start_log_read;
	env->files_scan = env_files_scan;
	/* S-15.194: the test OS (waffle/testos.h) hands over to no OS */
	env->os_once_check = OPXY_TESTOS_OR(NULL, env_os_once_check);
	env->os_once = OPXY_TESTOS_OR(NULL, env_os_once);
	/* S-15.203: the test OS starts no OS and writes none */
	env->os_files = OPXY_TESTOS_OR(NULL, env_os_files);
	env->os_file = OPXY_TESTOS_OR(NULL, env_os_file);
	opxy_logo_env(m, env);
}

__attribute__((weak)) const struct s15_geometry *opxy_display_geometry(void)
{
	return NULL;
}

__attribute__((weak)) void opxy_display_draw_ops(struct opxy_master *m,
						 struct s15_draw_ops *ops)
{
	opxy_draw_record(&m->sv.drawn, ops);
}

__attribute__((weak)) void opxy_display_power_off(struct opxy_master *m)
{
	(void)m;
}

__attribute__((weak)) void opxy_display_show(void)
{
}

__attribute__((weak)) void opxy_display_send(void)
{
}

__attribute__((weak)) void opxy_display_settle(void)
{
}

__attribute__((weak)) void opxy_display_held_start(struct opxy_master *m)
{
	(void)m;
}

__attribute__((weak)) bool opxy_display_logo_line(const char *s)
{
	(void)s;
	return false;
}

__attribute__((weak)) const uint8_t *opxy_display_fb(void)
{
	return NULL;
}

void opxy_settings_read(struct opxy_master *m, struct s15_settings *s)
{
	s->dev_mode = m->sv.store.dev_mode;
	s->color = m->sv.store.color;
	s->mtp_macos = m->sv.store.mtp_macos;
	s->start_menu = m->sv.store.start_menu;
	s->host_link = m->sv.store.host_link;
}

/* S-10c.057, S-16.095: the FSN from the ROM's ADI-area read; a failed read
 * is the empty reply.
 * On a locked part the read may fail (anomaly 19000061; S-10c question 4). */
__attribute__((weak)) bool opxy_read_fsn(uint32_t words[4])
{
	return opxy_otp_fsn(words);
}
