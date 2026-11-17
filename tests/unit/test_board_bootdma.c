/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BOOT_LINK_FC=2 on the board (S-09.009, S-13b.023): the boot stream's parts
 * by DMA4 under SPI0's flow control (wb_spi_dma_send_bytes()), the board's
 * ops under s09_boot_slave() (opxy_boot_ops_dma()), the fall-back to one byte
 * at a time, and the stream's report. Over fake registers: DMA4 moves one
 * byte of its array at each look at its status, and only while SPI0's
 * transmit channel is on and PA6 reads low, as the controller's flow control
 * holds the next word; the core's own bytes go through the transmit FIFO.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/opxy.h"
#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_ops.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/s06_bytes.h"
#include "waffle/s09_fast.h"
#include "vectors/s09_vectors.h"

#define N OPXY_SPI_LINK
#define CH (WB_DMA_SPI0_TX + 2u * N)
#define MAX_SESS 512u
#define CYCLES_PER_LOOK 400u		/* a microsecond each read of the counter */

static struct {
	/* DMA4 and SPI0 */
	bool on;
	unsigned at;
	uint32_t off, dstat, twc, txctl, ctl;
	bool tf, rx_ready;
	uint32_t rx;
	unsigned dma_starts, dma_cfg_bad, dma_without_fc;
	uint64_t opened_at, first_part_at;	/* the opening byte's end, the first part's start */
	/* the faults: before the controller's k-th byte (1-based) */
	unsigned long dma_err_at, stall_at;
	bool no_tf;
	/* the slave: PA6 high until high_until, or for good */
	uint64_t high_until;
	bool stuck;
	unsigned long hold_at, stuck_at;
	uint64_t hold_cycles;
	/* what came */
	unsigned long bytes, fc_bytes, core_bytes, while_high, core_fc;
	bool selected;
	/* the select high before each session after the opening: the least */
	uint64_t raised_at, gap_min;
	bool gap_seen;
	unsigned n;
	size_t len[MAX_SESS];
	uint8_t sess[MAX_SESS][1100];
} bd;

static uint64_t cycles_now(void)
{
	return fake_core_peek_cycles();
}

static bool pa6_high(void)
{
	return bd.stuck || cycles_now() < bd.high_until;
}

static void take(uint32_t b)
{
	if (pa6_high())
		bd.while_high++;
	if (bd.n < MAX_SESS && bd.len[bd.n] < sizeof bd.sess[0])
		bd.sess[bd.n][bd.len[bd.n]] = (uint8_t)b;
	if (bd.n < MAX_SESS)
		bd.len[bd.n]++;
	bd.bytes++;
	if (bd.hold_at != 0u && bd.bytes == bd.hold_at)
		bd.high_until = cycles_now() + bd.hold_cycles;
	if (bd.stuck_at != 0u && bd.bytes == bd.stuck_at)
		bd.stuck = true;
}

static uint32_t dma_left(void)
{
	const struct wb_spi_array *a = &wb_spi_tx_array;
	uint64_t n = 0;

	for (unsigned i = bd.at; i < a->n; i++)
		n += a->bytes[i];
	return (uint32_t)(n - (bd.at < a->n ? bd.off : 0u));
}

/* DMA4: one byte to the controller, which clocks it at once unless its
 * flow control holds it (PA6 high), or a fault. */
static void dma_move(void)
{
	const struct wb_spi_array *a = &wb_spi_tx_array;
	const unsigned long k = bd.fc_bytes + 1u;

	while (bd.at < a->n && bd.off == a->bytes[bd.at]) {
		bd.at++;
		bd.off = 0;
	}
	if (bd.at == a->n) {
		bd.dstat |= DMA_STAT_IRQDONE;
		return;
	}
	if (bd.dma_err_at == k) {
		bd.dstat |= DMA_STAT_IRQERR;
		return;
	}
	if (bd.stall_at == k || pa6_high() || !(bd.txctl & SPI_XCTL_EN))
		return;
	take(a->mem[bd.at][bd.off]);
	bd.off++;
	bd.fc_bytes++;
	if (bd.twc != 0u && --bd.twc == 0u && !bd.no_tf)
		bd.tf = true;
	if (dma_left() == 0u)
		bd.dstat |= DMA_STAT_IRQDONE;
}

