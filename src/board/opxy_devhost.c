/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Developer host requests (S-10c.110, waffle/devhost.h): register reads and
 * writes, slave peek and poke over the bridge, the hand-over options, a
 * hand-over of the active-partition OS, and held mode's screen and keys.
 * Part of the standard image, served
 * from held mode's USB serial loop while developer mode is on (S-15.172)
 * and the host link's session is approved (waffle/hostlink.h); otherwise
 * every request is refused and changes nothing. Files go by the host link. They are
 * another way to boot an OS and to recover a unit.
 *
 * The frames are tools/testos_mon.py's, id OPXY_DEV_REQ_ID. Registers and
 * this image's own memory only (opxy_dev_mem_ok): no write reaches the flash
 * or OTP controller, and no read the OTP. The one eMMC access is a read of
 * the yaffs2 region's tags and headers (src/board/opxy_dev_yaffs.c).
 */
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_bridge.h"

#include "waffle/devhost.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_coremmr.h"
#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/drivers/bf70x_time.h"
#include "waffle/drivers/bf70x_usb.h"
#include "waffle/drivers/mmr.h"
#include "waffle/s01_memmap.h"
#include "waffle/s05_handoff.h"
#include "waffle/s09_bridge.h"
#include "waffle/s09_files.h"
#include "waffle/s13c_panel.h"

#if !defined(OPXY_TEST_OS)

/* This image and its heap, and the registers. The OTP controller takes no
 * access; the master's flash controller takes no write. */
