/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The slave's boot steps on the OP-XY (see waffle/board/boot.h). */
#include "waffle/board/boot.h"

#include <stddef.h>
#include <string.h>

#include "waffle/board/emmc_bridge.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_dmc.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/mmr.h"
#include "waffle/drivers/bf70x_sec.h"
#include "waffle/util/bytes.h"
#include "waffle/bridge/records.h"
#include "waffle/host/devhost.h"
#include "waffle/util/crc.h"
#include "waffle/bridge/slave_files.h"
#include "waffle/fs/yaffs.h"
#include "wb_yaffs.h"
#if defined(OPXY_TEST_OS)
#include "yaffsfs.h"
#endif

struct opxy_slave opxy_slave;

/* The slave whose storage is a store op's ctx. */
static struct opxy_slave *slave_of(void *st)
{
	return (struct opxy_slave *)(void *)((uintptr_t)st - offsetof(struct opxy_slave, st));
}

/* One request session, header and payload (S-09.021). Write payloads go to
 * the eMMC driver's DMA from rxbuf + 12, a 32-byte boundary in every build
 * (S09_DMA_ALIGN); S-09.072: a request's check after them. */
#define RXBUF_LEN (S09_SLAVE_BIG + S09_CHECK_LEN)
static _Alignas(32) uint8_t rxroom[S09_DMA_LEAD + RXBUF_LEN];
#define rxbuf (rxroom + S09_DMA_LEAD)
_Static_assert((S09_DMA_LEAD + S09_HDR) % S09_DMA_ALIGN == 0 && S09_DMA_LEAD % 4u == 0,
	       "a write's payload must start on a 32-byte boundary");
/* Every byte s09_slave_rx_keep() keeps fits: a 128-sector write and the
 * longest payload len names (S-09.023, S-09.037), each with its trailer
 * outside the plain mode (S-09.072). */
_Static_assert(RXBUF_LEN >= S09_HDR + S09_MAX_SECTORS * S09_SECTOR + S09_CHECK_LEN &&
	       RXBUF_LEN >= S09_HDR + 0x10000u + S09_CHECK_LEN &&
	       RXBUF_LEN >= S09_HDR + S09_WRITE_MAX + S09_CHECK_LEN &&
	       RXBUF_LEN >= S09_HDR + S09_TEST_MAX + S09_CHECK_LEN,
	       "a request session overruns the slave's buffer");

#if defined(OPXY_TEST_OS)
/*
 * The bring-up's steps and outcomes (waffle/testos.h), which the master
 * fetches over the bridge. The server comes up first with the eMMC down;
 * the eMMC's bring-up runs when the master asks, between sessions.
 */
static uint32_t testos_rows[OPXY_TESTOS_STEP_COUNT];
static uint64_t testos_t0;
static bool testos_emmc_due;

static int testos_note(const struct opxy_slave *s, unsigned step, int rc)
{
	const uint64_t ms = (wb_time_us(&s->hw.time) - testos_t0) / 1000u;

	testos_rows[step] = OPXY_TESTOS_ROW(step, rc < 0 ? -rc : rc,
					    ms < 0xFFFFu ? (uint32_t)ms : 0xFFFFu);
	return rc;
}

/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static uint32_t testos_row(void *ctx, uint8_t cmd, uint32_t row, uint32_t start)
{
	if (cmd == OPXY_TESTOS_PEEK_REQ)
		return opxy_testos_mem_ok(row, 4u, false) ?
		       wb_mmr_read32(row) : 0xDEADBEEFu;
	if (cmd == OPXY_TESTOS_POKE_REQ) {
		if (!opxy_testos_mem_ok(row, 4u, true))
			return 0xDEADBEEFu;
		wb_mmr_write32(row, start);
		wb_mmr_sync();
		return 0u;
	}
	if (start != 0u)
		testos_emmc_due = true;
	if (row == OPXY_TESTOS_STEP_QUEUE) {
		const struct opxy_slave *s = slave_of(ctx);

		return OPXY_TESTOS_ROW(row, s->server.nqueue, 0u);
	}
	return row < OPXY_TESTOS_STEP_COUNT ? testos_rows[row] : UINT32_MAX;
}

static void testos_emmc_start(struct opxy_slave *s);

/*
 * The reset recorder (wiki/Test-OS.md): the slave stays up through a reset of
 * the master and counts the edges on its bridge pins, in 100 ms buckets, then
 * leaves the counts in a file of its own on /yaffs2 for the next boot to read.
 * Counts only: nothing that crosses the wires is kept. PA4 reads as a slave
 * ready for a stream, so a master that wants to stream one goes on to do so.
 * The monitor writes the seconds to record to this word.
 */
volatile uint32_t wb_testos_rec_seconds;
#define REC_BUCKET_MS 100u
#define REC_BUCKETS   900u
#define REC_PINS      6u	/* PB10 to PB15 */
#define REC_MASK      0xFC00u
#define REC_SHIFT     10u
#define REC_FILE      "/yaffs2/wb_reset_rec.bin"
static uint16_t rec_counts[REC_BUCKETS][REC_PINS];