static bool hook(void *ctx, enum fake_mmr_op op, unsigned width, wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_WRITE) {
		if (addr == DMA_CFG(CH)) {
			bd.on = (*value & DMA_CFG_EN) != 0u;
			if (bd.on) {
				bd.at = 0;
				bd.off = 0;
				if (bd.dma_starts++ == 0u)
					bd.first_part_at = cycles_now();
				if (*value != WB_DMA_ARRAY_SEND8)
					bd.dma_cfg_bad++;
				if (!(bd.ctl & SPI_CTL_FCEN))
					bd.dma_without_fc++;
			}
		} else if (addr == DMA_STAT(CH)) {
			bd.dstat &= ~*value;
		} else if (addr == SPI_TWC(N)) {
			bd.twc = *value;
		} else if (addr == SPI_TXCTL(N)) {
			bd.txctl = *value;
		} else if (addr == SPI_CTL(N)) {
			bd.ctl = *value;
		} else if (addr == SPI_STAT(N)) {
			if (*value & SPI_STAT_TF)
				bd.tf = false;
		} else if (addr == SPI_TFIFO(N)) {
			/* the core's byte: no flow control in the byte path */
			if (bd.ctl & SPI_CTL_FCEN)
				bd.core_fc++;
			take(*value);
			bd.core_bytes++;
			bd.rx = bd.bytes == 1u ? 0xFFu : 0u;
			if (bd.bytes == 1u)
				bd.opened_at = cycles_now();
			bd.rx_ready = true;
		} else if (addr == SPI_SLVSEL(N)) {
			const bool on = (*value & SPI_SLVSEL_SSE(3)) && !(*value & SPI_SLVSEL_SSEL(3));

			if (on && !bd.selected && bd.n >= 1u &&
			    (!bd.gap_seen || cycles_now() - bd.raised_at < bd.gap_min)) {
				bd.gap_min = cycles_now() - bd.raised_at;
				bd.gap_seen = true;
			}
			if (on && !bd.selected && bd.n < MAX_SESS)
				bd.len[bd.n] = 0;
			if (!on && bd.selected) {
				bd.n++;
				bd.raised_at = cycles_now();
			}
			bd.selected = on;
		}
		return true;
	}
	if (op != FAKE_MMR_READ)
		return true;
	if (addr == DMA_STAT(CH)) {
		if (bd.on && !(bd.dstat & DMA_STAT_IRQERR))
			dma_move();
		*value = bd.dstat;
	} else if (addr == DMA_XCNT_CUR(CH)) {
		*value = dma_left();
	} else if (addr == DMA_CFG(CH)) {
		*value = bd.on ? DMA_CFG_EN : 0u;
	} else if (addr == SPI_TWC(N)) {
		*value = bd.twc;
	} else if (addr == SPI_STAT(N)) {
		*value = SPI_STAT_TFS_EMPTY | (bd.tf ? SPI_STAT_TF : 0u) |
			 (bd.rx_ready ? 0u : SPI_STAT_RFE) |
			 (bd.on && pa6_high() ? SPI_STAT_FCS : 0u);
	} else if (addr == SPI_RFIFO(N)) {
		*value = bd.rx;
		bd.rx_ready = false;
	} else if (addr == PORT_DATA(WB_PORT_A)) {
		*value = pa6_high() ? 1u << 6 : 0u;
	}
	return true;
}

static struct opxy_hw hw;

static void idle(void *ctx, uint32_t until_us)
{
	(void)ctx;
	(void)until_us;
}

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(CYCLES_PER_LOOK);
	memset(&bd, 0, sizeof bd);
	fake_mmr_set_hook(hook, NULL);
	hw.time = opxy_time;
	CHECK(wb_spi_init(&hw.link_spi, OPXY_SPI_LINK, &opxy_spi_boot_link, &hw.time,
			  OPXY_BOOT_LINK_WORD_US) == WB_SPI_OK);
	bd.ctl = opxy_spi_boot_link.ctl | SPI_CTL_EN;
}

static int stream(const struct s09v_stream *v)
{
	const struct s06_clock clk = { wb_time_now_us, idle, &hw.time };
	struct s09_boot_ops ops = opxy_boot_ops_dma(&hw);
	int end = -1, rc;

	rc = s09_boot_slave(&ops, &clk, v->slot, v->len, true, &end);
	if (rc == S09_BOOT_STREAMED)
		CHECK(end == v->end);
	return rc;
}

