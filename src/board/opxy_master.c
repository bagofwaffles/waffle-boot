/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The master's boot steps on the OP-XY (see waffle/board/opxy_boot.h). */
#include "waffle/board/opxy_boot.h"

#include <string.h>

#include "waffle/board/opxy_bridge.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_dmc.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/bf70x_sec.h"
#include "waffle/s01_memmap.h"
#include "waffle/wire_crc.h"

struct opxy_master opxy_master;

/* Buffers. On the target these are in DDR .bss, below the heap. */
static uint8_t flash_scratch[S02_SECTOR_SIZE];
static uint8_t emmc_scratch[S03_SECTOR];
static uint8_t psoc_tx[OPXY_UART_BUF];
/* S-13b.070: DMA11's ring, on cache lines of its own (wb_uart_init_dma()). */
static uint8_t psoc_rx[OPXY_PSOC_RING] __attribute__((aligned(WB_DCACHE_LINE)));
/* S-09.002: the running slot's stream, read once, checked, compared, then sent. */
static OPXY_TESTOS_UNUSED uint8_t slot_stream[S02_SLOT_CAPACITY];	/* the test OS streams nothing */

/* S-07's events: their lines' wording is free (S-14.009) and their level
 * is 3 or less (S-14.060); each is logged at level 3 as
 * "psoc <event> <a> <b>" in hexadecimal. */
static void psoc_log(void *ctx, int event, uint32_t a, uint32_t b)
{
	struct opxy_master *m = ctx;
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "psoc ");
	s14_text_hex(&t, (uint32_t)event, 1u);
	s14_text_str(&t, " ");
	s14_text_hex(&t, a, 1u);
	s14_text_str(&t, " ");
	s14_text_hex(&t, b, 1u);
	s14_log_text(&m->sv.log, S14_INFO, S14_TAG("psoc"), &t);
}

/* S-07.031 to S-15: the menus keep the keys from the start (S-15.013). */
static void psoc_key(void *ctx, uint8_t key, bool down)
{
	struct opxy_master *m = ctx;

	s15_menu_key(&m->sv.menu, key, down);
}

static void psoc_shutdown(void *ctx)
{
	struct opxy_master *m = ctx;

	/* S-07.072 via S-04.063: the reports at once. Held mode replaces
	 * this with the full shutdown once the CPU link's tunnel is up. */
	s07_shutdown_reports(&m->psoc);
}

/* S-07.054: the battery wait's USB mass storage. */
void opxy_battery_usb(void *ctx, bool on)
{
	OPXY_TIMING_BATTERY(on); opxy_usb_battery(ctx, on);
	/* S-02.095: the wait loop has started, with USB up: alive. */
	if (on)
		opxy_start_finished(ctx);
}

/* Each round of a wait loop services USB: the stack's own work only, since a
 * wait never takes the host's input (S-10a.049; the battery wait's disk set
 * has no host link). Nothing but the early logo is on the screen before
 * held mode opens it (S-15.013), and no charger rule is applied here
 * (S-16.192). */
static void psoc_service(void *ctx)
{
	opxy_usb_breathe();
	opxy_psoc_rx_report(ctx);
}

/* S-07.076: the PSoC's other messages are S-10c's common command set. */
static void psoc_common(void *ctx, const struct s06_frame *f)
{
	opxy_copro_common(ctx, S10C_LINK_PSOC, f);
}

/* --- S-13b bring-up --- */

static void apply_gpio(const struct opxy_gpio *g, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		if (g[i].dir == OPXY_IN)
			(void)wb_gpio_input(g[i].pin);
		else
			(void)wb_gpio_output(g[i].pin,
					     g[i].dir == OPXY_OUT_HIGH);
	}
}

