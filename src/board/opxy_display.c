/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The OP-XY master's display: the board's display hooks
 * (waffle/board/opxy_boot.h, waffle/board/opxy_services.h) over
 * wb_display_* on the BF70x panel bus. Strong definitions of the board's
 * weak ones; the image links libwaffle whole.
 *
 * The frame buffer is the first frame of OPXY_FRAMES_START. The PSoC's
 * messages around the display (S-13c.019) stay the board's: the start-up
 * report before held mode's start, {3C FF 32} after the boot logo, and
 * {00 00 02} after the power-off's last frame has ended
 * (s07_shutdown_reports() follows the power-off hook).
 */
#include "waffle/board/opxy_display.h"
#include "waffle/drivers/bf70x_display.h"

#include <stdint.h>
#include <string.h>

#include "waffle/board/opxy.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_services.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/mmr.h"
#include "waffle/s01_memmap.h"
#include "waffle/testos.h"

#define OS_FRAME_BYTES (OPXY_OS_FRAME_END - OPXY_OS_FRAME_START)

#if defined(__bfin__)
#define FB       ((uint8_t *)(uintptr_t)OPXY_FRAMES_START)
#define RECORD   ((uint8_t *)(uintptr_t)S01_DISPLAY_RECORD_START)
#define OS_FRAME ((uint32_t *)(uintptr_t)OPXY_OS_FRAME_START)
#else
/* the host's tests: no DDR there */
static uint8_t host_fb[S13C_FRAME_BYTES];
static uint8_t host_record[S13C_RECORD_SIZE];
static uint32_t host_os_frame[OS_FRAME_BYTES / 4u];
#define FB       host_fb
#define RECORD   host_record
#define OS_FRAME host_os_frame
#endif

/* Staging (S-01.014) is read after this is written, so they must not meet. */
_Static_assert(OPXY_OS_FRAME_START >= S01_STAGING_END ||
	       OPXY_OS_FRAME_END <= S01_STAGING_START,
	       "the OS's frame is clear of staging");

static struct wb_display_bf70x bus;
static struct wb_display disp;
static bool up;				/* the panel bus came up */
static bool adopted;			/* the front's logo frame is the last one */

const struct wb_display *opxy_display(void)
{
	return up ? &disp : NULL;
}

#if defined(OPXY_TEST_OS)
/* The test OS's paints go through this display's bus (opxy_testos_paint.c). */
struct wb_display *opxy_testos_display(void)
{
	return up ? &disp : NULL;
}
#endif

/* S-13c.004: the reset decision, from the entry's DMA9 values and the PA8
 * strap sample (S-13b.001). Before any DMA9 write. */
/* S-13c.001: the panel's pins on the device (D/C, S-18.030). */
static const struct wb_lcd_config *lcd_of(const struct wb_device *dev)
{
	return dev->id == WB_DEV_OP1F ? &wb_lcd_op1f : &wb_lcd_opxy;
}

void opxy_display_handshake(uint32_t xmod, uint32_t ymod)
{
	struct opxy_master *m = &opxy_master;
	const struct wb_device *dev = opxy_dev(m);
	const struct s13c_bus *b = wb_display_bf70x_init_cfg(&bus, lcd_of(dev), &m->hw.time);

	if (b == NULL)
		return;
	wb_display_init(&disp, FB);
	/* S-13c.027: without straps PA8 is never sampled; the decision is
	 * the one PA8 high gives (a valid earlier handshake, else a pulse) */
	wb_display_begin(&disp, b, dev, xmod, ymod,
			 !wb_has(dev, WB_F_STRAPS) || (m->strap & OPXY_STRAP_REV1) != 0u ? 1u : 0u);
	adopted = false;
	up = true;
}

void opxy_display_adopt(uint32_t reset_ms)
{
	struct opxy_master *m = &opxy_master;
	const struct s13c_bus *b = wb_display_bf70x_init_cfg(&bus, lcd_of(opxy_dev(m)),
							      &m->hw.time);

	if (b == NULL)
		return;
	wb_display_init(&disp, FB);
	wb_display_adopt(&disp, b, opxy_dev(m), reset_ms);
	/* S-13c.012: this run's own gamma load, whatever the front's left. */
	(void)s13c_gamma(&disp.panel, true);
	adopted = true;
	up = true;
}

