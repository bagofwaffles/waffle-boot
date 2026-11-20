/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for the S-13c panel logic (src/s13c_panel.c), driven
 * through the board's display interface (src/display_draw.c) with frames
 * drawn by S-15's console renderer in its frame.
 *
 * Commands (one result line each):
 *   run PATH OUT [KEY=VALUE ...]  -> ok FRAMES TE_MISSES
 *   font  -> ok HEIGHT WIDTH PITCH MARGIN TOP TITLE_ROW STATUS_ROW GLYPHS
 *            TITLE_HEIGHT TITLE_WIDTH TITLE_GLYPHS
 *
 * font gives the declared font (S-15.003): GLYPHS is 95 glyphs, 0x20 to
 * 0x7E, of HEIGHT row bytes each (bit WIDTH-1 the leftmost pixel);
 * TITLE_GLYPHS the title bar's, of TITLE_HEIGHT rows of two bytes each,
 * high byte first (bit TITLE_WIDTH-1 the leftmost pixel);
 * MARGIN, TOP (body row 0, the rows PITCH apart), TITLE_ROW (the title
 * bar's text) and STATUS_ROW (the status bar's text) are screen positions
 * (S-13c.023). The body's text starts at MARGIN, the bars' text at the
 * bars' edge (wb_display_geometry.edge), both on the glyph grid.
 *
 * runs one bootloader run over a model of the panel bus and writes its
 * panel-bus trace (check_trace.py's format) to the file OUT. PATH is boot,
 * held or fail (the boot path's preparation, the load failing, then held
 * mode). Keys:
 *   hs=0|1        the first stage's reset pulse at 2 ms and its handshake
 *                 (DMA9_XMOD 0x12345678, DMA9_YMOD 2); default 1
 *   xmod=N ymod=N the DMA9 values instead
 *   strap=0|1     the board's PA8 level during the strap interval; 1
 *   te_us=N       the TE period (0: no TE edges); 16667
 *   frames=N      held-mode frames; 4. Frame k shows the console after
 *                 line k is appended: 56 characters (S15_COLS) from
 *                 0x20 + 13k cyclically in 0x20..0x7E (line_text(k)), in
 *                 the frame titled "Main Menu" with waffle-boot's version
 *                 "1.0.0"
 *   option=1      the option's indicator in every held frame
 *   status=N      the frame's status words (S15_STATUS_*); 0
 *   slot=N        the running slot's index, which SLOT names; not
 *                 established (S15_SLOT_UNKNOWN) by default
 *   pattern=1     the last held frame is S-13c.015's test pattern
 *   orient=1      the last held frame is S-13c.023's mark, drawn in
 *                 screen positions through the draw interface
 *                 (vectors/orientation.json)
 *   fatal=1       after the second frame, the line "fatal: test" and the
 *                 fatal-error frame, mid-period
 *   poweroff=N    the power-off (S-13c.022, scenario D-OFF): 1 in held
 *                 mode while its last frame goes out, 2 in held mode
 *                 between two frames, 3 in the start after the early
 *                 logo frame is lit, 4 in the start before the first
 *                 frame; MARK poweroff where it starts, the panel's
 *                 power-off, then 0x51 00 00 02, and the run ends there
 *   regreet=1     a PSoC greeting before the initialization
 *   image=1       scenario D-IMAGE (S-13c.024) in place of frames=N: a
 *                 full console, then 16 changes, each a line appended so
 *                 that every line moves, k ms (k = 0..15) after the first
 *                 byte of a frame of the screen as it stands, with CHANGE
 *                 at each; the new screen goes out, then once more (the
 *                 settled frame), and the next change comes at least four
 *                 TE periods (250 ms without TE) later
 *   prompt=1      a build with the boot prompt (S-05.025): after the early
 *                 logo, MARK prompt and the prompt's frame (S-15.180), lit
 *                 by the early logo's message; it waits 3 s, then the path
 *                 goes on (boot: digit 1; held: digit 2, S-15.021)
 *   line=1        held or fail: at held mode's entry, the line under the
 *                 logo (S-13c.025), 4 s before held mode's 3C FF 02
 *   cont=1        after held mode's frames, Continue to the OS (S-05.033,
 *                 S-15.190), a restart: the restarting view as
 *                 s15_menu_continue() draws it, with the lines of
 *                 src/s15_lines.c, sent whole; the board's 750 ms hold;
 *                 then the restart's shutdown, which starts with the
 *                 panel's power-off as poweroff=N's (MARK poweroff, then
 *                 0x51 00 00 02), and the run ends there
 *
 * The model stands for what is not S-13c's: the first stage, the strap
 * interval and the board's PSoC messages (MSG51, S-07) and marks, timed
 * as tests/oracle/S-13c-display/gen_vectors.py times them. Each bus operation
 * takes the time the BF70x driver's (SMC write cycle 70 ns; GPIO 1 us)
 * would. A command or a frame started while a frame is still being sent
 * is a misuse of the bus and fails the command. A frame's bytes are read
 * from its buffer as the transfer reaches them, one per write cycle from
 * its start: each bus call first takes in the bytes read by its time, so
 * a buffer written while the frame goes out shows in the trace as the
 * panel would receive it (S-13c.024).
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hlib.h"
#include "waffle/display.h"
#include "waffle/s13c_panel.h"
#include "waffle/s15_console.h"
#include "waffle/s15_lines.h"
#include "waffle/s15_prompt.h"

