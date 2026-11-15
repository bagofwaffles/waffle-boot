/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-05.004 on the board (src/board/opxy_master.c): the report before the
 * load reaches the PSoC link even behind a queued log line. The UART moves
 * at most a FIFO's worth of bytes each time it is serviced, so a frame
 * written behind a long line stays in the ring; opxy_psoc_drain() sends it
 * before the load, as the teardown (S-05.016) would drop it. The same holds
 * for S-04.063's shutdown reports before a hard reset (S-04.028).
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/opxy_boot.h"
#include "waffle/s06_frame.h"
#include "waffle/wire_crc.h"

static struct opxy_master m;
static uint8_t rx[OPXY_UART_BUF], tx[OPXY_UART_BUF];

/* The line: THR always empty, every byte written kept in out[]. */
static uint8_t out[256];
static size_t out_len;

static bool line_hook(void *ctx, enum fake_mmr_op op, unsigned width,
		      wb_mmr_addr addr, uint32_t *value)
{
	(void)ctx;
	(void)width;
	if (op == FAKE_MMR_READ && addr == UART_STAT(OPXY_UART_PSOC))
		*value = UART_STAT_THRE | UART_STAT_TEMT;
	else if (op == FAKE_MMR_WRITE && addr == UART_THR(OPXY_UART_PSOC) &&
		 out_len < sizeof(out))
		out[out_len++] = (uint8_t)*value;
	return true;
}

/* As S-06's put(): room asked first, then the frame written. */
static void put(const uint8_t *b, size_t n)
{
	struct wb_uart *u = &m.hw.uart[OPXY_UART_PSOC];

	CHECK(wb_uart_room(u) >= n);
	wb_uart_write(u, b, n);
}

static void line_setup(void)
{
	memset(&m, 0, sizeof(m));
	fake_mmr_reset();
	fake_core_reset();
	fake_core_set_cycle_step(400);
	fake_mmr_set_hook(line_hook, NULL);
	out_len = 0;
	m.hw.time.cycles_per_us = 400u;
	CHECK(wb_uart_init(&m.hw.uart[OPXY_UART_PSOC], OPXY_UART_PSOC,
			   &opxy_uart_psoc, &m.hw.time, rx, sizeof(rx), tx,
			   sizeof(tx)) == WB_UART_OK);
}

static void test_report_behind_a_log_line(void)
{
	uint8_t line[48], report[9], want[sizeof(line) + sizeof(report)];

	line_setup();
	for (size_t i = 0; i < sizeof(line); i++)
		line[i] = (uint8_t)(0x40u + i);
	line[sizeof(line) - 1] = 0;
	for (size_t i = 0; i < sizeof(report); i++)
		report[i] = (uint8_t)(0xA0u + i);
	memcpy(want, line, sizeof(line));
	memcpy(want + sizeof(line), report, sizeof(report));

	put(line, sizeof(line));
	put(report, sizeof(report));
	CHECK(out_len < sizeof(want));		/* the report still queued */

	CHECK(opxy_psoc_drain(&m) == WB_UART_OK);
	CHECK(out_len == sizeof(want));
	CHECK(memcmp(out, want, sizeof(want)) == 0);
	CHECK(fake_mmr_errors() == 0);
}

/*
 * S-04.063, S-04.028: the shutdown's 0x51 {00 00 02} and 0x40 {01} are on
 * the line when the shutdown returns, behind a queued log line, so the
 * hard reset that follows does not cut them off (no slave, no display).
 */
static void test_shutdown_behind_a_log_line(void)
{
	static const uint8_t dark[3] = { 0x00, 0x00, 0x02 }, leds[1] = { 0x01 };
	struct s06_link *l = &m.link[S06_LINK_PSOC];
	uint8_t line[160], want[sizeof(line) + 2u * S06_MAX_WIRE];
	size_t n;
	uint8_t route, seq;

	line_setup();
	m.clock = opxy_clock(&m.hw);
	s06_node_init(&m.node, wire_crc8_frame, &m.clock);
	s06_link_init(l, &opxy_port_ops, &m.hw.uart[OPXY_UART_PSOC],
		      S06_ADDR_MASTER, S06_ADDR_PSOC);
	m.node.link[S06_LINK_PSOC] = l;
	s07_init(&m.psoc, &m.node, S06_LINK_PSOC, &m.psoc_ops);
	route = s06_route(S06_ADDR_MASTER, S06_ADDR_PSOC);
	seq = l->seq;

	for (size_t i = 0; i < sizeof(line); i++)
		line[i] = (uint8_t)(0x40u + i);
	line[sizeof(line) - 1] = 0;
	put(line, sizeof(line));
	memcpy(want, line, sizeof(line));
	n = sizeof(line);
	n += s06_frame_encode(route, seq, 0x51u, dark, sizeof(dark),
			      wire_crc8_frame, want + n);
	n += s06_frame_encode(route, (uint8_t)(seq + 1u), 0x40u, leds,
			      sizeof(leds), wire_crc8_frame, want + n);

	CHECK(n <= sizeof(out));
	opxy_shutdown(&m);
	CHECK(m.psoc.shut);
	CHECK(out_len == n);
	CHECK(memcmp(out, want, n) == 0);
	CHECK(fake_mmr_errors() == 0);
}

int main(void)
{
	RUN(test_report_behind_a_log_line);
	RUN(test_shutdown_behind_a_log_line);
	return check_summary("test_board_report");
}
