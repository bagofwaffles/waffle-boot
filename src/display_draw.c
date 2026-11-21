/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15's draw interface on the frame buffer, and the display's calls of
 * S-13c in the order a run makes them.
 */
#include "waffle/display.h"

#include <string.h>

#include "waffle/s15_lines.h"

/* S-15.002, S-15.003: 33 + 8 * 19 + 16 = 201 < 202; 205 + 16 =
 * 221 <= 222; 16 + 56 * 8 + 8 = 472 <= 480. Row 0 stands 9 pixels
 * below the title bar. */
const struct s15_geometry wb_display_geometry = {
	.width = WB_DISPLAY_W,
	.height = WB_DISPLAY_H,
	.edge = 8,
	.margin = 16,
	.top = 33,
	.pitch = 19,
	.glyph_w = WB_FONT_W,
	.glyph_h = WB_FONT_H,
	.bar_h = 24,
	.status_y = 202,
	.title_w = WB_TITLE_W,
	.title_h = WB_TITLE_H,
	.title_y = 4,
};

/* ---- the frame buffer ---- */

/*
 * S-13c.024: the frame in flight is read from the buffer, so the buffer is
 * written only once it has ended. A frame that has not ended within
 * S13C_FRAME_TIMEOUT_US is stopped first; false when it cannot be, and
 * the draw is dropped.
 */
static bool settle(struct wb_display *d)
{
	if (!s13c_frame_busy(&d->panel) ||
	    s13c_frame_wait(&d->panel) == S13C_OK)
		return true;
	return s13c_frame_stop(&d->panel);
}

/* S-13c.023: screen (x, y), inside the visible area, is window row
 * 479 - x, column y; to its first byte. */
static uint8_t *pixel(const struct wb_display *d, unsigned x, unsigned y)
{
	unsigned r = WB_DISPLAY_W - 1u - x;

	return &d->fb[2u * ((size_t)r * S13C_FRAME_W + y)];
}

static void put(const struct wb_display *d, unsigned x, unsigned y,
		uint16_t rgb)
{
	uint8_t *p = pixel(d, x, y);

	p[0] = (uint8_t)(rgb >> 8);	/* S-13c.015: high byte first */
	p[1] = (uint8_t)rgb;
}

static void d_fill(void *ctx, uint16_t x, uint16_t y, uint16_t w,
		   uint16_t h, uint16_t rgb565)
{
	struct wb_display *d = ctx;
	unsigned x1 = (unsigned)x + w, y1 = (unsigned)y + h, i, j;

	if (!settle(d))
		return;
	d->dirty = true;
	if (x1 > WB_DISPLAY_W)
		x1 = WB_DISPLAY_W;
	if (y1 > WB_DISPLAY_H)
		y1 = WB_DISPLAY_H;
	for (i = (unsigned)x; i < x1; i++)
		for (j = (unsigned)y; j < y1; j++)
			put(d, i, j, rgb565);
}

/* n glyphs from (x, y) in fg; with clear, the pixels between the strokes
 * are left as they are, else drawn in bg. */
static void glyphs(struct wb_display *d, uint16_t x, uint16_t y,
		   const char *s, size_t n, uint16_t fg, uint16_t bg,
		   bool clear)
{
	size_t k;

	if (!settle(d))
		return;
	d->dirty = true;
	for (k = 0; k < n; k++) {
		unsigned ch = (unsigned char)s[k];
		unsigned gx = (unsigned)x + (unsigned)k * WB_FONT_W;
		const uint8_t *g;
		unsigned i, j;

		if (gx >= WB_DISPLAY_W)
			break;
		if (ch < WB_FONT_FIRST || ch >= WB_FONT_FIRST + WB_FONT_COUNT)
			ch = '?';
		g = wb_display_font[ch - WB_FONT_FIRST];
		for (j = 0; j < WB_FONT_H && (unsigned)y + j < WB_DISPLAY_H; j++)
			for (i = 0; i < WB_FONT_W && gx + i < WB_DISPLAY_W; i++) {
				bool on = (((unsigned)g[j] >>
					    (WB_FONT_W - 1u - i)) & 1u) != 0u;

				if (on || !clear)
					put(d, gx + i, (unsigned)y + j,
					    on ? fg : bg);
			}
	}
}

static void d_text(void *ctx, uint16_t x, uint16_t y, const char *s,
		   size_t n, uint16_t fg, uint16_t bg)
{
	glyphs(ctx, x, y, s, n, fg, bg, false);
}

