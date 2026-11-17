/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The OP-XY's driver-level operation structs (see waffle/board/opxy_ops.h). */
#include "waffle/board/opxy_ops.h"

#include "waffle/board/opxy_services.h"
#include "waffle/wire_crc.h"

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_port.h"

/* The test OS (waffle/testos.h) reads the flash and refuses every erase
 * and program, so no S-02 write path reaches it. */
/* S-02's flash operations, with USB serviced after each: a slot's erase,
 * program and read-back are thousands of them. */
static int flash_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	const int rc = wb_spiflash_read(ctx, off, buf, len);

	opxy_usb_breathe();
	return rc;
}

/* S-13b.011: a slot's stream, read at 50 MHz for the slave's boot. */
int opxy_flash_read_stream(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	const int rc = wb_spiflash_read_at(ctx, OPXY_SPI_FLASH_STREAM_CLK, off, buf, len);

	opxy_usb_breathe();
	return rc;
}

static OPXY_TESTOS_UNUSED int flash_erase(void *ctx, uint32_t sector_off)
{
	const int rc = wb_spiflash_erase(ctx, sector_off);

	opxy_usb_breathe();
	return rc;
}

static OPXY_TESTOS_UNUSED int flash_program(void *ctx, uint32_t off,
					    const uint8_t *buf, uint32_t len)
{
	const int rc = wb_spiflash_program(ctx, off, buf, len);

	opxy_usb_breathe();
	return rc;
}

void opxy_flash_writer(struct opxy_hw *hw, bool on)
{
	hw->flash.tries = (uint8_t)(on ? OPXY_FLASH_WRITER_TRIES : 1u);
}

struct s02_flash_ops opxy_flash_ops(struct opxy_hw *hw)
{
	const struct s02_flash_ops ops = {
		&hw->flash, flash_read,
		OPXY_TESTOS_OR(opxy_testos_flash_erase, flash_erase),
		OPXY_TESTOS_OR(opxy_testos_flash_program, flash_program),
	};

	return ops;
}

struct s03_emmc_ops opxy_emmc_ops(struct opxy_hw *hw)
{
	const struct s03_emmc_ops ops = {
		&hw->msi, wb_msi_emmc_select, wb_msi_emmc_read,
		wb_msi_emmc_write, wb_msi_emmc_erase, wb_msi_emmc_switch,
	};

	return ops;
}

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int down_select(void *ctx, unsigned part)
{
	(void)ctx;
	(void)part;
	return OPXY_EMMC_DOWN;
}

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int down_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	(void)ctx;
	(void)sector;
	(void)count;
	(void)buf;
	return OPXY_EMMC_DOWN;
}

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int down_write(void *ctx, uint32_t sector, uint32_t count,
		      const uint8_t *buf)
{
	(void)ctx;
	(void)sector;
	(void)count;
	(void)buf;
	return OPXY_EMMC_DOWN;
}

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int down_erase(void *ctx, uint32_t first_sector)
{
	(void)ctx;
	(void)first_sector;
	return OPXY_EMMC_DOWN;
}

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static int down_switch(void *ctx, uint8_t index, uint8_t value)
{
	(void)ctx;
	(void)index;
	(void)value;
	return OPXY_EMMC_DOWN;
}

const struct s03_emmc_ops opxy_emmc_down_ops = {
	NULL, down_select, down_read, down_write, down_erase, down_switch,
};

const struct s06_port_ops opxy_port_ops = {
	wb_uart_read, wb_uart_room, wb_uart_write,
};

/* Every wait of the serial protocols (the bridge's above all) comes here
 * when it has nothing to do: USB is serviced meanwhile (S-10a.049). */
static void clock_idle(void *ctx, uint32_t until_us)
{
	(void)ctx;
	(void)until_us;
	opxy_usb_breathe();
}

struct s06_clock opxy_clock(struct opxy_hw *hw)
{
	const struct s06_clock c = { wb_time_now_us, clock_idle, &hw->time };

