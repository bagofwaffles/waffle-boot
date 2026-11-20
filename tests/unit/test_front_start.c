/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front's start-up order (src/front/fr_start.c, S-07.023, S-01.023):
 * the key read is reached within a stated bound with a silent PSoC and
 * with no panel or a dead one; S-07.021's wait ends when the PSoC greets;
 * the messages go out in the new order; Power + M4 and a failed image check
 * lead to recovery, and no other key does anything; the start log's step
 * (S-02.095) comes after the key read and the image check, only on a run
 * that would start the main image, and names its entry in the hand-off
 * record, or after three unfinished starts leads to recovery. A model PSoC on the link
 * answers through libwaffle's own S-06 codec; the flash is a RAM model with
 * an op log. Recovery's loop runs end to end with a host on its USB serial
 * port: the order of the erases, the frames, the replies and the PSoC's
 * messages around an in-place write (S-02.084, S-07.051).
 */
#include <setjmp.h>
#include <string.h>

#include "support/check.h"
#include "fr_map_rule.h"

#include "front/fr_handoff.h"
#include "front/fr_start.h"
#include "front/fr_startlog.h"
#include "waffle/s06_frame.h"
#include "waffle/s06_rx.h"
#include "waffle/wire_crc.h"

/* ---- time: 1 us per look, and whatever a delay asks ---- */

static uint64_t t_us;

static uint64_t now_us(void *ctx)
{
	(void)ctx;
	return ++t_us;
}

static void delay_us(void *ctx, uint32_t us)
{
	(void)ctx;
	t_us += us;
}

static const struct fr_clock clock = { now_us, delay_us, NULL };

/* One order over the PSoC link, the panel, the flash, the image's reads and
 * recovery's serial port. */
static uint64_t g_order;

/* ---- the PSoC model ---- */

#define LOG_MAX 16384u
#define OUT_MAX 4096u

struct sent {
	uint8_t id, len, p0, p1, p2;
	bool request;
	uint64_t order;		/* g_order */
	uint8_t phase;		/* recovery's phase then (*g_phase), or 0xFF */
};

static struct model {
	bool answers;		/* answers greetings and requests */
	uint64_t greet_at;	/* sends 0x05 at this time; 0 never */
	bool greeted_self;
	uint8_t state;		/* the power state it reports */
	uint8_t keys[9];
	bool answer_keys;
	struct s06_rx rx;
	struct sent log[LOG_MAX];
	unsigned n_log;
	bool exit_seen;
	uint8_t last_raw[7];
	/* bytes for the front, each with the time it may be read */
	uint8_t out[OUT_MAX];
	uint64_t out_at[OUT_MAX];
	size_t out_n, out_read;
	uint8_t seq;
	uint64_t keys_at;	/* when the 0x41 request was seen */
	uint64_t off_at;	/* sends 0x31 {14} at this time; 0 never */
	bool off_sent;
	uint64_t off_order;	/* when it was sent, in g_order */
	uint64_t ping_at;	/* sends a ping at this time; 0 never */
	bool ping_sent;
	uint64_t deadline;	/* sends 0x31 {14} after this time; 0 never */
	bool silent_36;		/* answers no 0x36 request */
	bool deadline_sent;	/* the deadline's power-off was sent */
} psoc;

static void flash_reset(uint8_t index, uint8_t verified);

/* Recovery's key readings, scripted: from at_ms after the loop began, each
 * 0x41 request gets bytes 0 to 2 of keys, or no reply. */
struct kstep {
	uint32_t at_ms;
	uint8_t k0, k1, k2;
	bool reply;
};
static const struct kstep *kscript;
static unsigned kscript_n;
static uint64_t kscript_t0;	/* the loop's start; 0 before it */
static const uint8_t *g_phase;	/* recovery's phase while its loop runs */

/* The PSoC, and a flash with selector 00 01, both slots good, no log. */
static void model_reset(void)
{
	memset(&psoc, 0, sizeof psoc);
	s06_rx_init(&psoc.rx, 4u);
	psoc.state = 0x01u;
	flash_reset(0x00, 0x01);
}

static void emit(uint8_t route, uint8_t id, const uint8_t *pl, size_t n)
{
	uint8_t enc[300];
	size_t k = s06_frame_encode(route, psoc.seq++, id, pl, n, wire_crc8_frame, enc), i;

	/* the bytes read are dropped: recovery reads the keys 20 times a
	 * second */
	if (psoc.out_n + k > OUT_MAX && psoc.out_read != 0u) {
		memmove(psoc.out, psoc.out + psoc.out_read, psoc.out_n - psoc.out_read);
		memmove(psoc.out_at, psoc.out_at + psoc.out_read,
			(psoc.out_n - psoc.out_read) * sizeof psoc.out_at[0]);
		psoc.out_n -= psoc.out_read;
		psoc.out_read = 0;
	}
	for (i = 0; i < k && psoc.out_n < OUT_MAX; i++) {
		psoc.out[psoc.out_n] = enc[i];
		psoc.out_at[psoc.out_n++] = t_us + 200u;
	}
}

static void frame_in(const uint8_t *raw, size_t n)
{
	const uint8_t route = raw[0], id = raw[2], len = raw[3];
	const uint8_t *pl = raw + S06_HEADER;
	const bool request = s06_src(route) >= 8u;

	(void)n;
	if (psoc.n_log < LOG_MAX) {
		struct sent *e = &psoc.log[psoc.n_log++];

		e->id = id;
		e->len = len;
		e->p0 = len > 0 ? pl[0] : 0;
		e->p1 = len > 1 ? pl[1] : 0;
		e->p2 = len > 2 ? pl[2] : 0;
		e->request = request;
		e->phase = g_phase != NULL ? *g_phase : 0xFFu;
		e->order = ++g_order;
	}
	if (id == 0x41u)
		psoc.keys_at = t_us;
	if (id == 0x41u && request && kscript != NULL && kscript_t0 != 0u && t_us >= kscript_t0) {
		const struct kstep *k = NULL;

		for (unsigned i = 0; i < kscript_n; i++)
			if (t_us - kscript_t0 >= (uint64_t)kscript[i].at_ms * 1000u)
				k = &kscript[i];
		if (k != NULL) {
			memset(psoc.keys, 0, sizeof psoc.keys);
			psoc.keys[0] = k->k0;
			psoc.keys[1] = k->k1;
			psoc.keys[2] = k->k2;
			psoc.answer_keys = k->reply;
		}
	}
	if (!psoc.answers)
		return;
	if (id == 0x05u) {
		static const uint8_t ok[1] = { 0x40 };

		emit(0x41u, 0x06u, ok, 1);	/* from 4 to 1 */
	} else if (request && id == 0x30u) {
		emit(s06_swap_route(route), 0x31u, &psoc.state, 1);
	} else if (request && id == 0x36u && !psoc.silent_36) {
		static const uint8_t batt[4] = { 1, 80, 0x10, 0x0E };

		emit(s06_swap_route(route), 0x37u, batt, sizeof batt);
	} else if (request && id == 0x41u && psoc.answer_keys) {
		emit(s06_swap_route(route), 0x41u, psoc.keys, sizeof psoc.keys);
	}
}

static size_t p_write(void *ctx, const uint8_t *buf, size_t n)
{
	size_t i;

	(void)ctx;
	for (i = 0; i < n; i++) {
		memmove(psoc.last_raw, psoc.last_raw + 1, sizeof psoc.last_raw - 1u);
		psoc.last_raw[sizeof psoc.last_raw - 1u] = buf[i];
		if (!memcmp(psoc.last_raw, "\x01\x3B\x00\x00\xC4\xFF\x17", 7)) {
			/* The exit packet starts the PSoC's application, whose
			 * receiver starts afresh. */
			psoc.exit_seen = true;
			s06_rx_reset(&psoc.rx);
			continue;
		}
		if (s06_rx_byte(&psoc.rx, buf[i], wire_crc8_frame) == S06_RX_DELIVER)
			frame_in(psoc.rx.buf, psoc.rx.n);
	}
	return n;
}

static size_t p_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t n = 0;

	(void)ctx;
	if (psoc.greet_at != 0u && !psoc.greeted_self && t_us >= psoc.greet_at) {
		static const uint8_t hello[2] = { 0x01, 0x04 };

		psoc.greeted_self = true;
		psoc.answers = true;
		emit(0x41u, 0x05u, hello, 2);
	}
	if (!psoc.off_sent && ((psoc.off_at != 0u && t_us >= psoc.off_at) ||
			       (psoc.deadline != 0u && t_us >= psoc.deadline))) {
		static const uint8_t off[1] = { 0x14 };

		psoc.deadline_sent = !(psoc.off_at != 0u && t_us >= psoc.off_at);
		psoc.off_sent = true;
		psoc.off_order = ++g_order;
		emit(0x41u, 0x31u, off, 1);
	}
	if (psoc.ping_at != 0u && !psoc.ping_sent && t_us >= psoc.ping_at) {
		static const uint8_t ping[2] = { 0x12, 0x34 };

		psoc.ping_sent = true;
		emit(0x41u, 0x01u, ping, sizeof ping);
	}
	while (n < max && psoc.out_read < psoc.out_n && psoc.out_at[psoc.out_read] <= t_us)
		buf[n++] = psoc.out[psoc.out_read++];
	return n;
}

static bool p_idle(void *ctx)
{
	(void)ctx;
	return true;
}

static const struct fr_bytes link = { p_read, p_write, p_idle, NULL };

/* ---- a dead panel: the bus takes everything ---- */

static unsigned panel_bytes;

static uint64_t first_cmd_us;	/* the panel's first transaction, 0 none */
static uint64_t release_us;	/* RESET last driven high */

static void b_reset(void *ctx, bool high)
{
	(void)ctx;
	if (high)
		release_us = t_us;
}
static void b_select(void *ctx, bool on) { (void)ctx; (void)on; }

/* Frames (RAMWR), each with its order and whether the screen drawn says
 * an in-place write runs. */
#define FRAMES_MAX 256u
static const struct fr_screen *g_screen;
static struct {
	uint64_t order;
	bool in_place;
} frames[FRAMES_MAX];
static unsigned n_frames;

/* A body row says text, read as it shows: its control bytes and their
 * arguments out, a badge's digit kept (fr_draw.h). */
static bool screen_says(const struct fr_screen *scr, const char *text)
{
	for (unsigned i = 0; i < FR_LINES; i++) {
		char t[FR_LINE_MAX + 1];
		size_t n = 0;

		for (size_t k = 0; k < FR_LINE_MAX && scr->line[i][k] != '\0'; k++) {
			const unsigned char b = (unsigned char)scr->line[i][k];

			if (b == 0x01u || b == 0x05u)
				k += scr->line[i][k + 1u] != '\0';
			else if (b >= 0x20u)
				t[n++] = (char)b;
		}
		t[n] = '\0';
		if (strstr(t, text) != NULL)
			return true;
	}
	return false;
}

/* DISPON and the DISPOFFs after one (not the initialization's), each with
 * its order and the panel's bytes before it; the order of the panel's last
 * byte. */
static struct {
	uint8_t cmd;
	uint64_t order;
	unsigned bytes;
} onoff[8];
static unsigned n_onoff;
static uint64_t panel_last;

static void b_cmd(void *ctx, uint8_t c)
{
	(void)ctx;
	if ((c == 0x29u || (c == 0x28u && n_onoff != 0u)) && n_onoff < ARRAY_LEN(onoff)) {
		onoff[n_onoff].cmd = c;
		onoff[n_onoff].order = ++g_order;
		onoff[n_onoff].bytes = panel_bytes;
		n_onoff++;
	}
	if (c == 0x2Cu && n_frames < FRAMES_MAX) {
		frames[n_frames].order = ++g_order;
		frames[n_frames].in_place = g_screen != NULL &&
					    screen_says(g_screen, "Writing slot 1 in place");
		n_frames++;
	}
	if (first_cmd_us == 0u)
		first_cmd_us = t_us;
	panel_bytes++;
	panel_last = ++g_order;
}
static void b_data(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	(void)p;
	panel_bytes += (unsigned)n;
	panel_last = ++g_order;
}

/* The panel's frame buffer (S-01.023's DDR range on the unit). */
static uint8_t panel_fb[FR_PANEL_FRAME_BYTES];

static const struct fr_panel_bus bus = { b_reset, b_select, b_cmd, b_data, NULL, NULL, NULL,
					 NULL, NULL };

/* ---- the loaded image: a descriptor and one region ---- */

static uint8_t desc[FR_DESC_MAX], region[256];
#define REGION 0x8803D000u
static bool map_soft;		/* refuse the front's own ranges, as for the
				 * running slot's streams (fr_map_rule.h) */
static uint64_t last_map_at;	/* the last read of the region, in that order */
static uint64_t check_map_at;	/* the last one before the first program */
static bool off_in_check;	/* 0x31 {14} arrives while the image is checked */
static bool inbox_in_check;	/* a ping and a greeting arrive then */
static unsigned n_region_maps;	/* the region's reads: the image check's, then any
				 * comparison of a slot's stream with memory */

static bool any_program(void);

static const uint8_t *map(void *ctx, uint32_t addr, uint32_t len)
{
	(void)ctx;
	/* Never L1 code, a hole or the front's own memory (S-01.023). */
	if (map_soft && fr_map_forbidden(addr, len))
		return NULL;
	CHECK(!fr_map_forbidden(addr, len));
	if (addr == FR_DESC_ADDR && len <= sizeof desc)
		return desc;
	if (addr == REGION && len <= sizeof region) {
		if (off_in_check) {
			static const uint8_t s14[1] = { 0x14 };

			off_in_check = false;
			psoc.off_sent = true;
			psoc.off_order = ++g_order;
			emit(0x41u, 0x31u, s14, 1);
			t_us += 300u;
		}
		if (inbox_in_check) {
			static const uint8_t ping[2] = { 0x56, 0x78 }, hello[2] = { 0x01, 0x04 };

			inbox_in_check = false;
			emit(0x41u, 0x01u, ping, sizeof ping);
			emit(0x41u, 0x05u, hello, sizeof hello);
			t_us += 300u;
		}
		n_region_maps++;
		last_map_at = ++g_order;
		if (!any_program())
			check_map_at = last_map_at;
		return region;
	}
	return NULL;
}

static const struct fr_mem mem = { map, NULL };

/* The loaded image: its region `fill` (slot 0's stream 0x3C, slot 1's
 * 0x3D), its descriptor's main CRC right or not. */
static void image_of(bool good, uint8_t fill)
{
	const size_t n = FR_DESC_HEAD + 8u + 4u;

	memset(region, fill, sizeof region);
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

static void image(bool good)
{
	image_of(good, 0x3C);
}

/* ---- the flash: NOR in RAM with an op log; slot 0 and slot 1 hold small
 * streams whose one block loads the region, slot 0's with the bytes the
 * loaded memory holds, so slot 0 is the running slot (S-01.002) ---- */

#define FLASH_SIZE 0x100000u
#define FOPS_MAX   4096u

struct fop {
	char kind;		/* 'r', 'e', 'p' */
	uint32_t off, len;
	uint8_t val;		/* a program's first byte */
	uint64_t t;		/* the clock */
	uint64_t order;		/* g_order */
};

static uint8_t fmem[FLASH_SIZE];
static struct fop fops[FOPS_MAX];
static unsigned n_fops;
static bool fail_prog, lazy_prog;	/* programs fail; or take nothing */
/* the program at this offset fails; or reports success and takes nothing */
static uint32_t prog_fail_at = UINT32_MAX, prog_wrong_at = UINT32_MAX;
static bool sel_prog_lazy;		/* a selector program takes nothing */
static uint32_t fail_read_at = UINT32_MAX;	/* a read starting here fails */

static void fop(char kind, uint32_t off, uint32_t len, uint8_t val)
{
	if (n_fops < FOPS_MAX)
		fops[n_fops++] = (struct fop){ kind, off, len, val, t_us, ++g_order };
}

/* Reads of the selector fail from sel_fail_from (us) to sel_fail_until;
 * the window opens at the first erase of slot 0 when arm_sel_fail is set. */
static uint64_t sel_fail_from = UINT64_MAX, sel_fail_until;
static bool arm_sel_fail;
static uint64_t sel_fail_len = 1000000u;

static int f_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if (off > FLASH_SIZE || len > FLASH_SIZE - off)
		return -1;
	fop('r', off, len, 0);
	if (off == fail_read_at)
		return -1;
	if (off == 0xD000u && t_us >= sel_fail_from && t_us < sel_fail_until)
		return -1;
	memcpy(buf, fmem + off, len);
	return 0;
}

/* An erase takes erase_us. Each erase that succeeds at hook_off calls
 * erase_hook (hook_count counts them; first_erase_order is the first). An
 * erase listed in erase_faults fails once, having erased or not. */
static uint32_t erase_us;
static uint64_t first_erase_order;
static void (*erase_hook)(uint32_t off);
static uint32_t hook_off = 0x10000u;	/* FR_SLOT_BASE(0) */
static unsigned hook_count;
static struct {
	uint32_t off;
	bool erases, used;
} erase_faults[4];
static unsigned n_erase_faults;

static void erase_fault(uint32_t off, bool erases)
{
	erase_faults[n_erase_faults].off = off;
	erase_faults[n_erase_faults].erases = erases;
	erase_faults[n_erase_faults].used = false;
	n_erase_faults++;
}

static int f_erase(void *ctx, uint32_t off)
{
	(void)ctx;
	if (off % FR_SECTOR != 0u || off >= FLASH_SIZE)
		return -1;
	fop('e', off, FR_SECTOR, 0);
	t_us += erase_us;
	if (arm_sel_fail && off == 0x10000u) {
		arm_sel_fail = false;
		sel_fail_from = t_us;
		sel_fail_until = t_us + sel_fail_len;
	}
	for (unsigned i = 0; i < n_erase_faults; i++)
		if (erase_faults[i].off == off && !erase_faults[i].used) {
			/* fails once, having erased or not */
			erase_faults[i].used = true;
			if (erase_faults[i].erases)
				memset(fmem + off, 0xFF, FR_SECTOR);
			return -1;
		}
	memset(fmem + off, 0xFF, FR_SECTOR);
	if (off == hook_off) {
		if (first_erase_order == 0u)
			first_erase_order = g_order;
		hook_count++;
	}
	if (erase_hook != NULL && (off == 0x10000u || off == 0x88000u))
		erase_hook(off);	/* a slot's first sector */
	return 0;
}