#define US 1000u
#define MS 1000000u
#define BYTE_NS 70u		/* one SMC write cycle */
#define GPIO_NS 1000u		/* one GPIO change and its sync */

struct sim {
	FILE *f;
	uint64_t t;		/* ns */
	uint64_t frame_end;	/* the last frame's last byte + 1 */
	/* the frame going out: its buffer, first byte's time, length, the
	 * bytes read so far into got[] */
	const uint8_t *fbuf;
	uint64_t fstart;
	uint32_t fn, fread;
	bool cs_low;
	unsigned misuse;
	/* TE: edges at te0 + k * te_period from te0 on */
	uint64_t te0, te_period;
	unsigned frames;
};

static void ev(struct sim *s, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static void ev(struct sim *s, const char *fmt, ...)
{
	va_list ap;

	fprintf(s->f, "%" PRIu64 "\t", s->t);
	va_start(ap, fmt);
	vfprintf(s->f, fmt, ap);
	va_end(ap);
	fputc('\n', s->f);
}

static uint8_t got[S13C_FRAME_BYTES];

/* The frame's bytes as RUN events of equal pixels, each timed. */
static void frame_emit(struct sim *s)
{
	uint64_t t = s->fstart;
	uint32_t i = 0;

	while (i + 1u < s->fn) {
		uint32_t k = 1;

		while (i + 2u * k + 1u < s->fn && got[i + 2u * k] == got[i] &&
		       got[i + 2u * k + 1u] == got[i + 1u])
			k++;
		fprintf(s->f, "%" PRIu64 "\tRUN\t%" PRIu32 "\t%02X%02X\tend=%" PRIu64
			"\n", t, k, got[i], got[i + 1u], t + (2u * k - 1u) * BYTE_NS);
		t += 2u * k * BYTE_NS;
		i += 2u * k;
	}
}

/* The bytes of the frame going out that the transfer has read by now (all
 * of them with all), from the buffer as it is now. */
static void take(struct sim *s, bool all)
{
	uint64_t n;

	if (s->fbuf == NULL)
		return;
	n = all ? s->fn : s->t < s->fstart ? 0u : (s->t - s->fstart) / BYTE_NS + 1u;
	if (n > s->fn)
		n = s->fn;
	if (n > s->fread) {
		memcpy(got + s->fread, s->fbuf + s->fread, (size_t)n - s->fread);
		s->fread = (uint32_t)n;
	}
	if (s->fread == s->fn) {
		frame_emit(s);
		s->fbuf = NULL;
	}
}

static void busy_check(struct sim *s)
{
	if (s->t < s->frame_end)
		s->misuse++;
}

static void hexbytes(FILE *f, const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		fprintf(f, "%02X", p[i]);
}

/* ---- the bus ---- */

static void b_reset(void *ctx, bool high)
{
	struct sim *s = ctx;

	take(s, false);
	ev(s, "RESET\t%d", high ? 1 : 0);
	s->t += GPIO_NS;
}

static void b_select(void *ctx, bool on)
{
	struct sim *s = ctx;

	take(s, false);
	busy_check(s);
	ev(s, "CS\t%d", on ? 0 : 1);
	s->cs_low = on;
	s->t += 200u;
}

static void b_cmd(void *ctx, uint8_t c)
{
	struct sim *s = ctx;

	take(s, false);
	busy_check(s);
	s->t += 100u;		/* D/C low */
	ev(s, "%s\t%02X", s->cs_low ? "CMD" : "CMD!", c);
	s->t += BYTE_NS + 100u;
}

static void b_data(void *ctx, const uint8_t *p, size_t n)
{
	struct sim *s = ctx;

	take(s, false);
	busy_check(s);
	fprintf(s->f, "%" PRIu64 "\t%s\t", s->t, s->cs_low ? "DATA" : "DATA!");
	hexbytes(s->f, p, n);
	fprintf(s->f, "\tend=%" PRIu64 "\n", s->t + (n - 1u) * BYTE_NS);
	s->t += n * BYTE_NS;
}

static void b_repeat(void *ctx, const uint8_t *g, size_t len, uint32_t count)
{
	struct sim *s = ctx;
	uint64_t n = (uint64_t)len * count;

	take(s, false);
	busy_check(s);
	fprintf(s->f, "%" PRIu64 "\tRUN\t%" PRIu32 "\t", s->t, count);
	hexbytes(s->f, g, len);
	fprintf(s->f, "\tend=%" PRIu64 "\n", s->t + (n - 1u) * BYTE_NS);
	s->t += n * BYTE_NS;
}

/* S-13c.011: an identification responder (tests/oracle/conformance/hw
 * helpers' IdResponder values). */
static int b_read(void *ctx, uint8_t cmd, uint8_t *out, size_t n)
{
	static const uint8_t id[4] = { 0x00, 0x77, 0x96, 0x00 };
	struct sim *s = ctx;
	size_t i;

	take(s, false);
	if (s->cs_low)
		b_select(s, false);
	b_select(s, true);
	b_cmd(s, cmd);
	for (i = 0; i < n; i++) {
		out[i] = i < 4u ? id[i] : 0u;
		ev(s, "READ\t%02X", out[i]);
		s->t += 2u * US;
	}
	b_select(s, false);
	return 0;
}

/* The frame's bytes, read from buf as the transfer reaches them. */
static int b_frame_start(void *ctx, const uint8_t *buf, uint32_t n)
{
	struct sim *s = ctx;

	take(s, false);
	busy_check(s);
	if (n > sizeof got)
		return -1;
	s->fbuf = buf;
	s->fstart = s->t;
	s->fn = n;
	s->fread = 0;
	s->frame_end = s->t + (uint64_t)n * BYTE_NS;
	s->t += 500u;			/* its set-up; the DMA runs in the background */
	s->frames++;
	take(s, false);
	return 0;
}

static bool b_frame_busy(void *ctx)
{
	struct sim *s = ctx;

	take(s, false);
	return s->t < s->frame_end;
}

static int b_te_wait(void *ctx, uint32_t timeout_us)
{
	struct sim *s = ctx;
	uint64_t next, limit = s->t + (uint64_t)timeout_us * US;

	take(s, false);
	if (s->te_period == 0 || s->te0 == 0) {
		s->t = limit;
		take(s, false);
		return 1;
	}
	if (s->t < s->te0)
		next = s->te0;
	else
		next = s->te0 + ((s->t - s->te0) / s->te_period + 1u) * s->te_period;
	if (next > limit) {
		s->t = limit;
		take(s, false);
		return 1;
	}
	s->t = next + 2u * US;		/* the poll's latency */
	take(s, false);
	return 0;
}

static uint64_t b_now_us(void *ctx)
{
	struct sim *s = ctx;

	take(s, false);
	return s->t / US;
}

static void b_delay_us(void *ctx, uint32_t us)
{
	struct sim *s = ctx;

	take(s, false);
	s->t += (uint64_t)us * US;
	take(s, false);
}

static const struct s13c_bus bus_ops = {
	b_reset, b_select, b_cmd, b_data, b_repeat, b_read, b_frame_start,
	b_frame_busy, NULL, b_te_wait, b_now_us, b_delay_us, NULL,
};

/* ---- images ---- */

/* S-13c.015's pattern (vectors/frame_pattern.json). */
static void pattern(uint8_t *img)
{
	unsigned r, c;

	for (r = 0; r < S13C_ROWS; r++)
		for (c = 0; c < S13C_FRAME_W; c++) {
			unsigned v = (r * 0x0133u + c * 0x2B5Du + ((r ^ c) & 0xFFu)) & 0xFFFFu;
			size_t i = 2u * ((size_t)r * S13C_FRAME_W + c);

			if (r == 0 && c == 0)
				v = 0xF800u;
			else if (r == 0 && c == S13C_FRAME_W - 1u)
				v = 0x07E0u;
			else if (r == S13C_ROWS - 1u && c == 0)
				v = 0x001Fu;
			else if (r == S13C_ROWS - 1u && c == S13C_FRAME_W - 1u)
				v = 0xFFFFu;
			img[i] = (uint8_t)(v >> 8);
			img[i + 1u] = (uint8_t)v;
		}
}

/* S-13c.023's mark, an F, in screen positions (vectors/orientation.json). */
static void orient_mark(const struct s15_draw_ops *o)
{
	o->fill(o->ctx, 0, 0, WB_DISPLAY_W, WB_DISPLAY_H, S15_BLACK);
	o->fill(o->ctx, 10, 10, 1, 40, S15_WHITE);
	o->fill(o->ctx, 10, 10, 30, 1, S15_WHITE);
	o->fill(o->ctx, 10, 30, 20, 1, S15_WHITE);
}

/* ---- a run ---- */

struct opts {
	unsigned long hs, strap, te_us, frames, pattern, orient, fatal,
		poweroff, regreet, option, status, slot, prompt, image;
	unsigned long cont, line;
	unsigned long xmod, ymod;
	bool xmod_set, ymod_set;
};

static bool parse(const struct h_cmd *c, int from, struct opts *o)
{
	int i;

	memset(o, 0, sizeof *o);
	o->hs = 1;
	o->strap = 1;
	o->te_us = 16667;
	o->frames = 4;
	o->slot = S15_SLOT_UNKNOWN;
	for (i = from; i < c->n; i++) {
		const char *k = c->tok[i], *eq = strchr(k, '=');
		unsigned long v;
		size_t kl;

		if (eq == NULL)
			return false;
		v = strtoul(eq + 1, NULL, 0);
		kl = (size_t)(eq - k);
#define KEY(name) (kl == sizeof(name) - 1u && strncmp(k, name, kl) == 0)
		if (KEY("hs"))
			o->hs = v;
		else if (KEY("strap"))
			o->strap = v;
		else if (KEY("te_us"))
			o->te_us = v;
		else if (KEY("frames"))
			o->frames = v;
		else if (KEY("pattern"))
			o->pattern = v;
		else if (KEY("orient"))
			o->orient = v;
		else if (KEY("fatal"))
			o->fatal = v;
		else if (KEY("poweroff"))
			o->poweroff = v;
		else if (KEY("regreet"))
			o->regreet = v;
		else if (KEY("prompt"))
			o->prompt = v;
		else if (KEY("cont"))
			o->cont = v;
		else if (KEY("line"))
			o->line = v;
		else if (KEY("image"))
			o->image = v;
		else if (KEY("option"))
			o->option = v;
		else if (KEY("status"))
			o->status = v;
		else if (KEY("slot"))
			o->slot = v;
		else if (KEY("xmod")) {
			o->xmod = v;
			o->xmod_set = true;
		} else if (KEY("ymod")) {
			o->ymod = v;
			o->ymod_set = true;
		} else
			return false;
#undef KEY
	}
	return true;
}

static uint8_t fb[S13C_FRAME_BYTES];
static struct s15_console con;

static void line_text(unsigned k, char *out)
{
	unsigned i;

	for (i = 0; i < S15_COLS; i++)
		out[i] = (char)(0x20u + (k * 13u + i) % 95u);
	out[S15_COLS] = '\0';
}

/* The frame of a held-mode screen, or of a fatal error. */
static void frame_of(const struct opts *o, bool fatal, struct s15_frame *f)
{
	f->title = fatal ? "Fatal Error" : "Main Menu";
	f->version = "1.0.0";
	f->status = (uint8_t)o->status;
	f->slot = (uint8_t)o->slot;
	f->option_on = o->option != 0;
	f->fatal = fatal;
	f->gray = false;
}

static void show(struct wb_display *d, const struct opts *o, bool fatal)
{
	struct s15_frame f;

	frame_of(o, fatal, &f);
	s15_render_console(&con, &wb_display_geometry, wb_display_draw(d), &f);
}

/* S-05.025, S-15.180: the boot prompt's frame after the early logo, under
 * the title Boot, lit by the early logo's message (S-13c.019); the master
 * then waits for a digit, here 3 s. */
static void prompt(struct sim *s, struct wb_display *d, const struct opts *o)
{
	static const struct s15_prompt_values v = { 1, 1, 0x01, true, 0x080000E8u, false };
	struct s15_buf lines[S15_PROMPT_LINES];
	enum s15_style st[S15_PROMPT_LINES];
	struct s15_frame f;
	unsigned i;

	s->t += 1u * MS;
	ev(s, "MARK\tprompt");
	s->t += 100u * US;
	/* the console cleared under the title Boot, as s15_prompt_show() */
	s15_prompt_lines(&v, lines, st);
	s15_console_clear(&con);
	for (i = 0; i < S15_PROMPT_LINES; i++)
		s15_console_add(&con, st[i], lines[i].s);
	frame_of(o, false, &f);
	f.title = s15_prompt_title;
	s15_render_console(&con, &wb_display_geometry, wb_display_draw(d), &f);
	(void)wb_display_update(d);
	while (wb_display_busy(d))
		s->t += 10u * US;
	s->t += 3000u * MS;
	s15_console_clear(&con);		/* held mode's screen replaces it */
}

/* S-13c.022: the power-off starts (MARK poweroff); 0x51 00 00 02 once the
 * panel's power-off has returned, after the frame in progress. */
static void power_off(struct sim *s, struct wb_display *d)
{
	ev(s, "MARK\tpoweroff");
	(void)wb_display_poweroff(d);
	s->t += 1u * MS;
	ev(s, "MSG51\t00 00 02");
}

static void wait_frame(struct sim *s, struct wb_display *d)
{
	while (wb_display_busy(d))
		s->t += 10u * US;
}

/* Scenario D-IMAGE (S-13c.024): see image=1 above. */
static void image(struct sim *s, struct wb_display *d, const struct opts *o)
{
	const uint64_t gap = o->te_us != 0u ? 4u * (uint64_t)o->te_us * US + 1u * MS
					    : 250u * MS;
	char line[S15_COLS + 1u];
	unsigned k, n = 0;

	for (k = 0; k < S15_ROWS + 3u; k++) {
		line_text(n++, line);
		s15_console_append(&con, line);
	}
	show(d, o, false);
	(void)wb_display_update(d);
	wait_frame(s, d);
	for (k = 0; k < 16u; k++) {
		uint64_t at;

		(void)wb_display_flush(d);	/* the screen as it stands */
		at = s->fstart + (uint64_t)k * MS;
		if (s->t < at)
			s->t = at;
		ev(s, "CHANGE");
		line_text(n++, line);
		s15_console_append(&con, line);
		show(d, o, false);
		(void)wb_display_update(d);	/* the new screen */
		wait_frame(s, d);
		(void)wb_display_flush(d);	/* the settled frame */
		wait_frame(s, d);
		if (s->t < at + gap)
			s->t = at + gap;
	}
}

static void held(struct sim *s, struct wb_display *d, const struct opts *o)
{
	char line[S15_COLS + 1u];
	unsigned k;

	/* S-04.064 / S-07: held mode starts after DISPON. */
	ev(s, "MSG51\t3C FF 02");
	ev(s, "MARK\theld");
	s->te0 = s->t + 3u * MS;
	s->te_period = (uint64_t)o->te_us * US;
	if (o->image) {
		image(s, d, o);
		return;
	}
	for (k = 0; k < o->frames; k++) {
		line_text(k, line);
		s15_console_append(&con, line);
		show(d, o, false);
		if (o->pattern && k + 1u == o->frames) {
			while (wb_display_busy(d))
				s->t += 10u * US;
			pattern(d->fb);
		}
		if (o->orient && k + 1u == o->frames)
			orient_mark(wb_display_draw(d));
		(void)wb_display_update(d);
		if (o->fatal && k == 1u) {
			/* The fatal-error frame at once. */
			s->t += 5u * MS;
			ev(s, "MARK\tfatal");
			s15_console_append(&con, "fatal: test");
			show(d, o, true);
			(void)wb_display_flush_fatal(d);
			return;
		}
		/* the menus change again 100 ms later at the latest */
		s->t += 3u * MS;
	}
	if (o->poweroff == 1u) {
		power_off(s, d);		/* the last frame still going out */
	} else if (o->poweroff == 2u) {
		while (wb_display_busy(d))
			s->t += 10u * US;
		s->t += 2u * MS;		/* between two frames */
		power_off(s, d);
	} else {
		while (wb_display_busy(d))
			s->t += 10u * US;
	}
}

/* A view of its own: the console cleared, the lines, under the title. */
static void view(struct sim *s, struct wb_display *d, const struct opts *o, const char *title,
		 const enum s15_style *st, const char *const *lines, unsigned n)
{
	struct s15_frame f;
	unsigned i;

	s15_console_clear(&con);
	for (i = 0; i < n; i++)
		s15_console_add(&con, st[i], lines[i]);
	frame_of(o, false, &f);
	f.title = title;
	s15_render_console(&con, &wb_display_geometry, wb_display_draw(d), &f);
	(void)wb_display_update(d);
	(void)wb_display_wait(d);
	if (s->t < s->frame_end)
		s->t = s->frame_end;
}

/* S-05.033, S-15.190: Continue to the OS from held mode (cont=1): the
 * menu's restarting view (s15_menu_continue()), then the board's continue_os
 * (src/board/opxy_services.c): the view shown, held 750 ms, then the
 * restart, whose shutdown starts with the panel's power-off (S-13c.022). */
static void continue_os(struct sim *s, struct wb_display *d, const struct opts *o)
{
	static const enum s15_style st[] = { S15_ST_WARN };
	const char *lines[1];

	s->t += 5u * MS;
	lines[0] = s15_cont_restarting;
	view(s, d, o, s15_cont_title, st, lines, 1);
	s->t += 750u * MS;
	power_off(s, d);
}

/* The TE edges of the panel's own clock, from te0 to the end of the run,
 * appended (the checker sorts nothing, so they are merged by time). */
struct line {
	uint64_t t;
	size_t seq;
	char *text;
};

struct lines {
	struct line *v;
	size_t n, cap;
};

static bool push(struct lines *l, uint64_t t, const char *text)
{
	size_t n;

	if (l->n == l->cap) {
		size_t cap = l->cap ? 2u * l->cap : 4096u;
		struct line *v = realloc(l->v, cap * sizeof *v);

		if (v == NULL)
			return false;
		l->v = v;
		l->cap = cap;
	}
	n = strlen(text) + 1u;		/* strdup is not C11's */
	l->v[l->n].text = malloc(n);
	if (l->v[l->n].text == NULL)
		return false;
	memcpy(l->v[l->n].text, text, n);
	l->v[l->n].t = t;
	l->v[l->n].seq = l->n;
	l->n++;
	return true;
}

static void lines_free(struct lines *l)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		free(l->v[i].text);
	free(l->v);
}