static bool same_sessions(const struct s09v_stream *v)
{
	bool ok = bd.n == v->sn;

	for (size_t s = 0; ok && s < v->sn; s++) {
		const struct s09v_sess *e = &s09v_sessions[v->s0 + s];

		if (e->at < 0)
			ok = bd.len[s] == 1u && bd.sess[s][0] == 0x03u;
		else
			ok = bd.len[s] == e->len && memcmp(bd.sess[s], v->slot + e->at, e->len) == 0;
	}
	return ok;
}

static unsigned long slot_bytes(const struct s09v_stream *v)
{
	unsigned long n = 0;

	for (size_t s = 1; s < v->sn; s++)
		n += s09v_sessions[v->s0 + s].len;
	return n;
}

/* SPI0 as the bridge's set-up expects to find it after the stream: no
 * channel on, DMA4 off, the select released. */
static bool left_clean(void)
{
	return !(bd.txctl & SPI_XCTL_EN) && !(fake_mmr_get(SPI_RXCTL(N)) & SPI_XCTL_EN) &&
	       !bd.on && !bd.selected;
}

/*
 * One part's registers: the channels off and the status cleared, DMA4's
 * status, its array and its channel value with 8-bit peripheral words,
 * SPI_TWC, SPI_TXCTL without TEN, then TEN last; SPI_CTL never written, so
 * EN stays set under the flow-controlled value (FCEN, ready active low, mode
 * 1, 8-bit, master). The end: DMA4's done status acknowledged, TEN cleared,
 * the status cleared, DMA4 off and its status cleared.
 */
static void test_part_registers(void)
{
	static const uint8_t buf[64] = { 1, 2, 3, 4, 5 };
	struct fake_mmr_access w[64];
	static const wb_mmr_addr watched[] = {
		SPI_CTL(N), SPI_TXCTL(N), SPI_RXCTL(N), SPI_STAT(N), SPI_TWC(N),
		DMA_STAT(CH), DMA_DSCPTR_CUR(CH), DMA_CFG(CH),
	};
	struct fake_mmr_access got[32] = { { FAKE_MMR_READ, 0u, 0u, 0u } };
	size_t nw, k = 0;
	uint32_t left = 99u;

	reset();
	CHECK(wb_spi_reconfigure(&hw.link_spi, opxy_spi_boot_link_fc.ctl,
				 opxy_spi_boot_link_fc.clk) == WB_SPI_OK);
	CHECK(opxy_spi_boot_link_fc.ctl & SPI_CTL_FCEN);
	CHECK(!(opxy_spi_boot_link_fc.ctl & (SPI_CTL_FCPL | SPI_CTL_FCCH)));
	CHECK((opxy_spi_boot_link_fc.ctl & (SPI_CTL_MSTR | SPI_CTL_CPHA | SPI_CTL_CPOL |
					    SPI_CTL_SIZE_MASK)) == (SPI_CTL_MSTR | SPI_CTL_CPHA));
	fake_mmr_clear_log();
	CHECK(wb_spi_dma_send_bytes(&hw.link_spi, buf, sizeof buf, &left) == WB_SPI_OK);
	CHECK(left == 0u);
	CHECK(bd.fc_bytes == sizeof buf && bd.bytes == sizeof buf);
	CHECK(memcmp(bd.sess[0], buf, sizeof buf) == 0);
	nw = fake_mmr_writes(w, ARRAY_LEN(w));
	for (size_t i = 0; i < nw && k < ARRAY_LEN(got); i++)
		for (size_t j = 0; j < ARRAY_LEN(watched); j++)
			if (w[i].addr == watched[j])
				got[k++] = w[i];
	CHECK(k == 14u);
	CHECK(got[0].addr == SPI_TXCTL(N) && got[0].value == 0u);
	CHECK(got[1].addr == SPI_RXCTL(N) && got[1].value == 0u);
	CHECK(got[2].addr == SPI_STAT(N) && got[2].value == SPI_STAT_W1C);
	CHECK(got[3].addr == DMA_STAT(CH));
	CHECK(got[4].addr == DMA_DSCPTR_CUR(CH) &&
	      got[4].value == (uint32_t)(uintptr_t)wb_spi_tx_array.d);
	CHECK(got[5].addr == DMA_CFG(CH) && got[5].value == 0x00035001u);
	CHECK(got[6].addr == SPI_TWC(N) && got[6].value == sizeof buf);
	CHECK(got[7].addr == SPI_TXCTL(N) && got[7].value == 0x0001001Cu);
	CHECK(got[8].addr == SPI_TXCTL(N) && got[8].value == 0x0001001Du);
	CHECK(got[9].addr == DMA_STAT(CH) && got[9].value == DMA_STAT_IRQDONE);
	CHECK(got[10].addr == SPI_TXCTL(N) && got[10].value == 0u);
	CHECK(got[11].addr == SPI_STAT(N));
	CHECK(got[12].addr == DMA_CFG(CH) && got[12].value == 0u);
	CHECK(got[13].addr == DMA_STAT(CH));
	for (size_t i = 0; i < k; i++)
		CHECK(got[i].addr != SPI_CTL(N));
	/* the array: 8-bit peripheral words, the last descriptor's stop and
	 * interrupt */
	CHECK(wb_spi_tx_array.n >= 1u);
	for (unsigned i = 0; i < wb_spi_tx_array.n; i++)
		CHECK((wb_spi_tx_array.d[i].cfg & (7u << DMA_CFG_PSIZE_SHIFT)) == 0u);
	CHECK((wb_spi_tx_array.d[wb_spi_tx_array.n - 1u].cfg & DMA_CFG_FLOW_MASK) ==
	      DMA_CFG_FLOW_STOP);
	CHECK(wb_spi_tx_array.d[wb_spi_tx_array.n - 1u].cfg & DMA_CFG_INT_X);
	CHECK(fake_mmr_errors() == 0u);

	/* refused: a part of 32-bit words, of none, of more than SPI_TWC holds;
	 * nothing written */
	{
		static uint8_t big[SPI_TWC_MAX + 4u];
		struct wb_spi s32 = hw.link_spi;

		s32.ctl = (s32.ctl & ~SPI_CTL_SIZE_MASK) | SPI_CTL_SIZE32;
		fake_mmr_clear_log();
		CHECK(wb_spi_dma_send_bytes(&s32, buf, 8u, &left) == WB_SPI_ERR_ARG && left == 8u);
		CHECK(wb_spi_dma_send_bytes(&hw.link_spi, buf, 0u, &left) == WB_SPI_ERR_ARG);
		CHECK(wb_spi_dma_send_bytes(&hw.link_spi, big, sizeof big, &left) == WB_SPI_ERR_ARG);
		CHECK(fake_mmr_writes(w, ARRAY_LEN(w)) == 0u);
	}
}

