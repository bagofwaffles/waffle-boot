/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The test OS's readouts (waffle/testos.h, docs/test-os.md): what the
 * loading bootloader left (S-05.014's core state, the display record of
 * S-13c.021, DMA9's handshake pair of S-01.006), the registers S-13a
 * expects at OS entry, S-07.032's raw key-status reply, whether the OTP
 * items read (S-11's notion), and two files on the RAM disk for the host:
 * flash.bin, the whole SPI flash, and testos.txt, these lines. The S-02
 * flash ops' erase and program refuse here, so nothing is written below
 * 0xD000 (S-02) or anywhere else in the flash.
 *
 * Every line goes out as an S-14 line at level 3 under the tag "testos"
 * (the wording is free, S-14.009), so the PSoC link carries it at once;
 * the same text is kept for testos.txt, since a host's CDC link opens only
 * in held mode. Nothing here is compiled into the normal image.
 */
#include "waffle/testos.h"

#if defined(OPXY_TEST_OS)

#include <string.h>

#include "ff.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/bytes.h"
#include "waffle/display.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_sec.h"
#include "waffle/drivers/bf70x_timer.h"
#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/drivers/bf70x_lcd.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/drivers/bf70x_start.h"
#include "waffle/drivers/mmr.h"
#include "waffle/s01_memmap.h"
#include "waffle/s07_rules.h"
#include "waffle/s13c_panel.h"

/* S-01.006: the first stage's panel handshake is in MDMA0's DMA9 pair. */
#define HANDSHAKE_CH 9u
/* The table's rows read at entry; S-13a's OSENTRY rows are far fewer. */
#define REGS_MAX 128u
/* S-07.032's reply is a few bytes; S-06.002 bounds any payload. */
#define KEY_RAW_MAX 32u
/* testos.txt: the lines, LF-separated. */
#define REPORT_MAX 8192u

/* Initialized, so that it is loaded data, not .bss: the entry paint writes
 * [4] and [5] before wb_start_c() clears .bss. Its section goes with .data
 * in the DDR image (bf703_testos.ld). */
#if defined(__bfin__)
uint32_t opxy_testos_entry[8] __attribute__((section(".l1.data.testos"))) =
	{ 0, 0, 0, 0, 0, 0, 0, 1u };
#else
uint32_t opxy_testos_entry[8] = { 0, 0, 0, 0, 0, 0, 0, 1u };	/* the host's analysis of this file */
#endif

/* S-07's state and the last frame of the PSoC link, for a wait that the
 * test OS cut short. */
#define LAST_FRAME_MAX 32u
static uint8_t last_frame[LAST_FRAME_MAX];
static size_t last_frame_len;
static uint8_t last_frame_id;
static uint32_t frames_seen;

/* The power states the PSoC reported, in order (S-07.051). */
#define POWER_SEEN_MAX 24u
static uint8_t power_seen[POWER_SEEN_MAX];
static unsigned power_seen_n;
/* Display waits that ran out (S-13c.016, S-13c.017): counted, the first
 * few logged. */
#define DISPLAY_TIMEOUT_LOGGED 3u
static uint32_t te_misses, frame_timeouts;

static uint8_t record[S13C_RECORD_SIZE];
static uint32_t dma9[2];
static uint32_t regvals[REGS_MAX];
static uint8_t key_raw[KEY_RAW_MAX];
static size_t key_raw_len;
static bool key_replied;
static uint32_t flash_refused;
static char report[REPORT_MAX];
static size_t report_len;

void opxy_testos_capture(void)
{
	const volatile uint8_t *rec =
		(const volatile uint8_t *)(uintptr_t)S01_DISPLAY_RECORD_START;
	unsigned i;

	for (i = 0; i < S13C_RECORD_SIZE; i++)
		record[i] = rec[i];
	dma9[0] = wb_mmr_read32(DMA_XMOD(HANDSHAKE_CH));
	dma9[1] = wb_mmr_read32(DMA_YMOD(HANDSHAKE_CH));
	for (i = 0; i < opxy_testos_regs_n && i < REGS_MAX; i++)
		regvals[i] = wb_mmr_read32(opxy_testos_regs[i].addr);
}

/* The line to the log and, cut if need be, to the report. */
void opxy_testos_emit(struct opxy_master *m, const struct s14_text *t)
{
	s14_log_text(&m->sv.log, S14_INFO, S14_TAG("testos"), t);
	if (t->len + 1u <= REPORT_MAX - report_len) {
		memcpy(report + report_len, t->buf, t->len);
		report_len += t->len;
		report[report_len++] = '\n';
	}
}

static void emit(struct opxy_master *m, const struct s14_text *t)
{
	opxy_testos_emit(m, t);
}

static void hex_bytes(struct s14_text *t, const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		s14_text_hex(t, p[i], 2u);
}

static uint32_t field_of(uint32_t v, unsigned msb, unsigned lsb)
{
	const unsigned width = msb - lsb + 1u;

	if (width >= 32u)
		return v;
	return (v >> lsb) & ((1u << width) - 1u);
}

static bool holds(const struct opxy_testos_reg *r, uint32_t f)
{
	switch (r->rule) {
	case OPXY_TESTOS_RULE_EQ:
		return f == r->lo;
	case OPXY_TESTOS_RULE_NE:
		return f != r->lo;
	case OPXY_TESTOS_RULE_GE:
		return f >= r->lo;
	case OPXY_TESTOS_RULE_LE:
		return f <= r->lo;
	case OPXY_TESTOS_RULE_IN:
		return r->lo <= f && f <= r->hi;
	case OPXY_TESTOS_RULE_ANY:
	default:
		return true;
	}
}

/* S-13a's OSENTRY rows against the values read at entry: a line per
 * field that differs, then the count. */
static void log_regs(struct opxy_master *m)
{
	struct s14_text t;
	unsigned i, checked = 0, differ = 0;

	for (i = 0; i < opxy_testos_regs_n && i < REGS_MAX; i++) {
		const struct opxy_testos_reg *r = &opxy_testos_regs[i];
		const uint32_t f = field_of(regvals[i], r->msb, r->lsb);

		if (r->rule == OPXY_TESTOS_RULE_ANY)
			continue;
		checked++;
		if (holds(r, f))
			continue;
		differ++;
		s14_text_init(&t);
		s14_text_str(&t, "reg ");
		s14_text_str(&t, r->reg);
		s14_text_str(&t, " ");
		s14_text_str(&t, r->field);
		s14_text_str(&t, " ");
		s14_text_hex(&t, f, 1u);
		s14_text_str(&t, " expected ");
		s14_text_hex(&t, r->lo, 1u);
		if (r->rule == OPXY_TESTOS_RULE_IN) {
			s14_text_str(&t, "..");
			s14_text_hex(&t, r->hi, 1u);
		} else if (r->rule != OPXY_TESTOS_RULE_EQ) {
			s14_text_str(&t, r->rule == OPXY_TESTOS_RULE_NE ? " not" :
					 r->rule == OPXY_TESTOS_RULE_GE ? " min" :
							  " max");
		}
		s14_text_str(&t, " (");
		s14_text_str(&t, r->req);
		s14_text_str(&t, ")");
		emit(m, &t);
	}
	s14_text_init(&t);
	s14_text_str(&t, "regs ");
	s14_text_dec(&t, checked);
	s14_text_str(&t, " fields checked, ");
	s14_text_dec(&t, differ);
	s14_text_str(&t, " differ from S-13a at OS entry");
	emit(m, &t);
}

/* S-11: a key's read succeeds when every OTP item of it reads; the values
 * are wiped here and never logged. */
static void log_otp(struct opxy_master *m)
{
	struct s14_text t;
	char serial[OPXY_CHIP_SERIAL_MAX + 1u];
	uint8_t q[56], k[32];
	uint32_t fsn[4];
	bool ok_serial, ok_q0, ok_k0, ok_k1, ok_fsn;

	ok_serial = opxy_otp_serial(opxy_otp_get, serial);
	ok_q0 = opxy_otp_q0(opxy_otp_get, q);
	ok_k0 = opxy_otp_key(opxy_otp_get, OPXY_OTP_K0_LO, OPXY_OTP_K0_HI, k);
	ok_k1 = opxy_otp_key(opxy_otp_get, OPXY_OTP_K1_LO, OPXY_OTP_K1_HI, k);
	ok_fsn = opxy_otp_fsn(fsn);
	memset(q, 0, sizeof(q));
	memset(k, 0, sizeof(k));
	memset(fsn, 0, sizeof(fsn));
	s14_text_init(&t);
	s14_text_str(&t, "otp reads: serial ");
	s14_text_str(&t, ok_serial ? "ok" : "failed");
	s14_text_str(&t, ", q0 ");
	s14_text_str(&t, ok_q0 ? "ok" : "failed");
	s14_text_str(&t, ", k0 ");
	s14_text_str(&t, ok_k0 ? "ok" : "failed");
	s14_text_str(&t, ", k1 ");
	s14_text_str(&t, ok_k1 ? "ok" : "failed");
	s14_text_str(&t, ", fsn ");
	s14_text_str(&t, ok_fsn ? "ok" : "failed");
	s14_text_str(&t, ", serial length ");
	s14_text_dec(&t, (uint32_t)strlen(serial));
	emit(m, &t);
}

void opxy_testos_log(struct opxy_master *m)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "test os v");
	s14_text_dec(&t, OPXY_BL_VERSION_MAJOR);
	s14_text_str(&t, ".");
	s14_text_dec(&t, OPXY_BL_VERSION_MINOR);
	s14_text_str(&t, ".");
	s14_text_dec(&t, OPXY_BL_VERSION_PATCH);
	s14_text_str(&t, " (panel " OPXY_TESTOS_PANEL_NAME
#if defined(OPXY_TESTOS_INSTALLER)
			 "): held mode as an OS image, flash writes only on a hold of the unit's key");
#else
			 "): held mode as an OS image, no flash write");