static void testos_record(struct opxy_slave *s, uint32_t seconds)
{
	const uint32_t per_bucket = OPXY_CCLK_MHZ * 1000u * REC_BUCKET_MS;
	const uint32_t buckets = seconds * 10u < REC_BUCKETS ? seconds * 10u : REC_BUCKETS;
	uint32_t prev, bucket = 0, t0;
	unsigned i;
	int fd;

	(void)s;
	memset(rec_counts, 0, sizeof(rec_counts));
	wb_mmr_write32(PORT_INEN_SET(WB_PORT_B), REC_MASK);
	(void)wb_gpio_output(OPXY_PA4, false);
	prev = wb_mmr_read32(PORT_DATA(WB_PORT_B));
	t0 = (uint32_t)wb_core_cycles();
	while (bucket < buckets) {
		const uint32_t now = wb_mmr_read32(PORT_DATA(WB_PORT_B));
		const uint32_t diff = (now ^ prev) & REC_MASK;

		prev = now;
		for (i = 0; i < REC_PINS; i++) {
			if (((diff >> (REC_SHIFT + i)) & 1u) != 0u &&
			    rec_counts[bucket][i] != 0xFFFFu)
				rec_counts[bucket][i]++;
		}
		if ((uint32_t)wb_core_cycles() - t0 >= per_bucket) {
			t0 += per_bucket;
			bucket++;
		}
	}
	(void)wb_gpio_output(OPXY_PA4, true);
	fd = yaffs_open(REC_FILE, O_CREAT | O_TRUNC | O_WRONLY, S_IREAD | S_IWRITE);
	if (fd >= 0) {
		(void)yaffs_write(fd, rec_counts, buckets * REC_PINS * sizeof(uint16_t));
		(void)yaffs_fsync(fd);
		(void)yaffs_close(fd);
	}
	(void)yaffs_sync("/yaffs2");
}

/* The beacon (waffle/testos.h). */
#define BEACON_HALF_US 500u
#define BEACON_GAP_US  8000u
static unsigned testos_code = OPXY_TESTOS_BEACON_NOT_READY;

static void testos_reached(unsigned code)
{
	if (code > testos_code)
		testos_code = code;
}

/*
 * Once a request has come, a frame of bursts rather than the code alone: the
 * code; the first two words of the last request's header as received, eight
 * nibbles each; the queue; the reply waiting; the tunnel's bytes, each capped
 * at 15. A nibble is a burst of nibble + 1 pulses. Not blocking: the server
 * loop calls this each pass, so bursts stop if the loop stops.
 */
static uint32_t testos_hdr[2];
static bool testos_seen, testos_emmc_done;
#define FRAME_LEN 20u

static unsigned capped(size_t v)
{
	return v < 15u ? (unsigned)v : 15u;
}

static void testos_frame(const struct opxy_slave *s, uint8_t *f)
{
	unsigned i;

	f[0] = (uint8_t)testos_code;
	for (i = 0; i < 16u; i++)
		f[1u + i] = (uint8_t)((testos_hdr[i / 8u] >> (28u - 4u * (i % 8u)) & 15u) + 1u);
	f[17] = (uint8_t)(capped(s->server.nqueue) + 1u);
	f[18] = (uint8_t)(capped(s->server.reply_len) + 1u);
	f[19] = (uint8_t)(capped(s->server.tx_n) + 1u);
}

static bool testos_beacon(const struct opxy_slave *s)
{
	static uint8_t frame[FRAME_LEN];
	static uint64_t next;
	static unsigned left, pos = FRAME_LEN;
	static bool level = true;
	const uint64_t now = wb_time_us(&s->hw.time);

	if (now < next)
		return level;
	if (left == 0u) {
		if (!testos_seen) {
			left = 2u * testos_code;
		} else {
			if (pos >= FRAME_LEN) {
				testos_frame(s, frame);
				pos = 0;
			}
			left = 2u * frame[pos++];
		}
	}
	level = !level;
	left--;
	next = now + (left != 0u ? BEACON_HALF_US :
		      testos_seen && pos >= FRAME_LEN ? 30000u : BEACON_GAP_US);
	return level;
}
/* A request's header. One that an armed listen of S-13b Q-11 made ready is recorded
 * (waffle/testos.h) and served only when it is the echo or the peek, since a
 * quad header may come garbled. */
static bool testos_hdr_ok(int rc, size_t kept, const uint8_t *buf)
{
	if (wb_spi_testos_req_header(kept >= S09_HDR ? WB_SPI_OK : rc, buf))
		return rc == WB_SPI_OK &&
		       (buf[0] == S09_ECHO || buf[0] == OPXY_TESTOS_PEEK_REQ);
	return true;
}

#define REACHED(code) testos_reached(code)
#define STEP_NOTE(s, step, x) testos_note((s), (step), (x))
#else
#define REACHED(code) ((void)0)
#define STEP_NOTE(s, step, x) (x)
#endif

/* S-13b.042: PB15 high is the master ready, between sessions. */
static bool master_ready(void)
{
	return wb_gpio_get(OPXY_PB15);
}

/* Not in the test OS (waffle/testos.h): it stages nothing and reports
 * "no OS" to every release, so neither CPU ever hands over from it. */
