/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Held mode's keys across a long operation (src/board/opxy_held.c,
 * src/s15_menu.c): UART0's driver on a modeled controller and DMA11, the
 * S-06 link, S-07's bitmaps and S-15's menus, as the board joins them. The
 * PSoC link has no flow control (S-13b.070): what the PSoC sends while
 * nothing services the link waits in DMA11's ring.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/devhost.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_sec.h"
#include "waffle/s06_rx.h"
#include "waffle/s06_frame.h"
#include "waffle/wire_crc.h"

static struct opxy_master m;

/*
 * UART0 and DMA11: a byte that arrives is written to the ring at the
 * channel's position while the channel is enabled, its request (ERBFI)
 * unmasked and ELSI, which would take the request, masked; otherwise it is
 * dropped and counted. The status interrupt (SEC source 48) is the
 * transmit side's, so nothing arrives through it.
 */
static struct {
	uint32_t errors;
	uint32_t imsk;
	unsigned rbr_reads;
	unsigned dropped;
} hw;

static bool hook(void *ctx, enum fake_mmr_op op, unsigned width,
		 wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_READ && addr == UART_STAT(0)) {
		*value = hw.errors | UART_STAT_THRE | UART_STAT_TEMT;
	} else if (op == FAKE_MMR_READ && addr == UART_RBR(0)) {
		hw.rbr_reads++;
	} else if (op == FAKE_MMR_WRITE && addr == UART_STAT(0)) {
		hw.errors &= ~*value;
	} else if (op == FAKE_MMR_WRITE && addr == UART_IMSK_SET(0)) {
		hw.imsk |= *value;
	} else if (op == FAKE_MMR_WRITE && addr == UART_IMSK_CLR(0)) {
		hw.imsk &= ~*value;
	} else if (op == FAKE_MMR_WRITE && addr == DMA_STAT(11)) {
		*value = fake_mmr_get(addr) & ~(*value & 7u);	/* W1C */
	} else if (op == FAKE_MMR_WRITE && addr == DMA_CFG(11) &&
		   (*value & DMA_CFG_EN)) {
		fake_mmr_set(DMA_XCNT_CUR(11), fake_mmr_get(DMA_XCNT(11)));
	} else if (op == FAKE_MMR_READ && addr == SEC0_CSID0) {
		*value = 0xFFu;
	}
	return true;
}

static void arrive(const uint8_t *b, size_t n)
{
	const struct wb_uart *u = &m.hw.uart[OPXY_UART_PSOC];

	for (size_t i = 0; i < n; i++) {
		uint32_t cur = fake_mmr_get(DMA_XCNT_CUR(11));

		if (!(fake_mmr_get(DMA_CFG(11)) & DMA_CFG_EN) ||
		    !(hw.imsk & UART_IMSK_ERBFI) || (hw.imsk & UART_IMSK_ELSI) ||
		    !(fake_mmr_get(UART_CTL(0)) & UART_CTL_EN)) {
			hw.dropped++;
			continue;
		}
		if (cur == 0)
			cur = u->rx_size;
		u->rx[u->rx_size - cur] = b[i];
		fake_mmr_set(DMA_XCNT_CUR(11), cur - 1u);
	}
}

/* A frame from the PSoC (route 0x41), as the wire carries it. */
static uint8_t seq;

static void psoc_sends(uint8_t id, const uint8_t *pl, size_t len)
{
	uint8_t w[S06_MAX_WIRE];
	size_t n = s06_frame_encode(s06_route(S06_ADDR_PSOC, S06_ADDR_MASTER),
				    seq++, id, pl, len, wire_crc8_frame, w);

	arrive(w, n);
}

/* S-07.031: the keys down, as a 0x42 bitmap. */
static void bitmap(int k1, int k2)
{
	uint8_t bm[9] = { 0 };

	if (k1 >= 0)
		bm[k1 >> 3] |= (uint8_t)(1u << (k1 & 7));
	if (k2 >= 0)
		bm[k2 >> 3] |= (uint8_t)(1u << (k2 & 7));
	psoc_sends(0x42, bm, sizeof bm);
}