static void d_ink(void *ctx, uint16_t x, uint16_t y, const char *s,
		  size_t n, uint16_t fg)
{
	glyphs(ctx, x, y, s, n, fg, 0, true);
}

/* The title bar's text: n glyphs of its font, over the bar's squares. */
static void d_bold(void *ctx, uint16_t x, uint16_t y, const char *s,
		   size_t n, uint16_t fg)
{
	struct wb_display *d = ctx;
	size_t k;

	if (!settle(d))
		return;
	d->dirty = true;
	for (k = 0; k < n; k++) {
		unsigned ch = (unsigned char)s[k];
		unsigned gx = (unsigned)x + (unsigned)k * WB_TITLE_W;
		const uint16_t *g;
		unsigned i, j;

		if (gx >= WB_DISPLAY_W)
			break;
		if (ch < WB_FONT_FIRST || ch >= WB_FONT_FIRST + WB_FONT_COUNT)
			ch = '?';
		g = wb_display_title_font[ch - WB_FONT_FIRST];
		for (j = 0; j < WB_TITLE_H && (unsigned)y + j < WB_DISPLAY_H; j++)
			for (i = 0; i < WB_TITLE_W && gx + i < WB_DISPLAY_W; i++)
				if (((unsigned)g[j] >> (WB_TITLE_W - 1u - i)) & 1u)
					put(d, gx + i, (unsigned)y + j, fg);
	}
}

void wb_display_init(struct wb_display *d, uint8_t *fb)
{
	memset(d, 0, sizeof *d);
	d->panel.state = S13C_IDLE;
	d->fb = fb;
	d->draw.fill = d_fill;
	d->draw.text = d_text;
	d->draw.ink = d_ink;
	d->draw.bold = d_bold;
	d->draw.ctx = d;
	memset(fb, 0, S13C_FRAME_BYTES);
}

const struct s15_draw_ops *wb_display_draw(struct wb_display *d)
{
	return &d->draw;
}

/* ---- the panel ---- */

void wb_display_begin(struct wb_display *d, const struct s13c_bus *bus,
		      const struct wb_device *dev, uint32_t xmod, uint32_t ymod,
		      unsigned pa8)
{
	s13c_begin(&d->panel, bus, dev, xmod, ymod, pa8);
}

void wb_display_adopt(struct wb_display *d, const struct s13c_bus *bus,
		      const struct wb_device *dev, uint32_t reset_ms)
{
	s13c_adopt(&d->panel, bus, dev, reset_ms);
}

int wb_display_start(struct wb_display *d, bool gamma)
{
	return s13c_start(&d->panel, gamma);
}

int wb_display_flush(struct wb_display *d)
{
	if (d->stopped)
		return S13C_ERR_STATE;
	d->dirty = false;
	return s13c_frame(&d->panel, d->fb, true);
}

int wb_display_flush_fatal(struct wb_display *d)
{
	int rc;

	if (d->stopped)
		return S13C_ERR_STATE;
	d->stopped = true;
	d->dirty = false;
	rc = s13c_frame(&d->panel, d->fb, false);
	if (rc != S13C_OK)
		return rc;
	return s13c_frame_wait(&d->panel);
}

bool wb_display_busy(struct wb_display *d)
{
	return s13c_frame_busy(&d->panel);
}

int wb_display_wait(struct wb_display *d)
{
	return s13c_frame_wait(&d->panel);
}

int wb_display_update(struct wb_display *d)
{
	if (!d->dirty || d->panel.state != S13C_ON)
		return S13C_OK;
	return wb_display_flush(d);
}

/* ---- S-15.132: the hand-over frame ---- */

struct handover {
	const uint8_t *data;	/* the image's records; NULL: the logo */
	size_t len, pos;
	unsigned run;		/* pixels left in the record */
	bool literal;
	uint16_t rgb;		/* a repeat record's pixel */
};

static void handover_init(struct handover *h, const struct wb_display_image *image)
{
	memset(h, 0, sizeof *h);
	if (image != NULL && image->data != NULL) {
		h->data = image->data;
		h->len = image->len;
	}
}

/* The image's next two bytes as a pixel; black when they run out. */
static uint16_t image_word(struct handover *h)
{
	uint16_t v;

	if (h->len - h->pos < 2u) {
		h->pos = h->len;
		h->run = 0;
		return S15_BLACK;
	}
	v = (uint16_t)((unsigned)h->data[h->pos] << 8 | h->data[h->pos + 1u]);
	h->pos += 2u;
	return v;
}