bool opxy_dev_mem_ok(uint32_t addr, bool write)
{
	const uint64_t end = (uint64_t)addr + 4u;
	const bool image = addr >= OPXY_IMAGE_START && end <= OPXY_HEAP_END;
	const bool mmr = addr >= 0x20000000u && end <= 0x30000000u;
	const bool core = addr >= 0x1FC00000u && end <= 0x20000000u;
	const bool otp = addr < 0x20076000u && end > 0x20075000u;
	const bool flash = addr < SPI_BASE(OPXY_SPI_FLASH) + 0x1000u &&
			   end > SPI_BASE(OPXY_SPI_FLASH);
	/* S-01.019: the display record, read only (S-13c.021). */
	const bool disp = addr >= S01_DISPLAY_RECORD_START && end <= S01_DISPLAY_RECORD_END;

	if ((addr & 3u) != 0u || otp || (write && flash))
		return false;
	return image || mmr || core || (!write && disp);
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* A word to or from the slave over the bridge, or ~0 when the slave is
 * absent or the request failed. */
static uint32_t slave_word(struct opxy_master *m, uint8_t cmd, uint32_t addr,
			   uint32_t val)
{
	struct s09_req r = { cmd, 0, 0, addr, val };
	uint32_t err = 0;
	int32_t v = s09_master_call(&m->bridge, &r, 0, 0, 0, 0, 0, &err);

	return err == 0 ? (uint32_t)v : 0xFFFFFFFFu;
}

/*
 * The options a flags request holds for the next developer hand-over
 * (S-10c.110). Only a developer hand-over puts options in opxy_ho_flags, and
 * only while it runs: the ordinary hand-over always finds 0 there.
 */
static uint32_t dev_options;

/* Both CPUs' copies back to the normal hand-over; the slave's only while it
 * still serves the bridge. */
static void options_clear(struct opxy_master *m, bool slave)
{
	opxy_ho_flags = 0u;
	opxy_ho_slave_delay_ms = 0u;
	if (slave && !m->bridge.absent && wb_has(opxy_dev(m), WB_F_SECOND_CPU)) {
		(void)slave_word(m, OPXY_DEV_POKE_REQ,
				 (uint32_t)(uintptr_t)&opxy_ho_flags, 0u);
		(void)slave_word(m, OPXY_DEV_POKE_REQ,
				 (uint32_t)(uintptr_t)&opxy_ho_slave_delay_ms, 0u);
	}
}

/*
 * The hand-over of the active-partition OS with the given options (0 is the
 * normal hand-over), set in opxy_ho_flags for its run. The slave gets the
 * same options poked into its own opxy_ho_flags first, unless the options
 * leave it serving. Then the
 * sequence of S-05.018 runs by hand (not s05_master_handoff, so a failure
 * returns here instead of re-entering held mode): pre-load, stage, the
 * release and the wait for the slave's OS (S-05.032), the master's teardown,
 * the kernel call and the jump. On success it does not return. On a failed
 * stage, a refused image or a slave that is absent or replies non-zero it
 * returns a nonzero code (1 to 3) and the unit stays in held mode; pre_load
 * has then redrawn the boot logo. With OPXY_HO_STAY it returns 4 after the
 * line window, the slave released; when the slave's OS did not start, 5.
 */
/*
 * S-05.030: after the release and before the teardown (while USB and the
 * pins still work), poll the lines the slave drives to the master: PA6, its
 * ready line, and PC5, the bridge attention. PA6 rises when the slave's OS
 * frees PA4, so this shows whether the slave's OS reaches that step under
 * waffle-boot, and when. Each change of either level is logged with its
 * microsecond time, from the window's start: the release with OPXY_HO_STAY or
 * OPXY_HO_NO_OS_WAIT, else the slave OS's start (S-05.032). One frame carries
 * the log; USB is serviced throughout so it flushes.
 */
#define LINE_LOG_MAX 48u

static void usb_flush(uint32_t rounds)
{
	uint32_t i;

	for (i = 0; i < rounds; i++)
		wb_usb_task();
}

static uint8_t lines(void)
{
	return (uint8_t)((wb_gpio_get(OPXY_PA6) ? OPXY_HO_LINE_PA6 : 0u) |
			 (wb_gpio_get(OPXY_PC5) ? OPXY_HO_LINE_PC5 : 0u));
}

static void line_window(struct opxy_master *m, uint8_t route, uint32_t ms)
{
	static uint8_t out[3u + 5u * LINE_LOG_MAX];
	const uint64_t t0 = wb_time_us(&m->hw.time);
	const uint64_t end = wb_time_deadline(&m->hw.time, ms * 1000u);	/* in cycles */
	uint32_t n = 0;
	uint8_t last = lines();

#if OPXY_BOOT_TIMING
	/* the slave's server stop and OS start, when no wait stamped them */
	const bool timed = OPXY_HO_BIT(OPXY_HO_STAY) || OPXY_HO_BIT(OPXY_HO_NO_OS_WAIT);

	if (timed) {
		opxy_timing_pa6(true, (last & OPXY_HO_LINE_PA6) != 0u);
		opxy_timing_pc5(true);
	}
#endif
	out[1] = OPXY_HO_PA6_MARK;
	/* The initial levels at t = 0. */
	wr32(out + 3, 0u);
	out[7] = last;
	n = 1u;
	for (;;) {
		const uint8_t lvl = lines();

#if OPXY_BOOT_TIMING
		if (timed) {
			opxy_timing_pa6(false, (lvl & OPXY_HO_LINE_PA6) != 0u);
			opxy_timing_pc5(false);
		}
#endif
		if (lvl != last) {
			last = lvl;
			if (n < LINE_LOG_MAX) {
				wr32(out + 3u + 5u * n,
				     (uint32_t)(wb_time_us(&m->hw.time) - t0));
				out[3u + 5u * n + 4u] = lvl;
				n++;
			}
		}
		if (wb_time_passed(end))
			break;
		wb_usb_task();			/* keep the port alive */
	}
	out[0] = OPXY_DEV_OK;
	out[2] = (uint8_t)n;
	(void)s06_send(&m->node, S06_LINK_CDC, route, OPXY_DEV_REQ_ID,
		       out, 3u + 5u * n);
	usb_flush(200u);			/* push the frame before the teardown */
}

static uint32_t handover_run(struct opxy_master *m, uint32_t flags,
			     uint32_t mdelay_ms, uint32_t sdelay_ms,
			     uint8_t route)
{
	const struct s05_master_ops *h = &m->handoff;
	const bool stay = (flags & OPXY_HO_STAY) != 0u;
	const bool watch = stay || (flags & OPXY_HO_TRACE) != 0u;
	uint32_t length = 0;
	uint8_t reply = 0xFFu;

	opxy_ho_trace = OPXY_HO_M_NONE;
	opxy_ho_slave_delay_ms = sdelay_ms;
	opxy_ho_flags = flags;
	/* without second-cpu "either CPU" is the one (S-10c.110) */
	if (!wb_has(opxy_dev(m), WB_F_SECOND_CPU))
		flags |= OPXY_HO_NO_SLAVE_GO;
	if (!m->bridge.absent && (flags & OPXY_HO_NO_SLAVE_GO) == 0u) {
		(void)slave_word(m, OPXY_DEV_POKE_REQ,
				 (uint32_t)(uintptr_t)&opxy_ho_flags, flags);
		(void)slave_word(m, OPXY_DEV_POKE_REQ,
				 (uint32_t)(uintptr_t)&opxy_ho_slave_delay_ms, sdelay_ms);
	}
	if ((flags & OPXY_HO_NO_PRELOAD) == 0u)
		h->pre_load(h->ctx);
	if (h->stage(h->ctx, &length) != S05_FAIL_NONE)
		return 1u;			/* no OS to stage */
	if (s05_check_image(m->ops.staging, length) != S05_ACCEPT)
		return 2u;			/* the image was refused */
	/* The release op also waits for the slave's OS (S-05.032), unless
	 * OPXY_HO_NO_OS_WAIT or OPXY_HO_STAY is set. */
	if ((flags & OPXY_HO_NO_SLAVE_GO) == 0u) {
		if (h->release(h->ctx, &reply) != 0)
			return 3u;		/* the slave is absent */
		if (reply == S05_REPLY_NO_START)
			return 5u;		/* released, its OS did not start */
		if (reply != 0u)
			return 3u;		/* the slave said no */
	}
	/* S-05.036: one CPU's start-log steps, in the release's place */
	if (!wb_has(opxy_dev(m), WB_F_SECOND_CPU) && h->accepted != NULL)
		h->accepted(h->ctx);
	/* After the release, before the teardown: USB and the ready pin work. */
	if (watch)
		line_window(m, route, stay ? OPXY_HO_STAY_MS : OPXY_HO_PA6_MS);
	/* OPXY_HO_STAY: no teardown and no jump. The slave was released, so the
	 * bridge is gone: held mode goes on without it, for the host's reads. */
	if (stay) {
		m->slave_present = false;
		m->bridge.absent = true;
		return 4u;
	}
	if (mdelay_ms != 0u)
		wb_delay_us(&m->hw.time, (mdelay_ms > 5000u ? 5000u : mdelay_ms) * 1000u);
	h->teardown(h->ctx);
	h->kernel_call(h->ctx, m->ops.config_addr);
	h->jump(h->ctx, h->read_svect0(h->ctx));
	return 0u;				/* unreachable on the target */
}

/* A hand-over that returns leaves no option behind on either CPU; the
 * slave, once released, no longer serves the bridge. */
static uint32_t do_handover(struct opxy_master *m, uint32_t flags,
			    uint32_t mdelay_ms, uint32_t sdelay_ms, uint8_t route)
{
	const uint32_t why = handover_run(m, flags, mdelay_ms, sdelay_ms, route);

	options_clear(m, (flags & OPXY_HO_NO_SLAVE_GO) == 0u &&
			 (why == 1u || why == 2u || why == 3u));
	return why;
}

/* A file request that failed: the slave not answering is said apart from a
 * refusal (OPXY_DEV_NO_SLAVE). */
static uint8_t file_refused(const struct opxy_master *m)
{
	return s09_files_slave_lost(&m->sv.files) ? OPXY_DEV_NO_SLAVE :
						    OPXY_DEV_REFUSED;
}

/*
 * The raw yaffs2 reads go only to a volume the slave has mounted on the user
 * area: its statfs answers then (S-09.041), so no select is ever sent.
 */
static bool ydump_ready(struct opxy_master *m, struct s03_emmc_ops *emmc,
			struct opxy_ydump_ops *io)
{
	struct s09_statfs st;

	if (s09_files_statfs(&m->sv.files, "/yaffs2", &st) != 0)
		return false;
	s09_files_emmc_ops(&m->sv.files, emmc);
	io->ctx = emmc->ctx;
	io->read = emmc->read;
	return true;
}

/* --- the screen and the keys --- */

/* enum s15_screen's stable names. */
static const char *const screen_names[] = {
	[S15_SCR_MAIN] = "main",
	[S15_SCR_PACKS] = "packs",
	[S15_SCR_INFO] = "info",
	[S15_SCR_SYSTEM] = "system",
	[S15_SCR_SETTINGS] = "settings",
	[S15_SCR_FORMAT] = "format",
	[S15_SCR_FACTORY] = "developer",
	[S15_SCR_UPGRADE] = "upgrade",
	[S15_SCR_RESULT] = "result",
	[S15_SCR_STARTING] = "starting",
	[S15_SCR_INFO2] = "info2",
	[S15_SCR_OPTION] = "option",
	[S15_SCR_NONE] = "none",
	[S15_SCR_DISPLAY] = "display",
};

/* A NUL-terminated string at out[n], cut to fit max; the new length. */
static size_t put_str(uint8_t *out, size_t n, size_t max, const char *t)
{
	while (t != NULL && *t != '\0' && n + 1u < max)
		out[n++] = (uint8_t)*t++;
	out[n++] = 0u;
	return n;
}

static size_t dev_screen(struct opxy_master *m, unsigned part, uint8_t *out)
{
	const struct s15_menu *mn = &m->sv.menu;
	const unsigned rows = mn->con.count;
	const uint32_t changes = s15_menu_changes(mn);
	size_t n;

	if (part > rows) {
		out[0] = OPXY_DEV_BAD;
		return 1u;
	}
	out[1] = (uint8_t)(1u + rows);
	wr32(out + 2, changes);
	if (part == 0u) {
		const unsigned scr = (unsigned)mn->screen;

		out[6] = (uint8_t)scr;
		out[7] = (uint8_t)((opxy_usb_up() ? OPXY_SCREEN_USB_UP : 0u) |
				   (mn->keytest ? OPXY_SCREEN_KEYTEST : 0u) |
				   (mn->armed ? OPXY_SCREEN_ARMED : 0u) |
				   (mn->busy ? OPXY_SCREEN_BUSY : 0u) |
				   (mn->hold ? OPXY_SCREEN_HELD : 0u) |
				   (mn->shift ? OPXY_SCREEN_SHIFT : 0u));
		out[8] = (uint8_t)mn->mtp;
		out[9] = opxy_usb_set() < S10A_SET_COUNT ?
			 (uint8_t)opxy_usb_set() : 0xFFu;
		n = put_str(out, 10u, S06_MAX_PAYLOAD,
			    scr < sizeof(screen_names) / sizeof(screen_names[0]) ?
			    screen_names[scr] : "");
		return put_str(out, n, S06_MAX_PAYLOAD, mn->title);
	} else {
		const enum s15_style st = s15_console_row_style(&mn->con,
								part - 1u);
		size_t len;
		const char *t = s15_console_row(&mn->con, part - 1u, &len);

		out[6] = (uint8_t)(part - 1u);
		out[7] = (uint8_t)st;
		n = put_str(out, 8u, S06_MAX_PAYLOAD,
			    (unsigned)st < S15_ST_COUNT ? s15_style_names[st] : "");
		if (len > S06_MAX_PAYLOAD - n)
			len = S06_MAX_PAYLOAD - n;
		memcpy(out + n, t, len);
		return n + len;
	}
}

/* The queued key events: the key in bits 0-6, a key down in bit 7. */
static uint8_t keyq[OPXY_KEY_QUEUE];
static unsigned key_head, key_tail;

static unsigned keys_waiting(void)
{
	return key_head - key_tail;
}

static void key_put(uint8_t key, bool down)
{
	keyq[key_head++ % OPXY_KEY_QUEUE] = (uint8_t)(key | (down ? 0x80u : 0u));
}

/* key:1 action:1 pairs: all queued, or none. */
static uint8_t dev_key(const uint8_t *p, size_t n)
{
	unsigned need = 0, i;

	if (n % 2u != 0u || n / 2u > OPXY_KEY_PAIRS)
		return OPXY_DEV_BAD;
	for (i = 0; i < n; i += 2u) {
		if (p[i] >= S15_NKEYS || p[i + 1u] > OPXY_KEY_PRESS)
			return OPXY_DEV_BAD;
		need += p[i + 1u] == OPXY_KEY_PRESS ? 2u : 1u;
	}
	if (need > OPXY_KEY_QUEUE - keys_waiting())
		return OPXY_DEV_REFUSED;
	for (i = 0; i < n; i += 2u) {
		if (p[i + 1u] != OPXY_KEY_UP)
			key_put(p[i], true);
		if (p[i + 1u] != OPXY_KEY_DOWN)
			key_put(p[i], false);
	}
	return OPXY_DEV_OK;
}

/* The PSoC's key events go through s15_menu_event() (opxy_held.c); so do
 * these, one per pass, and none while a long operation holds the keys.
 * With developer mode off, no hand-over option is held (S-10c.110). */
void opxy_dev_keys(struct opxy_master *m)
{
	uint8_t ev;

	/* the slave's copy is 0 already: a request that returns clears it */
	if (!m->sv.menu.dev_mode && (dev_options | opxy_ho_flags) != 0u) {
		dev_options = 0u;
		options_clear(m, false);
	}

	if (keys_waiting() == 0u || s15_menu_held(&m->sv.menu))
		return;
	/* the time first (S-15.015); a hold that runs its long action now
	 * leaves the key queued until the action's end */
	opxy_menu_tick(m);
	if (s15_menu_held(&m->sv.menu))
		return;
	ev = keyq[key_tail++ % OPXY_KEY_QUEUE];
	s15_menu_host_event(&m->sv.menu, ev & 0x7Fu, (ev & 0x80u) != 0u);
}

bool opxy_dev_host(struct opxy_master *m, const struct s06_frame *f)
{
	static uint8_t out[S06_MAX_PAYLOAD];
	size_t n = 1u;

	if (f->id != OPXY_DEV_REQ_ID || f->len < 1u)
		return false;
	out[0] = OPXY_DEV_OK;
	/* S-15.172: off unless developer mode is on. Refused, nothing changed.
	 * S-10c.110: and every request but ping needs the host link's approval. */
	if (!m->sv.menu.dev_mode ||
	    (f->payload[0] != OPXY_DEV_PING && !opxy_hostlink_approved(m))) {
		out[0] = m->sv.menu.dev_mode ? OPXY_DEV_NOT_APPROVED : OPXY_DEV_REFUSED;
		(void)s06_send(&m->node, S06_LINK_CDC, s06_swap_route(f->route),
			       f->id, out, 1u);
		return true;
	}
	switch (f->payload[0]) {
	case OPXY_DEV_PING:
		wr32(out + 1, OPXY_DEV_MAGIC);
		n = 5u;
		break;
	case OPXY_DEV_READ: {
		const uint32_t addr = f->len >= 6u ? rd32(f->payload + 1) : 1u;
		const uint32_t words = f->len >= 6u ? f->payload[5] : 0u;

		if (words == 0u || words > 60u) {
			out[0] = OPXY_DEV_BAD;
		} else {
			for (uint32_t i = 0; i < words; i++) {
				const uint32_t a = addr + 4u * i;

				wr32(out + 1 + 4u * i,
				     opxy_dev_mem_ok(a, false) ? wb_mmr_read32(a) :
				     0xDEADBEEFu);
			}
			n = 1u + words * 4u;
		}
		break;
	}
	case OPXY_DEV_WRITE: {
		const uint32_t addr = f->len >= 9u ? rd32(f->payload + 1) : 1u;

		if (!opxy_dev_mem_ok(addr, true)) {
			out[0] = OPXY_DEV_REFUSED;
		} else {
			wb_mmr_write32(addr, rd32(f->payload + 5));
			wb_mmr_sync();
		}
		break;
	}
	case OPXY_DEV_SREAD: {
		const uint32_t addr = f->len >= 5u ? rd32(f->payload + 1) : 1u;

		/* S-10c.110: no slave to name without second-cpu */
		if (!wb_has(opxy_dev(m), WB_F_SECOND_CPU)) {
			out[0] = OPXY_DEV_REFUSED;
			break;
		}
		wr32(out + 1, slave_word(m, OPXY_DEV_PEEK_REQ, addr, 0));
		n = 5u;
		break;
	}
	case OPXY_DEV_SWRITE: {
		const uint32_t addr = f->len >= 9u ? rd32(f->payload + 1) : 1u;

		if (!wb_has(opxy_dev(m), WB_F_SECOND_CPU)) {
			out[0] = OPXY_DEV_REFUSED;
			break;
		}
		wr32(out + 1, slave_word(m, OPXY_DEV_POKE_REQ, addr,
					 rd32(f->payload + 5)));
		n = 5u;
		break;
	}
	case OPXY_DEV_FLAGS:
		/* set:1 flags:4, then the value held. */
		if (f->len >= 6u && f->payload[1] != 0u)
			dev_options = rd32(f->payload + 2);
		wr32(out + 1, dev_options);
		n = 5u;
		break;
	case OPXY_DEV_YTAGS: {
		struct s03_emmc_ops emmc;
		struct opxy_ydump_ops io;
		const uint32_t count = f->len >= 10u ? f->payload[9] : 0u;

		if (f->len < 10u)
			out[0] = OPXY_DEV_BAD;
		else if (!ydump_ready(m, &emmc, &io))
			out[0] = OPXY_DEV_REFUSED;
		else
			out[0] = opxy_ydump_tags(&io, rd32(f->payload + 1),
						 rd32(f->payload + 5), count,
						 out + 1);
		if (out[0] == OPXY_DEV_REFUSED)
			out[0] = file_refused(m);
		if (out[0] == OPXY_DEV_OK)
			n = 1u + OPXY_YTAGS_BYTES * count;
		break;
	}
	case OPXY_DEV_YMETA: {
		struct s03_emmc_ops emmc;
		struct opxy_ydump_ops io;
		const uint32_t len = f->len >= 8u ? f->payload[7] : 0u;

		if (f->len < 8u)
			out[0] = OPXY_DEV_BAD;
		else if (!ydump_ready(m, &emmc, &io))
			out[0] = OPXY_DEV_REFUSED;
		else
			out[0] = opxy_ydump_meta(&io, rd32(f->payload + 1),
						 (uint32_t)f->payload[5] |
						 (uint32_t)f->payload[6] << 8,
						 len, out + 1);
		if (out[0] == OPXY_DEV_REFUSED)
			out[0] = file_refused(m);
		if (out[0] == OPXY_DEV_OK)
			n = 1u + len;
		break;
	}
	case OPXY_DEV_L1READ: {
		/* addr:4 words:1, L1 instruction memory. A data read needs
		 * L1DM_DCTL.ENX (the Blackfin+ PRM's extended data access; verify
		 * the path against the PRM). To compare the loaded L1 code with the
		 * staged block after a hand-over that returned, or in a trace run. */
		const uint32_t addr = f->len >= 6u ? rd32(f->payload + 1) : 1u;
		const uint32_t words = f->len >= 6u ? f->payload[5] : 0u;

		if (words == 0u || words > 60u || (addr & 3u) != 0u ||
		    addr < S01_L1_CODE_START ||
		    (uint64_t)addr + 4u * words > S01_L1_CODE_END) {
			out[0] = OPXY_DEV_BAD;
		} else {
			const uint32_t saved = wb_mmr_read32(L1DM_DCTL);
			uint32_t i;

			wb_mmr_write32(L1DM_DCTL, saved | L1DM_DCTL_ENX);
			wb_core_csync();
			for (i = 0; i < words; i++)
				wr32(out + 1 + 4u * i,
				     *(volatile uint32_t *)(uintptr_t)(addr + 4u * i));
			wb_mmr_write32(L1DM_DCTL, saved);
			wb_core_csync();
			n = 1u + words * 4u;
		}
		break;
	}
	case OPXY_DEV_SCREEN:
		n = dev_screen(m, f->len >= 2u ? f->payload[1] : 0u, out);
		break;
	case OPXY_DEV_KEY:
		out[0] = dev_key(f->payload + 1, f->len - 1u);
		out[1] = (uint8_t)keys_waiting();
		n = 2u;
		break;
	case OPXY_DEV_FRAME: {
		/* offset:4 length:1 */
		const uint8_t *fb = opxy_display_fb();
		const uint32_t off = f->len >= 6u ? rd32(f->payload + 1) : 0u;
		const uint32_t len = f->len >= 6u ? f->payload[5] : 0u;

		if (len == 0u || len > OPXY_FRAME_CHUNK ||
		    (uint64_t)off + len > S13C_FRAME_BYTES) {
			out[0] = OPXY_DEV_BAD;
		} else if (fb == NULL) {
			out[0] = OPXY_DEV_REFUSED;
		} else {
			memcpy(out + 1, fb + off, len);
			n = 1u + len;
		}
		break;
	}
	case OPXY_DEV_BENCH:
		n = opxy_dev_bench(m, f->payload + 1, f->len - 1u, out);
		break;
	case OPXY_DEV_CHECK:
		n = opxy_dev_check(m, out);
		break;
	case OPXY_DEV_EXTRACT:
		n = opxy_dev_extract(m, out);
		break;
	case OPXY_DEV_LINK_RECORD: {
		/* part:1, the link's record at any time */
		const unsigned part = f->len >= 2u ? f->payload[1] : OPXY_RECORD_PARTS;

		if (part >= OPXY_RECORD_PARTS)
			out[0] = OPXY_DEV_BAD;
		else
			n = 1u + opxy_bridge_record(&m->bridge, part, out + 1);
		break;
	}
	case OPXY_DEV_LINK_TEST:
		n = opxy_dev_link_test(m, f->payload + 1, f->len - 1u, out);
		break;
	case OPXY_DEV_QUAD_REPLY:	/* S-09.080, S-13b.084 */
		n = opxy_dev_quad_reply(m, f->payload + 1, f->len - 1u, out);
		break;
	case OPXY_DEV_CKPT:
		n = opxy_dev_ckpt(m, f->payload + 1, f->len - 1u, out);
		break;
	case OPXY_DEV_QUAD_DATA:	/* S-09.081: this start's quad data, at any time */
		n = 1u + opxy_bridge_qdata_report(&m->bridge, out + 1);
		break;
	case OPXY_DEV_BOOT_START:	/* S-15.163's account, at any time */
		n = 1u + opxy_start_report(m, out + 1);
		break;
#if OPXY_BOOT_LINK_FC
	case OPXY_DEV_STREAM:		/* S-09.009: this start's stream, at any time */
		n = 1u + opxy_stream_report(out + 1);
		break;
#endif
	case OPXY_DEV_CAL_START:
		/* The answer first: the restart does not return. */
		(void)s06_send(&m->node, S06_LINK_CDC, s06_swap_route(f->route),
			       f->id, out, 1u);
		usb_flush(200u);
		opxy_reboot(m, S04_W10, 0u);
		return true;			/* only with a test's operations */
	case OPXY_DEV_HANDOVER: {
		/* flags:4, then optional master-delay:2 and slave-delay:2 (ms).
		 * The ack goes out first, since a good hand-over does not return;
		 * on a trace run a milestone frame follows (OPXY_HO_TRACE_MARK),
		 * and on a return a frame with the failure code. */
		const uint8_t route = s06_swap_route(f->route);
		const uint32_t flags = f->len >= 5u ? rd32(f->payload + 1) :
				       dev_options;
		const uint32_t md = f->len >= 7u ? (uint32_t)f->payload[5] |
				    (uint32_t)f->payload[6] << 8 : 0u;
		const uint32_t sd = f->len >= 9u ? (uint32_t)f->payload[7] |
				    (uint32_t)f->payload[8] << 8 : 0u;

		(void)s06_send(&m->node, S06_LINK_CDC, route, f->id, out, 1u);
		out[0] = OPXY_DEV_REFUSED;	/* it returned: a failed hand-over */
		out[1] = (uint8_t)do_handover(m, flags, md, sd, route);
		n = 2u;
		break;
	}
	default:
		out[0] = OPXY_DEV_BAD;
		break;
	}
	(void)s06_send(&m->node, S06_LINK_CDC, s06_swap_route(f->route), f->id,
		       out, n);
	return true;
}

#endif /* !OPXY_TEST_OS */