static void bringup(void *ctx)
{
	struct opxy_master *m = ctx;

	/* S-13b.003, .004, .006 */
	(void)wb_pinmux_apply(&opxy_pinmux_master);
	apply_gpio(opxy_gpio_master, opxy_gpio_master_n);
	(void)wb_sec_init();
	opxy_display_dma_take(); OPXY_TESTOS(opxy_testos_display_dma_stop();)	/* the test OS: S-05.016 lets the loading bootloader leave the display's DMA configured */
	/* S-01.023: the front reset and started the panel and sent the logo. */
	if (m->front && (m->ho.flags & FR_HO_PANEL_UP) != 0u)
		opxy_display_adopt(m->ho.panel_reset_ms);
	else
		opxy_display_handshake(m->dma9_xmod, m->dma9_ymod);

	/* S-13b.010, .011: the flash on SPI2. */
	(void)wb_spi_init(&m->hw.flash_spi, OPXY_SPI_FLASH, &opxy_spi_flash,
			  &m->hw.time, OPXY_SPI_WORD_US);
	m->hw.flash.spi = &m->hw.flash_spi;
	m->hw.flash.size = OPXY_FLASH_SIZE;
	m->hw.flash.busy_timeout_us = OPXY_FLASH_BUSY_US;
	/* S-13b.015: nothing is known of the part yet, so the first frame is
	 * an RDSR that waits for WIP = 0. */
	m->hw.flash.busy = true;
	m->hw.flash.idle = opxy_usb_breathe;	/* while a sector erases */
	opxy_flash_writer(&m->hw, false);
	m->flash_ops = opxy_flash_ops(&m->hw);
	m->flash.flash = &m->flash_ops;
	m->flash.crc32 = opxy_crc32_long;
	m->flash.scratch = flash_scratch;
	m->flash.scratch_len = sizeof(flash_scratch); OPXY_CRUMB(2u);

	/* S-13b.070: the PSoC link. UART1 waits for held mode (S-08.001). */
	(void)opxy_psoc_uart_open(m); OPXY_CRUMB(3u); OPXY_TIME(BOOT_TIME_BRINGUP);
}

/* bf703.ld; weak for the host build, where they are 0 and nothing is compared. */
extern char wb_ddr_image_start[] __attribute__((weak));
extern char wb_data_start[] __attribute__((weak));

/* S-13a.021: with interrupts off, so nothing else meets the probe. */
static bool ddr_check(void *ctx)
{
	uint32_t mask = wb_core_cli();
	bool aliased = wb_dmc_aliases(OPXY_DDR_BASE, OPXY_DDR_ALIAS);

	(void)ctx;
	wb_core_sti(mask); OPXY_CRUMB(4u); OPXY_TIME(BOOT_TIME_DDR_CHECK);
	return !aliased;
}

static void psoc_link_init(struct opxy_master *m);

/* The DDR check's line: a few frames at 1 Mbaud take well under this. */
#define DDR_LINE_FLUSH_US 20000u

/*
 * S-13a.021, S-16.023: the check runs before the PSoC link opens, so the
 * link is opened (S-07.020-S-07.022) for the one line, which goes out
 * before the halt (S-14.060). Then S-04.071's DDR row.
 * S-04.071.
 */
static void ddr_fatal(void *ctx)
{
	struct opxy_master *m = ctx;
	struct s14_text t;

	psoc_link_init(m);
	(void)s07_open(&m->psoc);
	s14_text_init(&t);
	s14_text_str(&t, "ddr bank configuration wrong");
	s14_log_text(&m->sv.log, S14_ERROR, S14_TAG("boot"), &t);
	(void)wb_uart_flush(&m->hw.uart[OPXY_UART_PSOC], &m->hw.time,
			    DDR_LINE_FLUSH_US);
	opxy_fatal(S04_FATAL_DDR_CHECK);
}