static int f_program(void *ctx, uint32_t off, const uint8_t *b, uint32_t n)
{
	(void)ctx;
	if (n == 0u || off > FLASH_SIZE || n > FLASH_SIZE - off)
		return -1;
	fop('p', off, n, b[0]);
	if (fail_prog || off == prog_fail_at)
		return -1;
	if (off == prog_wrong_at)
		return 0;	/* reports success, stores nothing */
	if (lazy_prog)
		return 0;
	if (sel_prog_lazy && off == FR_SELECTOR_OFF) {
		sel_prog_lazy = false;	/* once */
		return 0;
	}
	for (uint32_t i = 0; i < n; i++)
		fmem[off + i] &= b[i];
	return 0;
}

static const struct fr_flash flash = { f_read, f_erase, f_program, NULL };

static bool any_program(void)
{
	for (unsigned i = 0; i < n_fops; i++)
		if (fops[i].kind == 'p')
			return true;
	return false;
}

static size_t ldr_hdr(uint8_t *o, uint16_t flags, uint32_t target, uint32_t count)
{
	uint8_t x = 0;

	fr_put32(o, 0xAD000000u | (flags & 0xFFF0u) | 4u);
	fr_put32(o + 4, target);
	fr_put32(o + 8, count);
	fr_put32(o + 12, 0);
	for (unsigned i = 0; i < 16u; i++)
		x ^= o[i];
	o[2] = x;
	return 16;
}

/* Slot k: an empty INIT block in L1 code, a block of 256 bytes `fill` to
 * the region, then FINAL: a bootable stream (S-02.091), with no INIT block
 * when init is false. */