#endif
	emit(m, &t); OPXY_TESTOS_TIMING(opxy_testos_timing_log(m);)
	/* The monitor's image: the host reads the rest live, and the room counts. */
	if (OPXY_TESTOS_MON_ON) {
		opxy_testos_paint_log(m);
		return;
	}

	/* S-05.014: IPEND 0x8000, IMASK 0x8800, SP in the handover reserve. */
	s14_text_init(&t);
	s14_text_str(&t, "entry sp ");
	s14_text_hex(&t, opxy_testos_entry[0], 8u);
	s14_text_str(&t, " ipend ");
	s14_text_hex(&t, opxy_testos_entry[1], 8u);
	s14_text_str(&t, " imask ");
	s14_text_hex(&t, opxy_testos_entry[2], 8u);
	s14_text_str(&t, " rets ");
	s14_text_hex(&t, opxy_testos_entry[3], 8u);
	emit(m, &t);

	/* S-01.006: 0x12345678 and the reset time when the first stage's
	 * handshake survived the loading bootloader. */
	s14_text_init(&t);
	s14_text_str(&t, "dma9 xmod ");
	s14_text_hex(&t, dma9[0], 8u);
	s14_text_str(&t, " ymod ");
	s14_text_hex(&t, dma9[1], 8u);
	emit(m, &t);

	/* S-13c.021: the 44 bytes at 0x8FFFFF00 as found at entry. */
	s14_text_init(&t);
	s14_text_str(&t, "display record ");
	hex_bytes(&t, record, S13C_RECORD_SIZE);
	emit(m, &t);

	log_regs(m);
	log_otp(m);
	/* The paints made so far, in order; later ones log as they happen. */
	opxy_testos_paint_log(m);
}

/* --- The S-07 waits the test OS bounds (docs/test-os.md) --- */

void opxy_testos_frame_seen(const struct s06_frame *f)
{
	frames_seen++;
	last_frame_id = f->id;
	last_frame_len = f->len < LAST_FRAME_MAX ? f->len : LAST_FRAME_MAX;
	memcpy(last_frame, f->payload, last_frame_len);
}

