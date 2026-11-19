/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for the front's panel (src/front/fr_panel.c) on its way into
 * recovery: the whole of fr_start_run() and then fr_recovery_run(), over a
 * model of the panel bus with its TE line and memory DMA, a model PSoC on
 * the link, a flash and the loaded image.
 *
 * Commands (one result line each):
 *   run WAY OUT [KEY=VALUE ...]  -> ok SENDS STOPS TE_MISSES
 *
 * runs one start that goes into recovery and writes its panel-bus trace
 * (check_trace.py's format) to the file OUT. WAY is the way in (S-02.084):
 * chord (the key status reads key 4 alone), check (the loaded image's CRC
 * is wrong) or count (three starts that went on to the main image and did
 * not finish come first, untraced, with no panel). SENDS counts the DMA
 * sends from MARK recovery on, STOPS the sends stopped while going out.
 * Keys:
 *   te_us=N       the TE period (0: no TE edges); 16667. The edges start
 *                 1 ms after TEON
 *   te_stop=A te_back=B  no TE edge from A ms to B ms after MARK recovery
 *   frames=N      recovery's screens: its first, then N-1 changes, each a
 *                 key read (key 1 on the main screen, Back on the slot
 *                 menu, a reading with no key between); 4
 *   stall=K       the K-th DMA send from MARK recovery on stops taking
 *                 bytes after 50000 and never ends
 *   slow=K        the K-th send goes at 400 ns a byte
 *   off=K         the PSoC's power-off (0x31 {14}) 10 ms after the K-th
 *                 send's first byte; MARK poweroff when the front has read
 *                 it, and the run ends when recovery returns
 *   end=N         without a power-off, the run ends N ms after MARK
 *                 recovery, once no send is going out (at most 200 ms
 *                 later; a stalled send is then traced as far as it got);
 *                 600
 *
 * The model stands for what is not the front's: the first stage's pulse
 * and handshake (2 ms, DMA9_XMOD 0x12345678, DMA9_YMOD 2), MARK bootloader
 * at the front's entry and MARK strap (PA8 high), MARK recovery_<way> at
 * the line's CASET (the front decides just before it) and MARK
 * recovery when fr_start_run() returns. Each 0x51 the front sends is a
 * MSG51 event. The clock moves 1 us per look; a bus write cycle is 70 ns,
 * a DMA send's byte 70 ns from its start in the background, read from its
 * buffer as the transfer reaches it, so a buffer written while it goes
 * out shows as the panel would receive it. A command, a parameter byte or
 * a send started while a send goes out fails the command.
 */
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hlib.h"
#include "front/fr_handoff.h"
#include "front/fr_start.h"
#include "waffle/s06_frame.h"
#include "waffle/s06_rx.h"

#define US 1000u
#define MS 1000000u
#define BYTE_NS 70u
#define SLOW_NS 400u
#define TE_HIGH_NS (200u * US)
#define STALL_BYTES 50000u
#define FLASH_SIZE 0x100000u

/* ---- the trace: lines kept with their time, sorted at the end ---- */

struct line {
	uint64_t t;
	size_t seq;
	char *text;
};

static struct {
	struct line *v;
	size_t n, cap;
	bool on;		/* traced; the count way's earlier starts are not */
	bool oom;
} tr;

static void push(uint64_t t, const char *text)
{
	size_t n;

	if (!tr.on || tr.oom)
		return;
	if (tr.n == tr.cap) {
		size_t cap = tr.cap ? 2u * tr.cap : 65536u;
		struct line *v = realloc(tr.v, cap * sizeof *v);

		if (v == NULL) {
			tr.oom = true;
			return;
		}
		tr.v = v;
		tr.cap = cap;
	}
	n = strlen(text) + 1u;
	tr.v[tr.n].text = malloc(n);
	if (tr.v[tr.n].text == NULL) {
		tr.oom = true;
		return;
	}
	memcpy(tr.v[tr.n].text, text, n);
	tr.v[tr.n].t = t;
	tr.v[tr.n].seq = tr.n;
	tr.n++;
}

static void evf(uint64_t t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void evf(uint64_t t, const char *fmt, ...)
{
	static char buf[2048];
	int k;
	va_list ap;

	k = snprintf(buf, sizeof buf, "%" PRIu64 "\t", t);
	va_start(ap, fmt);
	vsnprintf(buf + k, sizeof buf - (size_t)k, fmt, ap);
	va_end(ap);
	push(t, buf);
}

static void trace_free(void)
{
	size_t i;

	for (i = 0; i < tr.n; i++)
		free(tr.v[i].text);
	free(tr.v);
	memset(&tr, 0, sizeof tr);
}

static int cmp_line(const void *a, const void *b)
{
	const struct line *x = a, *y = b;

	if (x->t != y->t)
		return x->t < y->t ? -1 : 1;
	return x->seq < y->seq ? -1 : 1;
}

/* Bytes from t0, byte_ns apart: RUN events of equal pixels, or DATA for a
 * short or odd run. */
static void emit_bytes(uint64_t t0, const uint8_t *p, size_t n, uint64_t byte_ns, bool cs_low)
{
	size_t i = 0;

	if (n == 0u)
		return;
	if (n < 64u || n % 2u != 0u) {
		char *buf = malloc(2u * n + 1u);

		if (buf == NULL) {
			tr.oom = true;
			return;
		}
		for (size_t j = 0; j < n; j++)
			snprintf(buf + 2u * j, 3, "%02X", p[j]);
		if (tr.on && !tr.oom) {
			size_t len = 2u * n + 96u;
			char *line = malloc(len);

			if (line == NULL) {
				tr.oom = true;
			} else {
				snprintf(line, len, "%" PRIu64 "\t%s\t%s\tend=%" PRIu64, t0,
					 cs_low ? "DATA" : "DATA!", buf, t0 + (n - 1u) * byte_ns);
				push(t0, line);
				free(line);
			}
		}
		free(buf);
		return;
	}
	while (i + 1u < n) {
		size_t k = 1;
		uint64_t t = t0 + i * byte_ns;

		while (i + 2u * k + 1u < n && p[i + 2u * k] == p[i] && p[i + 2u * k + 1u] == p[i + 1u])
			k++;
		evf(t, "%s\t%zu\t%02X%02X\tend=%" PRIu64, cs_low ? "RUN" : "RUN!", k, p[i], p[i + 1u],
		    t + (2u * k - 1u) * byte_ns);
		i += 2u * k;
	}
}

/* ---- the run's state ---- */

struct opts {
	unsigned long te_us, te_stop, te_back, frames, stall, slow, off, end;
	bool te_gap;
};

static struct sim {
	struct opts o;
	uint64_t t;		/* ns */
	bool cs_low;
	unsigned misuse;
	/* the DMA send going out: buffer, first byte, length, bytes read
	 * into got[], ns a byte, the bytes it will take (a stall's fewer) */
	const uint8_t *fbuf;
	uint64_t fstart, fns;
	uint32_t fn, fread, flimit;
	/* sends and stops from MARK recovery on */
	unsigned sends, stops;
	/* TE: edges te0 + k * te_period from te0 on (0: none yet) */
	uint64_t te0, te_period;
	uint64_t rec_ns;	/* MARK recovery, 0 before */
	uint64_t end_ns;	/* the run ends here, 0 never */
	bool line_marked;
	uint8_t cmd;		/* the last command and its time */
	uint64_t cmd_t;
	const char *way;
	jmp_buf done;
	bool can_jump;
} s;

static uint8_t got[FR_PANEL_FRAME_BYTES];

static void psoc_off_at(uint64_t t);

static void busy_check(void)
{
	if (s.fbuf != NULL)
		s.misuse++;
}

/* The send's bytes read by now (all of them when it ends), from its buffer
 * as it is now; a send that has taken all it will is emitted. */
static void take(void)
{
	uint64_t n;

	if (s.fbuf == NULL)
		return;
	n = s.t < s.fstart ? 0u : (s.t - s.fstart) / s.fns + 1u;
	if (n > s.flimit)
		n = s.flimit;
	if (n > s.fread) {
		memcpy(got + s.fread, s.fbuf + s.fread, (size_t)n - s.fread);
		s.fread = (uint32_t)n;
	}
	if (s.fread == s.fn) {
		emit_bytes(s.fstart, got, s.fn, s.fns, s.cs_low);
		s.fbuf = NULL;
	}
}

static void end_check(void)
{
	/* a send that never ends does not hold the end back more than 200 ms */
	if (s.can_jump && s.end_ns != 0u && s.t >= s.end_ns &&
	    (s.fbuf == NULL || s.t >= s.end_ns + 200u * MS))
		longjmp(s.done, 1);
}

/* ---- the clock ---- */

static uint64_t c_now_us(void *ctx)
{
	(void)ctx;
	s.t += 1u * US;
	take();
	end_check();
	return s.t / US;
}

static void c_delay_us(void *ctx, uint32_t us)
{
	(void)ctx;
	s.t += (uint64_t)us * US;
	take();
	end_check();
}

static const struct fr_clock clk = { c_now_us, c_delay_us, NULL };

/* ---- the panel bus ---- */

static bool te_edge(uint64_t e)
{
	if (s.o.te_gap && s.rec_ns != 0u && e >= s.rec_ns + s.o.te_stop * MS &&
	    e < s.rec_ns + s.o.te_back * MS)
		return false;
	return true;
}

static void b_reset(void *ctx, bool high)
{
	(void)ctx;
	take();
	busy_check();
	evf(s.t, "RESET\t%d", high ? 1 : 0);
	s.t += 1u * US;
}

static void b_select(void *ctx, bool on)
{
	(void)ctx;
	take();
	busy_check();
	evf(s.t, "CS\t%d", on ? 0 : 1);
	s.cs_low = on;
	s.t += 200u;
}

static void b_cmd(void *ctx, uint8_t c)
{
	(void)ctx;
	take();
	busy_check();
	s.t += 100u;		/* D/C low */
	s.cmd = c;
	s.cmd_t = s.t;
	evf(s.t, "%s\t%02X", s.cs_low ? "CMD" : "CMD!", c);
	if (c == 0x35u && s.te_period != 0u && s.te0 == 0u)
		s.te0 = s.t + 1u * MS;	/* TEON: the panel's TE output */
	s.t += BYTE_NS + 100u;
}

static void b_data(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	take();
	busy_check();
	/* the front decides on recovery just before its line's CASET */
	if (s.cmd == 0x2Au && n == 4u && p[1] == FR_PANEL_LINE_COL && !s.line_marked) {
		s.line_marked = true;
		evf(s.cmd_t - 50u, "MARK\trecovery_%s", s.way);
	}
	emit_bytes(s.t, p, n, BYTE_NS, s.cs_low);
	s.t += n * BYTE_NS;
}

/* The TE line's level: high for TE_HIGH_NS from each edge. */
static bool b_te(void *ctx)
{
	uint64_t k, e;

	(void)ctx;
	take();
	if (s.te_period == 0u || s.te0 == 0u || s.t < s.te0)
		return false;
	k = (s.t - s.te0) / s.te_period;
	e = s.te0 + k * s.te_period;
	return s.t - e < TE_HIGH_NS && te_edge(e);
}

static void b_dma_start(void *ctx, const uint8_t *p, uint32_t n)
{
	unsigned k;

	(void)ctx;
	take();
	busy_check();
	if (n > sizeof got) {
		s.misuse++;
		return;
	}
	k = s.rec_ns != 0u ? ++s.sends : 0u;
	s.fbuf = p;
	s.fstart = s.t;
	s.fn = n;
	s.fread = 0;
	s.fns = k != 0u && k == s.o.slow ? SLOW_NS : BYTE_NS;
	s.flimit = k != 0u && k == s.o.stall && n > STALL_BYTES ? STALL_BYTES : n;
	if (k != 0u && k == s.o.off)
		psoc_off_at(s.fstart + 10u * MS);
	s.t += 500u;		/* its set-up; it runs in the background */
	take();
}

static int b_dma_state(void *ctx)
{
	(void)ctx;
	take();
	return s.fbuf != NULL ? FR_DMA_BUSY : FR_DMA_IDLE;
}

/* Stopped where it is: the bytes it took are what the panel received. */
static void b_dma_stop(void *ctx)
{
	(void)ctx;
	take();
	if (s.fbuf != NULL) {
		emit_bytes(s.fstart, got, s.fread & ~1u, s.fns, s.cs_low);
		s.fbuf = NULL;
		if (s.rec_ns != 0u)
			s.stops++;
	}
	s.t += 1u * US;
}

static const struct fr_panel_bus bus = {
	b_reset, b_select, b_cmd, b_data, b_te, b_dma_start, b_dma_state, b_dma_stop, NULL,
};

/* ---- the PSoC: answers greetings and requests, reads the keys ---- */

#define OUT_MAX 8192u

static struct {
	struct s06_rx rx;
	uint8_t out[OUT_MAX];
	uint64_t out_at[OUT_MAX];
	size_t out_n, out_read;
	uint8_t seq;
	uint8_t keys[FR_KEYS_BYTES];	/* the start's key status */
	unsigned readings;		/* recovery's key readings */
	unsigned changes;		/* the screen changes they made */
	uint64_t off_at;		/* the power-off, 0 none */
	bool off_sent;
	size_t off_end;			/* its message's end in out[], 0 read */
} ps;

static uint8_t crc8(const uint8_t *p, size_t n)
{
	return fr_s06_crc8(p, n);
}

static void emit(uint8_t route, uint8_t id, const uint8_t *pl, size_t n)
{
	uint8_t enc[S06_MAX_WIRE];
	size_t k = s06_frame_encode(route, ps.seq++, id, pl, n, crc8, enc), i;

	if (ps.out_n + k > OUT_MAX && ps.out_read != 0u) {
		memmove(ps.out, ps.out + ps.out_read, ps.out_n - ps.out_read);
		memmove(ps.out_at, ps.out_at + ps.out_read, (ps.out_n - ps.out_read) * sizeof ps.out_at[0]);
		ps.out_n -= ps.out_read;
		ps.out_read = 0;
	}
	for (i = 0; i < k && ps.out_n < OUT_MAX; i++) {
		ps.out[ps.out_n] = enc[i];
		ps.out_at[ps.out_n++] = s.t + 200u * US;
	}
}

static void psoc_off_at(uint64_t t)
{
	ps.off_at = t;
}

/* Recovery's key readings: none, key 1 (main screen to the slot menu),
 * none, Back (to the main screen), and so on until frames-1 changes. */
static void recovery_keys(uint8_t *k)
{
	const unsigned i = ps.readings++;

	memset(k, 0, FR_KEYS_BYTES);
	if (i % 2u == 0u || ps.changes + 1u >= s.o.frames)
		return;
	ps.changes++;
	if (i % 4u == 1u)
		k[0] = 0x40;		/* key 1 (bit 6) */
	else
		k[2] = 0x20;		/* sound 8, Back (bit 21) */
}

static void frame_in(const uint8_t *raw)
{
	const uint8_t route = raw[0], id = raw[2], len = raw[3];
	const uint8_t *pl = raw + S06_HEADER;
	const bool request = s06_src(route) >= S06_ADDR_SLOT0;

	if (id == 0x51u && len == 3u)
		evf(s.t, "MSG51\t%02X %02X %02X", pl[0], pl[1], pl[2]);
	if (id == 0x05u) {
		static const uint8_t ok[1] = { 0x40 };

		emit(0x41u, 0x06u, ok, 1);
	} else if (request && id == 0x30u) {
		static const uint8_t on[1] = { FR_POWER_ON };

		emit(s06_swap_route(route), 0x31u, on, 1);
	} else if (request && id == 0x36u) {
		static const uint8_t batt[4] = { 1, 80, 0x10, 0x0E };

		emit(s06_swap_route(route), 0x37u, batt, sizeof batt);
	} else if (request && id == 0x41u) {
		uint8_t k[FR_KEYS_BYTES];

		if (s.rec_ns != 0u)
			recovery_keys(k);
		else
			memcpy(k, ps.keys, sizeof k);
		emit(s06_swap_route(route), 0x41u, k, sizeof k);
	}
}

static size_t p_write(void *ctx, const uint8_t *buf, size_t n)
{
	size_t i;

	(void)ctx;
	for (i = 0; i < n; i++)
		if (s06_rx_byte(&ps.rx, buf[i], crc8) == S06_RX_DELIVER)
			frame_in(ps.rx.buf);
	return n;
}

static size_t p_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t n = 0;

	(void)ctx;
	if (ps.off_at != 0u && !ps.off_sent && s.t >= ps.off_at) {
		static const uint8_t off[1] = { FR_POWER_OFF };

		ps.off_sent = true;
		emit(0x41u, 0x31u, off, 1);
		ps.off_end = ps.out_n;
	}
	while (n < max && ps.out_read < ps.out_n && ps.out_at[ps.out_read] <= s.t)
		buf[n++] = ps.out[ps.out_read++];
	/* the power-off starts when the front has its message */
	if (ps.off_end != 0u && ps.out_read >= ps.off_end) {
		ps.off_end = 0;
		evf(s.t, "MARK\tpoweroff");
	}
	return n;
}

static bool p_idle(void *ctx)
{
	(void)ctx;
	return true;
}

static const struct fr_bytes link = { p_read, p_write, p_idle, NULL };

/* ---- recovery's serial port and USB: no host ---- */

static size_t u_read(void *ctx, uint8_t *buf, size_t max)
{
	(void)ctx;
	(void)buf;
	(void)max;
	return 0;
}

static size_t u_write(void *ctx, const uint8_t *buf, size_t n)
{
	(void)ctx;
	(void)buf;
	return n;
}

static const struct fr_bytes serial = { u_read, u_write, p_idle, NULL };

static void usb_start(const struct fr_clock *c)
{
	(void)c;
}

static void usb_poll(void)
{
}

static bool usb_configured(void)
{
	return false;
}

static uint32_t usb_ma(void)
{
	return 100u;
}

/* ---- the loaded image and the flash: slot 0 holds the stream the loaded
 * memory came from, slot 1 another, the selector 00 01 ---- */

#define REGION 0x8803D000u

static uint8_t desc[FR_DESC_MAX], region[256];
static uint8_t fmem[FLASH_SIZE];

static const uint8_t *m_map(void *ctx, uint32_t addr, uint32_t len)
{
	(void)ctx;
	if (addr == FR_DESC_ADDR && len <= sizeof desc)
		return desc;
	if (addr == REGION && len <= sizeof region)
		return region;
	return NULL;
}

static const struct fr_mem mem = { m_map, NULL };

/* The descriptor of the one region, its main CRC right or not. */
static void image(bool good)
{
	const size_t n = FR_DESC_HEAD + 8u + 4u;

	memset(region, 0x3C, sizeof region);
	memset(desc, 0, sizeof desc);
	fr_put32(desc + FR_DESC_O_MAGIC, FR_DESC_MAGIC);
	fr_put16(desc + FR_DESC_O_VERSION, FR_DESC_VERSION);
	fr_put16(desc + FR_DESC_O_SIZE, (uint16_t)n);
	fr_put32(desc + FR_DESC_O_MAIN_ENTRY, 0x11A00000u);
	fr_put32(desc + FR_DESC_O_NREGIONS, 1u);
	fr_put32(desc + FR_DESC_O_MAIN_CRC, fr_crc32(0, region, sizeof region) ^ (good ? 0u : 1u));
	fr_put16(desc + FR_DESC_O_BL_VERSION, 1u);
	fr_put32(desc + FR_DESC_O_REGIONS, REGION);
	fr_put32(desc + FR_DESC_O_REGIONS + 4u, sizeof region);
	fr_put32(desc + n - 4u, fr_crc32(0, desc, n - 4u));
}

static size_t ldr_hdr(uint8_t *o, uint16_t flags, uint32_t target, uint32_t count)
{
	uint8_t x = 0;
	unsigned i;

	fr_put32(o, 0xAD000000u | (flags & 0xFFF0u) | 4u);
	fr_put32(o + 4, target);
	fr_put32(o + 8, count);
	fr_put32(o + 12, 0);
	for (i = 0; i < 16u; i++)
		x ^= o[i];
	o[2] = x;
	return 16;
}

/* Slot k: an empty INIT block, the region's 256 bytes of fill, FINAL; its
 * record. */
static void put_stream(unsigned k, uint8_t fill)
{
	uint8_t *p = fmem + FR_SLOT_BASE(k);
	size_t n = 0;

	n += ldr_hdr(p + n, 0x0800u, 0x11A04000u, 0);
	n += ldr_hdr(p + n, 0, REGION, sizeof region);
	memset(p + n, fill, sizeof region);
	n += sizeof region;
	n += ldr_hdr(p + n, 0x8000u, REGION, 0);
	fr_put32(fmem + FR_RECORD_OFF(k), fr_crc32(0, p, n));
	fr_put32(fmem + FR_RECORD_OFF(k) + 4u, (uint32_t)n);
	fr_put32(fmem + FR_RECORD_OFF(k) + 8u, FR_TAG);
}

static void flash_reset(void)
{
	memset(fmem, 0xFF, sizeof fmem);
	fmem[FR_SELECTOR_OFF] = 0x00;
	fmem[FR_SELECTOR_OFF + 1u] = 0x01;
	put_stream(0, 0x3C);
	put_stream(1, 0x3D);
}

static int f_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (off > FLASH_SIZE || len > FLASH_SIZE - off)
		return -1;
	memcpy(buf, fmem + off, len);
	return 0;
}