static int cmp_line(const void *a, const void *b)
{
	const struct line *x = a, *y = b;

	if (x->t != y->t)
		return x->t < y->t ? -1 : 1;
	return x->seq < y->seq ? -1 : 1;
}

static bool merge_te(const char *path, uint64_t te0, uint64_t period,
		     uint64_t end)
{
	struct lines l = { NULL, 0, 0 };
	static char buf[H_MAXLINE];
	bool ok = true;
	uint64_t t;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return false;
	while (ok && fgets(buf, sizeof buf, f))
		ok = push(&l, strtoull(buf, NULL, 10), buf);
	fclose(f);
	for (t = te0; ok && period != 0 && te0 != 0 && t <= end; t += period) {
		char te[48];

		snprintf(te, sizeof te, "%" PRIu64 "\tTE\t1\n", t);
		ok = push(&l, t, te);
	}
	if (ok) {
		/* lines of one time keep their order */
		qsort(l.v, l.n, sizeof *l.v, cmp_line);
		size_t i;

		f = fopen(path, "w");
		ok = f != NULL;
		for (i = 0; ok && i < l.n; i++)
			ok = fputs(l.v[i].text, f) >= 0;
		if (f != NULL && fclose(f) != 0)
			ok = false;
	}
	lines_free(&l);
	return ok;
}
static void run(const struct h_cmd *c)
{
	const char *path = h_str(c, 1), *out = h_str(c, 2);
	struct s13c_bus bus = bus_ops;
	struct wb_display d;
	struct sim s;
	struct opts o;
	uint32_t xmod, ymod;
	bool boot, fail;
	char buf[48];

	if (path == NULL || out == NULL || !parse(c, 3, &o)) {
		h_error(c, "usage: run PATH OUT [KEY=VALUE ...]");
		return;
	}
	boot = strcmp(path, "boot") == 0;
	fail = strcmp(path, "fail") == 0;
	if (!boot && !fail && strcmp(path, "held") != 0) {
		h_error(c, "PATH is boot, held or fail");
		return;
	}
	memset(&s, 0, sizeof s);
	s.f = fopen(out, "w");
	if (s.f == NULL) {
		h_error(c, "cannot write OUT");
		return;
	}
	bus.ctx = &s;
	fprintf(s.f, "# S-13c trace of src/s13c_panel.c (h_s13c run %s)\n", path);
	wb_display_init(&d, fb);
	s15_console_init(&con, NULL);

	/* The first stage (S-01.006): its pulse at 2 ms, the handshake. */
	s.t = 2u * MS;
	if (o.hs) {
		ev(&s, "RESET\t0");
		s.t += 10u * US + 500u;
		ev(&s, "RESET\t1");
	}
	xmod = o.xmod_set ? (uint32_t)o.xmod : o.hs ? S13C_HANDSHAKE_MAGIC : 0u;
	ymod = o.ymod_set ? (uint32_t)o.ymod : o.hs ? 2u : 0u;
	s.t = 40u * MS;
	ev(&s, "MARK\tbootloader");
	s.t += 100u * US;
	/* S-13b.001: PA8 an input; the RESET level follows the strap. */
	ev(&s, "MARK\tstrap");
	if (!o.strap) {
		s.t += 1u * US;
		ev(&s, "RESET\t0");
	}
	s.t = 50u * MS;			/* the display set-up */
	wb_display_begin(&d, &bus, xmod, ymod, (unsigned)o.strap);
	/* S-07.066: the start-up report, before the first panel command. */
	s.t += 50u * US;
	ev(&s, "MSG51\t00 00 00");
	if (o.regreet) {
		s.t += 1u * MS;
		ev(&s, "GREETING");
		s.t += 200u * US;
		ev(&s, "MSG51\t3C FF 02");
	}
	s.t += 30u * MS;		/* the battery wait and key status */
	if (o.poweroff == 4u) {
		power_off(&s, &d);	/* before the first frame */
		goto end;
	}
	/* S-05.004: the early logo on every path, with
	 * the run's one initialization and the gamma load, lit after its
	 * last byte; then the slave's stream and start-up. */
	(void)wb_display_start(&d, true);
	(void)wb_display_boot_logo(&d, NULL);
	s.t += 100u * US;
	ev(&s, "MSG51\t3C FF 02");
	if (o.poweroff == 3u) {
		s.t += 50u * MS;	/* the early logo lit, before held mode */
		power_off(&s, &d);
		goto end;
	}
	s.t += 500u * MS;
	if (o.prompt)
		prompt(&s, &d, &o);
	if (boot || fail) {
		(void)wb_display_boot_logo(&d, NULL);
		s.t += 100u * US;
		ev(&s, "MSG51\t3C FF 32");
		s.t += 10u * MS;
		if (boot) {
			uint8_t rec[S13C_RECORD_SIZE];
			int rc = wb_display_handover(&d, rec);
			size_t i;

			fprintf(s.f, "%" PRIu64 "\tHANDOVER\tnow_ms=%" PRIu64 "\trecord=",
				s.t, s.t / MS);
			for (i = 0; i < sizeof rec; i++)
				fprintf(s.f, "%02X", rc == 0 ? rec[i] : 0u);
			fputc('\n', s.f);
		} else {
			s.t += 200u * MS;	/* the load fails (S-05.006) */
		}
	}
	if (!boot) {
		s.t += 20u * MS;
		if (o.line) {
			/* S-13c.025: the line under the logo at held mode's
			 * entry, then the mount and the rest before the first
			 * frame. */
			(void)wb_display_logo_line(&d, "Loading boot menu...");
			s.t += 4000u * MS;
		}
		held(&s, &d, &o);
		if (o.cont)
			continue_os(&s, &d, &o);
	}
end:
	take(&s, true);			/* a frame still going out */
	if (fclose(s.f) != 0 || !merge_te(out, s.te0, s.te_period,
					  s.t + 20u * MS)) {
		h_error(c, "cannot write OUT");
		return;
	}
	if (s.misuse) {
		h_error(c, "bus used while a frame was sent");
		return;
	}
	h_put("ok");
	snprintf(buf, sizeof buf, "%u", s.frames);
	h_put(buf);
	h_putu(d.panel.te_misses);
	h_end();
}