void opxy_testos_wait_timeout(const struct s07_psoc *p, const char *what,
			      uint32_t waited_us)
{
	struct opxy_master *m = &opxy_master;
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "wait cut short: ");
	s14_text_str(&t, what);
	s14_text_str(&t, " after ");
	s14_text_dec(&t, waited_us / 1000u);
	s14_text_str(&t, " ms; power state ");
	s14_text_hex(&t, p->state, 2u);
	s14_text_str(&t, ", battery ");
	s14_text_str(&t, p->batt.present ? "present charge " : "absent");
	if (p->batt.present) {
		s14_text_dec(&t, p->batt.charge);
		s14_text_str(&t, " mv ");
		s14_text_dec(&t, p->batt.mv);
	}
	s14_text_str(&t, ", applied ");
	s14_text_dec(&t, p->applied_ma);
	s14_text_str(&t, " mA, target ");
	s14_text_dec(&t, p->target_ma);
	s14_text_str(&t, " mA, usb ");
	s14_text_str(&t, p->usb_configured ? "configured" : "not configured");
	s14_text_str(&t, ", greeted ");
	s14_text_str(&t, p->greeted ? "yes" : "no");
	emit(m, &t);

	s14_text_init(&t);
	s14_text_str(&t, "psoc frames received ");
	s14_text_dec(&t, frames_seen);
	if (frames_seen != 0u) {
		s14_text_str(&t, ", last id ");
		s14_text_hex(&t, last_frame_id, 2u);
		s14_text_str(&t, " payload ");
		hex_bytes(&t, last_frame, last_frame_len);
	}
	emit(m, &t);
}

/* --- The display waits the test OS logs (S-13c.016, S-13c.017) --- */

void opxy_testos_display_timeout(const char *what, uint32_t waited_us)
{
	struct opxy_master *m = &opxy_master;
	struct s14_text t;
	uint32_t *count = what[0] == 't' ? &te_misses : &frame_timeouts;

	(*count)++;
	if (*count > DISPLAY_TIMEOUT_LOGGED)
		return;
	s14_text_init(&t);
	s14_text_str(&t, "wait cut short: ");
	s14_text_str(&t, what);
	s14_text_str(&t, " after ");
	s14_text_dec(&t, waited_us / 1000u);
	s14_text_str(&t, " ms; the frame goes on");
	s14_text_str(&t, *count == DISPLAY_TIMEOUT_LOGGED ? " (further ones counted, not logged)" : "");
	emit(m, &t);
}

/* --- The frame timing (scenario HW-FRAME, S-13c.018) --- */

static uint64_t ft_cycles(void *ctx)
{
	(void)ctx;
	return wb_core_cycles();
}

static bool ft_te(void *ctx)
{
	(void)ctx;
	return wb_gpio_get(wb_lcd_opxy.te);
}

/* Held mode's frame (s13c_frame()) of the screen as it is, at once: the
 * caller has just seen the TE edge. */
static int ft_start(void *ctx)
{
	struct wb_display *d = ctx;

	return s13c_frame(&d->panel, d->fb, false);
}

static bool ft_busy(void *ctx)
{
	struct wb_display *d = ctx;

	return wb_display_busy(d);
}

void opxy_testos_frame_timing(struct opxy_master *m)
{
	struct wb_display *d = opxy_testos_display();
	struct opxy_testos_frame_ops o;
	struct opxy_testos_frame_time ft;
	struct s14_text te, frame;
	int rc;

	if (d == NULL || d->panel.state != S13C_ON || d->stopped) {
		s14_text_init(&te);
		s14_text_str(&te, "frame timing: the panel is not on");
		emit(m, &te);
		return;
	}
	(void)wb_display_wait(d);
	o.cycles = ft_cycles;
	o.te = ft_te;
	o.start = ft_start;
	o.busy = ft_busy;
	o.cycles_per_us = m->hw.time.cycles_per_us;
	o.ctx = d;
	rc = opxy_testos_frame_measure(&o, &ft);
	if (rc == OPXY_FT_STUCK)
		(void)s13c_frame_stop(&d->panel);
	opxy_testos_frame_lines(rc, &ft, o.cycles_per_us, &te, &frame);
	emit(m, &te);
	emit(m, &frame);
}

/* --- S-07.051 in the test OS: seen, shown, never acted on --- */

static void power_seen_line(struct s14_text *t)
{
	unsigned i;

	s14_text_str(t, "power states seen:");
	for (i = 0; i < power_seen_n; i++) {
		s14_text_str(t, " ");
		s14_text_hex(t, power_seen[i], 2u);
	}
	if (power_seen_n == 0u)
		s14_text_str(t, " none");
}

bool opxy_testos_power_state(unsigned prev, unsigned s)
{
	struct opxy_master *m = &opxy_master;
	struct s14_text t;

	if (power_seen_n < POWER_SEEN_MAX)
		power_seen[power_seen_n++] = (uint8_t)s;
	if (!s07_power_off_trigger(prev, s))
		return true;
	s14_text_init(&t);
	s14_text_str(&t, "power-off (S-07.051): state ");
	s14_text_hex(&t, s, 2u);
	s14_text_str(&t, " after ");
	if (prev == S07_POWER_NONE)
		s14_text_str(&t, "none");
	else
		s14_text_hex(&t, prev, 2u);
	s14_text_str(&t, "; the shutdown, then 0x32, then the PSoC removes the power");
	emit(m, &t);
	s14_text_init(&t);
	power_seen_line(&t);
	emit(m, &t);
	return false;
}

int opxy_testos_started(int rc)
{
	struct opxy_master *m = &opxy_master;
	struct s14_text t;

	/* Lit first, for the color trail: S-07.066's start report may have set
	 * the brightness to 0 (S-13c.019); the early logo sends {3C FF 02} again
	 * after it. The monitor's image paints nothing: dark instead, since the
	 * loading bootloader left it lit, until the logo is drawn. */
	s07_report(&m->psoc, OPXY_TESTOS_MON_ON ? S07_REPORT_START : S07_REPORT_HELD);
	opxy_testos_paint(OPXY_PAINT_GREEN);
	s14_text_init(&t);
	s14_text_str(&t, "s07 start-up returned ");
	s14_text_dec(&t, (uint32_t)rc);
	s14_text_str(&t, rc == S07_POWEROFF ? " (power-off: the PSoC removes the power)" : "");
	emit(m, &t);
	s14_text_init(&t);
	power_seen_line(&t);
	emit(m, &t);
	return rc;
}