static int f_erase(void *ctx, uint32_t off)
{
	(void)ctx;
	if (off % FR_SECTOR != 0u || off >= FLASH_SIZE)
		return -1;
	memset(fmem + off, 0xFF, FR_SECTOR);
	return 0;
}

static int f_program(void *ctx, uint32_t off, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	(void)ctx;
	if (n == 0u || off > FLASH_SIZE || n > FLASH_SIZE - off)
		return -1;
	for (i = 0; i < n; i++)
		fmem[off + i] &= b[i];
	return 0;
}

static const struct fr_flash flash = { f_read, f_erase, f_program, NULL };

/* ---- a run ---- */

static uint8_t fb[FR_PANEL_FRAME_BYTES];
static uint8_t staging[FR_SLOT_CAP];
static struct fr_start st;
static struct fr_start_hw hw;

static bool parse(const struct h_cmd *c, int from, struct opts *o)
{
	int i;

	memset(o, 0, sizeof *o);
	o->te_us = 16667;
	o->frames = 4;
	o->end = 600;
	for (i = from; i < c->n; i++) {
		const char *k = c->tok[i], *eq = strchr(k, '=');
		unsigned long v;
		size_t kl;

		if (eq == NULL)
			return false;
		v = strtoul(eq + 1, NULL, 0);
		kl = (size_t)(eq - k);
#define KEY(name) (kl == sizeof(name) - 1u && strncmp(k, name, kl) == 0)
		if (KEY("te_us"))
			o->te_us = v;
		else if (KEY("te_stop")) {
			o->te_stop = v;
			o->te_gap = true;
		} else if (KEY("te_back"))
			o->te_back = v;
		else if (KEY("frames"))
			o->frames = v;
		else if (KEY("stall"))
			o->stall = v;
		else if (KEY("slow"))
			o->slow = v;
		else if (KEY("off"))
			o->off = v;
		else if (KEY("end"))
			o->end = v;
		else
			return false;
#undef KEY
	}
	return o->frames >= 1u;
}