/* A part the line holds part-way for less than the word timeout goes on;
 * held for good, it fails after the word timeout with the bytes not clocked
 * in *left and the transmit channel off. A DMA error and a stall with the
 * line low do the same, the DMA stopped. */
static void test_part_failures(void)
{
	static const uint8_t buf[100] = { 9 };
	uint32_t left;
	uint64_t t0;

	reset();
	CHECK(wb_spi_reconfigure(&hw.link_spi, opxy_spi_boot_link_fc.ctl,
				 opxy_spi_boot_link_fc.clk) == WB_SPI_OK);
	bd.hold_at = 30u;
	bd.hold_cycles = 1000000u * OPXY_CCLK_MHZ;	/* a second */
	CHECK(wb_spi_dma_send_bytes(&hw.link_spi, buf, sizeof buf, &left) == WB_SPI_OK);
	CHECK(bd.bytes == 100u && bd.while_high == 0u);

	reset();
	CHECK(wb_spi_reconfigure(&hw.link_spi, opxy_spi_boot_link_fc.ctl,
				 opxy_spi_boot_link_fc.clk) == WB_SPI_OK);
	bd.stuck_at = 40u;
	t0 = cycles_now();
	CHECK(wb_spi_dma_send_bytes(&hw.link_spi, buf, sizeof buf, &left) == WB_SPI_TIMEOUT);
	CHECK(left == 60u && bd.bytes == 40u);
	CHECK(cycles_now() - t0 <= ((uint64_t)S09_T_STREAM_US + 1000u) * OPXY_CCLK_MHZ);
	CHECK(!(bd.txctl & SPI_XCTL_EN) && !bd.on);

	reset();
	CHECK(wb_spi_reconfigure(&hw.link_spi, opxy_spi_boot_link_fc.ctl,
				 opxy_spi_boot_link_fc.clk) == WB_SPI_OK);
	bd.dma_err_at = 11u;
	CHECK(wb_spi_dma_send_bytes(&hw.link_spi, buf, sizeof buf, &left) == WB_SPI_ERR_HW);
	CHECK(left == 90u && bd.bytes == 10u && !bd.on && !(bd.txctl & SPI_XCTL_EN));

	reset();
	CHECK(wb_spi_reconfigure(&hw.link_spi, opxy_spi_boot_link_fc.ctl,
				 opxy_spi_boot_link_fc.clk) == WB_SPI_OK);
	bd.no_tf = true;
	CHECK(wb_spi_dma_send_bytes(&hw.link_spi, buf, sizeof buf, &left) == WB_SPI_TIMEOUT);
	CHECK(left == 0u && bd.bytes == 100u);
}