	return c;
}

/* CRC-32 over a whole image, in pieces, with USB serviced between them. */
uint32_t opxy_crc32_long(uint32_t crc, const uint8_t *buf, uint32_t len)
{
	while (len != 0u) {
		const uint32_t n = len < 0x4000u ? len : 0x4000u;

		crc = wire_crc32(crc, buf, n);
		buf += n;
		len -= n;
		opxy_usb_breathe();
	}
	return crc;
}

struct s14_clock_ops opxy_log_clock(struct opxy_hw *hw)
{
	const struct s14_clock_ops c = { wb_time_now_us, &hw->time };

	return c;
}

/* S-13b.020, S-13b.040: PA6 low is "ready" on both links. */
static bool ready_pa6(void *ctx)
{
	(void)ctx;
	return !wb_gpio_get(OPXY_PA6);
}

/* S-13b.040: PC5 low is the slave's attention. */
static bool attention_pc5(void *ctx)
{
	(void)ctx;
	return !wb_gpio_get(OPXY_PC5);
}

/* S-13b.040: the bridge's chip select is PC11 as a GPIO, active low. A
 * session let go before its end (S-09.013) has its receive closed first;
 * every session's set-up clears SPI0's EN and sets it last, which is the
 * reset between sessions (S-13b.040, S-13b.083). */
/* cppcheck-suppress constParameterCallback ; the bus ops fix the type */
static void select_pc11(void *ctx, bool on)
{
	if (!on)
		wb_spi_dma_close(ctx);
	(void)wb_gpio_set(OPXY_PC11, !on);
	OPXY_TESTOS(opxy_testos_bus_note(on ? 'S' : 'E', 0, NULL, 0u);)
}

/*
 * S-09.075: the rung whose request session the sends that follow belong
 * to, 0 for none. On a rung a send is set up in S-13b.040's enable-last
 * order under the rung's control value and clock: S-13b.041's quad I/O
 * value, or the pre-link one, at SCLK0 / 4 or, with fast mode, SCLK0 / 2.
 */
static unsigned bus_rung;

static const struct wb_spi_config *rung_cfg(void)
{
	static const struct wb_spi_config cfg[S09_RUNG_TOP] = {
		[S09_RUNG_CHECKED - 1u] = { 0x00002592u, 3u, 0u, 0u },
		[S09_RUNG_CHECKED_FAST - 1u] = { 0x00002592u | SPI_CTL_FMODE, 1u, 0u, 0u },
		[S09_RUNG_QUAD - 1u] = { 0x00242592u, 3u, 0u, 0u },
		[S09_RUNG_QUAD_FAST - 1u] = { 0x00242592u, 1u, 0u, 0u },
	};

	return &cfg[bus_rung - 1u];
}

/* S-13b.083: a request session by DMA4, on a rung under the rung's set-up. */
/* cppcheck-suppress constParameterCallback ; s09_bus_ops fixes the type */
static bool bus_send(void *ctx, const uint8_t *d, size_t n)
{
	const int rc = wb_spi_dma_send(ctx, bus_rung != 0u ? rung_cfg() : NULL,
				       d, n);

	OPXY_TESTOS(opxy_testos_bus_note('T', rc, d, n);)
	return rc == WB_SPI_OK;
}

/* S-13b.083: a reply session's part by DMA5. */
/* cppcheck-suppress constParameterCallback ; s09_bus_ops fixes the type */
static bool bus_recv(void *ctx, uint8_t *d, size_t n)
{
	const int rc = wb_spi_dma_recv(ctx, d, n);

	OPXY_TESTOS(opxy_testos_bus_note('R', rc, d, n);)
	return rc == WB_SPI_OK;
}

/* The session's last word moved: its receive closed (EN cleared). */
/* cppcheck-suppress constParameterCallback ; s09_bus_ops fixes the type */
static void bus_end(void *ctx)
{
	wb_spi_dma_close(ctx);
}