static void battery(void)
{
	static const uint8_t b[4] = { 0x01, 0x50, 0x0F, 0xA0 };

	psoc_sends(0x37, b, sizeof b);
}

/* One pass of held mode's loop, as far as the PSoC's link goes. */
static void pass(void)
{
	const struct wb_uart *u = &m.hw.uart[OPXY_UART_PSOC];

	CHECK(s07_service_pass(&m.psoc) == S07_OK);
	CHECK(opxy_held_settle(&m) == S07_OK);
	/* Every byte reached the ring and was read from it, none from RBR. */
	CHECK(hw.dropped == 0 && hw.rbr_reads == 0);
	CHECK(u->rx_lost == 0 && u->rx_gaps == 0 && u->overruns == 0 &&
	      u->dma_errors == 0);
}

static void press(int key)
{
	bitmap(key, -1);
	pass();
	bitmap(-1, -1);
	pass();
}

static int long_key;		/* the key that started the operation */

/* A key that starts a long operation: its release comes during it. */
static void press_long(int key)
{
	long_key = key;
	bitmap(key, -1);
	pass();
}

/* The clock runs ahead by ms, as held mode's loop finds it. */
static void clock_ahead(uint32_t ms)
{
	fake_core_set_cycles(fake_core_peek_cycles() +
			     (uint64_t)ms * 1000u * m.hw.time.cycles_per_us);
}

/* The time passes, with the loop's passes meanwhile, and a pass tells the
 * menus (S-15.015). */
static void later(uint32_t ms)
{
	clock_ahead(ms);
	pass();
	opxy_menu_tick(&m);
}

/* S-15.015: a key held for the hold's time, whose action is a long
 * operation: the key's release comes during it. */
static void hold_long(int key)
{
	long_key = key;
	bitmap(key, -1);
	pass();
	later(S15_CONFIRM_HOLD_MS);
	pass();
}

/* --- the menus' environment --- */

/* What the PSoC sends while a long operation runs. */
enum during { D_RELEASE, D_PRESS_6, D_BACKLOG };
static enum during during;
static unsigned resets, continues, mains, formats, removes;

/* A long call: it holds the menu's keys as opxy_services.c's calls do,
 * and the PSoC sends meanwhile. */
static void meanwhile(void)
{
	s15_menu_hold(&m.sv.menu);
	/* The release of the key that started it, after a battery report:
	 * the report and the release's first bytes fill the FIFO. */
	battery();
	if (during == D_BACKLOG)
		bitmap(long_key, 0x13);	/* digit 6 down with it */
	bitmap(-1, during == D_BACKLOG ? 0x13 : -1);
	if (during == D_PRESS_6) {	/* digit 6, pressed during it */
		bitmap(0x13, -1);
		bitmap(-1, -1);
	}
	if (during == D_BACKLOG) {	/* more than the inbox holds */
		for (int i = 0; i < 9; i++)
			battery();
		bitmap(-1, -1);	/* digit 6 up */
	}
}

static void e_usb(void *c, enum s15_usb_set s)
{
	(void)c;
	if (s == S15_USB_MAIN)
		mains++;
}

static void e_none(void *c) { (void)c; }
static void e_leds(void *c, bool on) { (void)c; (void)on; }
static bool e_false(void *c) { (void)c; return false; }
static bool e_dir(void *c, const char *p) { (void)c; (void)p; return false; }

static bool e_pack(void *c, size_t i, char name[S15_NAME_MAX + 1])
{
	(void)c;
	(void)i;
	(void)name;
	return false;
}

static size_t e_manifest(void *c, const char *d, uint8_t *b, size_t max)
{
	(void)c;
	(void)d;
	(void)b;
	(void)max;
	return 0;
}

static bool e_remove(void *c, const char *p)
{
	(void)c;
	(void)p;
	if (removes++ == 0u)
		meanwhile();
	else
		s15_menu_hold(&m.sv.menu);
	return true;
}