/* The whole stream by the board's ops: the sessions and bytes are the
 * default path's, every part by DMA under flow control, no byte by the core
 * but the opening one, nothing left in SPI0, and the report says so. */
static void test_stream(void)
{
	for (size_t i = 0; i < s09v_streams_count; i++) {
		const struct s09v_stream *v = &s09v_streams[i];
		const struct s09_fast_stats *f;
		struct opxy_boot_fcs fcs;
		uint8_t r[OPXY_STREAM_REPORT];

		reset();
		opxy_stream_begin();
		CHECK(stream(v) == S09_BOOT_STREAMED);
		opxy_stream_end(S09_BOOT_STREAMED, v->end);
		CHECK(same_sessions(v));
		CHECK(bd.core_bytes == 1u && bd.fc_bytes == slot_bytes(v));
		CHECK(bd.while_high == 0u && bd.dma_cfg_bad == 0u && bd.dma_without_fc == 0u);
		CHECK(left_clean());
		/* the guard after the opening session too, and before every
		 * stream session with the select high (S-13b.023) */
		CHECK(bd.first_part_at - bd.opened_at >= (uint64_t)OPXY_BOOT_LINK_GUARD_US * OPXY_CCLK_MHZ);
		CHECK(bd.gap_seen && bd.gap_min >= (uint64_t)OPXY_BOOT_LINK_GUARD_US * OPXY_CCLK_MHZ);
		/* SPI0_STAT read while the parts ran; PA6 never high, so no FCS */
		opxy_boot_fcs_stats(&fcs);
		CHECK(fcs.samples != 0u && fcs.held == 0u && fcs.parts == 0u && fcs.first == 0u);
		f = &opxy_boot_fast()->st;
		CHECK(f->fell == S09_PART_OK && !f->held && f->slow_bytes == 0u);
		CHECK(f->parts == bd.dma_starts && f->fast_bytes == slot_bytes(v));
		CHECK(opxy_boot_way() == 2u);
		CHECK(opxy_stream_report(r) == OPXY_STREAM_REPORT);
		CHECK(r[0] == 2u && r[1] == 2u && r[2] == S09_BOOT_STREAMED && r[3] == (uint8_t)v->end);
		CHECK(s06_rd_le32(r + 72) == fcs.samples && s06_rd_le32(r + 76) == 0u &&
		      s06_rd_le32(r + 80) == 0u && s06_rd_le32(r + 84) == 0u);
		CHECK(r[4] == 0u && r[6] == 0u && r[7] == 1u);
		CHECK(s06_rd_le32(r + 8) == v->sn - 1u);
		CHECK(s06_rd_le32(r + 12) == f->parts);
		CHECK(s06_rd_le32(r + 16) == slot_bytes(v));
		CHECK(s06_rd_le32(r + 20) == 0u);
		CHECK(s06_rd_le32(r + 52) == OPXY_BOOT_LINK_GUARD_US);
	}
}

/* PA6 high at each point of a session, for a short time and for nearly
 * T_STREAM: waited out by the guard's sample or the controller's flow
 * control; no byte while it reads high, the stream whole. */
static void test_holds(void)
{
	const struct s09v_stream *v = &s09v_streams[0];
	static const unsigned long at[] = { 1u, 2u, 16u, 17u, 18u, 500u, 1041u };
	static const uint32_t us[] = { 5u, 300u, S09_T_STREAM_US - 50000u };

	for (size_t a = 0; a < ARRAY_LEN(at); a++)
		for (size_t u = 0; u < ARRAY_LEN(us); u++) {
			reset();
			bd.hold_at = at[a];
			bd.hold_cycles = (uint64_t)us[u] * OPXY_CCLK_MHZ;
			CHECK(stream(v) == S09_BOOT_STREAMED);
			CHECK(same_sessions(v));
			CHECK(bd.while_high == 0u);
			CHECK(opxy_boot_fast()->st.fell == S09_PART_OK);
			/* a hold inside a payload part holds the controller: FCS
			 * read set in that part, at or before the held byte */
			if (at[a] == 500u && us[u] >= 300u) {
				struct opxy_boot_fcs fcs;

				opxy_boot_fcs_stats(&fcs);
				CHECK(fcs.held != 0u && fcs.parts >= 1u);
				CHECK(fcs.first != 0u && fcs.first - 1u <= 500u);
			}
		}
}