static void put_stream_of(unsigned k, uint8_t fill, bool init)
{
	uint8_t *s = fmem + FR_SLOT_BASE(k);
	size_t n = 0;

	memset(s, 0xFF, FR_SLOT_CAP);
	if (init)
		n += ldr_hdr(s + n, 0x0800u, 0x11A04000u, 0);
	n += ldr_hdr(s + n, 0, REGION, sizeof region);
	memset(s + n, fill, sizeof region);
	n += sizeof region;
	n += ldr_hdr(s + n, 0x8000u, REGION, 0);
	memset(fmem + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
	fr_put32(fmem + FR_RECORD_OFF(k), fr_crc32(0, s, n));
	fr_put32(fmem + FR_RECORD_OFF(k) + 4u, (uint32_t)n);
	fr_put32(fmem + FR_RECORD_OFF(k) + 8u, FR_TAG);
}

static void put_stream(unsigned k, uint8_t fill)
{
	put_stream_of(k, fill, true);
}

/* The selector, slot 0 the running one, slot 1 another, both good, both
 * logs empty. */
static void flash_reset(uint8_t index, uint8_t verified)
{
	memset(fmem, 0xFF, sizeof fmem);
	fmem[FR_SELECTOR_OFF] = index;
	fmem[FR_SELECTOR_OFF + 1u] = verified;
	put_stream(0, 0x3C);
	put_stream(1, 0x3D);
	n_fops = 0;
	fail_prog = lazy_prog = false;
	prog_fail_at = prog_wrong_at = UINT32_MAX;
	fail_read_at = UINT32_MAX;
}

static unsigned count_fops(char kind)
{
	unsigned n = 0;

	for (unsigned i = 0; i < n_fops; i++)
		n += fops[i].kind == kind ? 1u : 0u;
	return n;
}

static bool in_log(uint32_t off)
{
	return (off >= FR_LOG_OFF(0) && off < FR_LOG_OFF(0) + FR_SECTOR) ||
	       (off >= FR_LOG_OFF(1) && off < FR_LOG_OFF(1) + FR_SECTOR);
}

/* The one program in the log sectors, or ~0u; anything programmed there
 * that is not one byte fails, and so does a second program. */
static unsigned log_program(void)
{
	unsigned at = ~0u;

	for (unsigned i = 0; i < n_fops; i++) {
		const struct fop *o = &fops[i];

		if (o->kind != 'p' || !in_log(o->off))
			continue;
		CHECK(o->len == 1u);
		CHECK(at == ~0u);
		at = i;
	}
	return at;
}

static struct fr_start st;

static enum fr_next run(bool panel, struct fr_handoff *ho)
{
	static struct fr_start_hw hw;	/* st keeps it for fr_halt() */

	memset(&hw, 0, sizeof hw);
	hw.clock = &clock;
	hw.psoc = &link;
	hw.panel = panel ? &bus : NULL;
	hw.frame = panel_fb;
	hw.flash = &flash;
	hw.mem = &mem;
	memset(ho, 0, sizeof *ho);
	/* The pulse path: no handshake, so the panel's 120 ms count from now. */
	ho->strap = 0x0104u;
	t_us = 1000u;
	panel_bytes = 0;
	first_cmd_us = 0;
	return fr_start_run(&st, &hw, ho);
}

static uint64_t ms(uint64_t cycles)
{
	return cycles / 400u / 1000u;
}

/* The order of S-07.023's messages, ids and the first payload bytes. */
static bool sent_in_order(const struct sent *want, unsigned n)
{
	unsigned i, j = 0;

	for (i = 0; i < psoc.n_log && j < n; i++) {
		const struct sent *e = &psoc.log[i];

		if (e->id == 0x05u)
			continue;		/* greetings */
		if (e->id != want[j].id || e->request != want[j].request ||
		    (want[j].len && (e->len != want[j].len || e->p0 != want[j].p0 ||
				     e->p1 != want[j].p1 || e->p2 != want[j].p2)))
			return false;
		j++;
	}
	return j == n;
}

static void test_normal_order(void)
{
	static const struct sent want[] = {
		{ 0x30, 0, 0, 0, 0, false, 0, 0 }, { 0x36, 0, 0, 0, 0, false, 0, 0 },
		{ 0x51, 3, 0x00, 0x00, 0x00, false, 0, 0 },
		{ 0x36, 0, 0, 0, 0, true, 0, 0 }, { 0x30, 0, 0, 0, 0, true, 0, 0 },
		{ 0x51, 3, 0x3C, 0xFF, 0x02, false, 0, 0 },
		{ 0x43, 2, 0x01, 0x01, 0x00, false, 0, 0 },
		{ 0x41, 0, 0, 0, 0, true, 0, 0 },
	};
	struct fr_handoff ho;

	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	image(true);
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	CHECK(sent_in_order(want, ARRAY_LEN(want)));
	CHECK((ho.flags & (FR_HO_GREETED | FR_HO_PANEL_UP | FR_HO_PANEL_LIT |
			   FR_HO_SUBSCRIBED | FR_HO_KEYS_REPLIED)) ==
	      (FR_HO_GREETED | FR_HO_PANEL_UP | FR_HO_PANEL_LIT | FR_HO_SUBSCRIBED |
	       FR_HO_KEYS_REPLIED));
	CHECK((ho.flags & FR_HO_GREET_WAITED) == 0u);
	CHECK(ho.key_len == 9u);
	CHECK(st.desc.main_entry == 0x11A00000u);
	/* The panel's set-up and the logo frame went out, and before the
	 * greeting; the logo was lit before the keys were read. */
	CHECK(panel_bytes > 215040u);
	CHECK(ho.stamps[FR_STAMP_LOGO] != 0u && ho.stamps[FR_STAMP_LOGO] <= ho.stamps[FR_STAMP_KEYS]);
	/* Nothing slow: the key read within the panel's 120 ms and some. */
	CHECK(ms(ho.stamps[FR_STAMP_KEYS]) < 400u);
	/* The flash: no erase, and one program, the start log's (S-02.095) */
	CHECK(count_fops('e') == 0u);
	CHECK(count_fops('p') == 1u && log_program() != ~0u);
}

/* The index of the first logged message after `from` with this id, or
 * psoc.n_log. */
static unsigned next_sent(unsigned from, uint8_t id, uint8_t p0)
{
	for (unsigned i = from; i < psoc.n_log; i++)
		if (psoc.log[i].id == id && (psoc.log[i].len == 0u || psoc.log[i].p0 == p0))
			return i;
	return psoc.n_log;
}

/* S-07.076, the front's point: a ping and a greeting that arrive while the
 * image is checked are answered before the jump: the ping 0x02, the
 * greeting S-07.024's answer, the re-greeting, 0x51 {3C FF 02}, 0x43
 * {01 01} and 0x06 {40}, in that order; the record's sequence number and
 * flags are the link's after them. */
static void test_inbox_before_jump(void)
{
	struct fr_handoff ho;
	unsigned keys, pong, greet, held, sub, ok;

	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	image(true);
	inbox_in_check = true;
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	CHECK(!inbox_in_check);
	keys = next_sent(0, 0x41u, 0);
	CHECK(keys < psoc.n_log);
	pong = next_sent(keys, 0x02u, 0x56u);
	greet = next_sent(keys, 0x05u, 0x01u);
	held = next_sent(greet, 0x51u, 0x3Cu);
	sub = next_sent(held, 0x43u, 0x01u);
	ok = next_sent(sub, 0x06u, 0x40u);
	CHECK(pong < psoc.n_log && greet < psoc.n_log && held < psoc.n_log && sub < psoc.n_log &&
	      ok < psoc.n_log);
	CHECK(psoc.log[pong].len == 2u && psoc.log[pong].p1 == 0x78u);
	CHECK(psoc.log[held].p1 == 0xFFu && psoc.log[held].p2 == 0x02u);
	CHECK(ok == psoc.n_log - 1u || next_sent(ok + 1u, 0x06u, 0x40u) == psoc.n_log);
	CHECK(ho.psoc_seq == st.psoc.seq);
	CHECK((ho.flags & (FR_HO_GREETED | FR_HO_SUBSCRIBED)) == (FR_HO_GREETED | FR_HO_SUBSCRIBED));
	CHECK(st.psoc.in_count == 0u);
	/* without them, no answer after the key read */
	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	image(true);
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	keys = next_sent(0, 0x41u, 0);
	CHECK(next_sent(keys, 0x02u, 0x56u) == psoc.n_log && next_sent(keys, 0x05u, 0x01u) == psoc.n_log);
}

/* S-01.023, version 2: the running slot established, every start that goes
 * on, with no trial marked on a normal start. */
static void test_record_running(void)
{
	struct fr_handoff ho;

	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	image(true);
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	CHECK((ho.flags & FR_HO_RUNNING) != 0u && ho.run_slot == 0u);
	CHECK((ho.flags & FR_HO_TRIAL_MARKED) == 0u);
}

/* The bound with a silent PSoC: the panel (<= 121 ms and its writes), ten
 * greetings (<= 200 ms), S-07.021's 1.5 s, two requests and the key
 * request (100 ms each): under 2.2 s. No reply is no chord: the boot goes
 * on, to held mode in the full bootloader (S-16.008). */
#define SILENT_BOUND_MS 2200u

static void test_silent_psoc(void)
{
	struct fr_handoff ho;

	model_reset();
	image(true);
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	CHECK(psoc.exit_seen);
	CHECK((ho.flags & FR_HO_GREET_WAITED) != 0u);
	CHECK((ho.flags & (FR_HO_GREETED | FR_HO_KEYS_REPLIED)) == 0u);
	CHECK(psoc.keys_at != 0u);
	CHECK(psoc.keys_at / 1000u < SILENT_BOUND_MS);
	CHECK(psoc.keys_at / 1000u >= 1500u);	/* the 1.5 s were waited */
}

static void test_no_panel(void)
{
	struct fr_handoff ho;

	model_reset();
	image(true);
	CHECK(run(false, &ho) == FR_NEXT_MAIN);
	CHECK(panel_bytes == 0u);
	CHECK((ho.flags & (FR_HO_PANEL_UP | FR_HO_PANEL_LIT)) == 0u);
	CHECK(psoc.keys_at / 1000u < SILENT_BOUND_MS - 150u);
	/* With nothing to light, no {3C FF 02}. */
	for (unsigned i = 0; i < psoc.n_log; i++)
		CHECK(!(psoc.log[i].id == 0x51u && psoc.log[i].p0 == 0x3Cu));
}

/* S-07.021: the wait ends at the PSoC's greeting, here 300 ms into it. */
static void test_greeting_ends_the_wait(void)
{
	struct fr_handoff ho;
	uint64_t wait_from;

	model_reset();
	image(true);
	/* The greetings end by about 125 ms after the panel's ~150 ms. */
	psoc.greet_at = 600000u;
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	CHECK(psoc.exit_seen);
	CHECK((ho.flags & FR_HO_GREET_WAITED) != 0u);
	CHECK((ho.flags & FR_HO_GREETED) != 0u);
	wait_from = psoc.greet_at;
	CHECK(ho.stamps[FR_STAMP_GREETED] / 400u >= wait_from);
	CHECK(ho.stamps[FR_STAMP_GREETED] / 400u < wait_from + 20000u);
	/* The PSoC's greeting was answered {40}. */
	{
		bool answered = false;

		for (unsigned i = 0; i < psoc.n_log; i++)
			if (psoc.log[i].id == 0x06u && psoc.log[i].p0 == 0x40u)
				answered = true;
		CHECK(answered);
	}
	CHECK(ms(ho.stamps[FR_STAMP_KEYS]) < 1000u);
}

/* Power + M4 (key 0x09: byte 1, bit 1): recovery whatever else. */
static void test_power4(void)
{
	struct fr_handoff ho;

	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	psoc.keys[1] = 0x02u;
	image(true);
	CHECK(run(true, &ho) == FR_NEXT_RECOVERY);
	CHECK(st.why == FR_WHY_KEYS);
	/* M4 held with M1 is two keys: the boot goes on. */
	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	psoc.keys[0] = 0x40u;
	psoc.keys[1] = 0x02u;
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	/* and with a step key, 9 or 16 (S-07.033: the 72 keys) */
	for (unsigned k = 0; k < 2u; k++) {
		model_reset();
		psoc.answers = true;
		psoc.answer_keys = true;
		psoc.keys[1] = 0x02u;
		psoc.keys[8] = k ? 0x80u : 0x01u;
		CHECK(run(true, &ho) == FR_NEXT_MAIN);
	}
}

/*
 * S-13c.006 without trusting DMA9_YMOD's time base: a handshake that dates
 * the release long before now leaves no wait by itself, and the first panel
 * command still comes 120 ms after the INIT stage began. On the pulse and
 * the strap paths the front drives the release itself, and the first
 * command comes 120 ms after it.
 *
 * Two things this cannot show. The per-byte sync of the panel's bus is
 * code for the target alone: no host test sees it. And the trace rule
 * (panel.py, rule_reset_wait) measures from the reset's release, not from
 * the INIT stage: the handshake case's floor is checked here only.
 */
static void test_reset_wait_floor(void)
{
	static struct fr_start_hw hw;
	struct fr_handoff ho;

	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	image(true);
	memset(&hw, 0, sizeof hw);
	hw.clock = &clock;
	hw.psoc = &link;
	hw.panel = &bus;
	hw.frame = panel_fb;
	hw.flash = &flash;
	hw.mem = &mem;
	memset(&ho, 0, sizeof ho);
	ho.strap = 0x0104u;			/* PA8 high: no strap reset */
	ho.dma9_xmod = 0x12345678u;		/* the handshake ... */
	ho.dma9_ymod = 2u;			/* ... released at 2 ms */
	t_us = 500000u;				/* the front at 500 ms */
	ho.stamps[FR_STAMP_INIT_ENTRY] = 480000u * 400u;	/* INIT at 480 ms */
	panel_bytes = 0;
	first_cmd_us = 0;
	CHECK(fr_start_run(&st, &hw, &ho) == FR_NEXT_MAIN);
	CHECK(st.panel.path == FR_PANEL_HANDSHAKE);
	CHECK(first_cmd_us >= 600000u);		/* 480 ms + 120 ms */
	CHECK(first_cmd_us < 602000u);

	/* INIT long enough ago: no wait beyond the handshake's own. */
	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	memset(&ho, 0, sizeof ho);
	ho.strap = 0x0104u;
	ho.dma9_xmod = 0x12345678u;
	ho.dma9_ymod = 2u;
	t_us = 500000u;
	ho.stamps[FR_STAMP_INIT_ENTRY] = 100000u * 400u;
	first_cmd_us = 0;
	CHECK(fr_start_run(&st, &hw, &ho) == FR_NEXT_MAIN);
	CHECK(first_cmd_us >= 500000u && first_cmd_us < 502000u);

	/* No handshake: the front's own pulse, the wait from its release,
	 * whether the INIT stage began just before or long ago. */
	for (unsigned i = 0; i < 2u; i++) {
		const uint64_t init_us = i == 0u ? 480000u : 100000u;

		model_reset();
		psoc.answers = true;
		psoc.answer_keys = true;
		memset(&ho, 0, sizeof ho);
		ho.strap = 0x0104u;
		ho.dma9_xmod = 0u;
		t_us = 500000u;
		ho.stamps[FR_STAMP_INIT_ENTRY] = init_us * 400u;
		first_cmd_us = 0;
		release_us = 0;
		CHECK(fr_start_run(&st, &hw, &ho) == FR_NEXT_MAIN);
		CHECK(st.panel.path == FR_PANEL_PULSE);
		CHECK(release_us >= 500000u && release_us < 501000u);
		CHECK(first_cmd_us >= release_us + 120000u);
		CHECK(first_cmd_us < release_us + 122000u);
		CHECK(first_cmd_us >= init_us + 120000u);
	}

	/* PA8 strapped low: the first drive of RESET is the release, and a
	 * handshake is not believed. */
	for (unsigned i = 0; i < 2u; i++) {
		model_reset();
		psoc.answers = true;
		psoc.answer_keys = true;
		memset(&ho, 0, sizeof ho);
		ho.strap = 0x0004u;			/* PA8 low */
		ho.dma9_xmod = i == 0u ? 0x12345678u : 0u;
		ho.dma9_ymod = 2u;
		t_us = 500000u;
		ho.stamps[FR_STAMP_INIT_ENTRY] = 480000u * 400u;
		first_cmd_us = 0;
		release_us = 0;
		CHECK(fr_start_run(&st, &hw, &ho) == FR_NEXT_MAIN);
		CHECK(st.panel.path == FR_PANEL_STRAP);
		CHECK(release_us >= 500000u && release_us < 501000u);
		CHECK(first_cmd_us >= release_us + 120000u);
		CHECK(first_cmd_us < release_us + 122000u);
		CHECK(first_cmd_us >= 480000u + 120000u);
	}
}

static void test_image_check_fails(void)
{
	struct fr_handoff ho;

	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	image(false);
	CHECK(run(true, &ho) == FR_NEXT_RECOVERY);
	CHECK(st.why == FR_WHY_CHECK);
}

static unsigned count_id(uint8_t id, unsigned from)
{
	unsigned i, n = 0;

	for (i = from; i < psoc.n_log; i++)
		n += psoc.log[i].id == id ? 1u : 0u;
	return n;
}

/*
 * The halt after a restart request (recovery's): one that has
 * no effect is not sent again, as the full bootloader waits (S-07.070); the
 * link is served meanwhile, so a ping is answered and the switch's power-off
 * runs (S-07.051); after it nothing more is answered or sent.
 */
static void test_halt(void)
{
	static const uint8_t ping[2] = { 0xAB, 0xCD }, off[1] = { 0x14 };
	struct fr_handoff ho;
	uint64_t t0;
	unsigned n;

	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	image(true);
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	fr_psoc_reset(&st.psoc);	/* the model loses it */
	t0 = t_us;
	fr_halt(&st, 3000000u);
	CHECK(t_us - t0 >= 3000000u && t_us - t0 < 3100000u);
	CHECK(count_id(0x97u, 0) == 1u);

	emit(0x41u, 0x01u, ping, sizeof ping);
	fr_halt(&st, 100000u);
	CHECK(count_id(0x02u, 0) == 1u);

	n = psoc.n_log;
	emit(0x41u, 0x31u, off, sizeof off);
	fr_halt(&st, 100000u);
	CHECK(st.psoc.off);
	{
		bool report = false, echo = false;

		for (unsigned i = n; i < psoc.n_log; i++) {
			report |= psoc.log[i].id == 0x51u && psoc.log[i].p0 == 0x00u &&
				  psoc.log[i].p2 == 0x02u;
			echo |= psoc.log[i].id == 0x32u && psoc.log[i].p0 == 0x14u;
		}
		CHECK(report && echo);
	}

	/* Powered off: the halt still returns on time and sends nothing. */
	n = psoc.n_log;
	emit(0x41u, 0x01u, ping, sizeof ping);
	t0 = t_us;
	fr_halt(&st, 1000000u);
	CHECK(t_us - t0 >= 1000000u);
	CHECK(psoc.n_log == n);
	CHECK(count_id(0x97u, 0) == 1u);
}

/* ---- S-02.095: the start log's step ---- */

static void answering(void)
{
	model_reset();
	psoc.answers = true;
	psoc.answer_keys = true;
	image(true);
}

/* SC-STARTLOG run 1: one program, 1 byte, the entry's started bit, after
 * the key read and the image check, then a read of that byte; the hand-off
 * record names the slot and the entry. */
static void expect_marked(const struct fr_handoff *ho, unsigned k, uint32_t e, uint8_t before)
{
	const unsigned at = log_program();
	const uint8_t v = (uint8_t)(0xFFu ^ (1u << (2u * (e % 4u))));

	CHECK(at != ~0u);
	if (at == ~0u)
		return;
	CHECK(fops[at].off == FR_LOG_OFF(k) + e / 4u && fops[at].val == v);
	CHECK(at + 1u < n_fops && fops[at + 1u].kind == 'r' && fops[at + 1u].off == fops[at].off &&
	      fops[at + 1u].len == 1u);
	CHECK(fmem[FR_LOG_OFF(k) + e / 4u] == (uint8_t)(before & v));
	/* after the key read, and after the image check's last read; the
	 * running slot's comparison for the record may follow (S-01.023) */
	CHECK(fops[at].t > psoc.keys_at && fops[at].order > check_map_at && check_map_at != 0u);
	CHECK(fops[at].t <= ho->stamps[FR_STAMP_HANDOFF] / 400u);
	CHECK((ho->flags & FR_HO_START_LOGGED) != 0u);
	CHECK(ho->log_slot == k && ho->log_entry == e);
	CHECK(count_fops('e') == 0u);
}

static void expect_no_log_write(const struct fr_handoff *ho)
{
	CHECK(log_program() == ~0u);
	CHECK(count_fops('e') == 0u);
	CHECK((ho->flags & FR_HO_START_LOGGED) == 0u);
	CHECK(ho->log_slot == 0u && ho->log_entry == 0u);
}

static void test_log_step(void)
{
	struct fr_handoff ho;

	/* selector 00 01: slot 0's log, entry 0 */
	answering();
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	expect_marked(&ho, 0, 0, 0xFF);
	CHECK(fmem[0x87000] == 0xFEu);
	/* selector 01 01: slot 1's, at 0xFF000 */
	answering();
	flash_reset(0x01, 0x01);
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	expect_marked(&ho, 1, 0, 0xFF);
	CHECK(fmem[0xFF000] == 0xFEu);
	for (unsigned i = 0; i < n_fops; i++)
		if (fops[i].kind != 'r')
			CHECK(fops[i].off >= 0xFF000u);
	/* entry 5: four finished, one finished, the next free */
	answering();
	fmem[FR_LOG_OFF(0)] = 0x00;
	fmem[FR_LOG_OFF(0) + 1u] = 0xFC;
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	expect_marked(&ho, 0, 5, 0xFC);
	CHECK(fmem[FR_LOG_OFF(0) + 1u] == 0xF8u);
	/* a record that gives no log: nothing written, no entry */
	answering();
	fr_put32(fmem + FR_RECORD_OFF(0) + 4u, 0x77001u);
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	expect_no_log_write(&ho);
	/* an odd log and a full one: nothing written */
	answering();
	fmem[FR_LOG_OFF(0)] = 0xFD;
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	expect_no_log_write(&ho);
	answering();
	memset(fmem + FR_LOG_OFF(0), 0x00, FR_SECTOR);
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	expect_no_log_write(&ho);
}

/*
 * S-02.095, S-02.093: the trial start of a version 7 front. The selector
 * {00 01} names slot 0; slot 1's log has entry 0 pending (FD, the rest FF).
 * Started from slot 1 (the memory holds its stream): entry 0 marked FC, one
 * program in all, no start logged anywhere. Started from slot 0: the trial
 * spent (F9), the start logged in slot 0 as before, and the next start
 * makes no comparison. From neither: no spend, and slot 1's trial start
 * still marks it.
 * Power + M4 in a trial start: no mark. A start with no trial pending makes
 * the log step's operations and one more 1-byte read, of slot 1's byte 0.
 */
static void test_log_trial_start(void)
{
	struct fr_handoff ho;
	struct fr_log l;
	static struct fop ref[FOPS_MAX];
	unsigned n_ref, j = 0, base_maps;

	/* the trial start, run from slot 1 */
	answering();
	fmem[FR_LOG_OFF(1)] = 0xFDu;
	image_of(true, 0x3D);
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	CHECK(fmem[FR_LOG_OFF(1)] == 0xFCu);
	CHECK(log_program() != ~0u && fops[log_program()].off == FR_LOG_OFF(1));
	CHECK(fops[log_program()].val == 0xFEu);
	CHECK(count_fops('p') == 1u && count_fops('e') == 0u);
	CHECK(fmem[FR_LOG_OFF(0)] == 0xFFu);
	CHECK((ho.flags & FR_HO_START_LOGGED) == 0u);
	CHECK(ho.log_slot == 0u && ho.log_entry == 0u);

	/* the old slot's start while a trial is pending: the comparison with
	 * memory runs once, the trial is spent (FD to F9, one program), and
	 * the start is logged as before */
	answering();
	n_region_maps = 0;
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	expect_marked(&ho, 0, 0, 0xFF);
	base_maps = n_region_maps;
	answering();
	fmem[FR_LOG_OFF(1)] = 0xFDu;
	n_region_maps = 0;
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	/* the log step's comparison is the record's too: one either way */
	CHECK(n_region_maps == base_maps);
	CHECK((ho.flags & FR_HO_RUNNING) != 0u && ho.run_slot == 0u);
	CHECK((ho.flags & FR_HO_TRIAL_MARKED) == 0u);
	CHECK(fmem[FR_LOG_OFF(1)] == 0xF9u && fmem[FR_LOG_OFF(0)] == 0xFEu);
	CHECK(count_fops('p') == 2u && count_fops('e') == 0u);
	for (unsigned i = 0, k = 0; i < n_fops; i++)
		if (fops[i].kind == 'p') {
			CHECK(fops[i].off == (k == 0u ? FR_LOG_OFF(1) : FR_LOG_OFF(0)));
			CHECK(fops[i].val == (k == 0u ? 0xFBu : 0xFEu));
			k++;
		}
	CHECK((ho.flags & FR_HO_START_LOGGED) != 0u && ho.log_slot == 0u && ho.log_entry == 0u);
	/* the next start: no comparison, one 1-byte read of slot 1's sector,
	 * nothing written there, the start logged as always */
	{
		static uint8_t keep[FLASH_SIZE];
		unsigned reads1 = 0;

		memcpy(keep, fmem, sizeof keep);
		answering();
		memcpy(fmem, keep, sizeof keep);
		n_fops = 0;
		n_region_maps = 0;
		CHECK(run(true, &ho) == FR_NEXT_MAIN);
		CHECK(n_region_maps == base_maps);
		for (unsigned i = 0; i < n_fops; i++)
			if (fops[i].off >= FR_LOG_OFF(1) && fops[i].off < FR_LOG_OFF(1) + FR_SECTOR) {
				CHECK(fops[i].kind == 'r' && fops[i].len == 1u);
				reads1++;
			}
		CHECK(reads1 == 1u);
		CHECK(fmem[FR_LOG_OFF(1)] == 0xF9u);
		expect_marked(&ho, 0, 1, 0xFE);
	}
	/* memory that matches neither slot: the running slot not established,
	 * no spend */
	answering();
	fmem[FR_LOG_OFF(1)] = 0xFDu;
	image_of(true, 0x3E);
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	expect_marked(&ho, 0, 0, 0xFF);
	CHECK(fmem[FR_LOG_OFF(1)] == 0xFDu);
	/* then the trial start of slot 1 still marks it */
	{
		static uint8_t keep[FLASH_SIZE];

		memcpy(keep, fmem, sizeof keep);
		answering();
		memcpy(fmem, keep, sizeof keep);
		n_fops = 0;
		image_of(true, 0x3D);
		CHECK(run(true, &ho) == FR_NEXT_MAIN);
		CHECK(fmem[FR_LOG_OFF(1)] == 0xFCu && (ho.flags & FR_HO_START_LOGGED) == 0u);
		CHECK(count_fops('p') == 1u);
		/* S-01.023: a trial start, from slot 1 */
		CHECK((ho.flags & (FR_HO_TRIAL_MARKED | FR_HO_RUNNING)) ==
		      (FR_HO_TRIAL_MARKED | FR_HO_RUNNING) && ho.run_slot == 1u);
	}

	/* Power + M4 in a trial start: recovery, and no mark */
	answering();
	fmem[FR_LOG_OFF(1)] = 0xFDu;
	image_of(true, 0x3D);
	psoc.keys[1] = 0x02u;
	CHECK(run(true, &ho) == FR_NEXT_RECOVERY);
	CHECK(count_fops('p') == 0u && fmem[FR_LOG_OFF(1)] == 0xFDu);

	/* no trial pending: the log step's own operations, and one 1-byte read
	 * of slot 1's byte 0 after the scan's reads */
	answering();
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	expect_marked(&ho, 0, 0, 0xFF);
	answering();
	n_fops = 0;
	(void)fr_log_start(&flash, &l);
	memcpy(ref, fops, sizeof ref);
	n_ref = n_fops;
	answering();
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	/* up to the mark's read-back; the record's comparison follows */
	{
		const unsigned at = log_program();

		CHECK(at != ~0u && at + 2u == n_ref + 1u);
		if (at != ~0u)
			n_fops = at + 2u;
	}
	for (unsigned i = 0; i < n_fops && j <= n_ref; i++) {
		if (fops[i].kind == 'r' && fops[i].off == FR_LOG_OFF(1)) {
			/* after the scan, before the mark's read of its byte */
			CHECK(fops[i].len == 1u && j < n_ref && ref[j].kind == 'r' &&
			      ref[j].off == FR_LOG_OFF(0) && ref[j].len == 1u);
			continue;
		}
		CHECK(j < n_ref && fops[i].kind == ref[j].kind && fops[i].off == ref[j].off &&
		      fops[i].len == ref[j].len && fops[i].val == ref[j].val);
		j++;
	}
	CHECK(j == n_ref);
}

/* Another start to the hand-off, the flash as it is, the memory slot k's
 * stream (0x3C slot 0, 0x3D slot 1). */
static enum fr_next run_again(unsigned k, struct fr_handoff *ho)
{
	static uint8_t keep[FLASH_SIZE];

	memcpy(keep, fmem, sizeof keep);
	answering();		/* the model's reset rewrites the flash */
	memcpy(fmem, keep, sizeof keep);
	n_fops = 0;
	image_of(true, k == 0u ? 0x3C : 0x3D);
	return run(true, ho);
}

/*
 * S-02.093's spend through fr_start_run(), with the pending trial in either
 * slot: the slot the selector names (k) runs, the other (o) reads FD. The
 * spend's program is one byte at o's byte 0, FB, before the start's own
 * entry in k's log. Its program failing, or reading back otherwise, changes
 * nothing else: the start is logged in k all the same (FR_LOG_GO_MARKED,
 * the entry marked, the hand-off naming it), and the next start tries the
 * spend again and it takes.
 */
static void test_log_trial_spend(void)
{
	struct fr_handoff ho;

	for (unsigned o = 0; o < 2u; o++)
		for (unsigned fault = 0; fault < 3u; fault++) {
			const unsigned k = 1u - o;
			unsigned progs = 0;

			answering();
			flash_reset(k == 0u ? 0x00 : 0x01, 0x01);
			fmem[FR_LOG_OFF(o)] = 0xFDu;
			image_of(true, k == 0u ? 0x3C : 0x3D);
			if (fault == 1u)
				prog_fail_at = FR_LOG_OFF(o);
			else if (fault == 2u)
				prog_wrong_at = FR_LOG_OFF(o);
			CHECK(run(true, &ho) == FR_NEXT_MAIN);
			CHECK((ho.flags & FR_HO_START_LOGGED) != 0u);
			CHECK(ho.log_slot == k && ho.log_entry == 0u);
			CHECK(fmem[FR_LOG_OFF(k)] == 0xFEu);
			CHECK(fmem[FR_LOG_OFF(o)] == (fault == 0u ? 0xF9u : 0xFDu));
			for (unsigned i = 0; i < n_fops; i++)
				if (fops[i].kind == 'p') {
					CHECK(fops[i].off == (progs == 0u ? FR_LOG_OFF(o) : FR_LOG_OFF(k)));
					CHECK(fops[i].val == (progs == 0u ? 0xFBu : 0xFEu));
					progs++;
				}
			CHECK(progs == 2u && count_fops('e') == 0u);
			/* the selector as it was, the rest of o's sector free */
			CHECK(fmem[FR_SELECTOR_OFF] == (k == 0u ? 0x00u : 0x01u) &&
			      fmem[FR_SELECTOR_OFF + 1u] == 0x01u);
			for (unsigned j = 1; j < FR_SECTOR; j++)
				if (fmem[FR_LOG_OFF(o) + j] != 0xFFu) {
					CHECK(0);
					break;
				}
			if (fault == 0u)
				continue;
			/* the next start tries the spend again, and it takes */
			prog_fail_at = prog_wrong_at = UINT32_MAX;
			CHECK(run_again(k, &ho) == FR_NEXT_MAIN);
			CHECK(fmem[FR_LOG_OFF(o)] == 0xF9u);
			CHECK((ho.flags & FR_HO_START_LOGGED) != 0u);
			CHECK(ho.log_slot == k && ho.log_entry == 1u);
			CHECK(fmem[FR_LOG_OFF(k)] == 0xFAu);
		}
}

/* SC-STARTLOG run 2 and SC-RECOVERY run 13: three unfinished starts, then
 * recovery with why 3, then a normal start. */
static void test_log_recovery(void)
{
	struct fr_handoff ho;
	unsigned at;

	answering();
	for (unsigned s = 0; s < 3u; s++) {
		n_fops = 0;
		CHECK(run(true, &ho) == FR_NEXT_MAIN);
		CHECK(ho.log_entry == s);
	}
	CHECK(fmem[0x87000] == 0xEAu);
	n_fops = 0;
	CHECK(run(true, &ho) == FR_NEXT_RECOVERY);
	CHECK(st.why == FR_WHY_COUNT);
	at = log_program();
	CHECK(at != ~0u && fops[at].off == 0x87000u && fops[at].val == 0xDFu);
	CHECK(fmem[0x87000] == 0xCAu);
	CHECK((ho.flags & FR_HO_START_LOGGED) == 0u);
	CHECK(count_fops('e') == 0u);
	n_fops = 0;
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	expect_marked(&ho, 0, 3, 0xCA);
	CHECK(fmem[0x87000] == 0x8Au);
}

/* S-02.095: the step is not reached, so not counted. */
static void test_log_not_reached(void)
{
	struct fr_handoff ho;

	/* Power + M4 */
	answering();
	psoc.keys[1] = 0x02u;
	CHECK(run(true, &ho) == FR_NEXT_RECOVERY);
	CHECK(st.why == FR_WHY_KEYS);
	CHECK(count_fops('p') == 0u && count_fops('e') == 0u);
	/* a failed image check */
	answering();
	image(false);
	CHECK(run(true, &ho) == FR_NEXT_RECOVERY);
	CHECK(st.why == FR_WHY_CHECK);
	CHECK(count_fops('p') == 0u && count_fops('e') == 0u);
	/* a power-off before the key read */
	answering();
	psoc.state = 0x14u;
	CHECK(run(true, &ho) == FR_NEXT_HALT);
	CHECK(psoc.keys_at == 0u);
	CHECK(count_fops('p') == 0u && count_fops('e') == 0u);
	/* the switch turned off while the image is checked: handled before
	 * the hand-off, which then never comes */
	answering();
	off_in_check = true;
	CHECK(run(true, &ho) == FR_NEXT_HALT);
	CHECK(!off_in_check && psoc.off_sent);
	CHECK(count_id(0x32u, 0) == 1u);	/* S-07.051: the echo */
	/* the charge wait, left by a power-off */
	answering();
	psoc.state = 0x16u;
	psoc.off_at = 3000000u;
	CHECK(run(true, &ho) == FR_NEXT_HALT);
	CHECK((ho.flags & FR_HO_CHARGE_WAITED) != 0u);
	CHECK(psoc.keys_at == 0u);
	CHECK(count_fops('p') == 0u && count_fops('e') == 0u);
}

/* S-02.095 step 5 and a failed read: nothing named, the main image starts */
static void test_log_flash_failures(void)
{
	struct fr_handoff ho;

	for (unsigned v = 0; v < 2u; v++) {
		/* a program that fails, one whose byte reads back unchanged */
		answering();
		fail_prog = v == 0u;
		lazy_prog = v == 1u;
		CHECK(run(true, &ho) == FR_NEXT_MAIN);
		CHECK((ho.flags & FR_HO_START_LOGGED) == 0u);
		CHECK(ho.log_slot == 0u && ho.log_entry == 0u);
		CHECK(count_fops('p') == 1u && count_fops('e') == 0u);
		/* three unfinished and such a model: the main image starts */
		answering();
		fmem[FR_LOG_OFF(0)] = 0xEA;
		fail_prog = v == 0u;
		lazy_prog = v == 1u;
		CHECK(run(true, &ho) == FR_NEXT_MAIN);
		CHECK((ho.flags & FR_HO_START_LOGGED) == 0u);
		CHECK(count_fops('p') == 1u);
	}
	/* a read that fails: the selector, the record, the sector */
	{
		const uint32_t at[] = { FR_SELECTOR_OFF, FR_RECORD_OFF(0), FR_LOG_OFF(0) };

		for (unsigned i = 0; i < ARRAY_LEN(at); i++) {
			answering();
			fail_read_at = at[i];
			CHECK(run(true, &ho) == FR_NEXT_MAIN);
			expect_no_log_write(&ho);
		}
	}
}

/* ---- the keys at the start: Power + M4 alone acts ---- */

/* Each frame the start draws: the screen's kind and status text when its
 * RAMWR went out, its bytes, a digest of them, and the times of its first
 * and last bytes. */
static struct {
	uint8_t kind;
	char right[FR_STATUS_MAX + 1];
	unsigned bytes;
	uint32_t hash;
	uint64_t t0, t1;
} shown[8];
static unsigned n_shown;
static bool in_frame;

static void k_cmd(void *ctx, uint8_t c)
{
	in_frame = c == 0x2Cu && n_shown < ARRAY_LEN(shown);
	if (in_frame) {
		shown[n_shown].kind = st.screen.kind;
		memcpy(shown[n_shown].right, st.screen.status_right, sizeof shown[0].right);
		shown[n_shown].bytes = 0;
		shown[n_shown].hash = 2166136261u;
		shown[n_shown].t0 = shown[n_shown].t1 = t_us;
		n_shown++;
	}
	b_cmd(ctx, c);
}

static void k_data(void *ctx, const uint8_t *p, size_t n)
{
	if (in_frame) {
		for (size_t i = 0; i < n; i++)
			shown[n_shown - 1u].hash = (shown[n_shown - 1u].hash ^ p[i]) * 16777619u;
		shown[n_shown - 1u].bytes += (unsigned)n;
		shown[n_shown - 1u].t1 = t_us;
	}
	b_data(ctx, p, n);
}

/* A start with this 0x41 reply's payload, NULL for no reply. */
static void keys_setup(const uint8_t *keys)
{
	answering();
	psoc.answer_keys = keys != NULL;
	if (keys != NULL)
		memcpy(psoc.keys, keys, sizeof psoc.keys);
}

/* run() over a bus that records the frames. */
static enum fr_next keys_go(bool panel, struct fr_handoff *ho)
{
	static const struct fr_panel_bus kbus = { b_reset, b_select, k_cmd, k_data, NULL, NULL,
						  NULL, NULL, NULL };
	static struct fr_start_hw hw;

	memset(&hw, 0, sizeof hw);
	hw.clock = &clock;
	hw.psoc = &link;
	hw.panel = panel ? &kbus : NULL;
	hw.frame = panel_fb;
	hw.flash = &flash;
	hw.mem = &mem;
	memset(ho, 0, sizeof *ho);
	ho->strap = 0x0104u;
	t_us = 1000u;
	panel_bytes = 0;
	first_cmd_us = 0;
	n_frames = n_onoff = 0;
	n_shown = 0;
	in_frame = false;
	return fr_start_run(&st, &hw, ho);
}

static enum fr_next run_keys(bool panel, const uint8_t *keys, struct fr_handoff *ho)
{
	keys_setup(keys);
	return keys_go(panel, ho);
}

static struct sent base_log[LOG_MAX];
static unsigned base_n;

static bool same_log(void)
{
	if (psoc.n_log != base_n)
		return false;
	for (unsigned i = 0; i < base_n; i++) {
		const struct sent *a = &psoc.log[i], *b = &base_log[i];

		if (a->id != b->id || a->len != b->len || a->p0 != b->p0 || a->p1 != b->p1 ||
		    a->p2 != b->p2 || a->request != b->request)
			return false;
	}
	return true;
}

/* The flash's operations of the start with no key, to compare. */
static struct fop base_fops[FOPS_MAX];
static unsigned base_nfops;

static bool same_fops(void)
{
	if (n_fops != base_nfops)
		return false;
	for (unsigned i = 0; i < n_fops; i++)
		if (fops[i].kind != base_fops[i].kind || fops[i].off != base_fops[i].off ||
		    fops[i].len != base_fops[i].len || fops[i].val != base_fops[i].val)
			return false;
	return true;
}

/* A start with no key, kept to compare: its frames, PSoC messages, flash
 * operations and hand-off time. */
static unsigned bytes0;
static uint64_t handoff0;

static void base_start(uint8_t index)
{
	static const uint8_t none[9] = { 0 };
	struct fr_handoff ho;

	keys_setup(none);
	flash_reset(index, 0x01);
	CHECK(keys_go(true, &ho) == FR_NEXT_MAIN);
	CHECK(n_shown == 1u && n_frames == 1u && shown[0].kind == FR_SCREEN_LOGO);
	bytes0 = panel_bytes;
	handoff0 = ho.stamps[FR_STAMP_HANDOFF];
	memcpy(base_log, psoc.log, sizeof base_log);
	base_n = psoc.n_log;
	memcpy(base_fops, fops, sizeof base_fops);
	base_nfops = n_fops;
}

/* A start with these keys, against base_start(): the same in every way. */
static void same_as_no_key(const uint8_t *keys, uint8_t index)
{
	struct fr_handoff ho;

	keys_setup(keys);
	flash_reset(index, 0x01);
	CHECK(keys_go(true, &ho) == FR_NEXT_MAIN);
	CHECK((ho.flags & FR_HO_KEYS_REPLIED) != 0u);
	/* no announcement, no blink: the logo frame alone */
	CHECK(n_shown == 1u && n_frames == 1u && shown[0].kind == FR_SCREEN_LOGO);
	CHECK(shown[0].right[0] == '\0');
	CHECK(panel_bytes == bytes0 && n_onoff == 1u);
	/* no wait: neither the 2 s nor the 100 ms */
	CHECK(ho.stamps[FR_STAMP_HANDOFF] == handoff0);
	/* the PSoC's messages those of a start with no key: no restart */
	CHECK(same_log());
	CHECK(count_id(0x97u, 0) == 0u);
	/* the flash's operations the same: no selector write, the log step */
	CHECK(same_fops());
	CHECK(fmem[FR_SELECTOR_OFF] == index && fmem[FR_SELECTOR_OFF + 1u] == 0x01u);
	CHECK((ho.flags & FR_HO_START_LOGGED) != 0u);
	CHECK(ho.chord == 0u && ho.chord_result == 0u && (ho.flags & FR_HO_CHORD_SHOWN) == 0u);
	CHECK(st.screen.kind == FR_SCREEN_LOGO);
}

/* S-15.132: the early logo frame is the logo alone, the same bytes whether
 * the loaded descriptor parses or not. */
static void test_logo_alone(void)
{
	static const uint8_t none[9] = { 0 };
	struct fr_handoff ho;
	uint32_t hash;

	CHECK(run_keys(true, none, &ho) == FR_NEXT_MAIN);
	CHECK(st.desc_ok && n_shown == 1u && shown[0].kind == FR_SCREEN_LOGO);
	CHECK(shown[0].right[0] == '\0' && shown[0].bytes == 215040u);
	hash = shown[0].hash;
	keys_setup(none);
	desc[FR_DESC_O_MAGIC] ^= 0x01u;		/* no descriptor */
	CHECK(keys_go(true, &ho) == FR_NEXT_RECOVERY && st.why == FR_WHY_CHECK);
	CHECK(!st.desc_ok);
	/* the logo, then the line under it (S-13c.025) */
	CHECK(n_shown == 2u && shown[0].kind == FR_SCREEN_LOGO && shown[0].right[0] == '\0');
	CHECK(shown[1].bytes == FR_PANEL_LINE_BYTES);
	CHECK(shown[0].hash == hash && shown[0].bytes == 215040u);
	image(true);
}

/*
 * S-13c.025's front's line, S-15.151's "Before the first screen": on each
 * way into recovery (Power + M4, a failed image check, three unfinished
 * starts), right after the early logo frame, one logo line: the band
 * fr_paint_line() paints, after its CASET; none on a start that goes on
 * to the main image, and none with no panel. The front's frame buffer is
 * sent from, and the frames' bytes are the buffer's.
 */
static void test_recovery_line(void)
{
	static const uint8_t m4[9] = { 0, 0x02 }, none[9] = { 0 };
	static uint8_t band[FR_PANEL_LINE_BYTES];
	struct fr_handoff ho;
	uint32_t h = 2166136261u;

	fr_paint_line(band, "Loading recovery menu...");
	for (size_t i = 0; i < sizeof band; i++)
		h = (h ^ band[i]) * 16777619u;
	CHECK(run_keys(true, m4, &ho) == FR_NEXT_RECOVERY && st.why == FR_WHY_KEYS);
	CHECK(n_shown == 2u && shown[0].kind == FR_SCREEN_LOGO && shown[0].bytes == 215040u);
	CHECK(shown[1].bytes == sizeof band && shown[1].hash == h);
	CHECK(memcmp(panel_fb, band, sizeof band) == 0 && !st.drawn_ok);
	keys_setup(none);
	image(false);
	CHECK(keys_go(true, &ho) == FR_NEXT_RECOVERY && st.why == FR_WHY_CHECK);
	CHECK(n_shown == 2u && shown[1].bytes == sizeof band && shown[1].hash == h);
	answering();
	for (unsigned k = 0; k < 3u; k++)
		CHECK(run(true, &ho) == FR_NEXT_MAIN);
	CHECK(keys_go(true, &ho) == FR_NEXT_RECOVERY && st.why == FR_WHY_COUNT);
	CHECK(n_shown == 2u && shown[1].bytes == sizeof band && shown[1].hash == h);
	/* none on the way to the main image, or with no panel */
	CHECK(run_keys(true, none, &ho) == FR_NEXT_MAIN && n_shown == 1u);
	CHECK(run_keys(false, m4, &ho) == FR_NEXT_RECOVERY && n_shown == 0u && panel_bytes == 0u);
	image(true);
}

/* Key 1 or key 2 alone (the old Power + 1 and Power + 2), with or without
 * precomp, and key 4 with step 9 or step 16 (S-07.033), with either slot
 * selected: a start as with no key. */
static void test_no_slot_chords(void)
{
	static const uint8_t keys[][9] = {
		{ 0x40 }, { 0x80 }, { 0x40, 0, 0x80 }, { 0x80, 0, 0x80 },
		{ 0, 0x02, 0, 0, 0, 0, 0, 0, 0x01 }, { 0, 0x02, 0, 0, 0, 0, 0, 0, 0x80 },
	};

	for (unsigned sel = 0; sel < 2u; sel++) {
		base_start((uint8_t)sel);
		for (unsigned i = 0; i < ARRAY_LEN(keys); i++)
			same_as_no_key(keys[i], (uint8_t)sel);
	}
}

/* The hold key by either bit (S-07.032), alone and with another key, and
 * the debug and precomp bits: no blink, no pause, nothing more sent. */
static void test_no_blink(void)
{
	static const uint8_t keys[][9] = {
		{ 0, 0, 0, 0x01 }, { 0, 0x20 }, { 0x40, 0, 0, 0x01 }, { 0, 0x02, 0, 0x01 },
		{ 0x80, 0x20 }, { 0, 0x04 }, { 0, 0x08 }, { 0, 0, 0x80 }, { 0, 0x0C, 0x80, 0xFE },
	};
	struct fr_handoff ho;

	base_start(0);
	for (unsigned i = 0; i < ARRAY_LEN(keys); i++)
		same_as_no_key(keys[i], 0);
	/* no reply: none seen, and the start the same but for the request's
	 * wait */
	CHECK(run_keys(true, NULL, &ho) == FR_NEXT_MAIN);
	CHECK((ho.flags & FR_HO_KEYS_REPLIED) == 0u);
	CHECK(n_shown == 1u && n_frames == 1u && panel_bytes == bytes0);
	/* Power + M4: recovery, whose screen recovery draws */
	{
		static const uint8_t m4[9] = { 0, 0x02 };

		CHECK(run_keys(true, m4, &ho) == FR_NEXT_RECOVERY);
		CHECK(st.why == FR_WHY_KEYS);
		/* the logo frame, then only the line under it, no blink */
		CHECK(n_shown == 2u && shown[1].bytes == FR_PANEL_LINE_BYTES);
	}
	/* no panel: nothing drawn, the same time */
	CHECK(run_keys(false, keys[0], &ho) == FR_NEXT_MAIN);
	handoff0 = ho.stamps[FR_STAMP_HANDOFF];
	{
		static const uint8_t none[9] = { 0 };

		CHECK(run_keys(false, none, &ho) == FR_NEXT_MAIN);
	}
	CHECK(panel_bytes == 0u && n_shown == 0u && ho.stamps[FR_STAMP_HANDOFF] == handoff0);
}

/* ---- recovery's loop, end to end (S-02.084, S-02.093, S-07.051) ---- */

#define FRONT_ENTRY 0x08002000u
#define FRONT_INIT  0x11A04000u
#define PAD_ADDR    0x88000000u

static size_t ldr_block(uint8_t *o, uint32_t target, const uint8_t *p, uint32_t n)
{
	const size_t k = ldr_hdr(o, 0, target, n);

	memcpy(o + k, p, n);
	return k + n;
}

/* rec_image()'s front: 0 the bytes and pin of every image, else another
 * front. */
static uint8_t g_front_alt;

/* A waffle-boot image: the front (the same bytes in every image), its
 * descriptor, 256 bytes `fill` to the region, and pad zero bytes in DDR.
 * d_out gets the descriptor's bytes. */
static size_t rec_image(uint8_t *img, uint8_t fill, uint16_t ver, uint32_t pad, uint8_t *d_out)
{
	static uint8_t fl1[32], fl2[64], d[FR_DESC_MAX], blk[256];
	static const uint8_t zero[0x10000];
	const uint32_t nreg = pad ? 2u : 1u, dsize = FR_DESC_HEAD + 8u * nreg + 4u;
	size_t n = 16;

	for (unsigned i = 0; i < sizeof fl1; i++)
		fl1[i] = (uint8_t)(i + 1u + g_front_alt);
	for (unsigned i = 0; i < sizeof fl2; i++)
		fl2[i] = (uint8_t)(3u * i + 7u);
	memset(blk, fill, sizeof blk);
	memset(d, 0, sizeof d);
	fr_put32(d + FR_DESC_O_MAGIC, FR_DESC_MAGIC);
	fr_put16(d + FR_DESC_O_VERSION, FR_DESC_VERSION);
	fr_put16(d + FR_DESC_O_SIZE, (uint16_t)dsize);
	fr_put32(d + FR_DESC_O_MAIN_ENTRY, REGION);
	fr_put32(d + FR_DESC_O_NREGIONS, nreg);
	fr_put32(d + FR_DESC_O_MAIN_CRC, fr_crc32(fr_crc32(0, blk, sizeof blk), zero, pad));
	fr_put32(d + FR_DESC_O_FRONT_CRC, fr_crc32(fr_crc32(0, fl1, sizeof fl1), fl2, sizeof fl2));
	fr_put16(d + FR_DESC_O_BL_VERSION, ver);
	memset(d + FR_DESC_O_FRONT_PIN, 0xA5 ^ g_front_alt, FR_SHA256_LEN);
	fr_put32(d + FR_DESC_O_REGIONS, REGION);
	fr_put32(d + FR_DESC_O_REGIONS + 4u, sizeof blk);
	if (pad) {
		fr_put32(d + FR_DESC_O_REGIONS + 8u, PAD_ADDR);
		fr_put32(d + FR_DESC_O_REGIONS + 12u, pad);
	}
	fr_put32(d + dsize - 4u, fr_crc32(0, d, dsize - 4u));
	n += ldr_block(img + n, FRONT_INIT, fl1, sizeof fl1);
	n += ldr_hdr(img + n, 0x0800u, FRONT_INIT, 0);
	n += ldr_block(img + n, FRONT_ENTRY, fl2, sizeof fl2);
	n += ldr_block(img + n, FR_DESC_ADDR, d, dsize);
	n += ldr_block(img + n, REGION, blk, sizeof blk);
	if (pad)
		n += ldr_block(img + n, PAD_ADDR, zero, pad);
	n += ldr_hdr(img + n, 0x8000u, FRONT_ENTRY, 0);
	(void)ldr_hdr(img, 0x5000u, FRONT_ENTRY, 0);	/* FIRST, IGNORE */
	if (d_out != NULL)
		memcpy(d_out, d, dsize);
	CHECK(fr_image_valid(img, (uint32_t)n, NULL) == FR_IMG_OK);
	return n;
}

static void put_image(unsigned k, const uint8_t *img, size_t n)
{
	memset(fmem + FR_SLOT_BASE(k), 0xFF, FR_SLOT_CAP);
	memcpy(fmem + FR_SLOT_BASE(k), img, n);
	memset(fmem + FR_RECORD_OFF(k), 0xFF, FR_SECTOR);
	fr_put32(fmem + FR_RECORD_OFF(k), fr_crc32(0, img, n));
	fr_put32(fmem + FR_RECORD_OFF(k) + 4u, (uint32_t)n);
	fr_put32(fmem + FR_RECORD_OFF(k) + 8u, FR_TAG);
}

/* The host on recovery's USB serial port: a script of requests, one at a
 * time, each sent once the reply to the one before has come. */
enum { H_BEGIN, H_DATA, H_END, H_INFO_WAIT, H_INFO, H_OFF, H_RESTART, H_CUT, H_STOP };

struct hstep {
	uint8_t kind;
	uint8_t flags;		/* H_BEGIN's flags byte */
	uint32_t len;		/* H_BEGIN's length; 0xFFFFFFFF: the image's */
};

#define WHOLE 0xFFFFFFFFu

static uint8_t h2d[2048], d2h[4096];
static size_t h2d_n, h2d_at, d2h_n;
static bool usb_conf;
static uint32_t usb_ma_v;

static struct {
	const struct hstep *steps;
	unsigned at;
	const uint8_t *img;
	uint32_t n, off, chunk;
	uint8_t sha[FR_SHA256_LEN];
	bool waiting;
	uint8_t seq;
	uint8_t begin_st, end_st, restart_st;
	uint8_t begin_way, begin_target;
	uint64_t end_out;	/* END's reply handed to the port, in g_order */
	uint64_t done_at;	/* INFO first showed the write ended */
	uint8_t info[FR_INFO_LEN];
	unsigned infos;
	unsigned replug;	/* H_CUT: 1 to leave once its bytes are read, 2 to come back */
} host;

static void host_reply(uint8_t type, const uint8_t *pl, size_t n)
{
	host.waiting = false;
	switch (type & 0x7Fu) {
	case FR_T_BEGIN:
		CHECK(n == FR_BEGIN_REPLY_LEN);
		host.begin_st = pl[0];
		host.begin_way = n > 1u ? pl[1] : 0xEEu;
		host.begin_target = n > 2u ? pl[2] : 0xEEu;
		host.off = 0;
		host.at++;
		break;
	case FR_T_RESTART:
		host.restart_st = pl[0];
		host.at++;
		break;
	case FR_T_DATA:
		if (pl[0] == 0u)
			host.off += host.chunk;
		if (pl[0] != 0u || host.off >= host.n)
			host.at++;
		break;
	case FR_T_END:
		host.end_st = pl[0];
		host.end_out = ++g_order;
		host.at++;
		break;
	case FR_T_INFO:
		memcpy(host.info, pl, n < FR_INFO_LEN ? n : FR_INFO_LEN);
		host.infos++;
		if (host.steps[host.at].kind == H_INFO) {
			host.at++;
		} else if (pl[FR_INFO_O_PHASE] != FR_PH_WRITING &&
			   pl[FR_INFO_O_PHASE] != FR_PH_COUNTDOWN) {
			host.done_at = ++g_order;
			host.at++;
		}
		break;
	default:
		break;
	}
}

static void host_send(uint8_t type, const uint8_t *pl, size_t n)
{
	uint8_t *o = h2d;

	o[0] = 'W';
	o[1] = 'R';
	o[2] = type;
	o[3] = ++host.seq;
	fr_put16(o + 4, (uint16_t)n);
	if (n)
		memcpy(o + 6, pl, n);
	fr_put32(o + 6 + n, fr_crc32(0, o + 2, 4 + n));
	h2d_n = 6 + n + 4;
	h2d_at = 0;
	host.waiting = true;
}

static void host_next(void)
{
	static uint8_t pl[4 + FR_PROTO_MAX_DATA];

	while (!host.waiting && h2d_at == h2d_n && host.replug == 0u) {
		const struct hstep *s = &host.steps[host.at];

		switch (s->kind) {
		case H_BEGIN:
			fr_put32(pl, s->len == WHOLE ? host.n : s->len);
			memcpy(pl + 4, host.sha, FR_SHA256_LEN);
			pl[4 + FR_SHA256_LEN] = s->flags;
			host_send(FR_T_BEGIN, pl, 4 + FR_SHA256_LEN + 1);
			return;
		case H_DATA:
			host.chunk = host.n - host.off < FR_PROTO_MAX_DATA ? host.n - host.off :
									  FR_PROTO_MAX_DATA;
			fr_put32(pl, host.off);
			memcpy(pl + 4, host.img + host.off, host.chunk);
			host_send(FR_T_DATA, pl, 4u + host.chunk);
			return;
		case H_END:
			host_send(FR_T_END, NULL, 0);
			return;
		case H_INFO_WAIT:
		case H_INFO:
			host_send(FR_T_INFO, NULL, 0);
			return;
		case H_OFF:
			psoc.off_at = t_us + 1u;
			host.at++;
			break;
		case H_RESTART:
			host_send(FR_T_RESTART, NULL, 0);
			return;
		case H_CUT:
			/* the head of an INFO that claims 256 bytes, and four
			 * of them; then the host leaves and another comes */
			host_send(FR_T_INFO, pl, 4);
			h2d[4] = 0x00;
			h2d[5] = 0x01;
			h2d_n = 10;
			host.waiting = false;
			host.replug = 1;
			host.at++;
			return;
		case H_STOP:
		default:
			return;
		}
	}
}

static size_t s_read(void *ctx, uint8_t *buf, size_t max)
{
	size_t k;

	(void)ctx;
	host_next();
	k = h2d_n - h2d_at < max ? h2d_n - h2d_at : max;
	memcpy(buf, h2d + h2d_at, k);
	h2d_at += k;
	return k;
}

static size_t s_write(void *ctx, const uint8_t *buf, size_t n)
{
	(void)ctx;
	CHECK(d2h_n + n <= sizeof d2h);
	if (d2h_n + n > sizeof d2h)
		return n;
	memcpy(d2h + d2h_n, buf, n);
	d2h_n += n;
	/* each whole reply, as it is handed to the port */
	while (d2h_n >= 6u) {
		const size_t len = fr_le16(d2h + 4);

		if (d2h_n < 6u + len + 4u)
			break;
		CHECK(d2h[0] == 'W' && d2h[1] == 'R');
		CHECK(fr_crc32(0, d2h + 2, 4 + len) == fr_le32(d2h + 6 + len));
		host_reply(d2h[2], d2h + 6, len);
		memmove(d2h, d2h + 6 + len + 4, d2h_n - (6 + len + 4));
		d2h_n -= 6 + len + 4;
	}
	return n;
}

static bool s_idle(void *ctx)
{
	(void)ctx;
	return true;
}

static const struct fr_bytes serial = { s_read, s_write, s_idle, NULL };

static void u_start(const struct fr_clock *c) { (void)c; }
/* A pass of the loop takes time even where nothing looks at the clock. A
 * loop that runs on past rec_stuck_at is left (the test then fails). */
static jmp_buf rec_escape;
static uint64_t rec_stuck_at;

/* S-02.084's countdown passes in 1500 passes of the loop, and the time it
 * takes moves the deadlines on: they count the time outside it. */
#define CD_STEP 10000u
static uint64_t cd_time;		/* the time spent in countdowns */

static void u_poll(void)
{
	t_us += 5u;
	if (st.proto.phase == FR_PH_COUNTDOWN) {
		t_us += CD_STEP;
		cd_time += CD_STEP;
		if (psoc.deadline != 0u)
			psoc.deadline += CD_STEP;
		if (rec_stuck_at != 0u)
			rec_stuck_at += CD_STEP;
	}
	if (host.replug == 1u && h2d_at == h2d_n) {
		usb_conf = false;
		host.replug = 2;
	} else if (host.replug == 2u) {
		usb_conf = true;
		host.replug = 0;
	}
	if (rec_stuck_at != 0u && t_us > rec_stuck_at) {
		rec_stuck_at = 0;
		longjmp(rec_escape, 1);
	}
}
static bool u_configured(void) { return usb_conf; }
static uint32_t u_ma(void) { return usb_ma_v; }

static unsigned rec_log_from;	/* the PSoC log's first entry of recovery */
static uint64_t rec_t0;		/* the clock when recovery's loop began */
static uint64_t rec_deadline_us = 3000000u;
static bool rec_deadline_ok;	/* the deadline's power-off is the test's own */

/* Power + M4 with the PSoC answering, the flash as it is, no host yet. */
static void rec_reset(void)
{
	answering();
	psoc.keys[1] = 0x02u;
	map_soft = true;
	n_fops = 0;
	n_frames = 0;
	g_screen = NULL;
	erase_us = 1000;	/* an erase takes 1 ms */
	first_erase_order = 0;
	erase_hook = NULL;
	sel_fail_from = UINT64_MAX;
	arm_sel_fail = false;
	hook_off = FR_SLOT_BASE(0);
	hook_count = 0;
	n_erase_faults = 0;
	sel_prog_lazy = false;
	usb_conf = false;
	usb_ma_v = 900;
	kscript = NULL;
	kscript_n = 0;
	kscript_t0 = 0;
	memset(&host, 0, sizeof host);
	h2d_n = h2d_at = d2h_n = 0;
}

/* Slot 0 runs and is selected, holding image A; slot 1 bad (erased), or
 * good holding image C, both with A's front; Power + M4. */
static void rec_setup(bool slot1_good)
{
	static uint8_t a[0x800];
	size_t n;

	rec_reset();
	n = rec_image(a, 0x3C, 1, 0, desc);	/* the loaded descriptor is A's */
	put_image(0, a, n);
	if (slot1_good) {
		static uint8_t c[0x800];

		n = rec_image(c, 0x77, 3, 0, NULL);
		put_image(1, c, n);
	} else {
		memset(fmem + FR_SLOT_BASE(1), 0xFF, FR_SLOT_CAP);
		memset(fmem + FR_RECORD_OFF(1), 0xFF, FR_SECTOR);
	}
}

static enum fr_next rec_go(const struct hstep *steps, const uint8_t *img, size_t n, bool silent36)
{
	static struct fr_start_hw hw;
	static uint8_t staging[FR_SLOT_CAP];
	struct fr_handoff ho;
	struct fr_sha256 sh;
	enum fr_next rc;

	host.steps = steps;
	host.img = img;
	host.n = (uint32_t)n;
	fr_sha256_init(&sh);
	fr_sha256_add(&sh, img, n);
	fr_sha256_end(&sh, host.sha);
	memset(&hw, 0, sizeof hw);
	hw.clock = &clock;
	hw.psoc = &link;
	hw.panel = &bus;
	hw.frame = panel_fb;
	hw.flash = &flash;
	hw.mem = &mem;
	hw.usb_start = u_start;
	hw.usb_poll = u_poll;
	hw.usb_configured = u_configured;
	hw.usb_ma = u_ma;
	hw.serial = &serial;
	hw.staging = staging;
	memset(&ho, 0, sizeof ho);
	ho.strap = 0x0104u;
	t_us = 1000u;
	rc = fr_start_run(&st, &hw, &ho);
	CHECK(rc == FR_NEXT_RECOVERY && st.why == FR_WHY_KEYS);
	if (rc != FR_NEXT_RECOVERY)
		return rc;
	g_screen = &st.screen;
	rec_log_from = psoc.n_log;
	psoc.silent_36 = silent36;
	rec_t0 = t_us;
	kscript_t0 = t_us;
	g_phase = &st.proto.phase;
	cd_time = 0;
	psoc.deadline = t_us + rec_deadline_us;	/* a power-off when it is over */
	rec_stuck_at = t_us + rec_deadline_us + 5000000u;
	if (setjmp(rec_escape) != 0) {
		CHECK(0);	/* the loop never ended */
		rc = FR_NEXT_HALT;
	} else {
		rc = fr_recovery_run(&st);
	}
	rec_stuck_at = 0;
	g_phase = NULL;
	/* S-02.084: the keys are read only while nothing is received, counted
	 * down to or written */
	for (unsigned i = rec_log_from; i < psoc.n_log; i++)
		if (psoc.log[i].id == 0x41u && psoc.log[i].request)
			CHECK(psoc.log[i].phase == FR_PH_IDLE || psoc.log[i].phase == FR_PH_DONE ||
			      psoc.log[i].phase == FR_PH_FAILED);
	CHECK(!psoc.deadline_sent || rec_deadline_ok);
	rec_deadline_us = 3000000u;
	rec_deadline_ok = false;
	map_soft = false;
	erase_hook = NULL;
	erase_us = 0;
	g_screen = NULL;
	return rc;
}

/* The first entry of recovery's PSoC log from `from` with this id (and p0,
 * when p0 >= 0), or ~0u. */
static unsigned sent_at(uint8_t id, int p0, unsigned from)
{
	for (unsigned i = from; i < psoc.n_log; i++)
		if (psoc.log[i].id == id && (p0 < 0 || psoc.log[i].p0 == (uint8_t)p0))
			return i;
	return ~0u;
}

static unsigned requests_of(uint8_t id)
{
	unsigned n = 0;

	for (unsigned i = rec_log_from; i < psoc.n_log; i++)
		n += psoc.log[i].id == id && psoc.log[i].request ? 1u : 0u;
	return n;
}

/* S-07.051's power-off in recovery's log: the shutdown report, the LED
 * clear and 0x32 {14} in a row, and none of them before. Its order, or 0. */
static uint64_t poweroff_order(void)
{
	unsigned i;

	for (i = rec_log_from; i < psoc.n_log; i++)
		if ((psoc.log[i].id == 0x51u && psoc.log[i].len == 3u && psoc.log[i].p0 == 0x00u &&
		     psoc.log[i].p2 == 0x02u) ||
		    (psoc.log[i].id == 0x40u && psoc.log[i].len == 1u && psoc.log[i].p0 == 0x01u) ||
		    psoc.log[i].id == 0x32u)
			break;
	if (i >= psoc.n_log)
		return 0;
	CHECK(psoc.log[i].id == 0x51u && psoc.log[i].p0 == 0x00u && psoc.log[i].p2 == 0x02u);
	CHECK(i + 2u < psoc.n_log && psoc.log[i + 1u].id == 0x40u && psoc.log[i + 1u].p0 == 0x01u);
	CHECK(i + 2u < psoc.n_log && psoc.log[i + 2u].id == 0x32u && psoc.log[i + 2u].p0 == 0x14u);
	CHECK(i + 3u == psoc.n_log);		/* then nothing */
	return psoc.log[i].order;
}

static uint64_t last_fop_order(void)
{
	return n_fops ? fops[n_fops - 1u].order : 0u;
}

static void hook_off_ping_usb(uint32_t off)
{
	if (off != hook_off || hook_count != 1u)
		return;
	psoc.off_at = t_us + 300u;
	psoc.ping_at = t_us + 300u;
	usb_conf = true;
}

static void hook_ping_usb(uint32_t off)
{
	if (off != hook_off || hook_count != 1u)
		return;
	psoc.ping_at = t_us + 300u;
	usb_conf = true;
}

static void hook_poweroff(uint32_t off)
{
	if (off != hook_off || hook_count != 1u)
		return;
	psoc.off_at = t_us + 300u;
	usb_conf = true;
}

static uint8_t img_w[0x11000];

/* S-02.084: a plain write of slot 1 that fails (its first erase, which
 * erases nothing), after which an in-place write may go. */
#define FAIL_OTHER { H_BEGIN, 0, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 }, { H_INFO_WAIT, 0, 0 }

static void fail_other_fault(void)
{
	erase_fault(FR_SLOT_BASE(1), false);
}

/* SC-RECOVERY run 10, through recovery's loop: slot 1 bad, the running
 * slot written in place, with the power switch turned off after the first
 * erase. */
static void test_rec_in_place_poweroff(void)
{
	static const struct hstep steps[] = {
		FAIL_OTHER,
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 }, { H_STOP, 0, 0 },
	};
	const size_t n = rec_image(img_w, 0x5A, 2, 0x10000, NULL);
	uint64_t last, off;
	unsigned pong;
	bool drawn = false;

	rec_setup(false);
	fail_other_fault();
	erase_hook = hook_off_ping_usb;
	CHECK(rec_go(steps, img_w, n, false) == FR_NEXT_HALT);
	CHECK(host.begin_st == FR_WR_OK && host.end_st == FR_WR_OK);
	CHECK(host.begin_way == FR_WAY_IN_PLACE && host.begin_target == 0u);
	CHECK(st.proto.w.way == FR_WAY_IN_PLACE);
	last = last_fop_order();
	/* (a) no erase of slot 0 before END's reply is out and the in-place
	 * screen drawn */
	CHECK(first_erase_order != 0u && first_erase_order > host.end_out);
	for (unsigned i = 0; i < n_fops; i++)
		CHECK(fops[i].kind != 'e' || fops[i].off < FR_SLOT_BASE(0) ||
		      fops[i].off >= FR_SLOT_BASE(1) || fops[i].order > host.end_out);
	for (unsigned i = 0; i < n_frames; i++)
		if (frames[i].in_place && frames[i].order < first_erase_order)
			drawn = true;
	CHECK(drawn);
	/* (c) no frame and no limit request while the write runs, though USB
	 * came up asking 900 mA */
	for (unsigned i = 0; i < n_frames; i++)
		CHECK(frames[i].order < first_erase_order || frames[i].order > last);
	for (unsigned i = rec_log_from; i < psoc.n_log; i++)
		if (psoc.log[i].order > first_erase_order && psoc.log[i].order <= last)
			CHECK(psoc.log[i].id != 0x33u && psoc.log[i].id != 0x34u);
	CHECK(st.psoc.target_ma == 900u);
	/* (d) the ping during the write answered */
	pong = sent_at(0x02u, 0x12, rec_log_from);
	CHECK(pong != ~0u && psoc.log[pong].order > first_erase_order && psoc.log[pong].order < last);
	/* (b) the power-off came during the write and ran only after it, at once */
	CHECK(psoc.off_sent && psoc.off_order > first_erase_order && psoc.off_order < last);
	off = poweroff_order();
	CHECK(off > last);
	/* the write complete: slot 0 holds the image, the selector as it was */
	CHECK(memcmp(fmem + FR_SLOT_BASE(0), img_w, n) == 0);
	{
		bool good = false;

		CHECK(fr_check_slot(&flash, 0, &good, NULL) == FR_FLASH_OK && good);
	}
	CHECK(fmem[FR_SELECTOR_OFF] == 0u && fmem[FR_SELECTOR_OFF + 1u] == 1u);
	for (unsigned i = 0; i < n_fops; i++)
		CHECK(fops[i].kind == 'r' || fops[i].off < FR_SELECTOR_OFF ||
		      fops[i].off >= FR_SELECTOR_OFF + FR_SECTOR);
	/* (g) one battery request, at BEGIN; INFO carries its report */
	CHECK(requests_of(0x36u) == 1u);
	CHECK(psoc.log[sent_at(0x36u, -1, rec_log_from)].order < host.end_out);
	CHECK(host.infos > 0u && host.info[FR_INFO_O_BATT_LEN] == 4u);
	CHECK(host.info[FR_INFO_O_BATT] == 1u && host.info[FR_INFO_O_BATT + 1] == 80u);
}