/* S-15's geometry, once the panel bus is up. */
const struct s15_geometry *opxy_display_geometry(void)
{
	return up ? &wb_display_geometry : NULL;
}

/*
 * What was drawn goes out as a frame, without waiting for its end. Held
 * mode's poll calls it after each render, so a change reaches the panel on
 * the pass that drew it (S-13c.017's 100 ms plus one frame time).
 */
void opxy_display_send(void)
{
	if (up)
		(void)OPXY_DISPLAY_UPDATE(&disp);
}

const uint8_t *opxy_display_fb(void)
{
	return up ? FB : NULL;
}

void opxy_display_draw_ops(struct opxy_master *m, struct s15_draw_ops *ops)
{
	if (up)
		*ops = *wb_display_draw(&disp);
	else
		opxy_draw_record(&m->sv.drawn, ops);
}

/* S-13c.006 - S-13c.013, S-15.021: initialized and on before the first
 * screen; a failed load's held mode finds it on already. */
void opxy_display_held_start(struct opxy_master *m)
{
	(void)m;
	if (up)
		(void)wb_display_start(&disp, false);
}

bool opxy_display_logo_line(const char *s)
{
	return up && wb_display_logo_line(&disp, s) == S13C_OK;
}

/* The frame drawn since the last one sent now, and ended on the return. */
void opxy_display_show(void)
{
	if (!up)
		return;
	(void)wb_display_update(&disp);
	while (wb_display_busy(&disp))
		;
}

/* What was drawn is sent and the frame ended, or its wait timed out: the
 * caller goes on either way. */
void opxy_display_settle(void)
{
	if (!up)
		return;
	(void)wb_display_update(&disp);
	(void)wb_display_wait(&disp);
}

/* S-15.132: a build given no image shows the logo; the file that
 * make image LOGO=<file>.png generates defines the image instead. */
__attribute__((weak)) const struct wb_display_image wb_display_boot_image = {
	NULL, 0u
};

/*
 * S-05.004, S-15.132: the hand-over frame before the OS is read, then the
 * record (S-13c.021): the board has no hook at the kernel call, and no
 * panel transaction comes between this and the call on the boot path.
 */
/* S-15.132: the boot logo's frame, sent at once and ended. */
static bool logo_frame(void)
{
	if (!up || wb_display_boot_logo(&disp, &wb_display_boot_image) != S13C_OK)
		return false;
	adopted = false;	/* this build's image on the panel now */
	return true;
}

/*
 * S-05.004: the early logo frame, on every run once
 * the keys are read: the run's one initialization (S-13c.013, with the
 * gamma load), then the logo. The hand-over frame below sends it again.
 */
void opxy_display_dma_take(void)
{
	/* A loader may have left CRC1 on: on a unit, DMA21 then delivered
	 * nothing. */
	wb_mmr_write32(CRC1_CTL, 0u);
	wb_mmr_sync();
}

bool opxy_display_early_logo(void)
{
	return OPXY_TESTOS_OR(opxy_testos_early_logo(), logo_frame());
}

#if defined(OPXY_TEST_OS)
/*
 * The test OS's early logo (wiki/Test-OS.md, "The color trail"): the
 * same steps, a paint after each, and the result logged. The panel's
 * start is a no-op here (yellow's initialization loaded the gamma, so
 * S-13c.013's one initialization is done); the frame goes out by CPU
 * writes on both byte paths (s13c_boot_frame(), S-13c.016), with its
 * wait for a frame in flight bounded by S13C_FRAME_TIMEOUT_US.
 */