void opxy_testos_key_raw(const uint8_t *payload, size_t len)
{
	key_replied = payload != NULL;
	key_raw_len = key_replied && len < KEY_RAW_MAX ? len :
		      key_replied ? KEY_RAW_MAX : 0u;
	if (payload != NULL && key_raw_len > 0u)
		memcpy(key_raw, payload, key_raw_len);
}

void opxy_testos_log_keys(struct opxy_master *m)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "key status 41 reply ");
	if (!key_replied) {
		s14_text_str(&t, "none (S-16.008: held)");
	} else {
		s14_text_dec(&t, (uint32_t)key_raw_len);
		s14_text_str(&t, " bytes ");
		hex_bytes(&t, key_raw, key_raw_len);
	}
	emit(m, &t);
}

/* --- The S-02 flash ops in the test OS --- */

int opxy_testos_flash_erase(void *ctx, uint32_t sector_off)
{
	(void)ctx;
	(void)sector_off;
	flash_refused++;
	return -1;
}

int opxy_testos_flash_program(void *ctx, uint32_t off, const uint8_t *buf,
			      uint32_t len)
{
	(void)ctx;
	(void)off;
	(void)buf;
	(void)len;
	flash_refused++;
	return -1;
}

/* --- The files on the RAM disk --- */

static bool write_flash_bin(struct opxy_master *m)
{
	FIL f;
	UINT w;
	uint32_t off;
	uint8_t *buf = m->flash.scratch;
	bool ok = m->flash.scratch_len >= S02_SECTOR_SIZE;

	if (!ok || f_open(&f, "flash.bin", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
		return false;
	for (off = 0; ok && off < OPXY_FLASH_SIZE; off += S02_SECTOR_SIZE) {
		ok = m->flash_ops.read(m->flash_ops.ctx, off, buf,
				       S02_SECTOR_SIZE) == 0 &&
		     f_write(&f, buf, S02_SECTOR_SIZE, &w) == FR_OK &&
		     w == S02_SECTOR_SIZE;
	}
	return f_close(&f) == FR_OK && ok;
}

static bool write_report(void)
{
	FIL f;
	UINT w;
	bool ok;

	if (f_open(&f, "testos.txt", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
		return false;
	ok = f_write(&f, report, (UINT)report_len, &w) == FR_OK &&
	     w == (UINT)report_len;
	return f_close(&f) == FR_OK && ok;
}

/* --- The bridge and the slave's bring-up (waffle/testos.h) --- */

static const char *const step_names[OPXY_TESTOS_STEP_COUNT] = {
	[OPXY_TESTOS_STEP_SERVER] = "server",
	[OPXY_TESTOS_STEP_MSI] = "msi init",
	[OPXY_TESTOS_STEP_EMMC] = "eMMC init",
	[OPXY_TESTOS_STEP_VOLUME] = "volume",
	[OPXY_TESTOS_STEP_FEATURES] = "eMMC features",
	[OPXY_TESTOS_STEP_MOUNT] = "mount",
	[OPXY_TESTOS_STEP_REQUEST] = "last request",
	[OPXY_TESTOS_STEP_SEND] = "last send",
	[OPXY_TESTOS_STEP_QUEUE] = "queue",
};

/* How far the bridge came: 0 nothing, 1 the ready line, 2 the first session. */
static unsigned bridge_stage;

/* The bridge's first bus operations. */
#define TRACE_MAX 40u
#define TRACE_BYTES 8u
struct trace_row {
	uint32_t us;
	int rc;
	uint16_t n;
	char what;
	bool pa6, pc5;
	uint8_t d[TRACE_BYTES];
};
static struct trace_row trace[TRACE_MAX];
static unsigned trace_n;
static uint64_t trace_t0;

void opxy_testos_bus_note(char what, int rc, const uint8_t *d, size_t n)
{
	struct trace_row *r;
	size_t i;

	if (OPXY_TESTOS_MON_ON)
		return;
	r = &trace[trace_n++ % TRACE_MAX];
	r->us = (uint32_t)(wb_time_us(&opxy_time) - trace_t0);
	r->rc = rc;
	r->n = (uint16_t)n;
	r->what = what;
	r->pa6 = wb_gpio_get(OPXY_PA6);
	r->pc5 = wb_gpio_get(OPXY_PC5);
	for (i = 0; i < TRACE_BYTES; i++)
		r->d[i] = d != NULL && i < n ? d[i] : 0u;
}

/* A line a row: microseconds since the ready line was seen, PA6, PC5. */
static void trace_log(struct opxy_master *m)
{
	const unsigned first = trace_n > TRACE_MAX ? trace_n - TRACE_MAX : 0u;
	unsigned i;

	for (i = first; i < trace_n; i++) {
		const struct trace_row *r = &trace[i % TRACE_MAX];
		const char what[2] = { r->what, '\0' };
		struct s14_text t;

		s14_text_init(&t);
		s14_text_str(&t, "bridge trace ");
		s14_text_str(&t, what);
		s14_text_str(&t, " at ");
		s14_text_dec(&t, r->us);
		s14_text_str(&t, " us pa6 ");
		s14_text_dec(&t, r->pa6 ? 1u : 0u);
		s14_text_str(&t, " pc5 ");
		s14_text_dec(&t, r->pc5 ? 1u : 0u);
		if (r->what == 'T' || r->what == 'R') {
			s14_text_str(&t, " rc ");
			s14_text_dec(&t, (uint32_t)(r->rc < 0 ? -r->rc : r->rc));
			s14_text_str(&t, " n ");
			s14_text_dec(&t, r->n);
			s14_text_str(&t, " ");
			hex_bytes(&t, r->d, r->n < TRACE_BYTES ? r->n : TRACE_BYTES);
		}
		opxy_testos_emit(m, &t);
	}
	trace_n = 0;
}

void opxy_testos_bridge_enter(void)
{
	opxy_testos_paint(OPXY_PAINT_OLIVE);
}

/* The slave's beacon (waffle/testos.h): a list of burst lengths from PC5,
 * 0 for a long gap. A fault's frame takes up to 270 ms. */
#define BEACON_GAP_US      4000u
#define BEACON_FRAME_GAP_US 20000u
#define BEACON_SEQ_MAX     64u
static size_t beacon_capture(uint32_t watch_us, uint8_t *seq, bool *level_out)
{
	const uint64_t start = wb_time_us(&opxy_time);
	uint64_t now = start, last = start;
	bool level = wb_gpio_get(OPXY_PC5), seen_gap = false, framed = false;
	unsigned falling = 0;
	size_t n = 0;

	while (now - start < watch_us) {
		const bool l = wb_gpio_get(OPXY_PC5);

		now = wb_time_us(&opxy_time);
		if (l != level) {
			level = l;
			last = now;
			framed = false;
			if (!l && seen_gap)
				falling++;
		} else if (now - last > BEACON_GAP_US) {
			seen_gap = true;
			if (falling != 0u) {
				if (n < BEACON_SEQ_MAX)
					seq[n++] = (uint8_t)falling;
				falling = 0;
			}
			if (now - last > BEACON_FRAME_GAP_US && !framed && n != 0u &&
			    n < BEACON_SEQ_MAX) {
				seq[n++] = 0u;
				framed = true;
			}
		}
	}
	*level_out = level;
	return n;
}

static void beacon_log(const char *when, uint32_t watch_us)
{
	uint8_t seq[BEACON_SEQ_MAX];
	bool level;
	const size_t n = beacon_capture(watch_us, seq, &level);
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "slave beacon ");
	s14_text_str(&t, when);
	if (n != 0u) {
		s14_text_str(&t, ": code ");
		for (size_t i = 0; i < n; i++) {
			s14_text_dec(&t, seq[i]);
			s14_text_str(&t, " ");
		}
	} else {
		s14_text_str(&t, ": none, pc5 ");
		s14_text_str(&t, level ? "high" : "low");
	}
	opxy_testos_emit(&opxy_master, &t);
}

/* A fault on the slave, which has no panel: EXCAUSE and the return address
 * on its attention line, for ever, a nibble a burst of nibble + 1 pulses. */
static void beacon_burst(unsigned pulses, uint32_t gap_us)
{
	unsigned i;

	for (i = 0; i < pulses; i++) {
		(void)wb_gpio_set(OPXY_PB2, false);
		wb_delay_us(&opxy_time, 500u);
		(void)wb_gpio_set(OPXY_PB2, true);
		wb_delay_us(&opxy_time, 500u);
	}
	wb_delay_us(&opxy_time, gap_us);
}

void wb_core_fault(uint32_t seqstat, uint32_t ret)
{
	if (opxy_strap_role((uint16_t)opxy_testos_entry[5]) == OPXY_MASTER) {
		for (;;)
			wb_core_idle();
	}
	(void)wb_gpio_output(OPXY_PB2, true);
	for (;;) {
		unsigned i;

		beacon_burst((seqstat >> 4 & 3u) + 1u, 8000u);
		beacon_burst((seqstat & 15u) + 1u, 8000u);
		for (i = 0; i < 8u; i++)
			beacon_burst((ret >> (28u - 4u * i) & 15u) + 1u, 8000u);
		wb_delay_us(&opxy_time, 30000u);
	}
}

bool opxy_testos_bridge_ready(const struct s09_bus_ops *bus, uint64_t deadline)
{
	uint64_t start;
	struct s14_text t;
	bool late, ready;

	if (!OPXY_TESTOS_MON_ON)
		beacon_log("before the first session", 60000u);
	start = wb_time_us(&opxy_time);
	do {
		late = wb_time_passed(deadline);
		ready = bus->ready(bus->ctx);
	} while (!ready && !late);
	s14_text_init(&t);
	if (ready) {
		bridge_stage = 1u;
		s14_text_str(&t, "bridge: ready line low after ");
	} else {
		s14_text_str(&t, "wait cut short: the slave's ready line (PA6) after ");
	}
	s14_text_dec(&t, (uint32_t)((wb_time_us(&opxy_time) - start) / 1000u));
	s14_text_str(&t, " ms");
	opxy_testos_emit(&opxy_master, &t);
	if (ready)
		opxy_testos_paint(OPXY_PAINT_PINK);
	trace_t0 = wb_time_us(&opxy_time);
	return ready;
}

void opxy_testos_bridge_session(void)
{
	struct s14_text t;

	bridge_stage = 2u;
	s14_text_init(&t);
	s14_text_str(&t, "bridge: init session answered");
	opxy_testos_emit(&opxy_master, &t);
	opxy_testos_paint(OPXY_PAINT_TEAL);
}

/* The slave's table, a line a row. */
static void slave_table(struct opxy_master *m)
{
	unsigned i;

	for (i = 0; i < OPXY_TESTOS_STEP_COUNT; i++) {
		struct s09_req r = { OPXY_TESTOS_ROW_REQ, 0, 0, i, 0 };
		uint32_t err, v, code;
		struct s14_text t;

		v = (uint32_t)s09_master_call(&m->bridge, &r, NULL, 0, NULL, 0,
					      NULL, &err);
		s14_text_init(&t);
		if (m->bridge.absent || err != 0u) {
			s14_text_str(&t, "slave table: no answer");
			opxy_testos_emit(m, &t);
			return;
		}
		code = v >> 16 & 0xFFu;
		s14_text_str(&t, "slave step ");
		s14_text_str(&t, step_names[i]);
		if (code == OPXY_TESTOS_ROW_NOT_RUN) {
			s14_text_str(&t, ": not run");
		} else if (i >= OPXY_TESTOS_STEP_REQUEST) {
			s14_text_str(&t, ": ");
			s14_text_hex(&t, code, 2u);
			s14_text_str(&t, " ");
			s14_text_hex(&t, v & 0xFFFFu, 4u);
		} else {
			if (code == 0u) {
				s14_text_str(&t, ": ok");
			} else {
				s14_text_str(&t, ": failed, code ");
				s14_text_dec(&t, code);
			}
			s14_text_str(&t, " at ");
			s14_text_dec(&t, v & 0xFFFFu);
			s14_text_str(&t, " ms");
		}
		opxy_testos_emit(m, &t);
	}
}

static void slave_gone(struct opxy_master *m, const char *why)
{
	struct s14_text t;

	s09_master_set_absent(&m->bridge);
	s14_text_init(&t);
	s14_text_str(&t, "slave absent: ");
	s14_text_str(&t, why);
	opxy_testos_emit(m, &t);
	if (OPXY_TESTOS_MON_ON) {	/* the host reads all this live */
		opxy_testos_paint(OPXY_PAINT_BROWN);
		return;
	}
	trace_log(m);
	beacon_log("given up", 1200000u);
	/* The bridge may still answer, once the slave has given up its own
	 * session: its own view, if it does. */
	wb_delay_us(&opxy_time, 1200000u);
	m->bridge.absent = false;
	slave_table(m);
	m->bridge.absent = true;
	opxy_testos_paint(OPXY_PAINT_BROWN);
}

void opxy_testos_bridge_check(struct opxy_master *m)
{
	if (m->slave_present && m->bridge.absent)
		slave_gone(m, "the bridge was lost after it came up");
}

void opxy_testos_slave_bringup(struct opxy_master *m)
{
	struct s09_req r = { OPXY_TESTOS_ROW_REQ, 0, 0, 0, 1 };
	uint64_t end;
	uint32_t err;

	if (m->bridge.absent) {
		slave_gone(m, bridge_stage == 0u ? "the ready line never went low" :
			      bridge_stage == 1u ? "the init session failed" :
						   "the echo or pull after the init session failed");
		return;
	}
	opxy_testos_paint(OPXY_PAINT_BLUE);
	if (!OPXY_TESTOS_MON_ON)
		trace_log(m);
	(void)s09_master_call(&m->bridge, &r, NULL, 0, NULL, 0, NULL, &err);
	if (m->bridge.absent || err != 0u) {
		slave_gone(m, "the request for the eMMC's bring-up failed");
		return;
	}
	/* The slave holds the line high until the bring-up has ended. */
	end = wb_time_deadline(&m->hw.time, OPXY_TESTOS_EMMC_CAP_US);
	while (!m->bus_ops.ready(m->bus_ops.ctx)) {
		if (wb_time_passed(end)) {
			slave_gone(m, "no answer within 8 s: it stopped in the eMMC's bring-up");
			return;
		}
	}
	slave_table(m);
}

/* --- The USB debug monitor (waffle/testos.h, tools/testos_mon.py) --- */

/* This image and its heap, and the registers. Nothing else of memory: it may
 * hold another bootloader's code. The master's flash controller and the OTP
 * controller take no write, and the OTP controller no read either. */
bool opxy_testos_mem_ok(uint32_t addr, uint32_t len, bool write)
{
	const uint64_t end = (uint64_t)addr + len;
	const bool image = addr >= OPXY_TESTOS_IMAGE_START && end <= OPXY_TESTOS_HEAP_END;
	const bool mmr = addr >= 0x20000000u && end <= 0x30000000u;
	const bool core = addr >= 0x1FC00000u && end <= 0x20000000u;
	const bool otp = addr < 0x20076000u && end > 0x20075000u;
	const bool flash = addr < SPI_BASE(OPXY_SPI_FLASH) + 0x1000u &&
			   end > SPI_BASE(OPXY_SPI_FLASH) &&
			   opxy_strap_role((uint16_t)opxy_testos_entry[5]) == OPXY_MASTER;

	if (otp || (write && flash))
		return false;
	return image || mmr || core;
}

#if defined(OPXY_TESTOS_MONITOR)

enum { MON_PING, MON_READ, MON_WRITE, MON_LOAD, MON_CALL, MON_REPORT, MON_BUS, MON_BEACON, MON_RUN, MON_LAST };
#define MON_MAGIC 0x4D4F4E31u	/* "MON1" */
/* Where MON_LOAD and MON_CALL may go: the top of the heap, which nothing else
 * reaches (sbrk() grows from the bottom). */
#define MON_LOAD_START (OPXY_TESTOS_HEAP_END - 0x100000u)
#define MON_ST_OK      0u
#define MON_ST_REFUSED 1u
#define MON_ST_BAD     2u

/*
 * MON_RUN's note: a file of our own on the volume, since the reboot below
 * loses the DDR. It says a routine is running, and goes when the routine
 * returns, so a note found after a boot is one that never did.
 */
#define MON_NOTE_FILE "/yaffs2/wb_run_note.bin"
#define MON_RUN_MAGIC 0x52554E31u	/* "RUN1" */

static void mon_note_write(struct opxy_master *m, uint32_t addr, uint32_t arg)
{
	const uint32_t note[3] = { MON_RUN_MAGIC, addr, arg };
	const int32_t fd = s09_files_open(&m->sv.files, MON_NOTE_FILE,
					  S09_O_WRONLY | S09_O_CREAT | S09_O_TRUNC);

	if (fd < 0)
		return;
	(void)s09_files_write(&m->sv.files, fd, (const uint8_t *)note, sizeof(note));
	(void)s09_files_close(&m->sv.files, fd);
}

/*
 * The watchdog's expiry as an interrupt, while a routine runs: S-04.065's PSoC
 * reset request, which reboots the unit, since a hard reset leaves a unit stuck
 * (docs/test-os.md). A routine that hangs on a bus access or has masked
 * interrupts never gets here, and that needs a power cycle.
 */
static void mon_watchdog(void *ctx, uint32_t sid)
{
	struct opxy_master *m = ctx;
	static const uint8_t magic[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

	(void)sid;
	(void)s06_send(&m->node, S06_LINK_PSOC, 0u, OPXY_PSOC_RESET_ID, magic, sizeof(magic));
	(void)opxy_psoc_drain(m);
	for (;;)
		wb_core_idle();
}

/* An uploaded routine under the watchdog. */
static uint32_t mon_run(struct opxy_master *m, uint32_t addr, uint32_t arg, uint32_t ms)
{
	uint32_t (*const fn)(uint32_t) = (uint32_t (*)(uint32_t))(uintptr_t)addr;
	uint32_t r;

	mon_note_write(m, addr, arg);
	(void)wb_sec_attach(WB_SID_WDOG0, 0u, false, mon_watchdog, m);
	(void)wb_sec_enable(WB_SID_WDOG0);
	wb_wdog_start(ms * (OPXY_WDOG_COUNT / 200u));
	r = fn(arg);
	wb_wdog_disable();
	(void)wb_sec_disable(WB_SID_WDOG0);
	(void)s09_files_unlink(&m->sv.files, MON_NOTE_FILE);
	return r;
}

/* T and R's status: 1 done, else 0x80 and the driver's code without its sign. */
static uint8_t mon_rc(int rc)
{
	return rc == WB_SPI_OK ? 1u : (uint8_t)(0x80u | ((unsigned)-rc & 0x7Fu));
}

/* A short program of bus steps, run without a USB round trip between them:
 * s<0|1> select; t<n><bytes> send; r<n> receive; w<0|1><us:4> wait for the
 * ready line; d<us:4> delay; c<phase> configure; T<ctl:4><clk:4><n><bytes> and
 * R<ctl:4><clk:4><n>, a send or receive under that SPI0_CTL and SPI0_CLK in
 * the enable-last order (S-13b Q-11, wb_spi_testos_send_last()). out gets one status
 * byte per step, the received bytes after r and R, the time waited (us:4) after
 * w, and the four seen words (4 bytes each) after T and R. */
static size_t mon_bus(const struct opxy_master *m, const uint8_t *p, size_t n,
		      uint8_t *out, size_t max)
{
	const struct s09_bus_ops *b = &m->bus_ops;
	size_t i = 0, o = 0;

	while (i < n && o + 6u <= max) {
		const uint8_t op = p[i++];

		if (op == 'T' && i + 9u <= n && i + 9u + p[i + 8u] <= n && o + 17u <= max) {
			const size_t k = p[i + 8u];
			uint32_t seen[4];
			unsigned j;

			out[o++] = mon_rc(wb_spi_testos_send_last(b->ctx, wb_get_le32(p + i),
								  wb_get_le32(p + i + 4u), p + i + 9u, k, seen));
			i += 9u + k;
			for (j = 0; j < 4u; j++, o += 4u)
				wb_put_le32(out + o, seen[j]);
		} else if (op == 'R' && i + 9u <= n && o + 17u + p[i + 8u] <= max) {
			const size_t k = p[i + 8u];
			uint32_t seen[4];
			unsigned j;

			out[o] = mon_rc(wb_spi_testos_recv_last(b->ctx, wb_get_le32(p + i),
								wb_get_le32(p + i + 4u), out + o + 1u, k, seen));
			o += 1u + k;
			i += 9u;
			for (j = 0; j < 4u; j++, o += 4u)
				wb_put_le32(out + o, seen[j]);
		} else if (op == 's' && i < n) {
			b->select(b->ctx, p[i++] != 0u);
			out[o++] = 1u;
		} else if (op == 't' && i < n && i + 1u + p[i] <= n) {
			const size_t k = p[i++];

			out[o++] = b->send(b->ctx, p + i, k) ? 1u : 0u;
			i += k;
		} else if (op == 'r' && i < n && o + 1u + p[i] <= max) {
			const size_t k = p[i++];
			const bool ok = b->recv(b->ctx, out + o + 1u, k);

			out[o++] = ok ? 1u : 0u;
			o += k;
		} else if (op == 'w' && i + 5u <= n) {
			const bool want = p[i] != 0u;
			const uint64_t t0 = wb_time_us(&opxy_time);
			const uint64_t end = wb_time_deadline(&opxy_time, wb_get_le32(p + i + 1u));
			bool ok;

			i += 5u;
			while (!(ok = (b->ready(b->ctx) == want)) && !wb_time_passed(end))
				;
			out[o++] = ok ? 1u : 0u;
			wb_put_le32(out + o, (uint32_t)(wb_time_us(&opxy_time) - t0));
			o += 4u;
		} else if (op == 'd' && i + 4u <= n) {
			wb_delay_us(&opxy_time, wb_get_le32(p + i));
			i += 4u;
			out[o++] = 1u;
		} else if (op == 'c' && i < n) {
			b->configure(b->ctx, p[i++]);
			out[o++] = 1u;
		} else {
			out[o++] = 0xFFu;
			break;
		}
	}
	return o;
}

bool opxy_testos_monitor(struct opxy_master *m, const struct s06_frame *f)
{
	static uint8_t out[S06_MAX_PAYLOAD];
	const uint8_t *p = f->payload;
	size_t n = 1u;

	if (f->id != OPXY_TESTOS_MON_ID || f->len < 1u)
		return false;
	out[0] = MON_ST_OK;
	switch (p[0]) {
	case MON_PING:
		wb_put_le32(out + 1, MON_MAGIC);
		n = 5u;
		break;
	case MON_READ: {
		/* addr:4 words:1 */
		const uint32_t addr = f->len >= 6u ? wb_get_le32(p + 1) : 1u;
		const uint32_t words = f->len >= 6u ? p[5] : 0u;

		if ((addr & 3u) != 0u || words == 0u || words > 60u) {
			out[0] = MON_ST_BAD;
		} else if (!opxy_testos_mem_ok(addr, words * 4u, false)) {
			out[0] = MON_ST_REFUSED;
		} else {
			for (uint32_t i = 0; i < words; i++)
				wb_put_le32(out + 1 + 4u * i, wb_mmr_read32(addr + 4u * i));
			n = 1u + words * 4u;
		}
		break;
	}
	case MON_WRITE: {
		/* addr:4 value:4 */
		const uint32_t addr = f->len >= 9u ? wb_get_le32(p + 1) : 1u;

		if ((addr & 3u) != 0u) {
			out[0] = MON_ST_BAD;
		} else if (!opxy_testos_mem_ok(addr, 4u, true) ||
			   (addr >= OPXY_TESTOS_IMAGE_START && addr < MON_LOAD_START)) {
			out[0] = MON_ST_REFUSED;
		} else {
			wb_mmr_write32(addr, wb_get_le32(p + 5));
			wb_mmr_sync();
		}
		break;
	}
	case MON_LOAD: {
		/* addr:4 bytes */
		const uint32_t addr = f->len >= 6u ? wb_get_le32(p + 1) : 0u;
		const uint32_t k = f->len >= 6u ? f->len - 5u : 0u;

		if (k == 0u || addr < MON_LOAD_START || (uint64_t)addr + k > OPXY_TESTOS_HEAP_END) {
			out[0] = MON_ST_REFUSED;
		} else {
			memcpy((void *)(uintptr_t)addr, p + 5, k);
			wb_dcache_flush_range(addr, k);
		}
		break;
	}
	case MON_CALL: {
		/* addr:4 arg:4, an uploaded routine: uint32_t f(uint32_t) */
		const uint32_t addr = f->len >= 9u ? wb_get_le32(p + 1) : 0u;

		if (addr < MON_LOAD_START || addr >= OPXY_TESTOS_HEAP_END) {
			out[0] = MON_ST_REFUSED;
		} else {
			uint32_t (*const fn)(uint32_t) = (uint32_t (*)(uint32_t))(uintptr_t)addr;

			wb_put_le32(out + 1, fn(wb_get_le32(p + 5)));
			n = 5u;
		}
		break;
	}
	case MON_RUN: {
		/* addr:4 arg:4 ms:4: as MON_CALL, under the watchdog */
		const uint32_t addr = f->len >= 13u ? wb_get_le32(p + 1) : 0u;
		const uint32_t ms = f->len >= 13u ? wb_get_le32(p + 9) : 0u;

		if (addr < MON_LOAD_START || addr >= OPXY_TESTOS_HEAP_END - 0x100u ||
		    ms < 10u || ms > 30000u) {
			out[0] = MON_ST_REFUSED;
		} else {
			wb_put_le32(out + 1, mon_run(m, addr, wb_get_le32(p + 5), ms));
			n = 5u;
		}
		break;
	}
	case MON_LAST: {
		/* clear:1: a routine that never returned, from the note; then the
		 * note's words, or nothing when there is no note */
		const int32_t fd = s09_files_open(&m->sv.files, MON_NOTE_FILE, S09_O_RDONLY);

		if (fd >= 0) {
			const int32_t k = s09_files_read(&m->sv.files, fd, out + 1, 12u);

			(void)s09_files_close(&m->sv.files, fd);
			n = k > 0 ? 1u + (size_t)k : 1u;
			if (f->len >= 2u && p[1] != 0u)
				(void)s09_files_unlink(&m->sv.files, MON_NOTE_FILE);
		}
		break;
	}
	case MON_REPORT: {
		/* offset:4: the text of testos.txt so far */
		const uint32_t off = f->len >= 5u ? wb_get_le32(p + 1) : 0u;
		const size_t left = off < report_len ? report_len - off : 0u;
		const size_t k = left < S06_MAX_PAYLOAD - 5u ? left : S06_MAX_PAYLOAD - 5u;

		wb_put_le32(out + 1, (uint32_t)report_len);
		if (k != 0u)
			memcpy(out + 5, report + off, k);
		n = 5u + k;
		break;
	}
	case MON_BUS:
		n = 1u + mon_bus(m, p + 1, (size_t)f->len - 1u, out + 1, sizeof(out) - 1u);
		break;
	case MON_BEACON: {
		/* us:4: the burst lengths on the attention line, 0 for a long gap */
		uint8_t seq[BEACON_SEQ_MAX];
		bool level;
		const size_t k = beacon_capture(f->len >= 5u ? wb_get_le32(p + 1) : 200000u,
						seq, &level);

		out[1] = level ? 1u : 0u;
		memcpy(out + 2, seq, k);
		n = 2u + k;
		break;
	}
	default:
		out[0] = MON_ST_BAD;
		break;
	}
	(void)s06_send(&m->node, S06_LINK_CDC, s06_swap_route(f->route), f->id, out, n);
	return true;
}

#else

bool opxy_testos_monitor(struct opxy_master *m, const struct s06_frame *f)
{
	(void)m;
	(void)f;
	return false;
}

#endif /* OPXY_TESTOS_MONITOR */

void opxy_testos_disk_files(struct opxy_master *m)
{
	struct s14_text t;
	const bool flash_ok = write_flash_bin(m);

	s14_text_init(&t);
	s14_text_str(&t, flash_ok ? "flash.bin written to the disk" :
				    "flash.bin could not be written");
	emit(m, &t);
	s14_text_init(&t);
	s14_text_str(&t, "flash writes refused so far: ");
	s14_text_dec(&t, flash_refused);
	emit(m, &t);
	s14_text_init(&t);
	s14_text_str(&t, "display waits cut short so far: te ");
	s14_text_dec(&t, te_misses);
	s14_text_str(&t, ", frame end ");
	s14_text_dec(&t, frame_timeouts);
	emit(m, &t);
	/* Again: the features and the mount have run since the first table. */
	if (m->bridge.absent) {
		s14_text_init(&t);
		s14_text_str(&t, "slave table: the bridge is down");
		emit(m, &t);
	} else {
		slave_table(m);
	}
	if (!write_report())
		s14_log_str(&m->sv.log, S14_WARNING, S14_TAG("testos"),
			    "testos.txt could not be written");
}

#if defined(OPXY_TESTOS_CHECK)
void opxy_testos_check_kept(const uint32_t **regs, unsigned *n, bool *replied,
			    uint32_t *te, uint32_t *frame)
{
	*regs = regvals;
	*n = opxy_testos_regs_n < REGS_MAX ? opxy_testos_regs_n : REGS_MAX;
	*replied = key_replied;
	*te = te_misses;
	*frame = frame_timeouts;
}
#endif

#else

/* ISO C wants a declaration in every translation unit. */
typedef int opxy_testos_not_built;

#endif /* OPXY_TEST_OS */