/* The image's next pixel; black once its records run out. */
static uint16_t image_pixel(struct handover *h)
{
	if (h->run == 0u) {
		uint8_t b;

		if (h->pos >= h->len)
			return S15_BLACK;
		b = h->data[h->pos++];
		h->literal = b < 0x80u;
		h->run = h->literal ? b + 1u : b - 0x7Fu;
		if (!h->literal)
			h->rgb = image_word(h);
		if (h->run == 0u)
			return S15_BLACK;
	}
	h->run--;
	return h->literal ? image_word(h) : h->rgb;
}

/* Window row r: screen column 479 - r, from the top down (S-13c.023). */
static void handover_row(void *ctx, unsigned r, uint8_t *out)
{
	struct handover *h = ctx;
	unsigned x = WB_DISPLAY_W - 1u - r, c;

	for (c = 0; c < S13C_FRAME_W; c++) {
		uint16_t p = S15_BLACK;

		if (c < S15_HANDOVER_VISIBLE)
			p = h->data != NULL ? image_pixel(h) : s15_logo_pixel(x, c);
		out[2u * c] = (uint8_t)(p >> 8);
		out[2u * c + 1u] = (uint8_t)p;
	}
}

int wb_display_boot_logo(struct wb_display *d, const struct wb_display_image *image)
{
	struct handover h;
	int rc = s13c_start(&d->panel, true);

	if (rc != S13C_OK)
		return rc;
	handover_init(&h, image);
	return s13c_boot_frame(&d->panel, handover_row, &h);
}

/* The logo line's window row r: screen x 479 - r, its column from the top. */
struct logo_line {
	const char *s;
	unsigned x0, n;
};

static void logo_line_row(void *ctx, unsigned r, uint8_t *out)
{
	const struct logo_line *l = ctx;
	const unsigned x = WB_DISPLAY_W - 1u - r;
	const uint8_t *g = NULL;
	unsigned bit = 0, j;

	if (x >= l->x0 && x < l->x0 + l->n * WB_FONT_W) {
		unsigned ch = (unsigned char)l->s[(x - l->x0) / WB_FONT_W];

		if (ch < WB_FONT_FIRST || ch >= WB_FONT_FIRST + WB_FONT_COUNT)
			ch = '?';
		g = wb_display_font[ch - WB_FONT_FIRST];
		bit = WB_FONT_W - 1u - (x - l->x0) % WB_FONT_W;
	}
	for (j = 0; j < WB_FONT_H; j++) {
		const uint16_t c = g != NULL && (((unsigned)g[j] >> bit) & 1u) != 0u ?
					   S15_DIM : S15_BLACK;

		out[2u * j] = (uint8_t)(c >> 8);
		out[2u * j + 1u] = (uint8_t)c;
	}
}

int wb_display_logo_line(struct wb_display *d, const char *s)
{
	struct logo_line l;
	size_t n = strlen(s);

	if (n > WB_DISPLAY_W / WB_FONT_W)
		n = WB_DISPLAY_W / WB_FONT_W;
	l.s = s;
	l.n = (unsigned)n;
	l.x0 = (WB_DISPLAY_W - l.n * WB_FONT_W) / 2u;
	return s13c_logo_line(&d->panel, WB_LOGO_LINE_Y, WB_FONT_H, logo_line_row, &l);
}

void wb_display_handover_frame(const struct wb_display_image *image, uint8_t *frame)
{
	struct handover h;
	unsigned r;

	handover_init(&h, image);
	for (r = 0; r < S13C_ROWS; r++)
		handover_row(&h, r, frame + (size_t)r * S13C_ROW_BYTES);
}

int wb_display_handover(struct wb_display *d, uint8_t rec[S13C_RECORD_SIZE])
{
	return s13c_handover(&d->panel, rec);
}

int wb_display_record(struct wb_display *d, uint8_t rec[S13C_RECORD_SIZE])
{
	const struct s13c_panel *p = &d->panel;

	if (p->state != S13C_ON || !p->boot_frame || !p->gamma_on)
		return S13C_ERR_STATE;
	s13c_record(p->reset_ms, rec);
	return S13C_OK;
}

int wb_display_poweroff(struct wb_display *d)
{
	return s13c_poweroff(&d->panel);
}
