/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * How this start's slave boot stream went, with BOOT_LINK_FC=1 or 2 (S-09.009;
 * waffle/board/opxy_boot.h): the way it was sent, whether it fell back and
 * why, its sessions, bytes and time, and whether the bridge then linked. A
 * log line once the bridge's start-up has ended, and the same for a
 * developer's request (OPXY_DEV_STREAM, S-10c.110). Reading only.
 */
#include "waffle/board/opxy_boot.h"

#if OPXY_BOOT_LINK_FC && !defined(OPXY_TEST_OS)
#include <string.h>

#include "waffle/drivers/bf70x_core.h"
#include "waffle/s06_bytes.h"

#define STREAM_REPORT_VERSION 1u

static struct {
	bool ran, linked;
	int rc, end;
	uint64_t from, to;
} stream;

void opxy_stream_begin(void)
{
	memset(&stream, 0, sizeof stream);
	stream.from = wb_core_cycles();
}

void opxy_stream_end(int rc, int end)
{
	stream.to = wb_core_cycles();
	stream.rc = rc;
	stream.end = end;
	stream.ran = true;
}

static uint32_t us_of(uint64_t cycles)
{
	const uint64_t us = cycles / OPXY_CCLK_MHZ;

	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

/* Log wording is free (S-14.009). */
static const char *const cause_text[S09_PART_CAUSES] = {
	"none", "the slave's hold", "a DMA error", "the controller stopped with the line low",
	"no end of the transmit with the line low", "the driver refused the part",
};

void opxy_stream_linked(struct opxy_master *m)
{
	const struct s09_fast_stats *f = &opxy_boot_fast()->st;
	struct s14_text t;
	const bool dma = opxy_boot_way() == 2u;

	stream.linked = !m->bridge.absent;
	s14_text_init(&t);
	s14_text_str(&t, dma ? "stream by DMA under flow control: " : "stream under flow control: ");
	if (dma) {
		s14_text_dec(&t, f->sessions);
		s14_text_str(&t, " sessions, ");
		s14_text_dec(&t, f->fast_bytes + f->slow_bytes);
		s14_text_str(&t, " bytes, ");
	}
	s14_text_dec(&t, us_of(stream.to - stream.from) / 1000u);
	s14_text_str(&t, " ms, outcome ");
	s14_text_dec(&t, (uint32_t)stream.rc);
	if (f->fell != S09_PART_OK && f->fell < S09_PART_CAUSES) {
		s14_text_str(&t, "; fell back at byte ");
		s14_text_dec(&t, f->fell_at);
		s14_text_str(&t, " of session ");
		s14_text_dec(&t, f->fell_session);
		s14_text_str(&t, " (");
		s14_text_str(&t, cause_text[f->fell]);
		s14_text_str(&t, "), then ");
		s14_text_dec(&t, f->slow_bytes);
		s14_text_str(&t, " bytes one at a time");
	}
	if (f->held)
		s14_text_str(&t, "; the slave held the line past T_STREAM");
	s14_text_str(&t, stream.linked ? "; the bridge linked" : "; the bridge did not link");
	s14_log_text(&m->sv.log, stream.linked && f->fell == S09_PART_OK ? S14_INFO : S14_WARNING,
		     S14_TAG("boot"), &t);
}

size_t opxy_stream_report(uint8_t *out)
{
	const struct s09_fast_stats *f = &opxy_boot_fast()->st;
	const bool dma = opxy_boot_way() == 2u;
	const uint32_t w[] = {
		dma ? f->sessions : 0u, dma ? f->parts : 0u, dma ? f->fast_bytes : 0u,
		dma ? f->slow_bytes : 0u, dma ? f->fell_session : 0u, dma ? f->fell_at : 0u,
		stream.ran ? us_of(stream.to - stream.from) : 0u,
		dma ? f->guard_us : 0u, dma ? f->wait_us : 0u, dma ? f->wait_max_us : 0u,
		dma ? f->high : 0u, dma ? OPXY_BOOT_LINK_GUARD_US : 0u,
	};
	uint32_t fault[4];
	unsigned i;

	_Static_assert(8u + 4u * (sizeof w / sizeof w[0]) + 16u == OPXY_STREAM_REPORT,
		       "the report's layout (waffle/board/opxy_boot.h)");
	opxy_boot_fast_fault(fault);
	out[0] = STREAM_REPORT_VERSION;
	out[1] = (uint8_t)opxy_boot_way();
	out[2] = (uint8_t)stream.rc;
	out[3] = (uint8_t)stream.end;
	out[4] = (uint8_t)(dma ? f->fell : 0u);
	out[5] = (uint8_t)(stream.linked ? 1u : 0u);
	out[6] = (uint8_t)(dma && f->held ? 1u : 0u);
	out[7] = (uint8_t)(stream.ran ? 1u : 0u);
	for (i = 0; i < sizeof w / sizeof w[0]; i++)
		s06_wr_le32(out + 8u + 4u * i, w[i]);
	for (i = 0; i < 4u; i++)
		s06_wr_le32(out + 56u + 4u * i, dma ? fault[i] : 0u);
	return OPXY_STREAM_REPORT;
}
#else
typedef int opxy_stream_absent;	/* a translation unit is not empty */
#endif