/* S-02.065, then S-04.042. */
static void metadata(void *ctx, uint32_t *state, bool *nonzero)
{
	struct opxy_master *m = ctx;
	bool restored;

	opxy_flash_writer(&m->hw, true);	/* S-13b.017 */
	m->meta_restore_failed = s02_meta_restore(&m->flash, &restored) != 0;
	opxy_flash_writer(&m->hw, false);
	if (m->flash_ops.read(m->flash_ops.ctx, S02_META_OFF, m->meta_raw,
			      S04_META_SIZE) != 0) {
		unsigned i;

		for (i = 0; i < S04_META_SIZE; i++)
			m->meta_raw[i] = 0xFFu;
		m->meta_failed = true;
	}
	s04_meta_read(m->meta_raw, &m->meta);
	opxy_startup_read(m, opxy_otp_get);	/* S-04.047, S-17.015 */
	/* S-02.097: the boot store, once per start, before the decision; the
	 * test OS (waffle/testos.h) is held mode whatever it says */
	OPXY_TESTOS_OR((void)0, opxy_boot_store_load(m));
	*state = m->meta.state;
	*nonzero = s04_m6_needed(m->meta.sw_version); OPXY_CRUMB(5u); OPXY_TIME(BOOT_TIME_METADATA);
}

/* S-04.001, S-04.004: the record in this core's L1 scratch. */
static uint16_t reboot_reason(void *ctx)
{
	struct opxy_master *m = ctx;
	uint8_t *rec = (uint8_t *)(uintptr_t)S04_REBOOT_ADDR;

	m->boot_reason = s04_reboot_consume(rec);
	return m->boot_reason;
}

#if !defined(OPXY_TEST_OS)
/*
 * The memory this image was loaded into, for the running slot's inference:
 * only its own DDR code and constant data, which no other image's run can
 * have left there, this run has not written, and which is always mapped. Its
 * initial data is not compared: this run may have written it. L1 is not
 * compared: this image runs part of L1 data as cache, a load from which
 * faults (EXCAUSE 0x23, seen on a unit), and L1 code takes no data loads. A
 * block elsewhere is simply not compared.
 */
static bool loaded_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
	const uint64_t end = (uint64_t)addr + len;

	(void)ctx;
	if (addr < (uint32_t)(uintptr_t)wb_ddr_image_start ||
	    end > (uint32_t)(uintptr_t)wb_data_start)
		return false;
	OPXY_CRUMB_READ(addr); memcpy(buf, (const void *)(uintptr_t)addr, len);
	return true;
}

/*
 * S-01.002, S-02.054, S-09.002: the slot this image was booted from, inferred
 * from memory against both slots' streams (S-01 question 10), with that
 * slot's stream read once into slot_stream: checked there (S-02.016),
 * compared from there, and sent from there. The slot whose bootloader version
 * matches the loaded one is read first; the other is ruled out by those
 * bytes, or compared whole when they match. A failed slot can start once
 * while the selector names the other (S-02.023). Where both slots hold
 * this image it is the slot later starts settle on (S-02.054's tie-break),
 * as the front names it. When the slot is not established (neither slot
 * matches, a read failure) it is the selected slot, 0 when the selector
 * cannot be read, and a log line; *established says which.
 */
static struct s02_image stream_image;

unsigned opxy_stream_slot(struct opxy_master *m, const struct opxy_stream_src *src,
			  bool *established)
{
	struct s02_image *im = &stream_image;
	struct s02_selector sel;
	unsigned selected = 0u, first, slot = 2u;

	if (s02_read_selector(&m->flash, &sel) == S02_OK)
		selected = s02_selected_slot(sel.index);
	s02_image_init(im, slot_stream, sizeof(slot_stream), src->fast, src->fast_ctx);
	OPXY_CRUMB(11u); first = s02_running_first(&m->flash, src->mem, &src->probe, selected, im);
	OPXY_TIME(BOOT_TIME_SLOT_PROBED);
	(void)s02_image_read(&m->flash, first, im); OPXY_TIME(BOOT_TIME_SLOT_READ); OPXY_TIMING_SLOT_READ(im);
	(void)s02_image_check(&m->flash, im); OPXY_TIME(BOOT_TIME_SLOT_CHECKED); OPXY_TIMING_SLOT_CHECK(im);
	/* 2, no slot: neither matches, or a failure */
	if (s02_running_image(&m->flash, src->mem, &src->probe, im, first, &slot) != S02_OK)
		slot = 2u;
	*established = slot < 2u; OPXY_CRUMB(12u); OPXY_TIME(BOOT_TIME_SLOT_FOUND);
	if (*established)
		return slot;
	s14_log_str(&m->sv.log, S14_WARNING, S14_TAG("boot"),
		    "running slot not established: the selected slot");
	return selected;
}