static OPXY_TESTOS_UNUSED enum s05_fail stage_os(struct opxy_slave *s,
						 uint32_t *length)
{
	struct s03_load ld;

	s03_load_os(&s->st.emmc, (uint8_t *)(uintptr_t)OPXY_STAGING, S03_OS_MAX,
		    &ld);
	s->st.part = ld.select0_failed ? OPXY_PART_UNKNOWN : S03_PART_USER;
	*length = ld.length;
	switch (ld.result) {
	case S03_LOAD_OK:
		break;
	case S03_LOAD_ENCRYPTED:
		return S05_FAIL_ENCRYPTED;
	case S03_LOAD_IO:
		return S05_FAIL_SELECT;
	case S03_LOAD_NO_ACTIVE:
	case S03_LOAD_ZERO_LENGTH:
	case S03_LOAD_TOO_LONG:
	case S03_LOAD_PAST_PARTITION:
	case S03_LOAD_FORMAT:
	case S03_LOAD_CRC:
	default:
		return S05_FAIL_NO_OS;
	}
	/* S-05.020 step 1: checked by S-05.024. */
	if (s05_check_image((const uint8_t *)(uintptr_t)OPXY_STAGING,
			    ld.length) != S05_ACCEPT)
		return S05_FAIL_REFUSED;
	return S05_FAIL_NONE;
}

/*
 * S-09.082, S-05.035: a file start's image, from /os.ldr on the volume the
 * master's 0x08 mounted, into the same staging area; only a copy whose
 * length and CRC-32 are the master's goes on to S-05.024's check. The user
 * area stays selected; nothing is written.
 */
static OPXY_TESTOS_UNUSED enum s05_fail stage_file(struct opxy_slave *s,
						   uint32_t len, uint32_t crc,
						   uint32_t *length)
{
	uint8_t *staging = (uint8_t *)(uintptr_t)OPXY_STAGING;
	uint32_t err;

	*length = 0;
	if (!opxy_storage_volume_usable(&s->st, &err) ||
	    wire_s09_os_file(opxy_storage_files(), s->st.emmc.crc32, s->st.emmc.crc32_fast, staging,
			     S03_OS_MAX, len, crc) != WIRE_OS_FILE_OK)
		return S05_FAIL_NO_OS;
	*length = len;
	if (s05_check_image(staging, len) != S05_ACCEPT)
		return S05_FAIL_REFUSED;
	return S05_FAIL_NONE;
}

#if !defined(OPXY_TEST_OS)
/*
 * S-09.084: an image the master holds in RAM, pushed in order from offset 0
 * into the staging area; pushed_len is how much of it has come. A write
 * at 0 begins it again.
 */
static uint32_t pushed_len;

static int32_t stage_write(void *ctx, uint32_t off, const uint8_t *data, uint32_t n)
{
	(void)ctx;
	if (off == 0u)
		pushed_len = 0;
	if (off != pushed_len || n > S03_OS_MAX - off)
		return -1;
	memcpy((uint8_t *)(uintptr_t)OPXY_STAGING + off, data, n);
	pushed_len = off + n;
	return 0;
}

/* S-09.084: the pushed copy, whole and of the release's length and CRC-32
 * (S-03.044's rule: the peripheral's first, the software's when it
 * differs), then S-05.024's check. Nothing of the eMMC is read. */
static enum s05_fail stage_pushed(struct opxy_slave *s, uint32_t len, uint32_t crc,
				  uint32_t *length)
{
	const uint8_t *staging = (const uint8_t *)(uintptr_t)OPXY_STAGING;

	*length = 0;
	if (pushed_len != len)
		return S05_FAIL_NO_OS;
	if ((s->st.emmc.crc32_fast == NULL || s->st.emmc.crc32_fast(0u, staging, len) != crc) &&
	    s->st.emmc.crc32(0u, staging, len) != crc)
		return S05_FAIL_NO_OS;
	*length = len;
	if (s05_check_image(staging, len) != S05_ACCEPT)
		return S05_FAIL_REFUSED;
	return S05_FAIL_NONE;
}
#endif

/*
 * S-09.051, S-05.020 step 1: staged and checked inside the release
 * request, while the server takes no further header and PA4 is high;
 * from the file a release with arguments names (S-09.082).
 */
static uint32_t os_status(void *ctx, uint32_t len, uint32_t crc)
{
	struct opxy_slave *s = slave_of(ctx);

	(void)len;	/* the test OS stages nothing */
	(void)crc;
	s->staged = OPXY_TESTOS_OR(S05_FAIL_NO_OS,
				   len != 0u && pushed_len != 0u ?
					stage_pushed(s, len, crc, &s->staged_len) :	/* S-09.084 */
				   !opxy_storage_features(&s->st) ? S05_FAIL_SELECT :
				   len != 0u ? stage_file(s, len, crc, &s->staged_len) :
					       stage_os(s, &s->staged_len));
	/* cppcheck-suppress knownConditionTrueFalse ; true of the test OS's build only (waffle/testos.h) */
	return s->staged == S05_FAIL_NONE ? 0u : 1u;
}

/* S-09.036: the slave's counter itself follows the master's (S-13a.030).
 * The adjustment runs between sessions, with no wait in progress. */
static uint64_t cycles(void *ctx)
{
	(void)ctx;
	return wb_core_cycles();
}

static void adjust(void *ctx, int64_t delta)
{
	(void)ctx;
	wb_core_cycles_add((uint64_t)delta);
}

/* --- Sessions (S-09.011), on the driver's slave end (S-13b.042, .043) --- */