/* One start from the front's entry at 50 ms, the first stage's handshake
 * given; with_panel false leaves the panel out. */
static enum fr_next start(bool with_panel, bool m4, struct fr_handoff *ho)
{
	memset(&hw, 0, sizeof hw);
	hw.clock = &clk;
	hw.psoc = &link;
	hw.panel = with_panel ? &bus : NULL;
	hw.flash = &flash;
	hw.mem = &mem;
	hw.usb_start = usb_start;
	hw.usb_poll = usb_poll;
	hw.usb_configured = usb_configured;
	hw.usb_ma = usb_ma;
	hw.serial = &serial;
	hw.staging = staging;
	hw.frame = fb;
	memset(ho, 0, sizeof *ho);
	ho->strap = 0x0104u;		/* PA8 high */
	ho->dma9_xmod = 0x12345678u;
	ho->dma9_ymod = 2u;
	memset(&ps, 0, sizeof ps);
	s06_rx_init(&ps.rx, S06_ADDR_PSOC);
	if (m4)
		ps.keys[1] = 0x02;	/* key 4 alone (bit 9): Power + M4 */
	s.t = 2u * MS;
	evf(s.t, "RESET\t0");
	s.t += 10u * US + 500u;
	evf(s.t, "RESET\t1");
	s.t = 40u * MS;
	evf(s.t, "MARK\tbootloader");
	s.t += 100u * US;
	evf(s.t, "MARK\tstrap");
	s.t = 50u * MS;
	return fr_start_run(&st, &hw, ho);
}