/* S-09.002: the stream of `slot` in slot_stream, read now if another slot's
 * or none is there, and S-02.016's verdict on it; *len is 0 for a slot that
 * is not to be sent. */
bool opxy_stream_checked(struct opxy_master *m, unsigned slot, const uint8_t **stream,
			 uint32_t *len)
{
	struct s02_image *im = &stream_image;

	if (im->slot != slot) {
		(void)s02_image_read(&m->flash, slot, im); OPXY_TIMING_SLOT_READ(im);
		(void)s02_image_check(&m->flash, im); OPXY_TIMING_SLOT_CHECK(im);
	}
	*stream = slot_stream;
	*len = im->slot == slot && im->good ? im->len : 0u;
	return *len != 0u;
}

/* The running slot by this image's own memory and version, the stream read
 * at 50 MHz. */
static unsigned running_slot(struct opxy_master *m, bool *established)
{
	static const struct s02_mem_ops loaded = { NULL, loaded_read };
	const struct opxy_stream_src src = {
		&loaded,
		{ (uint32_t)(uintptr_t)opxy_bl_version, sizeof(opxy_bl_version) },
		opxy_flash_read_stream, &m->hw.flash,
	};

	return opxy_stream_slot(m, &src, established);
}
#endif

/* The bridge controller's last fault words and its second four, for the
 * link's record. */
static void bridge_fault(void *ctx, uint32_t out[4])
{
	opxy_bus_fault(ctx, out);
}

static void bridge_fault_more(void *ctx, uint32_t out[4])
{
	opxy_bus_fault_more(ctx, out);
}