bool opxy_testos_early_logo(void)
{
	struct s14_text t;
	int rc;
	bool sent;

	if (!up)
		return false;
	rc = wb_display_start(&disp, true);
	opxy_testos_paint(OPXY_PAINT_CYAN);
	s14_text_init(&t);
	s14_text_str(&t, "early logo: panel start returned ");
	s14_text_dec(&t, (uint32_t)-rc);
	opxy_testos_emit(&opxy_master, &t);
	sent = rc == S13C_OK && logo_frame();
	opxy_testos_paint(OPXY_PAINT_GRAY);
	s14_text_init(&t);
	s14_text_str(&t, sent ? "early logo: frame sent" :
			    "early logo: frame not sent (the panel's state or a frame in flight)");
	opxy_testos_emit(&opxy_master, &t);
	return sent;
}
#endif

/*
 * S-05.016, display frame memory: where the display DMA reads its frame
 * at the OS's first instruction (S-05.031), the logo alone, since the OS draws its first
 * screen over what it finds there; with reason 70 (S-04.012) the
 * calibration screen instead. Either is drawn as a frame, then each row
 * moved to OPXY_OS_FRAME_ROW bytes apart with the pixel's low byte first,
 * the layout the OS reads (measured on a unit with the calibration
 * screen). The rest of a row is black for the logo and magenta for the
 * calibration screen, which shows only if the OS sends more of a row.
 * The data pages are write-through (S-05.014), so the bytes reach DDR.
 */
#define OS_ROWS_END   (S13C_ROWS * OPXY_OS_FRAME_ROW)
#define CAL_WHITE     0xFFFFu
#define CAL_RED       0xF800u
#define CAL_GREEN     0x07E0u
#define CAL_BLUE      0x001Fu
#define CAL_YELLOW    0xFFE0u
#define CAL_GRAY      0x5ACBu
#define CAL_MAGENTA   0xF81Fu

_Static_assert(OPXY_OS_FRAME_ROW >= S13C_ROW_BYTES &&
	       OPXY_OS_FRAME_ROW % 4u == 0u && OS_ROWS_END <= OS_FRAME_BYTES,
	       "the OS's rows hold a frame's and fit the range");

static void cal_text(const struct s15_draw_ops *ops, unsigned x, unsigned y,
		     const char *s)
{
	ops->text(ops->ctx, (uint16_t)x, (uint16_t)y, s, strlen(s), CAL_WHITE,
		  0u);
}

static void cal_draw(const struct s15_draw_ops *ops)
{
	static const struct {
		const char *name;
		uint16_t rgb;
	} bars[] = {
		{ "F800 red", CAL_RED }, { "07E0 green", CAL_GREEN },
		{ "001F blue", CAL_BLUE }, { "FFFF white", CAL_WHITE },
		{ "5ACB gray", CAL_GRAY },
	};
	const unsigned w = WB_DISPLAY_W, h = WB_DISPLAY_H;
	unsigned i;

	/* the border, one pixel, and a tick every 32 pixels */
	ops->fill(ops->ctx, 0, 0, (uint16_t)w, 1, CAL_WHITE);
	ops->fill(ops->ctx, 0, (uint16_t)(h - 1u), (uint16_t)w, 1, CAL_WHITE);
	ops->fill(ops->ctx, 0, 0, 1, (uint16_t)h, CAL_WHITE);
	ops->fill(ops->ctx, (uint16_t)(w - 1u), 0, 1, (uint16_t)h, CAL_WHITE);
	for (i = 32u; i < w; i += 32u) {
		ops->fill(ops->ctx, (uint16_t)i, 1, 1, 5, CAL_WHITE);
		ops->fill(ops->ctx, (uint16_t)i, (uint16_t)(h - 6u), 1, 5,
			  CAL_WHITE);
	}
	for (i = 32u; i < h; i += 32u) {
		ops->fill(ops->ctx, 1, (uint16_t)i, 5, 1, CAL_WHITE);
		ops->fill(ops->ctx, (uint16_t)(w - 6u), (uint16_t)i, 5, 1,
			  CAL_WHITE);
	}
	/* a different marker in each corner, named in white */
	ops->fill(ops->ctx, 8, 8, 16, 16, CAL_RED);
	cal_text(ops, 28, 8, "TOP LEFT red");
	ops->fill(ops->ctx, (uint16_t)(w - 24u), 8, 16, 16, CAL_GREEN);
	cal_text(ops, w - 28u - 15u * WB_FONT_W, 8, "TOP RIGHT green");
	ops->fill(ops->ctx, 8, (uint16_t)(h - 22u), 16, 16, CAL_BLUE);
	cal_text(ops, 28, h - 22u, "BOTTOM LEFT blue");
	ops->fill(ops->ctx, (uint16_t)(w - 24u), (uint16_t)(h - 22u), 16, 16,
		  CAL_YELLOW);
	cal_text(ops, w - 28u - 19u * WB_FONT_W, h - 22u,
		 "BOTTOM RIGHT yellow");
	/* stripes, each named with its RGB565 value */
	for (i = 0; i < sizeof bars / sizeof bars[0]; i++) {
		const unsigned y = 40u + 20u * i;

		cal_text(ops, 16, y, bars[i].name);
		ops->fill(ops->ctx, 104, (uint16_t)y, (uint16_t)(w - 120u), 16,
			  bars[i].rgb);
	}
	cal_text(ops, 16, 140, "rows 512 bytes apart, 224 pixels, low byte first");
}