static bool write_trace(const char *out)
{
	uint64_t e, end = s.t + 20u * MS;
	size_t i;
	FILE *f;
	bool ok;

	for (e = s.te0; s.te_period != 0u && s.te0 != 0u && e <= end; e += s.te_period)
		if (te_edge(e))
			evf(e, "TE\t1");
	if (tr.oom)
		return false;
	qsort(tr.v, tr.n, sizeof *tr.v, cmp_line);
	f = fopen(out, "w");
	if (f == NULL)
		return false;
	ok = fprintf(f, "# S-13c trace of src/front (h_s13cf run %s)\n", s.way) > 0;
	for (i = 0; ok && i < tr.n; i++)
		ok = fputs(tr.v[i].text, f) >= 0 && fputc('\n', f) != EOF;
	return fclose(f) == 0 && ok;
}

static void run(const struct h_cmd *c)
{
	const char *way = h_str(c, 1), *out = h_str(c, 2);
	struct fr_handoff ho;
	enum fr_next next;
	bool chord, check, count;
	unsigned k;
	char buf[32];

	memset(&s, 0, sizeof s);
	if (way == NULL || out == NULL || !parse(c, 3, &s.o)) {
		h_error(c, "usage: run WAY OUT [KEY=VALUE ...]");
		return;
	}
	chord = strcmp(way, "chord") == 0;
	check = strcmp(way, "check") == 0;
	count = strcmp(way, "count") == 0;
	if (!chord && !check && !count) {
		h_error(c, "WAY is chord, check or count");
		return;
	}
	s.way = way;
	s.te_period = (uint64_t)s.o.te_us * US;
	trace_free();
	flash_reset();
	image(!check);
	/* S-02.095: three starts that went on and did not finish */
	for (k = 0; count && k < 3u; k++)
		if (start(false, false, &ho) != FR_NEXT_MAIN) {
			h_error(c, "an earlier start did not go on");
			return;
		}
	memset(&s, 0, sizeof s);
	(void)parse(c, 3, &s.o);
	s.way = way;
	s.te_period = (uint64_t)s.o.te_us * US;
	tr.on = true;
	next = start(true, chord, &ho);
	if (next != FR_NEXT_RECOVERY) {
		h_error(c, "the start did not go into recovery");
		trace_free();
		return;
	}
	s.rec_ns = s.t;
	evf(s.t, "MARK\trecovery");
	if (s.o.off == 0u)
		s.end_ns = s.rec_ns + (uint64_t)s.o.end * MS;
	s.can_jump = true;
	if (setjmp(s.done) == 0)
		(void)fr_recovery_run(&st);
	s.can_jump = false;
	s.t += 1u * US;
	take();
	if (s.fbuf != NULL) {		/* a send still going out ends whole */
		s.t = s.fstart + (uint64_t)s.flimit * s.fns;
		take();
	}
	if (s.fbuf != NULL) {		/* or, stalled, with what it took */
		emit_bytes(s.fstart, got, s.fread & ~1u, s.fns, s.cs_low);
		s.fbuf = NULL;
	}
	if (!write_trace(out)) {
		h_error(c, "cannot write OUT");
		trace_free();
		return;
	}
	trace_free();
	if (s.misuse) {
		h_error(c, "bus used while a send went out");
		return;
	}
	h_put("ok");
	snprintf(buf, sizeof buf, "%u", s.sends);
	h_put(buf);
	h_putu(s.stops);
	h_putu(st.panel.te_misses);
	h_end();
}

int main(void)
{
	struct h_cmd c;

	while (h_next(&c)) {
		if (strcmp(c.tok[0], "run") == 0)
			run(&c);
		else
			h_error(&c, "unknown command");
	}
	return h_status();
}