/*
 * S-09.080, S-13b.084: a quad reply test's reply session, in quad I/O at the
 * pre-link control value with MIOM 2, with or without fast mode, at
 * SCLK0 / 4 or SCLK0 / 2; SPI0_CTL 0 and the pre-link clock after it, as
 * after a rung's send.
 */
/* cppcheck-suppress constParameterPointer ; s09_quad_recv_fn fixes the type */
bool opxy_bus_quad_recv(void *ctx, uint8_t *d, size_t n, unsigned clock,
			  uint32_t out[4])
{
	const uint32_t ctl = opxy_spi_bridge_pre.ctl | SPI_CTL_MIOM_QUAD |
			     ((clock & S09_QT_FMODE) != 0u ? SPI_CTL_FMODE : 0u);
	const int rc = wb_spi_qtest_recv(ctx, ctl, (clock & S09_QT_FAST) != 0u ? 1u : 3u,
					 d, n, out);

	OPXY_TESTOS(opxy_testos_bus_note('R', rc, d, n);)
	return rc == WB_SPI_OK;
}

/* S-13b.040, .041: single I/O before the link and after it. After a failed
 * operation, or with a DMA channel of SPI0 still enabled, the plain set-up
 * is a restart, so that the plain exchanges that follow (S-09.074) start
 * from nothing a failed session left. */
static void bus_configure(void *ctx, int phase)
{
	/* The bridge stays in single I/O after the link (S-13b Q-11); a
	 * request session on a rung changes nothing until its send. */
	const struct wb_spi_config *c = &opxy_spi_bridge_pre;

	if (phase > 1 && phase <= S09_PHASE_RUNG(S09_RUNG_TOP)) {
		bus_rung = (unsigned)phase - 1u;
		return;
	}
	bus_rung = 0u;
	OPXY_TESTOS(if (phase != 0) opxy_testos_bridge_session();)
	if (wb_spi_spoiled(ctx))
		(void)wb_spi_restart(ctx, c->ctl, c->clk);
	else
		(void)wb_spi_reconfigure(ctx, c->ctl, c->clk);
}

/* S-09.005: a byte of the boot link, inside a chip-select session, which
 * has the controller's channels open: the FIFOs and the status, no more. */
/* cppcheck-suppress constParameterPointer ; a boot op's shape */
uint8_t opxy_boot_byte(void *ctx, uint8_t tx)
{
	return (uint8_t)wb_spi_word(ctx, tx);
}

/*
 * The byte, then the link's settle time (OPXY_BOOT_LINK_SETTLE_US), so that
 * the sample of PA6 that follows comes that long after the byte's end: the
 * cycle counter against a count fixed at build time, no clock arithmetic.
 */
/* cppcheck-suppress constParameterCallback ; s09_boot_ops fixes the type */
static uint8_t boot_xfer(void *ctx, uint8_t tx)
{
	const uint8_t rx = opxy_boot_byte(ctx, tx);
#if OPXY_BOOT_LINK_SETTLE_US != 0
	const uint64_t from = wb_core_cycles();

	while (wb_core_cycles() - from < OPXY_BOOT_LINK_SETTLE_CYCLES)
		;
#endif
	return rx;
}

/* The chip select, and with it the session's set-up, made once for all its
 * bytes; outside a session the controller is as wb_spi_init() left it. */
/* cppcheck-suppress constParameterCallback ; s09_boot_ops fixes the type */
static void boot_select(void *ctx, bool on)
{
	if (on) {
		wb_spi_select(ctx, true);
		(void)wb_spi_words_begin(ctx);
	} else {
		(void)wb_spi_words_end(ctx);
		wb_spi_select(ctx, false);
	}
}

/* cppcheck-suppress constParameterCallback ; s09_boot_ops fixes the type */
static bool boot_failed(void *ctx)
{
	return wb_spi_words_failed(ctx);
}