/* (f) The same write with no power-off message: phase done, the hold
 * released, the current limit's steps again, and a later 0x31 {14} powers
 * off at once. */
static void test_rec_in_place_done(void)
{
	static const struct hstep steps[] = {
		FAIL_OTHER,
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 }, { H_OFF, 0, 0 }, { H_STOP, 0, 0 },
	};
	const size_t n = rec_image(img_w, 0x5A, 2, 0x10000, NULL);
	uint64_t last, off;
	bool frame_after = false;

	rec_setup(false);
	fail_other_fault();
	erase_hook = hook_ping_usb;
	CHECK(rec_go(steps, img_w, n, false) == FR_NEXT_HALT);
	last = last_fop_order();
	CHECK(st.proto.phase == FR_PH_DONE && st.proto.result == FR_WR_OK);
	CHECK(host.info[FR_INFO_O_PHASE] == FR_PH_DONE);
	CHECK(host.info[FR_INFO_O_WAY] == FR_WAY_IN_PLACE);
	CHECK(!st.psoc.hold_off && !st.psoc.off_due);
	for (unsigned i = 0; i < n_frames; i++) {
		CHECK(frames[i].order < first_erase_order || frames[i].order > last);
		frame_after |= frames[i].order > last;
	}
	CHECK(frame_after);		/* the done screen drawn */
	off = poweroff_order();
	CHECK(psoc.off_sent && psoc.off_order > host.done_at && off > psoc.off_order);
	/* the limit steps again once the write has ended */
	{
		const unsigned v = sent_at(0x33u, -1, rec_log_from);

		CHECK(v != ~0u && psoc.log[v].order > last);
	}
	CHECK(memcmp(fmem + FR_SLOT_BASE(0), img_w, n) == 0);
}