/* S-09.001-S-09.008, then S-09.015. */
static bool slave_start(void *ctx)
{
	struct opxy_master *m = ctx;
	bool established;
	unsigned slot = OPXY_TESTOS_OR((opxy_testos_bridge_enter(), established = false, (void)established, 0u), running_slot(m, &established));	/* the test OS: olive; it streams no slot, so no flash or memory is read to find one */
	bool good;
	const uint8_t *stream = slot_stream;
	uint32_t len = 0;
	uint64_t init_by;
	int end = 0, rc;
	OPXY_CRUMB(9u);
	/* S-01.002: for the status bar and the prompt's Running value. */
	m->running_established = OPXY_TESTOS_OR(false, established);	/* the test OS streams no slot */
	m->running_slot = (uint8_t)slot;
	/* S-09.002: the slot's CRC first (S-02.016), over the bytes to be sent. */
	good = OPXY_TESTOS_OR(((void)slot, false), opxy_stream_checked(m, slot, &stream, &len));
	/* S-13b.020, .021: the boot link on SPI0, SEL3. */
	(void)wb_spi_init(&m->hw.link_spi, OPXY_SPI_LINK, &opxy_spi_boot_link,
			  &m->hw.time, OPXY_BOOT_LINK_WORD_US);
	m->boot_ops = OPXY_BOOT_OPS(&m->hw); OPXY_TIMING_STREAM(&m->boot_ops);
	rc = OPXY_TESTOS_OR(((void)len, (void)good, (void)end, (void)stream, S09_BOOT_STREAMED), /* S-05.022: the loading bootloader booted the slave too */
			    s09_boot_slave(&m->boot_ops, &m->clock, stream, len, good, &end)); OPXY_TIME(BOOT_TIME_STREAM_END);
	/* S-09.060: T_INIT for the init request session runs from the
	 * stream's last byte; the slave's start-up and its eMMC bring-up come
	 * first (S-09.018). */
	init_by = wb_time_deadline(&m->hw.time, OPXY_TESTOS_OR(OPXY_TESTOS_READY_CAP_US, S09_T_INIT_US));

	/* S-13b.040: the bridge's end, PC11 a GPIO driven high, PC5 in. */
	(void)wb_spi_init(&m->hw.link_spi, OPXY_SPI_LINK, &opxy_spi_bridge_pre,
			  &m->hw.time, S09_T_REPLY_US);
	(void)wb_gpio_gpio(OPXY_PC11); (void)wb_gpio_output(OPXY_PC11, true);	/* S-13b.040: PORTC_FER's bit first, or the select never reaches the pin */
	(void)wb_gpio_input(OPXY_PC5);
	m->bus_ops = opxy_bus_ops(&m->hw);
	s09_master_init(&m->bridge, &m->bus_ops, &m->clock);
	s09_master_fault_source(&m->bridge, bridge_fault, &m->hw);	/* the link's record */
	s09_master_fault_more(&m->bridge, bridge_fault_more, &m->hw);
	if (rc != S09_BOOT_STREAMED)
		s09_master_set_absent(&m->bridge);
	else if (!OPXY_TESTOS_OR(opxy_testos_bridge_ready(&m->bus_ops, init_by), opxy_bridge_init_ready(&m->bus_ops, init_by)) ||
		 !s09_master_start(&m->bridge))
		s09_master_set_absent(&m->bridge);
	OPXY_TIME(BOOT_TIME_LINKED); OPXY_TESTOS(opxy_testos_slave_bringup(m);) m->slave_present = !m->bridge.absent;	/* the test OS: blue and the eMMC, or brown */
	/* S-13b.083: the end the slave's replies use, which the slave cannot log */
	s14_log_str(&m->sv.log, S14_INFO, S14_NO_TAG,
		    opxy_bridge_end_line(WB_SPI_REPLY_END_BUILD));

	/* S-05.005: the eMMC is the slave's, through the bridge. */
	m->emmc_ops = opxy_bridge_emmc(&m->bridge);
	m->emmc.ops = &m->emmc_ops;
	m->emmc.crc32 = opxy_crc32_long;
	m->emmc.boot_size = m->slave_present ?
				    opxy_bridge_boot_size(&m->bridge) : 0u;
	m->emmc.scratch = emmc_scratch;
	m->emmc.scratch_len = sizeof(emmc_scratch);
	m->emmc.max_xfer = 128u; OPXY_TIME(BOOT_TIME_SLAVE_DONE);
	return m->slave_present;
}

/* The PSoC link's node and S-07's state, and the log over them. */
/*
 * S-05.004: draw and light the boot logo, called by s07_startup() after the
 * battery wait's first requests and any charge wait, before the keys. The panel is black from DISPON
 * (S-13c.010); 0x51 {3C FF 02} (S07_REPORT_HELD) comes after the frame's last
 * byte, so the logo is the first thing lit (S-13c.019).
 */
static void early_logo_hook(void *ctx)
{
	struct opxy_master *m = ctx;

	if (opxy_display_early_logo()) {
		s07_report(&m->psoc, S07_REPORT_HELD);
		m->display_lit = true; OPXY_TESTOS(opxy_testos_paint(OPXY_PAINT_VIOLET);)	/* the test OS: violet, the brightness message sent */
	} OPXY_CRUMB(8u); OPXY_TIME(BOOT_TIME_LOGO);
}

static void psoc_link_init(struct opxy_master *m)
{
	s06_node_init(&m->node, wire_crc8_frame, &m->clock);
	s06_link_init(&m->link[S06_LINK_PSOC], &opxy_port_ops,
		      &m->hw.uart[OPXY_UART_PSOC], S06_ADDR_MASTER,
		      S06_ADDR_PSOC);
	m->node.link[S06_LINK_PSOC] = &m->link[S06_LINK_PSOC];
	/* S-14.022: the log's PSoC sink is on from the start. */
	opxy_log_start(m);
	m->psoc_ops.ctx = m;
	m->psoc_ops.log = psoc_log;
	m->psoc_ops.key_event = psoc_key;
	m->psoc_ops.shutdown = psoc_shutdown;
	m->psoc_ops.usb_msc = opxy_battery_usb;
	m->psoc_ops.early_logo = early_logo_hook;
	m->psoc_ops.service = psoc_service;
	m->psoc_ops.common = psoc_common;
	m->psoc_ops.crc16 = wire_crc16_x25;
	s07_init(&m->psoc, &m->node, S06_LINK_PSOC, &m->psoc_ops);
}