static bool e_format(void *c)
{
	(void)c;
	formats++;
	meanwhile();
	return true;
}

static void e_os_write(void *c, struct s15_osw_end *end)
{
	(void)c;
	meanwhile();
	end->result = S15_OSW_OK;
	end->why = S15_OSW_WHY_NONE;
}

static void e_reset(void *c) { (void)c; resets++; }
static void e_continue(void *c) { (void)c; continues++; }
static void e_option(void *c, bool on) { (void)c; (void)on; }

static bool e_settings(void *c, const struct s15_settings *s)
{
	(void)c;
	(void)s;
	return true;
}

static const struct s15_env_ops env = {
	.usb_set = e_usb, .volcal_reset = e_none, .keytest_leds = e_leds,
	.volume_refused = e_false, .dir_exists = e_dir, .pack_dir = e_pack,
	.read_manifest = e_manifest, .remove_tree = e_remove,
	.erase_os_fat = e_none, .format = e_format, .os_write = e_os_write,
	.reset_w7 = e_reset, .system_info = e_none, .scan_now = e_none,
	.option = e_option, .settings_save = e_settings, .continue_os = e_continue,
};

static struct s15_boot boot;


/* Held mode with the developer menu on, as the board has it before the
 * first screen. */
static void setup_before(enum during d)
{
	memset(&m, 0, sizeof m);
	memset(&hw, 0, sizeof hw);
	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set_hook(hook, NULL);
	during = d;
	resets = continues = mains = formats = removes = 0;
	m.hw.time = opxy_time;
	m.clock = opxy_clock(&m.hw);
	CHECK(opxy_psoc_uart_open(&m) == WB_UART_OK);
	CHECK(m.hw.uart[OPXY_UART_PSOC].dma &&
	      m.hw.uart[OPXY_UART_PSOC].rx_size == 0x1000u);
	s06_node_init(&m.node, wire_crc8_frame, &m.clock);
	s06_link_init(&m.link[S06_LINK_PSOC], &opxy_port_ops,
		      &m.hw.uart[OPXY_UART_PSOC], S06_ADDR_MASTER, S06_ADDR_PSOC);
	m.node.link[S06_LINK_PSOC] = &m.link[S06_LINK_PSOC];
	m.psoc_ops.ctx = &m;
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	(void)wb_sec_init();
	opxy_held_psoc(&m);

	memset(&boot, 0, sizeof boot);
	boot.version = "1.0.0.0";
	boot.settings.dev_mode = true;
	boot.settings.color = true;
	s15_menu_init(&m.sv.menu, &boot, &env, NULL);
}

/* The first screen, as held mode's loop opens it: what arrived before it
 * taken first (S-15.013). */
static void menu_start(void)
{
	CHECK(opxy_held_keys_before(&m) == S07_OK);
	s15_menu_start(&m.sv.menu);
	pass();
}

static void setup(enum during d)
{
	setup_before(d);
	menu_start();
}

/* --- a developer host on the CDC link (waffle/devhost.h) --- */

static uint8_t cdc_wire[1024];
static size_t cdc_n;

static size_t cdc_read(void *ctx, uint8_t *b, size_t max)
{
	(void)ctx;
	(void)b;
	(void)max;
	return 0;
}

static size_t cdc_room(void *ctx)
{
	(void)ctx;
	return sizeof(cdc_wire) - cdc_n;
}

static void cdc_write(void *ctx, const uint8_t *d, size_t n)
{
	(void)ctx;
	if (n <= sizeof(cdc_wire) - cdc_n) {
		memcpy(cdc_wire + cdc_n, d, n);
		cdc_n += n;
	}
}

static const struct s06_port_ops cdc_port = { cdc_read, cdc_room, cdc_write };