/*
 * (e) A plain write and a write of the running slot with the other slot
 * selected first: the power-off message ends recovery at once, as
 * S-07.051 says, with no hold. This leaves the target slot partial: the
 * slot being written is neither its old image nor the new one. In the
 * plain write that is the slot the selector does not name, so the unit
 * still starts its selected slot; in the select-first write the selector
 * already names the other slot, which every reset then starts.
 */
static void test_rec_other_ways_poweroff(void)
{
	for (unsigned way = 0; way < 2u; way++) {
		const struct hstep steps[] = {
			{ H_BEGIN, way ? FR_BEGIN_F_RUNNING : 0u, WHOLE }, { H_DATA, 0, 0 },
			{ H_END, 0, 0 }, { H_INFO_WAIT, 0, 0 }, { H_STOP, 0, 0 },
		};
		const size_t n = rec_image(img_w, 0x5A, 2, 0x10000, NULL);
		const unsigned t = way ? 0u : 1u;
		uint64_t off;
		bool good = true;

		rec_setup(way != 0u);
		hook_off = FR_SLOT_BASE(t);
		erase_hook = hook_poweroff;
		CHECK(rec_go(steps, img_w, n, false) == FR_NEXT_HALT);
		CHECK(st.proto.w.way == (way ? FR_WAY_SELECT_FIRST : FR_WAY_PLAIN));
		CHECK(st.proto.phase == FR_PH_WRITING);	/* never ended */
		off = poweroff_order();
		CHECK(off != 0u && psoc.off_order > first_erase_order);
		CHECK(last_fop_order() < off);		/* nothing after it */
		CHECK(fr_check_slot(&flash, t, &good, NULL) == FR_FLASH_OK && !good);
		CHECK(memcmp(fmem + FR_SLOT_BASE(t), img_w, n) != 0);
		CHECK(requests_of(0x36u) == 0u);
		if (way) {
			/* {01 01}: the other slot starts */
			CHECK(fmem[FR_SELECTOR_OFF] == 1u && fmem[FR_SELECTOR_OFF + 1u] == 1u);
			CHECK(fr_check_slot(&flash, 1, &good, NULL) == FR_FLASH_OK && good);
		} else {
			CHECK(fmem[FR_SELECTOR_OFF] == 0u && fmem[FR_SELECTOR_OFF + 1u] == 1u);
			CHECK(fr_check_slot(&flash, 0, &good, NULL) == FR_FLASH_OK && good);
		}
	}
}