/* Rows from the last, pixels from the last: each source byte is read
 * before its row's new place covers it. */
static void os_relayout(uint8_t *b, uint16_t pad)
{
	unsigned r, c;

	for (r = S13C_ROWS; r-- > 0u;) {
		const uint8_t *src = b + (size_t)r * S13C_ROW_BYTES;
		uint8_t *dst = b + (size_t)r * OPXY_OS_FRAME_ROW;

		for (c = OPXY_OS_FRAME_ROW / 2u; c-- > S13C_FRAME_W;) {
			dst[2u * c] = (uint8_t)pad;
			dst[2u * c + 1u] = (uint8_t)(pad >> 8);
		}
		for (c = S13C_FRAME_W; c-- > 0u;) {
			const uint8_t hi = src[2u * c], lo = src[2u * c + 1u];

			dst[2u * c] = lo;
			dst[2u * c + 1u] = hi;
		}
	}
}

/* The rows past the frame's, to the end of the range, black. */
static void os_frame_tail(uint32_t *w)
{
	size_t i;

	for (i = OS_ROWS_END / 4u; i < OS_FRAME_BYTES / 4u; i++)
		w[i] = 0u;
}

static void os_frame_logo(void)
{
	uint32_t *w = OS_FRAME;

	wb_display_handover_frame(&wb_display_boot_image, (uint8_t *)w);
	os_relayout((uint8_t *)w, 0u);
	os_frame_tail(w);
}

static void os_frame_calibration(void)
{
	static struct wb_display cal;
	uint32_t *w = OS_FRAME;

	wb_display_init(&cal, (uint8_t *)w);	/* the panel stays idle */
	cal_draw(wb_display_draw(&cal));
	os_relayout((uint8_t *)w, CAL_MAGENTA);
	os_frame_tail(w);
}

/*
 * S-05.004, S-13c.020: the panel shows the hand-over frame already when its
 * last frame is the early logo frame, nothing sent since but the gamma load
 * (no line under it, S-13c.025),
 * and that frame has this build's image: the front's is the logo, so a
 * build-time image (S-15.132) is sent all the same.
 */
static bool logo_kept(void)
{
	const struct s13c_panel *p = &disp.panel;

	return up && p->state == S13C_ON && p->boot_frame && !p->lined &&
	       p->gamma_on && !p->frame &&
	       (!adopted || wb_display_boot_image.data == NULL);
}

bool opxy_display_boot_logo(void)
{
	const bool kept = logo_kept();

	if (kept || logo_frame())
		(void)wb_display_record(&disp, RECORD);
	if (opxy_master.boot_reason == S04_REASON_OS_FRAME_CAL)
		os_frame_calibration();
	else
		os_frame_logo();
	return kept;
}

const uint8_t *opxy_display_os_frame(void)
{
	return (const uint8_t *)OS_FRAME;
}

/* S-13c.022: no frame from here; the frame in progress ends, or is
 * stopped where it is after its bound, and nothing more is sent: the
 * panel keeps its last frame. */
void opxy_display_power_off(struct opxy_master *m)
{
	(void)m;
	if (up)
		(void)wb_display_poweroff(&disp);
}