static void host_setup(enum during d, bool dev_mode)
{
	setup(d);
	s06_link_init(&m.link[S06_LINK_CDC], &cdc_port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m.node.link[S06_LINK_CDC] = &m.link[S06_LINK_CDC];
	m.sv.menu.dev_mode = dev_mode;
}

/* A request; its reply's payload into r (its length returned). */
static size_t host(uint8_t op, const uint8_t *p, size_t n, uint8_t *r)
{
	struct s06_frame f;
	struct s06_rx dec;
	size_t i, len = 0;

	memset(&f, 0, sizeof f);
	f.route = s06_route(S06_ADDR_HOST, S06_ADDR_MASTER);
	f.id = OPXY_DEV_REQ_ID;
	f.payload[0] = op;
	memcpy(f.payload + 1, p, n);
	f.len = (uint8_t)(1u + n);
	cdc_n = 0;
	memset(r, 0, S06_MAX_PAYLOAD);
	CHECK(opxy_dev_host(&m, &f));
	s06_rx_init(&dec, S06_ADDR_HOST);
	for (i = 0; i < cdc_n; i++)
		if (s06_rx_byte(&dec, cdc_wire[i], wire_crc8_frame) ==
		    S06_RX_DELIVER) {
			len = dec.buf[3];
			memcpy(r, dec.buf + S06_HEADER, len);
		}
	return len;
}

/* The keys queued, each pressed. */
static uint8_t keys(const uint8_t *k, size_t n)
{
	uint8_t p[2u * OPXY_KEY_PAIRS] = { 0 }, r[S06_MAX_PAYLOAD];
	size_t i;

	for (i = 0; i < n; i++) {
		p[2u * i] = k[i];
		p[2u * i + 1u] = OPXY_KEY_PRESS;
	}
	/* Two bytes, or the status alone with developer mode off. */
	CHECK(host(OPXY_DEV_KEY, p, 2u * n, r) == (m.sv.menu.dev_mode ? 2u : 1u));
	return r[0];
}

/* Held mode's loop until the host's keys are all taken. */
static void run_keys(void)
{
	for (int i = 0; i < 4 * (int)OPXY_KEY_QUEUE; i++) {
		pass();
		opxy_dev_keys(&m);
	}
}

/* The screen's name and the text of every row, as a host reads them. */
static void screen(char *name, char rows[S15_ROWS][S15_LINE + 1], uint8_t *flags)
{
	uint8_t r[S06_MAX_PAYLOAD], part;
	size_t n;
	unsigned parts;

	part = 0;
	n = host(OPXY_DEV_SCREEN, &part, 1, r);
	CHECK(n > 11u && r[0] == OPXY_DEV_OK);
	parts = r[1];
	*flags = r[7];
	CHECK(r[9] == 0xFFu);	/* no set started here */
	strcpy(name, (const char *)r + 10);
	CHECK(strcmp((const char *)r + 10 + strlen(name) + 1u,
		     m.sv.menu.title) == 0);
	for (part = 1; part < parts; part++) {
		size_t at;

		n = host(OPXY_DEV_SCREEN, &part, 1, r);
		CHECK(r[0] == OPXY_DEV_OK && r[6] == part - 1u);
		at = 8u + strlen((const char *)r + 8) + 1u;
		memcpy(rows[part - 1u], r + at, n - at);
		rows[part - 1u][n - at] = '\0';
	}
	part = (uint8_t)parts;
	CHECK(host(OPXY_DEV_SCREEN, &part, 1, r) == 1u && r[0] == OPXY_DEV_BAD);
}

/* The rows a host read are the console's. */
/* cppcheck-suppress constParameter ; a char array of rows does not convert to const in C11 */
static bool rows_shown(char rows[S15_ROWS][S15_LINE + 1])
{
	for (unsigned i = 0; i < m.sv.menu.con.count; i++) {
		size_t n;
		const char *t = s15_console_row(&m.sv.menu.con, i, &n);

		if (strlen(rows[i]) != n || memcmp(rows[i], t, n) != 0)
			return false;
	}
	return m.sv.menu.con.count != 0u;
}

/* Key events queued as (key, action) pairs. */
static uint8_t queue(const uint8_t *pairs, size_t n)
{
	uint8_t r[S06_MAX_PAYLOAD];

	CHECK(host(OPXY_DEV_KEY, pairs, 2u * n, r) == 2u);
	return r[0];
}

/* The host walks to System and holds the factory reset as a person would
 * (S-15.015): the white knob only cancels, 7 held for 5 s runs it once. */
static void test_host_keys_reset(void)
{
	static const uint8_t system[] = { 0x12 }, white[] = { 0x0D },
			     down[] = { 0x14, OPXY_KEY_DOWN },
			     up[] = { 0x14, OPXY_KEY_UP };
	char name[32], rows[S15_ROWS][S15_LINE + 1];
	uint8_t flags;

	host_setup(D_RELEASE, true);
	screen(name, rows, &flags);
	CHECK(strcmp(name, "main") == 0);
	CHECK(keys(system, 1) == OPXY_DEV_OK);
	run_keys();
	screen(name, rows, &flags);
	CHECK(strcmp(name, "system") == 0 && (flags & OPXY_SCREEN_ARMED) == 0u);
	CHECK(rows_shown(rows));
	CHECK(queue(down, 1) == OPXY_DEV_OK);
	run_keys();
	screen(name, rows, &flags);
	CHECK((flags & OPXY_SCREEN_ARMED) != 0u && removes == 0u);
	CHECK(rows_shown(rows));
	CHECK(keys(white, 1) == OPXY_DEV_OK);
	run_keys();
	later(S15_CONFIRM_HOLD_MS);
	screen(name, rows, &flags);
	CHECK((flags & OPXY_SCREEN_ARMED) == 0u && removes == 0u);
	CHECK(queue(up, 1) == OPXY_DEV_OK && queue(down, 1) == OPXY_DEV_OK);
	run_keys();
	later(S15_CONFIRM_HOLD_MS - 1u);
	CHECK(removes == 0u);
	later(1u);
	CHECK(removes == 3u);
	CHECK(queue(up, 1) == OPXY_DEV_OK);
	run_keys();
	later(S15_CONFIRM_HOLD_MS);
	CHECK(removes == 3u && m.sv.menu.screen == S15_SCR_RESULT);
}

/* Keys queued behind a hold whose time runs a long operation wait for its
 * end, then act, where the PSoC's keys pressed during it have none. */
static void test_host_keys_after_long(void)
{
	static const uint8_t k[] = { 0x12, 0x12 },
			     down[] = { 0x06, OPXY_KEY_DOWN },
			     rest[] = { 0x06, OPXY_KEY_UP, 0x15, OPXY_KEY_PRESS };
	char name[32], rows[S15_ROWS][S15_LINE + 1];
	uint8_t flags;

	host_setup(D_RELEASE, true);
	CHECK(keys(k, 2) == OPXY_DEV_OK && queue(down, 1) == OPXY_DEV_OK);
	run_keys();
	CHECK(formats == 0u && m.sv.menu.armed == S15_ARM_FORMAT);
	CHECK(queue(rest, 2) == OPXY_DEV_OK);
	clock_ahead(S15_CONFIRM_HOLD_MS);
	mains = 0;
	run_keys();
	CHECK(formats == 1u && mains == 1u);
	screen(name, rows, &flags);
	CHECK(strcmp(name, "main") == 0);
}

/* Developer mode off: refused, nothing queued, nothing reported. */
static void test_host_keys_refused(void)
{
	static const uint8_t k[] = { 0x12 };
	uint8_t r[S06_MAX_PAYLOAD], p = 0;

	host_setup(D_RELEASE, false);
	CHECK(keys(k, 1) == OPXY_DEV_REFUSED);
	run_keys();
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	CHECK(host(OPXY_DEV_SCREEN, &p, 1, r) == 1u &&
	      r[0] == OPXY_DEV_REFUSED);
}

/* Malformed requests queue nothing; a full queue refuses the request. */
static void test_host_keys_bad(void)
{
	uint8_t p[2u * OPXY_KEY_PAIRS], r[S06_MAX_PAYLOAD];

	host_setup(D_RELEASE, true);
	p[0] = S15_NKEYS;
	p[1] = OPXY_KEY_PRESS;
	CHECK(host(OPXY_DEV_KEY, p, 2, r) == 2u && r[0] == OPXY_DEV_BAD);
	p[0] = 0x12;
	p[1] = 3;
	CHECK(host(OPXY_DEV_KEY, p, 2, r) == 2u && r[0] == OPXY_DEV_BAD);
	CHECK(host(OPXY_DEV_KEY, p, 1, r) == 2u && r[0] == OPXY_DEV_BAD);
	CHECK(host(OPXY_DEV_KEY, p, 0, r) == 2u && r[0] == OPXY_DEV_OK &&
	      r[1] == 0u);
	/* 32 presses fill the queue; one more down is refused. */
	for (unsigned i = 0; i < OPXY_KEY_PAIRS; i++) {
		p[2u * i] = 0x01;		/* tempo: no effect on the main screen */
		p[2u * i + 1u] = OPXY_KEY_PRESS;
	}
	CHECK(host(OPXY_DEV_KEY, p, sizeof p, r) == 2u && r[0] == OPXY_DEV_OK &&
	      r[1] == OPXY_KEY_QUEUE);
	p[1] = OPXY_KEY_DOWN;
	CHECK(host(OPXY_DEV_KEY, p, 2, r) == 2u && r[0] == OPXY_DEV_REFUSED &&
	      r[1] == OPXY_KEY_QUEUE);
	run_keys();
	CHECK(host(OPXY_DEV_KEY, p, 0, r) == 2u && r[1] == 0u);
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	/* No frame buffer without the panel bus. */
	memset(p, 0, 6);
	p[4] = 16u;
	CHECK(host(OPXY_DEV_FRAME, p, 5, r) == 1u && r[0] == OPXY_DEV_REFUSED);
	p[4] = 0u;
	CHECK(host(OPXY_DEV_FRAME, p, 5, r) == 1u && r[0] == OPXY_DEV_BAD);
}

/* After the OS write, digit 6 continues to the OS at the first press
 * (S-15.190), and digit 8 leaves the result at the first press. */
static void test_restart_after_os_write(void)
{
	setup(D_RELEASE);
	press(0x10);			/* digit 3: the developer menu */
	press_long(0x12);			/* digit 5: the OS write */
	CHECK(m.sv.menu.osw_written);
	press(0x13);			/* digit 6 */
	CHECK(continues == 1u && resets == 0u);
}

static void test_back_after_os_write(void)
{
	setup(D_RELEASE);
	press(0x10);
	press_long(0x12);
	press(0x15);			/* digit 8: back to the developer menu */
	CHECK(m.sv.menu.osw == S15_OSW_VIEW_NONE);
	CHECK(strcmp(m.sv.menu.title, "Developer") == 0);
}

/* S-15.092: digit 6 pressed while the write runs has no effect, also when
 * its frames are read after it; a press after it continues to the OS. */
static void test_press_during_os_write(void)
{
	setup(D_PRESS_6);
	press(0x10);
	press_long(0x12);
	pass();
	CHECK(continues == 0u);
	press(0x13);
	CHECK(continues == 1u && resets == 0u);
}

/* More frames during the write than the link's inbox holds (S06_INBOX):
 * the last bitmap still sets the key state, so digit 6, held during the
 * write and released, continues to the OS at the next press. */
static void test_backlog_after_os_write(void)
{
	setup(D_BACKLOG);
	press(0x10);
	press_long(0x12);
	CHECK(continues == 0u);
	press(0x13);
	CHECK(continues == 1u && resets == 0u);
}

/* The format and the factory reset: the first digit 8 after them acts. */
static void test_back_after_format(void)
{
	setup(D_RELEASE);
	press(0x12);			/* digit 5: System */
	press(0x12);			/* digit 5: the format menu */
	hold_long(0x06);		/* digit 1 held: the format */
	CHECK(formats == 1u);
	CHECK(m.sv.menu.screen == S15_SCR_UPGRADE);
	mains = 0;
	press(0x15);			/* digit 8: back to the main screen */
	CHECK(mains == 1u);
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
}

/* --- S-15.013: the keys before the first screen --- */

/* A press made while the line under the logo shows (S-13c.025) chooses
 * nothing: digit 4 pressed and released, digit 5 pressed and released. */
static void test_press_while_loading(void)
{
	setup_before(D_RELEASE);
	bitmap(0x0D, -1);		/* the white knob, digit 4 */
	bitmap(-1, -1);
	bitmap(0x12, -1);		/* digit 5 */
	bitmap(-1, -1);
	menu_start();
	pass();
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	/* the first press with the menu up acts */
	press(0x0D);
	CHECK(m.sv.menu.screen == S15_SCR_INFO);
}

/* A key pressed while loading and held through the start does nothing,
 * not even at its release; pressed again it acts. */
static void test_held_through_start(void)
{
	setup_before(D_RELEASE);
	bitmap(0x09, -1);		/* M4, digit 4 */
	menu_start();
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	bitmap(0x09, 0x18);		/* another key changes: M4 still down */
	pass();
	bitmap(-1, -1);
	pass();
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	press(0x09);
	CHECK(m.sv.menu.screen == S15_SCR_INFO);
}

/* The white knob held from power-on, which the key-status reply names and
 * no bitmap does (S-07.032): a bitmap for another key while it is still
 * down does not make it a press, nor does its release. */
static void test_knob_from_power_on(void)
{
	uint8_t status[10] = { 0 };
	struct s07_key_status ks;

	setup_before(D_RELEASE);
	status[0x0D >> 3] = (uint8_t)(1u << (0x0D & 7u));
	s07_key_status_given(&m.psoc, status, sizeof status, &ks);
	CHECK(ks.replied && ks.hold);
	menu_start();
	bitmap(0x0D, 0x18);		/* com pressed, the knob still down */
	pass();
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	bitmap(0x0D, -1);
	pass();
	bitmap(-1, -1);			/* the knob let go */
	pass();
	CHECK(m.sv.menu.screen == S15_SCR_MAIN);
	press(0x0D);
	CHECK(m.sv.menu.screen == S15_SCR_INFO);
}

/* A key held through the start arms no hold: digit 7 down from the
 * loading on does not start the factory reset on System; let go and held
 * again, it does (S-15.015). */
static void test_held_key_arms_nothing(void)
{
	setup_before(D_RELEASE);
	bitmap(0x14, -1);		/* digit 7 */
	menu_start();
	bitmap(0x14, 0x12);		/* digit 5: System */
	pass();
	bitmap(0x14, -1);
	pass();
	CHECK(m.sv.menu.screen == S15_SCR_SYSTEM);
	later(S15_CONFIRM_HOLD_MS);
	pass();
	CHECK(removes == 0u && m.sv.menu.armed == S15_ARM_NONE);
	bitmap(-1, -1);
	pass();
	hold_long(0x14);
	CHECK(removes == 3u);
}

static void test_back_after_factory_reset(void)
{
	setup(D_RELEASE);
	press(0x12);			/* System */
	hold_long(0x14);		/* digit 7 held: the reset */
	CHECK(removes == 3u);
	CHECK(m.sv.menu.screen == S15_SCR_RESULT);
	press(0x15);			/* digit 8 */
	CHECK(m.sv.menu.screen == S15_SCR_SYSTEM);
}

int main(void)
{
	RUN(test_restart_after_os_write);
	RUN(test_back_after_os_write);
	RUN(test_press_during_os_write);
	RUN(test_backlog_after_os_write);
	RUN(test_back_after_format);
	RUN(test_back_after_factory_reset);
	RUN(test_press_while_loading);
	RUN(test_held_through_start);
	RUN(test_knob_from_power_on);
	RUN(test_held_key_arms_nothing);
	RUN(test_host_keys_reset);
	RUN(test_host_keys_after_long);
	RUN(test_host_keys_refused);
	RUN(test_host_keys_bad);
	return check_summary("test_board_held_keys");
}