/* (g) The battery request: none for a refused in-place BEGIN or another
 * way; with the PSoC silent to it, INFO's report is empty. */
static void test_rec_battery(void)
{
	static const struct hstep size0[] = {
		FAIL_OTHER,
		{ H_BEGIN, FR_BEGIN_F_RUNNING, 0 }, { H_INFO, 0, 0 }, { H_OFF, 0, 0 }, { H_STOP, 0, 0 },
	};
	static const struct hstep running[] = {
		FAIL_OTHER,
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_INFO, 0, 0 }, { H_OFF, 0, 0 },
		{ H_STOP, 0, 0 },
	};
	static const struct hstep running_sf[] = {
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_INFO, 0, 0 }, { H_OFF, 0, 0 },
		{ H_STOP, 0, 0 },
	};
	static const struct hstep plain[] = {
		{ H_BEGIN, 0, WHOLE }, { H_INFO, 0, 0 }, { H_OFF, 0, 0 }, { H_STOP, 0, 0 },
	};
	const size_t n = rec_image(img_w, 0x5A, 2, 0x100, NULL);

	/* refused for its size */
	rec_setup(false);
	fail_other_fault();
	CHECK(rec_go(size0, img_w, n, false) == FR_NEXT_HALT);
	CHECK(host.begin_st == FR_WR_SIZE);
	CHECK(requests_of(0x36u) == 0u);
	/* the default write, slot 1 bad */
	rec_setup(false);
	CHECK(rec_go(plain, img_w, n, false) == FR_NEXT_HALT);
	CHECK(host.begin_st == FR_WR_OK && requests_of(0x36u) == 0u);
	/* the running slot with slot 1 good: selected first, no request */
	rec_setup(true);
	CHECK(rec_go(running_sf, img_w, n, false) == FR_NEXT_HALT);
	CHECK(host.begin_st == FR_WR_OK && requests_of(0x36u) == 0u);
	CHECK(host.info[FR_INFO_O_RUN_WAY] == FR_WAY_SELECT_FIRST);
	/* in place, answered: one request, its report in INFO */
	rec_setup(false);
	fail_other_fault();
	CHECK(rec_go(running, img_w, n, false) == FR_NEXT_HALT);
	CHECK(host.begin_st == FR_WR_OK && requests_of(0x36u) == 1u);
	CHECK(host.info[FR_INFO_O_BATT_LEN] == 4u);
	/* in place, the PSoC silent to it: one request, no report, though the
	 * start-up had one */
	rec_setup(false);
	fail_other_fault();
	CHECK(rec_go(running, img_w, n, true) == FR_NEXT_HALT);
	CHECK(host.begin_st == FR_WR_OK && requests_of(0x36u) == 1u);
	CHECK(host.info[FR_INFO_O_BATT_LEN] == 0u);
	for (unsigned i = 0; i < FR_BATT_MAX; i++)
		CHECK(host.info[FR_INFO_O_BATT + i] == 0u);
}

/* A second power-off message, sent at the plain write of slot 1's first
 * erase (after the one at the in-place write's). */
static uint64_t off1_order;

static void hook_two_offs(uint32_t off)
{
	if (off == FR_SLOT_BASE(0) && hook_count == 1u) {
		psoc.off_at = t_us + 300u;
	} else if (off == FR_SLOT_BASE(1) && psoc.off_sent) {
		off1_order = psoc.off_order;
		psoc.off_sent = false;
		psoc.off_at = t_us + 300u;
	}
}

/*
 * SC-RECOVERY run 14 through the loop: an in-place write that fails, with
 * a 0x31 {14} kept from during it. The hold lasts through the failure, so
 * no shutdown report, LED clear or 0x32 follows it; RESTART is answered
 * 0x44 with no reset request. Then (i) a retry in place that succeeds, or
 * (ii) a plain write of slot 1 that succeeds (selector {01 00}, slot 1
 * good), during which a second power-off message is kept as well: the
 * power-off runs only once a good slot is selected again, and the loop
 * returns FR_NEXT_HALT.
 */
static void test_rec_in_place_fails(void)
{
	for (unsigned then = 0; then < 2u; then++) {
		const struct hstep steps[] = {
			FAIL_OTHER,
			{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
			{ H_INFO_WAIT, 0, 0 }, { H_RESTART, 0, 0 },
			{ H_BEGIN, (uint8_t)(then ? 0u : FR_BEGIN_F_RUNNING), WHOLE }, { H_DATA, 0, 0 },
			{ H_END, 0, 0 }, { H_INFO_WAIT, 0, 0 }, { H_STOP, 0, 0 },
		};
		const size_t n = rec_image(img_w, 0x5A, 2, 0x10000, NULL);
		uint64_t last, off;
		bool good = false;

		rec_setup(false);
		fail_other_fault();
		/* the in-place write's second erase takes, then fails */
		erase_fault(FR_SLOT_BASE(0) + FR_SECTOR, true);
		off1_order = 0;
		erase_hook = hook_two_offs;
		CHECK(rec_go(steps, img_w, n, false) == FR_NEXT_HALT);
		last = last_fop_order();
		/* every request answered but the last INFO, which the power-off
		 * at the write's end may cut short */
		CHECK(host.at >= ARRAY_LEN(steps) - 2u);
		CHECK(host.restart_st == FR_ST_DAMAGED);
		CHECK(sent_at(0x97u, -1, rec_log_from) == ~0u);	/* no reset request */
		CHECK(host.begin_st == FR_WR_OK && host.end_st == FR_WR_OK);
		CHECK(st.proto.phase == FR_PH_DONE && st.proto.result == FR_WR_OK);
		/* the power-off after the last write's end, once */
		off = poweroff_order();
		CHECK(off != 0u && off > last);
		if (then == 0u) {
			CHECK(psoc.off_order < last);
			CHECK(host.begin_way == FR_WAY_IN_PLACE);
			CHECK(memcmp(fmem + FR_SLOT_BASE(0), img_w, n) == 0);
			CHECK(fr_check_slot(&flash, 0, &good, NULL) == FR_FLASH_OK && good);
			CHECK(fmem[FR_SELECTOR_OFF] == 0u && fmem[FR_SELECTOR_OFF + 1u] == 1u);
		} else {
			/* both messages came before the plain write ended */
			CHECK(off1_order != 0u && off1_order < last);
			CHECK(psoc.off_order > off1_order && psoc.off_order < last);
			CHECK(host.begin_way == FR_WAY_PLAIN && host.begin_target == 1u);
			CHECK(fr_check_slot(&flash, 1, &good, NULL) == FR_FLASH_OK && good);
			CHECK(fmem[FR_SELECTOR_OFF] == 1u && fmem[FR_SELECTOR_OFF + 1u] == 1u);
			CHECK(fr_check_slot(&flash, 0, &good, NULL) == FR_FLASH_OK && !good);
		}
	}
}

/* Slot 1 holds image A and runs, selected {01 01}; slot 0 erased (bad). */
static void rec_setup_run1(void)
{
	static uint8_t a[0x800];
	size_t n;

	rec_setup(false);
	memset(fmem + FR_SLOT_BASE(0), 0xFF, FR_SLOT_CAP);
	memset(fmem + FR_RECORD_OFF(0), 0xFF, FR_SECTOR);
	n = rec_image(a, 0x3C, 1, 0, desc);
	put_image(1, a, n);
	fmem[FR_SELECTOR_OFF] = 0x01;
	fmem[FR_SELECTOR_OFF + 1u] = 0x01;
}

static void hook_off_on_slot0(uint32_t off)
{
	if (off == FR_SLOT_BASE(0) && hook_count == 1u)
		psoc.off_at = t_us + 300u;
}

/*
 * Review case 1(a) (SC-RECOVERY run 16) through the loop: slot 1 runs;
 * a write of slot 0 fails, the in-place write of slot 1 fails with its
 * first erase taken, then a write of slot 0 whose index program is not
 * stored: result 11, the selector FF FF. A fallback to slot 0 does not
 * count (the first stage's own {00, 01} could be cut to {01, 01}): held,
 * with a power-off kept during that write. Then slot 0's own image without
 * the flag: the selector alone, settled {00, 01}; the hold ends and the
 * kept power-off runs.
 */
static void test_rec_rescue_case_a(void)
{
	static const struct hstep steps[] = {
		FAIL_OTHER,
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 },
		{ H_BEGIN, 0, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 }, { H_INFO_WAIT, 0, 0 },
		{ H_INFO, 0, 0 }, { H_RESTART, 0, 0 },
		{ H_BEGIN, 0, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 }, { H_INFO_WAIT, 0, 0 },
		{ H_STOP, 0, 0 },
	};
	const size_t n = rec_image(img_w, 0x5A, 2, 0x2000, NULL);
	uint64_t off, last;
	bool good = false;

	rec_setup_run1();
	erase_fault(FR_SLOT_BASE(0), false);	/* the first write of slot 0 */
	erase_fault(FR_SLOT_BASE(1), true);	/* the in-place write */
	sel_prog_lazy = true;			/* the next index program: not stored */
	hook_off = FR_SLOT_BASE(0);
	erase_hook = hook_off_on_slot0;		/* during the third write */
	CHECK(rec_go(steps, img_w, n, false) == FR_NEXT_HALT);
	last = last_fop_order();	/* before this test's own reads below */
	CHECK(st.rec.running.established && st.rec.running.slot == 1u);
	CHECK(host.restart_st == FR_ST_DAMAGED);	/* while FF FF */
	CHECK(st.proto.phase == FR_PH_DONE && st.proto.result == FR_WR_OK);
	CHECK(fmem[FR_SELECTOR_OFF] == 0x00u && fmem[FR_SELECTOR_OFF + 1u] == 0x01u);
	CHECK(fr_check_slot(&flash, 0, &good, NULL) == FR_FLASH_OK && good);
	CHECK(fr_check_slot(&flash, 1, &good, NULL) == FR_FLASH_OK && !good);
	CHECK(fr_rec_reach(&st.rec) == FR_REACH_NAMED);
	CHECK(!fr_proto_hold(&st.proto) && !st.psoc.hold_off);
	/* the kept power-off came during the third write, ran after the last */
	off = poweroff_order();
	CHECK(psoc.off_sent && psoc.off_order < last);
	CHECK(off != 0u && off > last);
	CHECK(sent_at(0x97u, -1, rec_log_from) == ~0u);
}

/* At the retry's first erase of the running slot: the selector set to
 * {00 00} (SC-RECOVERY run 22's start) and a power-off message. */
static void hook_sel00_off(uint32_t off)
{
	if (off != hook_off || hook_count != 1u)
		return;
	fmem[FR_SELECTOR_OFF] = 0x00u;
	fmem[FR_SELECTOR_OFF + 1u] = 0x00u;
	psoc.off_at = t_us + 300u;
}

/*
 * SC-RECOVERY run 22 through the loop (S-02.084). Slot 0 runs, slot 1 bad: a write of slot 1 fails,
 * the in-place write of slot 0 fails with its first erase taken, then the
 * in-place write again, during which the selector is set to {00 00} and a
 * power-off message comes; its settling's index program is not stored:
 * result 11, slot 0 good, FF FF. Held: the kept power-off does not run,
 * RESTART is answered 0x44, INFO has bits 5 and 7. Then slot 0's own image
 * with the flag: the selector alone, settled {00 01}; the hold ends and the
 * kept power-off runs. Mirrored with slot 1 running: FF FF names slot 1,
 * good, so nothing is held and the power-off runs once the write has ended.
 */