static void font(void)
{
	const struct s15_geometry *g = &wb_display_geometry;
	static uint8_t title[WB_FONT_COUNT * WB_TITLE_H * 2u];

	h_put("ok");
	h_putu(WB_FONT_H);
	h_putu(WB_FONT_W);
	h_putu(g->pitch);
	h_putu(g->margin);
	h_putu(g->top);
	h_putu(g->title_y);
	h_putu(s15_status_text_y(g));
	h_puthex(&wb_display_font[0][0], sizeof wb_display_font);
	h_putu(WB_TITLE_H);
	h_putu(WB_TITLE_W);
	for (size_t c = 0, n = 0; c < WB_FONT_COUNT; c++)
		for (size_t r = 0; r < WB_TITLE_H; r++, n += 2u) {
			title[n] = (uint8_t)(wb_display_title_font[c][r] >> 8);
			title[n + 1u] = (uint8_t)wb_display_title_font[c][r];
		}
	h_puthex(title, sizeof title);
	h_end();
}

int main(void)
{
	struct h_cmd c;

	while (h_next(&c)) {
		if (strcmp(c.tok[0], "run") == 0)
			run(&c);
		else if (strcmp(c.tok[0], "font") == 0)
			font();
		else
			h_error(&c, "unknown command");
	}
	return h_status();
}