/* S-07.023 steps 1-6a. */
static enum s05_keys psoc_start(void *ctx)
{
	struct opxy_master *m = ctx;
	struct s07_key_status *ks = &m->key_status;
	struct s14_text t;

	OPXY_TESTOS(opxy_testos_paint(OPXY_PAINT_YELLOW);) psoc_link_init(m);	/* the test OS: the panel's one initialization, then yellow, before the link */
	/* S-01.007: the SP the ROM entered the INIT stage with, which no unit
	 * has shown yet (L2 or L1 scratch), and the first stage's L1DM_DCTL
	 * and L1DM_DCPLB_DFLT at that entry (opxy_init_entry.S; S-01.021
	 * gives 0x00000202 and 0x00000F00). */
	s14_text_init(&t);
	s14_text_str(&t, OPXY_TESTOS_OR("entry sp ", "init sp "));	/* the test OS: S-05.014's SP */
	s14_text_hex(&t, OPXY_TESTOS_OR(opxy_testos_entry[0], opxy_init_sp), 8u);
	OPXY_TESTOS_OR((void)0, s14_text_str(&t, " dctl "));	/* S-01.021, Q12: the INIT stage's, so not in the test OS */
	OPXY_TESTOS_OR((void)0, s14_text_hex(&t, opxy_init_dctl, 8u));
	OPXY_TESTOS_OR((void)0, s14_text_str(&t, " dflt "));
	OPXY_TESTOS_OR((void)0, s14_text_hex(&t, opxy_init_dflt, 8u));
	s14_log_text(&m->sv.log, OPXY_TESTOS_OR(S14_INFO, S14_DEBUG), S14_TAG("boot"), &t); OPXY_TESTOS(opxy_testos_log(m);) OPXY_CRUMB(6u);
	/* S-07.032: the one key-status reply */
	ks->replied = false;
	ks->hold = false;
	ks->debug = false;
	ks->precomp = false;
	if (m->front) {
		/* S-01.023: the front greeted, read the power state and battery,
		 * lit the logo, subscribed and read the keys; S-07.054's wait loop
		 * is left for here. A log line when S-07.021's wait ran. */
		const uint32_t f = m->ho.flags;

		m->link[S06_LINK_PSOC].seq = m->ho.psoc_seq;
		s07_resume(&m->psoc, (f & FR_HO_GREETED) != 0u, (f & FR_HO_POWER_KNOWN) != 0u,
			   m->ho.power_state, m->ho.battery, m->ho.battery_len);
		if ((f & FR_HO_GREET_WAITED) != 0u)
			psoc_log(m, S07_LOG_GREET_FAILED, 0u, 0u);
		m->display_lit = (f & FR_HO_PANEL_LIT) != 0u;
		OPXY_TIMING_FRONT(m);
		if (s07_battery_rest(&m->psoc) == S07_POWEROFF)
			opxy_powered_off(m);	/* S-07.051 */
		s07_key_status_given(&m->psoc, (f & FR_HO_KEYS_REPLIED) != 0u ? m->ho.keys : NULL,
				     m->ho.key_len, ks);
	} else if (OPXY_TESTOS_OR(opxy_testos_started(s07_startup(&m->psoc, ks)), OPXY_STARTUP(&m->psoc, ks)) == S07_POWEROFF) {	/* the test OS: green once it returns */
		/* S-07.051: the PSoC removes the power. */
		opxy_powered_off(m);
	} OPXY_CRUMB(7u);
	/* S-05.004: the boot logo is lit in early_logo_hook(), before the key
	 * status and the battery wait's loop, so it shows during the loop and
	 * not seconds later; a charge wait stays dark. */
	/* S-10a's serial flag: the debug key is not held when the PSoC does
	 * not answer. */
	m->debug_key = ks->replied && ks->debug; OPXY_TESTOS(opxy_testos_log_keys(m);)
	/* S-16.008: no answer counts as held. */
	if (!ks->replied)
		return S05_KEYS_SILENT;
	return ks->hold ? S05_KEYS_HELD : S05_KEYS_NOT_HELD;
}