static void test_rec_unsettled(void)
{
	static const struct hstep steps0[] = {
		FAIL_OTHER,
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 },
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 }, { H_INFO, 0, 0 }, { H_RESTART, 0, 0 },
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 }, { H_STOP, 0, 0 },
	};
	static const struct hstep steps1[] = {
		FAIL_OTHER,
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 },
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 }, { H_INFO, 0, 0 }, { H_STOP, 0, 0 },
	};
	const size_t n = rec_image(img_w, 0x5A, 2, 0x2000, NULL);
	uint64_t off, last;
	bool good = false;

	/* slot 0 running */
	rec_setup(false);
	fail_other_fault();
	erase_fault(FR_SLOT_BASE(0), true);	/* the first in-place write */
	sel_prog_lazy = true;			/* the next index program: not stored */
	hook_off = FR_SLOT_BASE(0);
	erase_hook = hook_sel00_off;		/* during the second */
	CHECK(rec_go(steps0, img_w, n, false) == FR_NEXT_HALT);
	last = last_fop_order();
	CHECK(host.restart_st == FR_ST_DAMAGED);
	CHECK(sent_at(0x97u, -1, rec_log_from) == ~0u);	/* no reset request */
	CHECK(st.proto.phase == FR_PH_DONE && st.proto.result == FR_WR_OK);
	CHECK(fmem[FR_SELECTOR_OFF] == 0x00u && fmem[FR_SELECTOR_OFF + 1u] == 0x01u);
	CHECK(fr_check_slot(&flash, 0, &good, NULL) == FR_FLASH_OK && good);
	CHECK(!st.rec.home.unsettled);
	CHECK(!fr_proto_hold(&st.proto) && !st.psoc.hold_off);
	/* the last write: nothing in slot 0 or its record */
	for (unsigned i = 0; i < n_fops; i++)
		if (fops[i].order > host.end_out && fops[i].kind != 'r')
			CHECK(fops[i].off >= FR_SELECTOR_OFF && fops[i].off < FR_SELECTOR_OFF + FR_SECTOR);
	/* the kept power-off came during the second write and ran after the last */
	off = poweroff_order();
	CHECK(psoc.off_sent && psoc.off_order < host.end_out);
	CHECK(off != 0u && off > last);

	/* slot 1 running: not held after the failure */
	rec_setup_run1();
	erase_fault(FR_SLOT_BASE(0), false);	/* the write of slot 0 */
	erase_fault(FR_SLOT_BASE(1), true);	/* the first in-place write */
	sel_prog_lazy = true;
	hook_off = FR_SLOT_BASE(1);
	erase_hook = hook_sel00_off;
	CHECK(rec_go(steps1, img_w, n, false) == FR_NEXT_HALT);
	last = last_fop_order();
	CHECK(st.proto.phase == FR_PH_FAILED && st.proto.result == FR_WR_SELECTOR);
	CHECK(fmem[FR_SELECTOR_OFF] == 0xFFu && fmem[FR_SELECTOR_OFF + 1u] == 0xFFu);
	CHECK(fr_check_slot(&flash, 1, &good, NULL) == FR_FLASH_OK && good);
	CHECK(st.rec.home.unsettled && fr_rec_rescue(&st.rec) && !fr_rec_held(&st.rec));
	/* the power-off at the write's end may cut INFO short: its bits are
	 * test_front_proto's */
	off = poweroff_order();
	CHECK(psoc.off_sent && off != 0u && off > last);
}

/* The selector reads that failed in the window, the last one's time, and
 * the first read after them that worked: its time and order. */
static unsigned sel_reads(uint64_t *fail_t, uint64_t *next_t, uint64_t *next_order)
{
	unsigned fails = 0;

	*fail_t = *next_t = *next_order = 0;
	for (unsigned i = 0; i < n_fops; i++) {
		if (fops[i].kind != 'r' || fops[i].off != FR_SELECTOR_OFF)
			continue;
		if (fops[i].t >= sel_fail_from && fops[i].t < sel_fail_until) {
			fails++;
			*fail_t = fops[i].t;
		} else if (*fail_t != 0u && *next_t == 0u) {
			*next_t = fops[i].t;
			*next_order = fops[i].order;
		}
	}
	return fails;
}

/*
 * S-02.084: after a read that failed, recovery reads the flash again by
 * itself every 5 s while it holds the power-off (fr_proto_tick()). Slot 0
 * runs, slot 1 good; a write of slot 0 with the flag selects slot 1 first,
 * then its first erase of slot 0 takes and fails; the selector cannot be
 * read for 1 s from then, so the hold is on with no host request after.
 * The period counts from the loop's start, and the countdown before the
 * write took 15 s: the first read again comes at once, inside that second,
 * and fails; the next comes 5 s after it, not before; it works, the hold
 * ends, and the deadline's power-off runs.
 */
static void test_rec_resurvey(void)
{
	static const struct hstep steps[] = {
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_STOP, 0, 0 },
	};
	const size_t n = rec_image(img_w, 0x5A, 2, 0x2000, NULL);
	uint64_t fail_t, next_t, next_order, off;

	rec_setup(true);
	erase_fault(FR_SLOT_BASE(0), true);
	arm_sel_fail = true;
	rec_deadline_us = 20000000u;	/* its power-off ends the loop */
	rec_deadline_ok = true;
	CHECK(rec_go(steps, img_w, n, false) == FR_NEXT_HALT);
	CHECK(cd_time >= FR_COUNTDOWN_US);
	/* the re-read after the write, and the first read again */
	CHECK(sel_reads(&fail_t, &next_t, &next_order) == 2u);
	CHECK(fail_t < sel_fail_from + 100000u);
	CHECK(next_t != 0u);
	CHECK(next_t >= fail_t + FR_RESURVEY_US);	/* not before 5 s */
	CHECK(next_t < fail_t + FR_RESURVEY_US + 500000u);
	CHECK(!st.rec.read_error && !fr_proto_hold(&st.proto));
	CHECK(fmem[FR_SELECTOR_OFF] == 0x01u && fmem[FR_SELECTOR_OFF + 1u] == 0x01u);
	/* the deadline's power-off ran, once the hold had ended */
	off = poweroff_order();
	CHECK(off != 0u && off > next_order);
}

/* The same, with a power-off message 2 s in (outside the countdown): kept
 * while the hold lasts, run after the read that ends it. */
static void test_rec_resurvey_kept(void)
{
	static const struct hstep steps[] = {
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_STOP, 0, 0 },
	};
	const size_t n = rec_image(img_w, 0x5A, 2, 0x2000, NULL);
	uint64_t off, fail_t, next_t, next_order;

	rec_setup(true);
	erase_fault(FR_SLOT_BASE(0), true);
	arm_sel_fail = true;
	rec_deadline_us = 2000000u;	/* a power-off message 2 s in */
	rec_deadline_ok = true;
	CHECK(rec_go(steps, img_w, n, false) == FR_NEXT_HALT);
	CHECK(sel_reads(&fail_t, &next_t, &next_order) == 2u);
	off = poweroff_order();
	CHECK(next_order != 0u);
	CHECK(psoc.off_sent);
	/* sent during the hold: kept until the read again ended it */
	CHECK(off != 0u && off > next_order);
	CHECK(psoc.off_order < next_order);
}

/* A host leaves halfway through a frame and another comes: the half frame
 * is dropped with the port's configured state, and the new host's first
 * request is answered. */
static void test_rec_replug(void)
{
	static const struct hstep steps[] = {
		{ H_CUT, 0, 0 }, { H_INFO, 0, 0 }, { H_STOP, 0, 0 },
	};
	const size_t n = rec_image(img_w, 0x5A, 2, 0x2000, NULL);

	rec_setup(true);
	usb_conf = true;
	rec_deadline_us = 1000000u;	/* its power-off ends the loop */
	rec_deadline_ok = true;
	CHECK(rec_go(steps, img_w, n, false) == FR_NEXT_HALT);
	CHECK(host.infos == 1u && host.at == 2u);
	CHECK(st.proto.phase == FR_PH_IDLE);
}

/* A host leaves after BEGIN, in the middle of its transfer: the transfer
 * ends with status 24 and nothing written (S-02.084), so the keys act
 * again; a host that comes back begins again, and its write runs. */
static void test_rec_replug_transfer(void)
{
	static const struct hstep left[] = {
		{ H_BEGIN, 0, WHOLE }, { H_CUT, 0, 0 }, { H_INFO, 0, 0 }, { H_STOP, 0, 0 },
	};
	static const struct hstep again[] = {
		{ H_BEGIN, 0, WHOLE }, { H_CUT, 0, 0 },
		{ H_BEGIN, 0, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 }, { H_INFO_WAIT, 0, 0 },
		{ H_STOP, 0, 0 },
	};
	static uint8_t before[0x800], rec[12];
	const size_t n = rec_image(img_w, 0x5A, 2, 0x2000, NULL);

	rec_setup(true);
	memcpy(before, fmem + FR_SLOT_BASE(1), sizeof before);
	memcpy(rec, fmem + FR_RECORD_OFF(1), sizeof rec);
	usb_conf = true;
	rec_deadline_us = 1000000u;	/* its power-off ends the loop */
	rec_deadline_ok = true;
	CHECK(rec_go(left, img_w, n, false) == FR_NEXT_HALT);
	CHECK(host.begin_st == FR_WR_OK && host.infos == 1u);
	CHECK(host.info[FR_INFO_O_PHASE] == FR_PH_FAILED);
	CHECK(host.info[FR_INFO_O_RESULT] == FR_WR_STOPPED);
	CHECK(st.proto.phase == FR_PH_FAILED);
	CHECK(memcmp(fmem + FR_SLOT_BASE(1), before, sizeof before) == 0);
	CHECK(memcmp(fmem + FR_RECORD_OFF(1), rec, sizeof rec) == 0);

	rec_setup(true);
	usb_conf = true;
	rec_deadline_us = 30000000u;
	rec_deadline_ok = true;
	CHECK(rec_go(again, img_w, n, false) == FR_NEXT_HALT);
	CHECK(host.begin_st == FR_WR_OK && host.end_st == FR_WR_OK);
	CHECK(st.proto.phase == FR_PH_DONE && st.proto.result == FR_WR_OK);
	CHECK(memcmp(fmem + FR_SLOT_BASE(1), img_w, n) == 0);
}

/* The switch on again during the hold drops the kept message. */
static void hook_off_then_on(uint32_t off)
{
	static const uint8_t s14[1] = { 0x14 }, s10[1] = { 0x10 };

	if (off != hook_off || hook_count != 1u)
		return;
	emit(0x41u, 0x31u, s14, 1);
	emit(0x41u, 0x31u, s10, 1);
}

/* SC-RECOVERY run 18 through the loop: during the in-place write the PSoC
 * sends 0x31 {14} and then 0x31 {10}: no power-off when the write has
 * ended, and recovery goes on; a later 0x31 {14} powers off at once. */
static void test_rec_switch_on_again(void)
{
	static const struct hstep steps[] = {
		FAIL_OTHER,
		{ H_BEGIN, FR_BEGIN_F_RUNNING, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 }, { H_INFO, 0, 0 }, { H_INFO, 0, 0 }, { H_OFF, 0, 0 },
		{ H_STOP, 0, 0 },
	};
	const size_t n = rec_image(img_w, 0x5A, 2, 0x10000, NULL);
	uint64_t off;

	rec_setup(false);
	fail_other_fault();
	erase_hook = hook_off_then_on;
	CHECK(rec_go(steps, img_w, n, false) == FR_NEXT_HALT);
	CHECK(st.proto.phase == FR_PH_DONE && st.proto.result == FR_WR_OK);
	CHECK(host.infos >= 3u);	/* recovery went on after the write */
	off = poweroff_order();
	/* the one power-off is the later message's, after the INFOs */
	CHECK(psoc.off_sent && off > psoc.off_order && psoc.off_order > host.done_at);
	CHECK(off > last_fop_order());
}

/* The 0x51 with payload {p0 .. p2} first sent from `from`, by order, or 0. */
static uint64_t report_order(uint8_t p0, uint8_t p2, uint64_t from)
{
	for (unsigned i = 0; i < psoc.n_log; i++)
		if (psoc.log[i].id == 0x51u && psoc.log[i].len == 3u && psoc.log[i].p0 == p0 &&
		    psoc.log[i].p2 == p2 && psoc.log[i].order > from)
			return psoc.log[i].order;
	return 0;
}

/*
 * S-13c.010, S-13c.019, S-13c.022 in the start: the logo frame goes out
 * with the display off, every byte of it before DISPON, the run's one;
 * 0x51 {3C FF 02} follows DISPON. A power-off reported at the start comes
 * after the frame (the frame precedes the greeting): 0x51 {00 00 02} after
 * the panel's last byte, and nothing to the panel after it, no DISPOFF
 * and no frame. Recovery's first screen is a later frame: no second
 * DISPON; a power-off there sends the panel nothing either.
 */
static void test_panel_order(void)
{
	static const struct hstep off_steps[] = { { H_OFF, 0, 0 }, { H_STOP, 0, 0 } };
	static uint8_t img[16];
	struct fr_handoff ho;
	uint64_t lit;

	answering();
	n_frames = n_onoff = 0;
	CHECK(run(true, &ho) == FR_NEXT_MAIN);
	CHECK(n_frames == 1u && n_onoff == 1u && onoff[0].cmd == 0x29u);
	CHECK(frames[0].order < onoff[0].order);
	CHECK(onoff[0].bytes >= 215040u);	/* the whole frame before DISPON */
	CHECK(panel_bytes == onoff[0].bytes + 1u);
	lit = report_order(0x3Cu, 0x02u, 0);
	CHECK(lit > onoff[0].order);
	CHECK((ho.flags & (FR_HO_PANEL_UP | FR_HO_PANEL_LIT)) == (FR_HO_PANEL_UP | FR_HO_PANEL_LIT));

	/* switched off at the start, before the logo is lit */
	answering();
	psoc.state = 0x14u;
	n_frames = n_onoff = 0;
	CHECK(run(true, &ho) == FR_NEXT_HALT);
	CHECK(n_frames == 1u && n_onoff == 1u && onoff[0].cmd == 0x29u);
	CHECK(frames[0].order < onoff[0].order && panel_bytes == onoff[0].bytes + 1u);
	CHECK(report_order(0x00u, 0x02u, panel_last) != 0u);
	CHECK(report_order(0x3Cu, 0x02u, 0) == 0u && (ho.flags & FR_HO_PANEL_LIT) == 0u);
	CHECK((ho.flags & FR_HO_PANEL_UP) != 0u);

	/* Power + M4: recovery's screen, then a power-off */
	rec_setup(false);
	n_onoff = 0;
	CHECK(rec_go(off_steps, img, sizeof img, false) == FR_NEXT_HALT);
	CHECK(n_frames >= 2u && n_onoff == 1u && onoff[0].cmd == 0x29u);
	CHECK(frames[0].order < onoff[0].order && onoff[0].order < frames[1].order);
	CHECK(frames[n_frames - 1u].order < panel_last);
	CHECK(report_order(0x00u, 0x02u, panel_last) != 0u);
}

/* ---- S-02.093 through recovery's loop: a trial, the trial start, the
 * select ---- */

/* The factory first stage's start on the flash (S-02.020-S-02.023): the
 * slot it boots. With verified not 0x01 the named slot is checked, then
 * the selector erased and programmed {index, 01}, or {other, 01} after a
 * check that failed. */
static unsigned first_stage(void)
{
	const uint8_t idx = fmem[FR_SELECTOR_OFF];
	const unsigned k = idx == 0x00u ? 0u : 1u;
	const uint32_t len = fr_le32(fmem + FR_RECORD_OFF(k) + 4u);
	bool pass;

	if (fmem[FR_SELECTOR_OFF + 1u] == 0x01u)
		return k;
	pass = fr_le32(fmem + FR_RECORD_OFF(k) + 8u) == FR_TAG && len <= FR_SLOT_CAP &&
	       fr_crc32(0, fmem + FR_SLOT_BASE(k), len) == fr_le32(fmem + FR_RECORD_OFF(k));
	memset(fmem + FR_SELECTOR_OFF, 0xFF, FR_SECTOR);
	fmem[FR_SELECTOR_OFF] = pass ? idx : (uint8_t)(k == 0u ? 0x01u : 0x00u);
	fmem[FR_SELECTOR_OFF + 1u] = 0x01u;
	return k;
}

/* Another start into recovery, Power + M4, with the flash as it is and the
 * memory an image loaded: its region's bytes and its descriptor. */
static void rec_again(uint8_t fill, const uint8_t *d)
{
	static uint8_t keep[FLASH_SIZE];

	memcpy(keep, fmem, sizeof keep);
	rec_reset();		/* the model's reset rewrites the flash */
	memcpy(fmem, keep, sizeof keep);
	n_fops = 0;
	memset(region, fill, sizeof region);
	memcpy(desc, d, sizeof desc);
}

/* A start with no key, the flash as it is and the memory an image loaded,
 * to the hand-off. */
static void start_again(uint8_t fill, const uint8_t *d, struct fr_handoff *ho)
{
	static uint8_t keep[FLASH_SIZE];

	memcpy(keep, fmem, sizeof keep);
	answering();		/* the model's reset rewrites the flash */
	memcpy(fmem, keep, sizeof keep);
	memset(region, fill, sizeof region);
	memcpy(desc, d, sizeof desc);
	map_soft = true;
	CHECK(run(true, ho) == FR_NEXT_MAIN);
	map_soft = false;
}

/* An erase or program in [lo, hi). */
static bool wrote_in(uint32_t lo, uint32_t hi)
{
	for (unsigned i = 0; i < n_fops && i < FOPS_MAX; i++)
		if (fops[i].kind != 'r' && fops[i].off >= lo && fops[i].off < hi)
			return true;
	return false;
}

/*
 * Slot 1 runs, slot 2 is bad: an image with another front written for a
 * trial through recovery's loop; the trial start (the first stage's check
 * of slot 2 fails, {00 01}, slot 2 boots once); then the select, from slot
 * 2's own recovery during the trial start, or from slot 1's after the
 * power cycle. The next start checks slot 2 and trusts it.
 */