/* The slave end's last failure as rc, status, words left and stage. */
/* cppcheck-suppress constParameterCallback ; s09_slave_fault_source() fixes the type */
static void receive_fault(void *ctx, uint32_t out[4])
{
	struct wb_spi_fault f;

	wb_spi_slave_last_fault(ctx, &f);
	out[0] = (uint32_t)f.rc;
	out[1] = f.stat;
	out[2] = f.left;
	out[3] = f.stage;
}

#if !defined(OPXY_TEST_OS)
/*
 * S-09.080, S-13b.084: the quad reply test, a developer's measurement. ARM
 * keeps its arguments; SEND builds the test reply in rxbuf, which holds no
 * request until the next listen, and serve_until_release() sends it in quad
 * I/O through wb_spi_slave_qtest(); RECORD answers what that send saw. It
 * touches no storage. The bounds: the candidate's end within QT_FLAG_US of
 * the master's first word, far under the master's T_REPLY for PA6 to rise
 * (S-09.060); SPIF's end once SPI_STAT has not changed for QT_QUIET_US,
 * many words' time at every clock, far under 1 ms (S-13b.045).
 */
#define QT_FLAG_US 30000u
#define QT_QUIET_US 20u
_Static_assert(RXBUF_LEN >= S09_QT_MAX, "a test reply overruns rxbuf");
_Static_assert(WB_SPI_QE_FLAG == S09_QE_FLAG && WB_SPI_QE_BOUND == S09_QE_BOUND &&
	       WB_SPI_QE_MASTER == S09_QE_MASTER && WB_SPI_QE_TX_ERR == S09_QE_TX_ERR &&
	       WB_SPI_QE_DMA_ERR == S09_QE_DMA_ERR &&
	       WB_SPI_QE_NO_START == S09_QE_NO_START &&
	       WB_SPI_QE_REFUSED == S09_QE_REFUSED &&
	       WB_SPI_QF_TF_STAT == S09_QF_TF_STAT && WB_SPI_QF_TF_ILAT == S09_QF_TF_ILAT &&
	       WB_SPI_QF_FED == S09_QF_FED && WB_SPI_QF_BEGAN == S09_QF_BEGAN &&
	       WB_SPI_QF_ENDED == S09_QF_ENDED && WB_SPI_QF_SINGLE == S09_QF_SINGLE &&
	       WB_SPI_QT_TF_DMA == S09_QT_TF_DMA && WB_SPI_QT_TF_CORE == S09_QT_TF_CORE &&
	       WB_SPI_QT_SPIF == S09_QT_SPIF && WB_SPI_QT_NONE == S09_QT_NONE,
	       "the driver's and S-09.080's codes");

static struct {
	bool armed, due;
	uint32_t arg0, len, sends;
	uint32_t rec[S09_QT_REC_WORDS];
	uint8_t small[8];
	uint8_t out[S09_QT_REC_LEN];
} qt;

static size_t qt_value(uint8_t slot, uint32_t v)
{
	wb_put_le32(qt.small, s09_hdr_word(S09_QUAD_TEST, slot));
	wb_put_le32(qt.small + 4, v);
	return sizeof qt.small;
}

/* cppcheck-suppress constParameterCallback ; s09_qtest_fn fixes the type */
static size_t qtest(void *ctx, const struct s09_req *r, const uint8_t **out)
{
	const bool armed = qt.armed;

	(void)ctx;
	qt.armed = false;	/* a 0x1C that does not send the test disarms it */
	qt.due = false;
	*out = qt.small;
	switch (S09_QT_OP(r->arg0)) {
	case S09_QT_ARM:
		if (r->len != 0u || !s09_qt_args_ok(r->arg0, r->arg1) || !s09_check_ready())
			return qt_value(r->slot, S09_QT_BAD);
		qt.armed = true;
		qt.arg0 = r->arg0;
		qt.len = r->arg1;
		return qt_value(r->slot, S09_QT_ARMED);
	case S09_QT_SEND:
		if (!armed || r->arg0 != S09_QT_SEND || r->arg1 != 0u || r->len != 0u)
			return qt_value(r->slot, S09_QT_NOT_ARMED);
		s09_qt_build(rxbuf, qt.len, r->slot, qt.arg0);
		qt.due = true;
		*out = rxbuf;
		return qt.len;
	case S09_QT_RECORD:
		if (r->arg0 != S09_QT_RECORD || r->arg1 != 0u || r->len != 0u)
			return qt_value(r->slot, S09_QT_BAD);
		qt.rec[S09_QR_HDR] = s09_hdr_word(S09_QUAD_TEST, r->slot);
		for (unsigned k = 0; k < S09_QT_REC_WORDS; k++)
			wb_put_le32(qt.out + 4u * k, qt.rec[k]);
		*out = qt.out;
		return S09_QT_REC_LEN;
	default:
		return qt_value(r->slot, S09_QT_BAD);
	}
}

/* The armed test reply, sent in quad I/O; its record kept for RECORD.
 * The driver leaves SPI2 disabled in single I/O, the session over. */