struct s09_boot_ops opxy_boot_ops(struct opxy_hw *hw)
{
	const struct s09_boot_ops ops = {
		&hw->link_spi, ready_pa6, boot_select, boot_xfer, NULL, boot_failed,
	};

	return ops;
}

#if OPXY_BOOT_LINK_FC
/*
 * BOOT_LINK_FC=1. The stream's sessions go through the transmit FIFO under
 * a word count, with the controller's flow control on PA6 (S-13b.022): the
 * control value changes between the opening session and the first of the
 * stream, with the chip select high. The opening byte and S-09.007's bytes
 * are sent one at a time without it, since S-09.007's go whatever PA6
 * reads. A byte held for longer than the controller's word timeout, which
 * the caller sets to T_STREAM (S-09.060), fails the session, and the
 * controller is then restarted (wb_spi_restart()).
 */
static unsigned boot_fc_sessions;
static bool boot_fc_fault;

/* cppcheck-suppress constParameterCallback ; s09_boot_ops fixes the type */
static uint8_t boot_fc_xfer(void *ctx, uint8_t tx)
{
	uint8_t rx = 0xFFu;

	if (wb_spi_xfer(ctx, &tx, &rx, 1u) != WB_SPI_OK)
		boot_fc_fault = true;
	return rx;
}

static bool boot_fc_failed(void *ctx)
{
	(void)ctx;
	return boot_fc_fault;
}

static void boot_fc_select(void *ctx, bool on)
{
	struct wb_spi *s = ctx;

	if (on && boot_fc_sessions++ == 1u)
		(void)wb_spi_reconfigure(s, opxy_spi_boot_link_fc.ctl,
					 opxy_spi_boot_link_fc.clk);
	wb_spi_select(s, on);
}

static bool boot_fc_send(void *ctx, const uint8_t *d, size_t n)
{
	struct wb_spi *s = ctx;

	if ((s->ctl & SPI_CTL_FCEN) == 0u)
		return false;
	if (wb_spi_send(s, d, n) == WB_SPI_OK)
		return true;
	/* A session given up leaves its word count and whatever was in the
	 * FIFO: all of it cleared, so the bridge's set-up starts from nothing. */
	(void)wb_spi_restart(s, s->ctl, s->clk);
	return false;
}

struct s09_boot_ops opxy_boot_ops_fc(struct opxy_hw *hw)
{
	const struct s09_boot_ops ops = {
		&hw->link_spi, ready_pa6, boot_fc_select, boot_fc_xfer, boot_fc_send,
		boot_fc_failed,
	};

	boot_fc_sessions = 0u;
	boot_fc_fault = false;
	/* a byte the slave holds for long services USB meanwhile (S-10a.049) */
	wb_spi_set_idle(&hw->link_spi, opxy_usb_breathe);
	return ops;
}
#endif

/* Also gives the bridge's SPI0 the clock's idle: a send or receive held by
 * the slave for up to S09_T_REPLY_US services USB meanwhile (S-10a.049). */
struct s09_bus_ops opxy_bus_ops(struct opxy_hw *hw)
{
	const struct s09_bus_ops ops = {
		&hw->link_spi, ready_pa6, attention_pc5, select_pc11,
		bus_send, bus_recv, bus_configure, bus_end,
	};

	wb_spi_set_idle(&hw->link_spi, opxy_usb_breathe);
	return ops;
}

/* cppcheck-suppress constParameterPointer ; the board's ops take the hw as it is */
void opxy_bus_fault(struct opxy_hw *hw, uint32_t out[4])
{
	struct wb_spi_fault f;

	wb_spi_last_fault(&hw->link_spi, &f);
	out[0] = (uint32_t)f.rc;
	out[1] = f.stat;
	out[2] = f.left;
	out[3] = f.stage;
}