/* --- S-05 on the master --- */

int opxy_psoc_uart_open(struct opxy_master *m)
{
	m->psoc_rx_logged = 0;
	return wb_uart_init_dma(&m->hw.uart[OPXY_UART_PSOC], OPXY_UART_PSOC,
				&opxy_uart_psoc, &m->hw.time, OPXY_PSOC_BYTE_NS,
				psoc_rx, sizeof(psoc_rx), psoc_tx,
				sizeof(psoc_tx));
}

void opxy_psoc_rx_report(struct opxy_master *m)
{
	const struct wb_uart *u = &m->hw.uart[OPXY_UART_PSOC];
	uint32_t sum = u->overruns + u->rx_lost + u->rx_gaps + u->dma_errors;
	struct s14_text t;

	if (sum == m->psoc_rx_logged)
		return;
	m->psoc_rx_logged = sum;
	s14_text_init(&t);
	s14_text_str(&t, "rx overruns ");
	s14_text_dec(&t, u->overruns);
	s14_text_str(&t, ", ring lost ");
	s14_text_dec(&t, u->rx_lost);
	s14_text_str(&t, ", maybe lost ");
	s14_text_dec(&t, u->rx_gaps);
	s14_text_str(&t, ", dma errors ");
	s14_text_dec(&t, u->dma_errors);
	s14_log_text(&m->sv.log, S14_WARNING, S14_TAG("psoc"), &t);
}

int opxy_psoc_drain(struct opxy_master *m)
{
	opxy_psoc_rx_report(m);
	return wb_uart_flush(&m->hw.uart[OPXY_UART_PSOC], &m->hw.time,
			     OPXY_PSOC_DRAIN_US);
}

/*
 * S-05.003, S-05.004: the PMIC, the display, then 0x51 {3C FF 32}, sent
 * before the load: a log line queued ahead of it would otherwise keep it in
 * the ring until the teardown (S-05.016).
 */
static void pre_load(void *ctx)
{
	struct opxy_master *m = ctx;

	(void)s07_pmic(&m->psoc);
	opxy_display_boot_logo();
	s07_report(&m->psoc, S07_REPORT_BOOT);
	(void)opxy_psoc_drain(m); OPXY_TIME(BOOT_TIME_PRE_LOAD);
}

/* The last stage's and release's outcomes in full, for a failed start's
 * account (load_failed()). */
static struct s03_load stage_load;
static bool release_replied;
static uint8_t release_reply;

/* S-05.005-S-05.007 over S-03.040-S-03.048. */
static enum s05_fail stage(void *ctx, uint32_t *length)
{
	struct opxy_master *m = ctx;
	const struct s03_load *ld = &stage_load;

	OPXY_TIMING_COPY(true); s03_load_os(&m->emmc, (uint8_t *)(uintptr_t)OPXY_STAGING, S03_OS_MAX,
		    &stage_load); OPXY_TIMING_COPY(false);
	*length = ld->length;
	return opxy_stage_fail(ld);
}

/*
 * S-05.019, S-09.050-S-09.052, then after a reply of 0 S-05.018 step 3: the
 * wait for the slave's OS (S-05.032). The bridge is shut from the reply on,
 * so held mode after that wait makes no bridge request (S-09.008's rules).
 */