/* PA6 high for good after the opening, in a header, in a payload: the
 * stream fails within T_STREAM, no fall-back, SPI0 clean. */
static void test_never_falls(void)
{
	const struct s09v_stream *v = &s09v_streams[0];
	static const unsigned long at[] = { 1u, 5u, 17u, 600u };

	for (size_t a = 0; a < ARRAY_LEN(at); a++) {
		uint64_t t0;
		uint8_t r[OPXY_STREAM_REPORT];

		reset();
		bd.stuck_at = at[a];
		t0 = cycles_now();
		opxy_stream_begin();
		CHECK(stream(v) == S09_BOOT_STUCK);
		opxy_stream_end(S09_BOOT_STUCK, 0);
		CHECK(cycles_now() - t0 <= ((uint64_t)S09_T_STREAM_US + 100000u) * OPXY_CCLK_MHZ);
		CHECK(bd.bytes == at[a]);
		CHECK(left_clean());
		CHECK(opxy_boot_fast()->st.fell == S09_PART_OK && opxy_boot_fast()->st.held);
		CHECK(bd.core_bytes == 1u);
		(void)opxy_stream_report(r);
		CHECK(r[2] == S09_BOOT_STUCK && r[4] == 0u && r[6] == 1u);
	}
}

/* Each fault of the controller's own: the stream whole and the same, the
 * rest by the core at the byte path's set-up (no flow control), DMA4 never
 * started again, and the report names the cause and the place. */
static void test_fall_back(void)
{
	const struct s09v_stream *v = &s09v_streams[0];
	const unsigned long total = slot_bytes(v);
	static const unsigned long at[] = { 1u, 16u, 17u, 700u, 1040u, 2000u };

	for (unsigned c = 0; c < 3u; c++)
		for (size_t a = 0; a < ARRAY_LEN(at); a++) {
			const struct s09_fast_stats *f;
			uint8_t r[OPXY_STREAM_REPORT];
			unsigned starts;

			if (at[a] > total)
				continue;
			reset();
			if (c == 0u)
				bd.dma_err_at = at[a];
			else if (c == 1u)
				bd.stall_at = at[a];
			else
				bd.no_tf = true;
			opxy_stream_begin();
			CHECK(stream(v) == S09_BOOT_STREAMED);
			opxy_stream_end(S09_BOOT_STREAMED, v->end);
			CHECK(same_sessions(v));
			CHECK(bd.bytes == 1u + total && bd.while_high == 0u);
			CHECK(bd.core_fc == 0u);
			f = &opxy_boot_fast()->st;
			CHECK(f->fell == (c == 0u ? S09_PART_DMA : c == 1u ? S09_PART_STALL : S09_PART_NO_END));
			starts = bd.dma_starts;
			CHECK(starts == f->parts + 1u);	/* the one that failed, then none */
			CHECK(left_clean());
			(void)opxy_stream_report(r);
			CHECK(r[4] == f->fell);
			if (c < 2u) {
				CHECK(bd.fc_bytes == at[a] - 1u);
				CHECK(f->fell_at == at[a] - 1u);
				CHECK(f->slow_bytes == total - (at[a] - 1u));
				CHECK(bd.core_bytes == 1u + total - (at[a] - 1u));
			} else {
				/* every byte of the first part clocked, then its end missing */
				CHECK(f->fell_at == bd.fc_bytes);
				CHECK(bd.core_bytes == 1u + total - bd.fc_bytes);
				CHECK(s06_rd_le32(r + 56) == (uint32_t)WB_SPI_TIMEOUT);
			}
			if (c == 2u)
				break;
		}
}

int main(void)
{
	RUN(test_part_registers);
	RUN(test_part_failures);
	RUN(test_stream);
	RUN(test_holds);
	RUN(test_never_falls);
	RUN(test_fall_back);
	return check_summary("board_bootdma");
}