static void test_rec_trial(void)
{
	static const struct hstep trial[] = {
		{ H_BEGIN, FR_BEGIN_F_TRIAL, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 }, { H_OFF, 0, 0 }, { H_STOP, 0, 0 },
	};
	static const struct hstep look[] = { { H_INFO, 0, 0 }, { H_OFF, 0, 0 }, { H_STOP, 0, 0 } };
	static const struct hstep pick[] = {
		{ H_BEGIN, FR_BEGIN_F_SELECT, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 },
		{ H_INFO_WAIT, 0, 0 }, { H_OFF, 0, 0 }, { H_STOP, 0, 0 },
	};
	static uint8_t img_t[0x800], d_a[FR_DESC_MAX], d_t[FR_DESC_MAX];

	for (unsigned from_b = 0; from_b < 2u; from_b++) {
		size_t n;

		flash_reset(0x00, 0x01);
		rec_setup(false);
		memcpy(d_a, desc, sizeof d_a);
		g_front_alt = 0x40u;
		n = rec_image(img_t, 0x66, 7, 0, d_t);
		g_front_alt = 0;
		CHECK(rec_go(trial, img_t, n, false) == FR_NEXT_HALT);
		CHECK(host.begin_st == FR_WR_OK && host.begin_way == FR_WAY_TRIAL);
		CHECK(host.begin_target == 1u && host.end_st == FR_WR_OK);
		CHECK(host.info[FR_INFO_O_PHASE] == FR_PH_DONE);
		CHECK(host.info[FR_INFO_O_WAY] == FR_WAY_TRIAL && host.info[FR_INFO_O_WRITTEN] == 1u);
		CHECK((host.info[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] & (FR_SLOT_F_TRIED | FR_SLOT_F_GOOD)) ==
		      FR_SLOT_F_TRIED);
		CHECK(memcmp(fmem + FR_SLOT_BASE(1), img_t, n) == 0);
		CHECK(fr_le32(fmem + FR_RECORD_OFF(1)) == ~fr_crc32(0, img_t, n));
		CHECK(fmem[FR_SELECTOR_OFF] == 0xFFu && fmem[FR_SELECTOR_OFF + 1u] == 0xFFu);
		CHECK(fmem[FR_LOG_OFF(1)] == 0xFDu);
		CHECK(!wrote_in(FR_SLOT_BASE(0), FR_SLOT_BASE(0) + FR_SLOT_CAP));
		CHECK(!wrote_in(FR_RECORD_OFF(0), FR_RECORD_OFF(0) + FR_SECTOR));
		CHECK(screen_says(&st.screen, "Done: slot 2 is written for one trial start"));
		CHECK(screen_says(&st.screen, "If it does not start, switch off and on: slot 1 boots"));
		CHECK(strcmp(st.screen.status_right, "v1.0.0 F19") == 0);

		/* the trial start */
		CHECK(first_stage() == 1u);
		CHECK(fmem[FR_SELECTOR_OFF] == 0x00u && fmem[FR_SELECTOR_OFF + 1u] == 0x01u);
		if (from_b) {
			/* slot 2's own front, Power + M4: running, tried, and the
			 * trial start's line in place of the refusal */
			rec_again(0x66, d_t);
			CHECK(rec_go(look, img_t, n, false) == FR_NEXT_HALT);
			CHECK((host.info[FR_INFO_O_FLAGS] & FR_INFO_F_ESTABLISHED) != 0u);
			CHECK(host.info[FR_INFO_O_RUNNING] == 1u);
			CHECK(host.info[FR_INFO_O_TARGET] == FR_NO_SLOT);
			CHECK(host.info[FR_INFO_O_REFUSAL] == FR_WR_NOT_SELECTED);
			CHECK(screen_says(&st.screen, "Trial start of slot 2"));
			CHECK(!screen_says(&st.screen, "No write"));
			CHECK(!wrote_in(0, FLASH_SIZE));
			rec_again(0x66, d_t);
		} else {
			struct fr_handoff ho;

			/* a power cycle before slot 2's front ran: slot 1's
			 * recovery refuses the select, 21, and writes nothing */
			CHECK(first_stage() == 0u);
			rec_again(0x3C, d_a);
			CHECK(rec_go(pick, img_t, n, false) == FR_NEXT_HALT);
			CHECK(host.begin_st == FR_WR_NOT_STARTED && host.begin_way == FR_WAY_NONE);
			CHECK((host.info[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] & FR_SLOT_F_STARTED) == 0u);
			CHECK(!wrote_in(0, FLASH_SIZE));
			CHECK(screen_says(&st.screen, "Failed: the tried slot has not started; restart first"));
			/* the trial start, slot 2's own front, no key: the mark */
			start_again(0x66, d_t, &ho);
			CHECK(fmem[FR_LOG_OFF(1)] == 0xFCu);
			CHECK((ho.flags & FR_HO_START_LOGGED) == 0u);
			CHECK(count_fops('p') == 1u && count_fops('e') == 0u);
			CHECK(fmem[FR_SELECTOR_OFF] == 0x00u && fmem[FR_SELECTOR_OFF + 1u] == 0x01u);
			/* the power cycle: slot 1 trusted, its own front */
			CHECK(first_stage() == 0u);
			rec_again(0x3C, d_a);
		}
		CHECK(rec_go(pick, img_t, n, false) == FR_NEXT_HALT);
		CHECK(host.begin_st == FR_WR_OK && host.begin_way == FR_WAY_SELECT);
		CHECK(host.begin_target == 1u && host.end_st == FR_WR_OK);
		CHECK(host.info[FR_INFO_O_RUNNING] == (from_b ? 1u : 0u));
		CHECK(host.info[FR_INFO_O_PHASE] == FR_PH_DONE);
		CHECK(host.info[FR_INFO_O_WAY] == FR_WAY_SELECT && host.info[FR_INFO_O_WRITTEN] == 1u);
		CHECK((host.info[FR_INFO_O_SLOT(1) + FR_INFO_S_FLAGS] & (FR_SLOT_F_TRIED | FR_SLOT_F_GOOD)) ==
		      FR_SLOT_F_GOOD);
		CHECK(fr_le32(fmem + FR_RECORD_OFF(1)) == fr_crc32(0, img_t, n));
		CHECK(memcmp(fmem + FR_SLOT_BASE(1), img_t, n) == 0);
		CHECK(fmem[FR_SELECTOR_OFF] == 0x01u && fmem[FR_SELECTOR_OFF + 1u] == 0x00u);
		/* nothing in either slot's sectors or slot 1's record but, from
		 * slot 2's own front, its log's entry 0 marked */
		CHECK(!wrote_in(FR_SLOT_BASE(0), FR_LOG_OFF(1)));
		CHECK(wrote_in(FR_LOG_OFF(1), FR_LOG_OFF(1) + FR_SECTOR) == (from_b != 0u));
		CHECK(fmem[FR_LOG_OFF(1)] == 0xFCu);
		CHECK(!wrote_in(FR_RECORD_OFF(0), FR_RECORD_OFF(0) + FR_SECTOR));
		CHECK(wrote_in(FR_RECORD_OFF(1), FR_RECORD_OFF(1) + FR_SECTOR));
		CHECK(screen_says(&st.screen, "Done: slot 2 is kept as the boot slot"));
		/* the next start checks slot 2 and trusts it */
		CHECK(first_stage() == 1u);
		CHECK(fmem[FR_SELECTOR_OFF] == 0x01u && fmem[FR_SELECTOR_OFF + 1u] == 0x01u);
	}
}

/* ---- recovery's keys through the loop (S-02.084, fr_recui.c) ---- */

#define K4   0x00u, 0x02u, 0x00u, true	/* key 4 (M4) */
#define K1   0x40u, 0x00u, 0x00u, true	/* key 1 (M1) */
#define K12  0xC0u, 0x00u, 0x00u, true	/* keys 1 and 2 */
#define KUP  0x00u, 0x00u, 0x80u, true	/* none down, precomp set */
#define KNONE 0x00u, 0x00u, 0x00u, false	/* no reply */
#define K2_SW 0x80u, 0x00u, 0x00u, true	/* key 2 (M2): switch, on the menu */

/* The key-status requests recovery sent from `from` (in g_order) to `to`. */
static unsigned keys_between(uint64_t from, uint64_t to)
{
	unsigned n = 0;

	for (unsigned i = rec_log_from; i < psoc.n_log; i++)
		if (psoc.log[i].id == 0x41u && psoc.log[i].request && psoc.log[i].order > from &&
		    psoc.log[i].order < to)
			n++;
	return n;
}

static unsigned writes(void)
{
	return count_fops('e') + count_fops('p');
}

static enum fr_next rec_keys(const struct kstep *k, unsigned n, uint32_t deadline_ms)
{
	static const struct hstep idle[] = { { H_STOP, 0, 0 } };
	static uint8_t img[16];

	kscript = k;
	kscript_n = n;
	rec_deadline_us = (uint64_t)deadline_ms * 1000u;
	rec_deadline_ok = true;
	return rec_go(idle, img, sizeof img, false);
}

/*
 * Recovery entered with key 4 still held: no key acts until a reading
 * shows none down. Key 1 after key 4 with no release between, or after
 * readings that did not come, or after two keys, opens nothing; after a
 * release it opens the slot menu. Keys are read every 50 ms, and every
 * 500 ms after a reading that did not come.
 */
static void test_rec_keys_gate(void)
{
	static const struct kstep held[] = { { 0, K4 } };
	static const struct kstep k4_k1[] = { { 0, K4 }, { 1000, K1 } };
	static const struct kstep unread[] = { { 0, K4 }, { 1000, KNONE }, { 2000, K1 } };
	static const struct kstep many[] = { { 0, K4 }, { 1000, K12 }, { 2000, K1 } };
	static const struct kstep rel[] = { { 0, K4 }, { 1000, KUP }, { 1200, K1 } };
	unsigned reqs;

	rec_setup(true);
	CHECK(rec_keys(held, ARRAY_LEN(held), 3000) == FR_NEXT_HALT);
	CHECK(st.ui.screen == FR_UI_MAIN && !st.ui.armed && writes() == 0u);
	reqs = requests_of(0x41u);
	CHECK(reqs >= 3000u / 50u - 4u && reqs <= 3000u / 50u + 1u);

	rec_setup(true);
	CHECK(rec_keys(k4_k1, ARRAY_LEN(k4_k1), 3000) == FR_NEXT_HALT);
	CHECK(st.ui.screen == FR_UI_MAIN && writes() == 0u);

	rec_setup(true);
	CHECK(rec_keys(unread, ARRAY_LEN(unread), 3000) == FR_NEXT_HALT);
	CHECK(st.ui.screen == FR_UI_MAIN && writes() == 0u);
	/* a second of no replies: a request every 500 ms (and its 100 ms) */
	reqs = requests_of(0x41u);
	CHECK(reqs <= 2000u / 50u + 4u && reqs >= 2000u / 50u - 6u);

	rec_setup(true);
	CHECK(rec_keys(many, ARRAY_LEN(many), 3000) == FR_NEXT_HALT);
	CHECK(st.ui.screen == FR_UI_MAIN && writes() == 0u);

	rec_setup(true);
	CHECK(rec_keys(rel, ARRAY_LEN(rel), 3000) == FR_NEXT_HALT);
	CHECK(st.ui.screen == FR_UI_SLOTS && writes() == 0u);
	CHECK(strcmp(st.screen.title, "Recovery: slots") == 0);
	CHECK(screen_says(&st.screen, "1  Try slot 2 once (suggested)"));
}

/*
 * A try of slot 2 chosen on the unit: key 1 to the menu, key 1 for the
 * try, key 1 held 5 s. The action starts at once (no countdown), forced,
 * with no key read while it runs; slot 2 is marked tried and the
 * selector names it once; the first stage starts it once.
 */
static void test_rec_keys_try(void)
{
	static const struct kstep k[] = {
		{ 0, K4 }, { 500, KUP }, { 600, K1 }, { 700, KUP }, { 800, K1 }, { 900, KUP },
		{ 1000, K1 }, { 6500, KUP },
	};
	uint64_t first = 0, last = 0, first_t = 0;
	uint32_t crc;

	rec_setup(true);
	CHECK(rec_keys(k, ARRAY_LEN(k), 9000) == FR_NEXT_HALT);
	CHECK(cd_time == 0u);
	CHECK(st.proto.local && st.proto.w.act == FR_ACT_TRY && st.proto.w.way == FR_WAY_TRY);
	CHECK(st.proto.phase == FR_PH_DONE && st.proto.result == FR_WR_OK);
	for (unsigned i = 0; i < n_fops; i++)
		if (fops[i].kind != 'r') {
			if (first == 0u) {
				first = fops[i].order;
				first_t = fops[i].t;
			}
			last = fops[i].order;
		}
	/* 5 s of key 1 from the reading at 1 s: the first write after them */
	CHECK(first != 0u && first_t >= rec_t0 + 6000000u && first_t < rec_t0 + 6200000u);
	CHECK(keys_between(first, last) == 0u);
	crc = fr_crc32(0, fmem + FR_SLOT_BASE(1), fr_le32(fmem + FR_RECORD_OFF(1) + 4u));
	CHECK(fr_le32(fmem + FR_RECORD_OFF(1)) == ~crc);
	CHECK(fmem[FR_SELECTOR_OFF] == 0xFFu && fmem[FR_SELECTOR_OFF + 1u] == 0xFFu);
	CHECK(fmem[FR_LOG_OFF(1)] == FR_LOG0_PENDING);
	CHECK(!wrote_in(FR_SLOT_BASE(0), FR_SLOT_BASE(0) + FR_SLOT_CAP));
	CHECK(!wrote_in(FR_RECORD_OFF(0), FR_RECORD_OFF(0) + FR_SECTOR));
	CHECK(screen_says(&st.screen, "Done: slot 2 starts once, as a trial"));
	CHECK(st.ui.screen == FR_UI_MAIN);
	/* the first stage starts slot 2 once, then slot 1 again */
	CHECK(first_stage() == 1u);
	CHECK(fmem[FR_SELECTOR_OFF] == 0x00u && fmem[FR_SELECTOR_OFF + 1u] == 0x01u);
	CHECK(first_stage() == 0u);
}

/* The hold let go after 3 s, or broken by two keys: nothing written, the
 * confirm screen still up with an empty bar. */
static void test_rec_keys_abort(void)
{
	static const struct kstep let_go[] = {
		{ 0, K4 }, { 500, KUP }, { 600, K1 }, { 700, KUP }, { 800, K2_SW }, { 900, KUP },
		{ 1000, K1 }, { 4000, KUP },
	};
	static const struct kstep broken[] = {
		{ 0, K4 }, { 500, KUP }, { 600, K1 }, { 700, KUP }, { 800, K2_SW }, { 900, KUP },
		{ 1000, K1 }, { 3000, K12 }, { 3100, K1 },
	};

	rec_setup(true);
	CHECK(rec_keys(let_go, ARRAY_LEN(let_go), 9000) == FR_NEXT_HALT);
	CHECK(writes() == 0u && st.proto.phase == FR_PH_IDLE);
	CHECK(st.ui.screen == FR_UI_CONFIRM && st.ui.act == FR_ACT_SWITCH && st.ui.hold_pct == 0u);
	CHECK(screen_says(&st.screen, "Hold 1 for 5 s to confirm"));
	rec_setup(true);
	CHECK(rec_keys(broken, ARRAY_LEN(broken), 9000) == FR_NEXT_HALT);
	CHECK(writes() == 0u && !st.ui.holding);
	CHECK(st.ui.screen == FR_UI_CONFIRM && st.ui.hold_pct == 0u);
}

/* No key is read while a host's write is counted down to or runs. */
static void test_rec_keys_quiet(void)
{
	static const struct hstep steps[] = {
		{ H_BEGIN, 0, WHOLE }, { H_DATA, 0, 0 }, { H_END, 0, 0 }, { H_INFO_WAIT, 0, 0 },
		{ H_STOP, 0, 0 },
	};
	const size_t n = rec_image(img_w, 0x5A, 2, 0x2000, NULL);

	rec_setup(false);
	rec_deadline_us = 4000000u;
	rec_deadline_ok = true;
	CHECK(rec_go(steps, img_w, n, false) == FR_NEXT_HALT);
	CHECK(host.end_st == FR_WR_OK && host.info[FR_INFO_O_PHASE] == FR_PH_DONE);
	CHECK(cd_time >= FR_COUNTDOWN_US);
	/* none while it was received, counted down to and written (rec_go()
	 * checks each request's phase), and again once it was done */
	CHECK(keys_between(host.end_out, UINT64_MAX) > 0u);
}

int main(void)
{
	RUN(test_rec_trial);
	RUN(test_rec_rescue_case_a);
	RUN(test_rec_unsettled);
	RUN(test_rec_resurvey);
	RUN(test_rec_replug);
	RUN(test_rec_replug_transfer);
	RUN(test_rec_resurvey_kept);
	RUN(test_rec_switch_on_again);
	RUN(test_rec_in_place_fails);
	RUN(test_rec_in_place_poweroff);
	RUN(test_rec_in_place_done);
	RUN(test_rec_other_ways_poweroff);
	RUN(test_rec_battery);
	RUN(test_normal_order);
	RUN(test_inbox_before_jump);
	RUN(test_record_running);
	RUN(test_log_step);
	RUN(test_log_trial_start);
	RUN(test_log_trial_spend);
	RUN(test_log_recovery);
	RUN(test_log_not_reached);
	RUN(test_log_flash_failures);
	RUN(test_silent_psoc);
	RUN(test_no_panel);
	RUN(test_greeting_ends_the_wait);
	RUN(test_power4);
	RUN(test_image_check_fails);
	RUN(test_reset_wait_floor);
	RUN(test_halt);
	RUN(test_panel_order);
	RUN(test_rec_keys_gate);
	RUN(test_rec_keys_try);
	RUN(test_rec_keys_abort);
	RUN(test_rec_keys_quiet);
	RUN(test_logo_alone);
	RUN(test_recovery_line);
	RUN(test_no_slot_chords);
	RUN(test_no_blink);
	return check_summary("test_front_start");
}