static int release(void *ctx, uint8_t *reply)
{
	struct opxy_master *m = ctx;
	int v;

	OPXY_CRUMB(17u); OPXY_TIME(BOOT_TIME_CHECKED);
	/* S-02.095: the OS image staged and checked, the release next: alive. */
	opxy_start_finished(m);
	v = s09_master_release(&m->bridge);

	OPXY_CRUMB(19u); OPXY_TIME(BOOT_TIME_RELEASED);
	release_replied = v >= 0;
	release_reply = (uint8_t)v;
	if (v < 0)
		return -1;
	*reply = (uint8_t)v;
	if (v != 0)
		return 0;
	if (opxy_os_wait(m) != 0) {
		m->bridge.absent = true;
		m->slave_present = false;
		*reply = S05_REPLY_NO_START;
		return 0;
	}
	OPXY_TIMING_LEAVE();
	return 0;
}

/* S-05.006, S-04.045 M6: sw_version 0.0.0.0, written by S-02.064. */
static void clear_sw_version(void *ctx)
{
	struct opxy_master *m = ctx;
	unsigned i;

	for (i = 0; i < 4u; i++)
		m->meta.sw_version[i] = 0;
	(void)opxy_meta_write(m);
}

/*
 * S-05.006: the failed start's account, from what the stage, the image check
 * and the release left, with how the bridge lost the slave (S-09.008) unless
 * the release shut it on purpose (S-05.032); its log line, then held mode.
 */
static void load_failed(void *ctx, enum s05_fail cause)
{
	struct opxy_master *m = ctx;
	struct boot_start st;

	memset(&st, 0, sizeof(st));
	st.load = stage_load;
	st.boot_size = m->emmc.boot_size;
	if (cause == S05_FAIL_REFUSED)
		st.refusal = s05_check_image(m->ops.staging, stage_load.length);
	st.replied = cause == S05_FAIL_SLAVE && release_replied;
	st.reply = st.replied ? release_reply : 0u;
	st.lost = cause != S05_FAIL_SLAVE_START &&
		  s09_master_lost(&m->bridge, &st.why);
	opxy_start_log(m, cause, &st);
	boot_master_load_failed(&m->ops, cause, &st);
}

#if !defined(OPXY_TEST_OS)
/* S-15.175: the boot store's setting, as metadata() read it. */
static bool start_in_menu(void *ctx)
{
	const struct opxy_master *m = ctx;

	return m->sv.store.start_menu;
}
#endif

void opxy_master_handoff(struct opxy_master *m)
{
	m->handoff.ctx = m;
	m->handoff.pre_load = pre_load;
	m->handoff.stage = stage;
	m->handoff.release = release;
	m->handoff.teardown = opxy_teardown_master;
	m->handoff.kernel_call = opxy_kernel_call;
	m->handoff.read_svect0 = opxy_read_svect0;
	m->handoff.jump = opxy_jump;
	m->handoff.clear_sw_version = clear_sw_version;
	m->handoff.held = load_failed;
}

void opxy_master_main(struct opxy_master *m)
{
	OPXY_TIMING_START(); m->hw.time = opxy_time; OPXY_CRUMB(1u);
	m->clock = opxy_clock(&m->hw);
	opxy_master_handoff(m);

	m->ops.ctx = m;
	m->ops.bringup = bringup;
	m->ops.ddr_check = ddr_check;
	m->ops.ddr_fatal = ddr_fatal;
	m->ops.metadata = metadata;
	m->ops.reboot_reason = reboot_reason;
	m->ops.slave_start = slave_start;
	m->ops.psoc_start = psoc_start;
#if OPXY_BOOT_PROMPT
	m->ops.boot_prompt = opxy_boot_prompt;		/* S-05.025 */
#endif
	m->ops.handoff = &m->handoff;
	m->ops.staging = (const uint8_t *)(uintptr_t)OPXY_STAGING;
	m->ops.config_addr = opxy_handover_config();
	m->ops.held = opxy_held;
	m->ops.start_in_menu = OPXY_TESTOS_OR(NULL, start_in_menu);	/* S-15.175 */
	boot_master(&m->ops);
}