static bool qt_send(struct opxy_slave *s, const uint8_t *d, size_t n)
{
	struct wb_spi_qt q;
	uint32_t *r = qt.rec;
	int rc;

	qt.due = false;
	rc = wb_spi_slave_qtest(&s->hw.link_slave, d, n, S09_QT_CAND(qt.arg0),
				(qt.arg0 & S09_QT_REN) != 0u, S09_T_REPLY_US,
				QT_FLAG_US, QT_QUIET_US, &q);
	r[S09_QR_NO] = ++qt.sends;
	r[S09_QR_ARM] = qt.arg0;
	r[S09_QR_LEN] = qt.len;
	r[S09_QR_END] = q.end;
	r[S09_QR_STAT] = q.stat;
	r[S09_QR_DMA] = q.dma;
	r[S09_QR_TWC] = q.twc;
	r[S09_QR_STAT_OR] = q.stat_or;
	r[S09_QR_STAT_BOUND] = q.stat_bound;
	r[S09_QR_HAND] = q.hand;
	r[S09_QR_T_START] = q.t_start;
	r[S09_QR_T_FEED] = q.t_feed;
	r[S09_QR_T_FLAG] = q.t_flag;
	r[S09_QR_T_END] = q.t_end;
	r[S09_QR_SPIF] = q.spif;
	r[S09_QR_RX] = q.rx;
	r[S09_QR_STAGE] = (uint32_t)q.rc << 8 | (q.stage & 0xFFu);
	return rc == WB_SPI_OK;
}

/*
 * S-09.081, S-13b.085: a sector read's data session in mode 3, after its
 * head went out whole: n bytes in quad I/O, ended by TF with the transmit
 * DMA, REN clear, the quad reply test's set-up and bounds. A session given
 * up changes nothing here: the master's check word judges it, and its
 * fall-back reaches the slave as a request that does not arrive whole.
 */
static void qd_send(struct opxy_slave *s, const uint8_t *d, size_t n)
{
	struct wb_spi_qt q;

	(void)wb_spi_slave_qtest(&s->hw.link_slave, d, n, WB_SPI_QT_TF_DMA, false,
				 S09_T_REPLY_US, QT_FLAG_US, QT_QUIET_US, &q);
}
#endif

/* The reply is clocked out completely, or given up (S-13b.045, S-09.013),
 * before this returns; then the master ends the session with PC11 high,
 * which the slave reads on PB15. */
static bool session_tx(struct opxy_slave *s, const uint8_t *d, size_t n)
{
	uint64_t end;
	bool ok, was, now;

	(void)wb_spi_slave_reply_end(&s->hw.link_slave, &was);
	ok = wb_spi_slave_send(&s->hw.link_slave, d, n, WB_SPI_SLAVE_SESSION,
			       S09_T_REPLY_US) == WB_SPI_OK;
	/* S-13b.083: this reply changed the driver to the count end; the
	 * link's status says so (S-09.079 cause 9) */
	(void)wb_spi_slave_reply_end(&s->hw.link_slave, &now);
	if (now && !was)
		s09_slave_reply_end_changed(&s->server, n != 0u ? d[0] : 0u, n);
	end = wb_time_deadline(&s->hw.time, S09_T_REPLY_US);
	while (!master_ready() && !wb_time_passed(end))
		;
	return ok;
}

/* wb_spi_slave_request()'s length of the session whose header came, and
 * the bytes a master may leave out; it runs while the master is held. */
/* cppcheck-suppress constParameterCallback ; wb_spi_session_len fixes the type */
static uint64_t session_len(void *ctx, const uint8_t *hdr, uint32_t *tail)
{
	const struct opxy_slave *s = ctx;

	*tail = s09_slave_rx_trail(&s->server, hdr);
	return s09_slave_rx_len(&s->server, hdr);
}

/*
 * S-09.036's second session is no header: its 8 bytes alone, by DMA9 as
 * the listen's one part (S-13b.083). This bootloader's master never sends
 * 0x0B.
 */
#define CLOCK_LEN 8u
/* cppcheck-suppress constParameterCallback ; wb_spi_session_len fixes the type */
static uint64_t clock_len(void *ctx, const uint8_t *hdr, uint32_t *tail)
{
	(void)ctx;
	(void)hdr;
	*tail = 0u;
	return CLOCK_LEN;
}

/*
 * A request session, after a listen has heard its first word (S-09.011,
 * S-13b.083): the header the listen asked for, then the rest its length
 * gives, both by DMA9 with PA4 handed over throughout; the master is held
 * between them only by the receive FIFO's watermark (S-13b.044). It may be
 * longer than anything the slave keeps: a malformed write's bytes are
 * received and dropped (S-09.037), up to about 2^41 of them. rxbuf holds
 * the leading RXBUF_LEN bytes, which cover every byte the slave keeps. A
 * request that does not come whole is recorded with the driver's words
 * (S-09.079); outside the plain mode it puts the link back in it (S-09.074).
 */