/* cppcheck-suppress constParameterPointer ; the board's ops take the hw as it is */
void opxy_bus_fault_more(struct opxy_hw *hw, uint32_t out[4])
{
	wb_spi_last_fault_more(&hw->link_spi, out);
}

/*
 * S-16.095: an item's bytes are its words in the order the ROM returns
 * them, each least significant byte first; a key in two items is the low
 * item's 16 bytes, then the high item's.
 */
static void words_to_bytes(const uint32_t *w, unsigned n, uint8_t *out)
{
	unsigned i;

	for (i = 0; i < n * 4u; i++)
		out[i] = (uint8_t)(w[i / 4u] >> (8u * (i % 4u)));
}

/* S-16.095: Q0 is the first 0x38 bytes of item 9's 16 words, x then y,
 * each 28 bytes big-endian; the rest of the item is not used. */
bool opxy_otp_q0(wb_otp_get_fn get, uint8_t q[56])
{
	uint32_t w[16];
	uint8_t b[64];
	unsigned i;

	if (wb_otp_read(get, OPXY_OTP_Q0, w, 16u) != WB_OTP_OK)
		return false;
	words_to_bytes(w, 16u, b);
	for (i = 0; i < 56u; i++)
		q[i] = b[i];
	return true;
}

bool opxy_otp_serial(wb_otp_get_fn get, char s[OPXY_CHIP_SERIAL_MAX + 1u])
{
	uint32_t w[8];
	uint8_t b[32];
	unsigned i, n = 0;

	s[0] = '\0';
	if (wb_otp_read(get, OPXY_OTP_SERIAL, w, 8u) != WB_OTP_OK)
		return false;
	words_to_bytes(w, 8u, b);
	while (n < OPXY_CHIP_SERIAL_MAX && b[n] != 0u)
		n++;
	/* S-04.047: a byte outside 0x20-0x7E, or a ';', shows the chip
	 * serial as empty; the item still counts as read. */
	for (i = 0; i < n; i++)
		if (b[i] < 0x20u || b[i] > 0x7Eu || b[i] == (uint8_t)';')
			return true;
	for (i = 0; i < n; i++)
		s[i] = (char)b[i];
	s[n] = '\0';
	return true;
}

bool opxy_otp_provisioned(wb_otp_get_fn get)
{
	uint32_t w[16];
	uint8_t b[64];
	unsigned i, nonzero = 0;

	if (wb_otp_read(get, WB_OTP_GP1, w, 16u) != WB_OTP_OK)
		return false;
	words_to_bytes(w, 16u, b);
	for (i = 0; i < 28u; i++)
		if (b[i] != 0u)
			nonzero++;
	return nonzero > 14u;
}

bool opxy_otp_key(wb_otp_get_fn get, uint32_t lo, uint32_t hi,
		  uint8_t key[32])
{
	uint32_t w[4];

	if (wb_otp_read(get, lo, w, 4u) != WB_OTP_OK)
		return false;
	words_to_bytes(w, 4u, key);
	if (wb_otp_read(get, hi, w, 4u) != WB_OTP_OK) {
		unsigned i;

		for (i = 0; i < 16u; i++)	/* never half a key */
			key[i] = 0;
		return false;
	}
	words_to_bytes(w, 4u, key + 16);
	return true;
}

static bool unit_q0(void *ctx, uint8_t q[56])
{
	(void)ctx;
	return opxy_otp_q0(opxy_otp_get, q);
}

static bool unit_k0(void *ctx, uint8_t key[32])
{
	(void)ctx;
	return opxy_otp_key(opxy_otp_get, OPXY_OTP_K0_LO, OPXY_OTP_K0_HI, key);
}

static bool unit_k1(void *ctx, uint8_t key[32])
{
	(void)ctx;
	return opxy_otp_key(opxy_otp_get, OPXY_OTP_K1_LO, OPXY_OTP_K1_HI, key);
}

const struct s11_unit_ops opxy_unit_ops = { unit_q0, unit_k0, unit_k1 };