static void request(struct opxy_slave *s)
{
	const uint8_t *buf = rxbuf;
	size_t n = 0;
	int rc;

	REACHED(OPXY_TESTOS_BEACON_HEARD);
	if (s->server.clock_second) {
		/* S-09.036: the clock sync's second session, 8 bytes */
		if (wb_spi_slave_request(&s->hw.link_slave, RXBUF_LEN, clock_len,
					 s, &n, S09_T_REPLY_US) == WB_SPI_OK)
			s09_slave_request(&s->server, buf, CLOCK_LEN);
		return;
	}
	rc = wb_spi_slave_request(&s->hw.link_slave, RXBUF_LEN, session_len, s,
				  &n, S09_T_REPLY_US);
	if (!OPXY_TESTOS_OR(testos_hdr_ok(rc, n, buf), true) || n < S09_HDR) {
		s09_slave_link_fault(&s->server);
		return;
	}
	if (rc != WB_SPI_OK) {
		s09_slave_rx_failed(&s->server, buf, n);
		return;
	}
	s->last_cmd = buf[0];
	OPXY_TESTOS(testos_rows[OPXY_TESTOS_STEP_REQUEST] = OPXY_TESTOS_ROW(OPXY_TESTOS_STEP_REQUEST, buf[0], (uint32_t)buf[2] | (uint32_t)buf[3] << 8);)
#if defined(OPXY_TEST_OS)
	testos_hdr[0] = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
	testos_hdr[1] = (uint32_t)buf[4] | (uint32_t)buf[5] << 8 | (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 24;
	testos_seen = true;
#endif
#if defined(OPXY_TESTOS_BENCH) && OPXY_TESTOS_BENCH == 4
	/* A test build whose slave drops the pull. */
	if (buf[0] == S09_PULL)
		return;
#endif
#if defined(OPXY_TESTOS_BENCH) && OPXY_TESTOS_BENCH == 3
	/* A test build whose slave faults on the pull. */
	if (buf[0] == S09_PULL)
		__asm__ __volatile__("excpt 5;");
#endif
	REACHED(OPXY_TESTOS_BEACON_REQUEST);
	s09_slave_request(&s->server, buf, n);
}

/*
 * S-09.011 until the release's reply has gone out. Between sessions the
 * slave is ready for the next request (listen()) and stays ready while it
 * serves the CPU link, until the master's first word comes: it never takes
 * the line back on time alone (S-13b.043 names no such case). Were each
 * wait for a request bounded by T_REPLY, a master that read the line low
 * just before it rose would lose its session and find the slave absent
 * (S-09.013). Held mode's clock message once a second (S-09.046) would
 * meet such a give-up sooner or later, and an install would then fail at
 * a staging write (S-11.031, MEMBER).
 */
static uint8_t serve_until_release(void *ctx)
{
	struct opxy_slave *s = ctx;
	const uint8_t *d;
	uint32_t value;
	size_t n;
	bool listening = false;
	uint64_t trial_end = 0;		/* S-09.073: a trial's listen runs out */

	/* S-13b.083: the slave emits no line (S-14.026). The master logs the
	 * build's reply end, and a change to the count end comes to it as
	 * the link's status (S-09.079 cause 9, session_tx()). */
#if defined(OPXY_TESTOS_BENCH) && OPXY_TESTOS_BENCH == 1
	/* A test build whose slave never answers. */
	for (;;)
		wb_core_idle();
#endif
	for (;;) {
		/* S-09.031: PB2 low while a finished reply waits. */
		(void)wb_gpio_set(OPXY_PB2, OPXY_TESTOS_OR(!testos_emmc_done ? testos_beacon(s) : !s09_slave_attention(&s->server), !s09_slave_attention(&s->server)));
		s06_node_poll(&s->node);
		s09_slave_cpu_poll(&s->node, S06_LINK_CPU, &s->cpu_ops);
		if (listening) {
			if (!wb_spi_slave_heard(&s->hw.link_slave)) {
				/* S-13b Q-11: a quad listen not heard in 2 s listens again in single I/O */
				OPXY_TESTOS(listening = !wb_spi_testos_listen_lapsed(&s->hw.link_slave);)
				/* S-09.073: a mode on trial in which no request
				 * came for T_TRIAL: the plain mode again */
				/* cppcheck-suppress knownConditionTrueFalse ; the test OS may have ended the listen */
				if (listening && s09_slave_on_trial(&s->server) &&
				    wb_time_passed(trial_end)) {
					wb_spi_slave_unlisten(&s->hw.link_slave);
					s09_slave_trial_lapsed(&s->server);
					listening = false;
				}
				continue;
			}
			listening = false;
			request(s);
			continue;
		}
		if (!master_ready())
			continue;
		if (s09_slave_reply(&s->server, &d, &n)) {
			/* S-09.013: a reply not sent whole is dropped, not
			 * sent again, and the slave waits for the next
			 * session (PB15 high first, the loop's test). */
			bool sent = OPXY_TESTOS_OR(session_tx(s, d, n),
						   qt.due && d == rxbuf ? qt_send(s, d, n) :
									 session_tx(s, d, n));

			REACHED(sent ? OPXY_TESTOS_BEACON_SENT : OPXY_TESTOS_BEACON_SEND_FAIL);
#if !defined(OPXY_TEST_OS)
			/* S-09.081: the data session at once, after a whole head */
			if (s09_slave_qdata(&s->server, &d, &n) && sent)
				qd_send(s, d, n);
#endif
			OPXY_TESTOS(testos_rows[OPXY_TESTOS_STEP_SEND] = OPXY_TESTOS_ROW(OPXY_TESTOS_STEP_SEND, sent ? 0u : 1u, n);)
			/* The bridge stays in single I/O after the link, where
			 * S-09.015 step 3 would switch (S-13b Q-11). */
			/* S-09.052: a release is acted on once it is sent;
			 * one whose reply failed is not (S-09.013), and the
			 * slave keeps serving as after a reply of 1 */
			if (s09_slave_take_release(&s->server, &value)) {
				if (sent)
					return (uint8_t)value;
				s->server.released = false;
			}
			continue;
		}
#if defined(OPXY_TEST_OS)
		if (wb_testos_rec_seconds != 0u) {
			const uint32_t secs = wb_testos_rec_seconds;

			wb_testos_rec_seconds = 0u;
			testos_record(s, secs);
			continue;
		}
		/* The reply has gone out; PA4 stays high until this ends. */
		if (testos_emmc_due) {
			testos_emmc_due = false;
			testos_emmc_start(s);
			continue;
		}
#endif
		/* PB15 has just read high: no session is under way. S-09.070:
		 * the next request comes in the link's mode, its first part
		 * the header (S-13b.083) */
		listening = wb_spi_slave_listen(&s->hw.link_slave, rxbuf,
						s->server.clock_second ?
						CLOCK_LEN : S09_HDR,
						s09_slave_mode(&s->server) == S09_MODE_QUAD,
						S09_T_REPLY_US) == WB_SPI_OK;
		trial_end = wb_time_deadline(&s->hw.time, S09_T_TRIAL_US);
		REACHED(listening ? OPXY_TESTOS_BEACON_LISTENING : OPXY_TESTOS_BEACON_NO_LISTEN);
	}
}

/* --- S-05 on the slave --- */

/* The release's staging already ran in os_status (S-05.020 step 1). */
/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static enum s05_fail staged(void *ctx, uint32_t *length)
{
	const struct opxy_slave *s = ctx;

	*length = s->staged_len;
	return s->staged;
}

/* S-09 sent the reply; nothing more goes on the wire here. */
static void replied(void *ctx, uint8_t value)
{
	(void)ctx;
	(void)value;
}

/* S-05.020 step 3, S-05.016 for the slave: PA4 (the slave's ready line) a
 * GPIO driven low first, so it is never undriven, then the server's SPI2 off. */
static void stop_server(void *ctx)
{
	(void)ctx;
	opxy_ho_leave_pa4();
	wb_spi_disable(OPXY_SPI_BRIDGE_SLAVE);
}

/* S-04.025 (W9): after the answer, reason 0 in this core's record and a
 * hard reset, so the master's next boot finds the slave in ROM boot. */
static void cpu_power_off(void *ctx)
{
	(void)ctx;
	s04_reboot_write((uint8_t *)(uintptr_t)S04_REBOOT_ADDR, 0u);
	opxy_hard_reset();
}

static void cpu_clock(void *ctx, uint32_t secs)
{
	(void)ctx;
	(void)secs;
}

static void cpu_common(void *ctx, const struct s06_frame *f)
{
	(void)ctx;
	(void)f;
}

static void bringup(void *ctx)
{
	struct opxy_slave *s = ctx;

	OPXY_TESTOS((void)wb_gpio_output(OPXY_PB2, false);)	/* our code is running: wiki/Test-OS.md */
	(void)wb_pinmux_apply(&opxy_pinmux_slave);
	{
		unsigned i;

		for (i = 0; i < opxy_gpio_slave_n; i++) {
			const struct opxy_gpio *g = &opxy_gpio_slave[i];

			if (g->dir == OPXY_IN)
				(void)wb_gpio_input(g->pin);
			else
				(void)wb_gpio_output(g->pin,
						     g->dir == OPXY_OUT_HIGH);
		}
	}
	(void)wb_sec_init();
	/* S-13b.042, .043: PB2 high; SPI2 slave with PA4 high, PB15 in. */
	(void)wb_gpio_output(OPXY_PB2, OPXY_TESTOS_OR(false, true));
	(void)wb_spi_slave_init(&s->hw.link_slave, OPXY_SPI_BRIDGE_SLAVE,
				&opxy_spi_bridge_slave_pre, OPXY_PA4, OPXY_PB15,
				&s->hw.time, S09_T_REPLY_US);
#if OPXY_BOOT_L1_CHECK && !defined(OPXY_TEST_OS)
	/*
	 * S-09.019: the L1 code the stream loaded, checked before anything is
	 * served or the eMMC touched. A stream that arrived damaged there
	 * leaves PA4 high for good: the master's init finds no slave
	 * (S-09.017), as for a stream that never ended.
	 */
	if (!opxy_l1_code_ok())
		for (;;)
			;
#endif
}

#if defined(OPXY_TEST_OS)
/* opxy_slave_storage()'s eMMC half, on the master's word: every wait in it
 * is opxy_msi_bringup's. A failure leaves the server's down eMMC ops, so
 * every storage request fails as after S-13b.059. */
static void testos_emmc_start(struct opxy_slave *s)
{
	testos_emmc_done = true;	/* PB2 is the attention line from here */
#if defined(OPXY_TESTOS_BENCH) && OPXY_TESTOS_BENCH == 2
	/* A test build whose eMMC never comes up. */
	(void)testos_note(s, OPXY_TESTOS_STEP_EMMC, WB_MSI_TIMEOUT);
	s->st.emmc_up = false;
#else
	s->st.emmc_up = opxy_storage_up(&s->st);
#endif
	if (!s->st.emmc_up)
		return;
	opxy_storage_start(&s->st, true);
	opxy_storage_testos_volume(&s->st);
}

/* cppcheck-suppress constParameterCallback ; the storage's note fixes the type */
static int testos_note_cb(void *ctx, unsigned step, int rc)
{
	return testos_note(ctx, step, rc);
}
#endif

#if !defined(OPXY_TEST_OS)
/* S-10c.110: the developer requests' reach into the slave, a word read
 * (0x0E) or written (0x0D), inside opxy_dev_mem_ok(). The master gates them
 * on developer mode before any reaches the bridge. */
/* cppcheck-suppress constParameterCallback ; the ops struct fixes the type */
static uint32_t dev_cmd(void *ctx, uint8_t cmd, uint32_t addr, uint32_t val)
{
	(void)ctx;
	if (cmd == OPXY_DEV_PEEK_REQ)
		return opxy_dev_mem_ok(addr, false) ? wb_mmr_read32(addr) : 0xDEADBEEFu;
	if (!opxy_dev_mem_ok(addr, true))
		return 0xDEADBEEFu;
	wb_mmr_write32(addr, val);
	wb_mmr_sync();
	return 0u;
}
#endif

/* S-13b.050-.059, S-03.008, then S-12's volume (opxy_storage_init()),
 * and the S-09 server over its store. */
void opxy_slave_storage(struct opxy_slave *s)
{
#if defined(OPXY_TEST_OS)
	unsigned step;

	testos_t0 = wb_time_us(&s->hw.time);
	for (step = 0; step < OPXY_TESTOS_STEP_COUNT; step++)
		testos_rows[step] = OPXY_TESTOS_ROW(step, OPXY_TESTOS_ROW_NOT_RUN, 0u);
	s->st.note = testos_note_cb;
	s->st.note_ctx = s;
#endif
	opxy_storage_init(&s->st, &s->hw, &wb_device_opxy, OPXY_TESTOS_OR(false, true));	/* S-18.002: the slave is the OP-XY's */
#if defined(OPXY_TEST_OS)
	s->st.store.testos_row = testos_row;
#else
	s->st.store.dev_cmd = dev_cmd;
#endif
	s->st.store.os_status = os_status;
	s->st.store.stage_write = OPXY_TESTOS_OR(NULL, stage_write);	/* S-09.084 */
	s->st.store.cycles = cycles;
	s->st.store.adjust = adjust;
	s09_slave_init(&s->server, &s->st.store);
	/* S-09.079: a failed receive's own account, for 0x1B */
	s09_slave_fault_source(&s->server, receive_fault, &s->hw.link_slave);
	OPXY_TESTOS_OR((void)0, s09_slave_qtest_source(&s->server, qtest, s));	/* S-09.080 */
	OPXY_TESTOS_OR((void)0, s09_slave_qdata_able(&s->server, true));	/* S-09.081 */
	OPXY_TESTOS((void)testos_note(s, OPXY_TESTOS_STEP_SERVER, 0);)
}

void opxy_slave_user_bd(struct opxy_slave *s, struct wb_blockdev *bd)
{
	opxy_storage_user_bd(&s->st, bd);
}

static bool ddr_check(void *ctx)
{
	uint32_t mask = wb_core_cli();
	bool aliased = wb_dmc_aliases(OPXY_DDR_BASE, OPXY_DDR_ALIAS);

	wb_core_sti(mask);
	if (!aliased)
		opxy_slave_storage(ctx);
	return !aliased;
}

static void ddr_fatal(void *ctx)
{
	(void)ctx;
#if defined(OPXY_TEST_OS)
	testos_code = OPXY_TESTOS_BEACON_DDR_FAIL;
	testos_seen = false;
	for (;;)
		(void)wb_gpio_set(OPXY_PB2, testos_beacon(ctx));
#endif
	opxy_fatal(S04_FATAL_DDR_CHECK);	/* S-04.071's DDR row */
}

void opxy_slave_main(struct opxy_slave *s)
{
	s->hw.time = opxy_time;
	s->clock = opxy_clock(&s->hw);

	/* The CPU link to the master, over channel 0 of the tunnel. */
	s06_node_init(&s->node, wire_crc8_frame, &s->clock);	/* software: short frames */
	s06_link_init(&s->cpu_link, &s09_slave_tunnel_port, &s->server,
		      S06_ADDR_SLAVE, S06_ADDR_MASTER);
	s->node.link[S06_LINK_CPU] = &s->cpu_link;
	s->cpu_ops.ctx = s;
	s->cpu_ops.power_off = cpu_power_off;
	s->cpu_ops.clock = cpu_clock;
	s->cpu_ops.common = cpu_common;

	s->handoff.ctx = s;
	s->handoff.stage = staged;
	s->handoff.reply = replied;
	s->handoff.stop_server = stop_server;
	s->handoff.teardown = opxy_teardown_slave;
	s->handoff.kernel_call = opxy_kernel_call;
	s->handoff.read_svect0 = opxy_read_svect0;
	s->handoff.jump = opxy_jump;

	s->ops.ctx = s;
	s->ops.bringup = bringup;
	s->ops.ddr_check = ddr_check;
	s->ops.ddr_fatal = ddr_fatal;
	s->ops.serve_until_release = serve_until_release;
	s->ops.handoff = &s->handoff;
	s->ops.staging = (const uint8_t *)(uintptr_t)OPXY_STAGING;
	s->ops.config_addr = opxy_handover_config();
	boot_slave(&s->ops);
}
