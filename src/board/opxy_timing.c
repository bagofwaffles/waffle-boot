/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Boot milestone timing: built only with BOOT_TIMING=1, never in a release
 * (waffle/board/opxy_boot.h, waffle/boot_time.h). The bootloader stamps the
 * master's milestones as it goes. In held mode it repeats the report on the
 * log, a line every 100 ms and the whole again 2 s later, so a host that has
 * just switched the CDC sink on (S-14.013) gets all of it. At a hand-over it
 * leaves the report at OPXY_BOOT_TIME_RECORD, outside everything an OS image
 * may load over that this project builds, and the test OS built with the
 * option logs it. That record departs from S-01.018 in this build alone.
 */
#include "waffle/board/opxy_boot.h"

#if OPXY_BOOT_TIMING
#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/s01_memmap.h"

/* The record lies below the display record, clear of it. */
_Static_assert(OPXY_BOOT_TIME_RECORD + BOOT_TIME_RECORD <= S01_DISPLAY_RECORD_START,
	       "the timing record ends before the display record");

struct boot_time opxy_boot_time;
static bool copying;

/* The power at the start: the PSoC's state and the battery report's status
 * byte, charge, mV and mA (S-07.050, S-07.053). */
static void power_known(bool state_known, unsigned state, bool batt_known, uint8_t status,
			const struct s07_battery *b)
{
	if (state_known) {
		opxy_boot_time.power_state = state;
		opxy_boot_time.flags |= BOOT_TIME_POWER_KNOWN;
	}
	if (!batt_known)
		return;
	opxy_boot_time.battery = status | (uint32_t)b->charge << 8 | (uint32_t)b->mv << 16;
	opxy_boot_time.battery_ma = b->ma;
	opxy_boot_time.flags |= BOOT_TIME_BATTERY_KNOWN;
}

void opxy_timing_start(void)
{
	unsigned i;

	for (i = 0; i < 3u; i++)
		boot_time_mark(&opxy_boot_time, BOOT_TIME_INIT_ENTRY + i, opxy_init_cycles[i]);
	OPXY_TIME(BOOT_TIME_MAIN);
}

/* s07_startup()'s steps, in its order, with a stamp after the greeting and
 * after the key status. */
int opxy_timing_startup(struct s07_psoc *p, struct s07_key_status *ks)
{
	int rc = s07_open(p);

	OPXY_TIME(BOOT_TIME_PSOC_OPEN);
	if (p->greeted)
		opxy_boot_time.flags |= BOOT_TIME_GREETED;
	if (rc)
		return rc;
	s07_report(p, S07_REPORT_START);
	rc = s07_battery_wait(p);
	if (rc)
		return rc;
	if (p->ops->early_logo)		/* S-05.004 */
		p->ops->early_logo(p->ops->ctx);
	s07_subscribe(p);
	rc = s07_key_status(p, ks);
	OPXY_TIME(BOOT_TIME_KEYS);
	power_known(p->state != S07_POWER_NONE, p->state, p->batt.present,
		    p->batt.charging ? 2u : 1u, &p->batt);
	if (rc)
		return rc;
	return s07_battery_rest(p);
}

/* The front stamps its entry, where it greets, lights the logo and reads the
 * keys, and its hand-off, in every build: the timing build reads the same
 * points as the normal one. */
void opxy_timing_front(const struct fr_handoff *ho)
{
	static const struct { unsigned from, to; } map[] = {
		{ FR_STAMP_ENTRY, BOOT_TIME_FRONT_ENTRY },
		{ FR_STAMP_GREETED, BOOT_TIME_PSOC_OPEN },
		{ FR_STAMP_LOGO, BOOT_TIME_LOGO },
		{ FR_STAMP_KEYS, BOOT_TIME_KEYS },
		{ FR_STAMP_HANDOFF, BOOT_TIME_FRONT_HANDOFF },
	};
	static const struct { uint32_t from, to; } flag[] = {
		{ FR_HO_GREETED, BOOT_TIME_GREETED },
		{ FR_HO_GREET_WAITED, BOOT_TIME_GREET_WAITED },
		{ FR_HO_CHARGE_WAITED, BOOT_TIME_CHARGE_WAITED },
	};
	unsigned i;

	for (i = 0; i < sizeof map / sizeof map[0]; i++)
		if (ho->stamps[map[i].from] != 0u)
			boot_time_mark(&opxy_boot_time, map[i].to, ho->stamps[map[i].from]);
	for (i = 0; i < sizeof flag / sizeof flag[0]; i++)
		if ((ho->flags & flag[i].from) != 0u)
			opxy_boot_time.flags |= flag[i].to;
	if (ho->battery_len != 0u) {
		struct s07_battery b;

		s07_battery_decode(ho->battery, ho->battery_len, &b);
		power_known((ho->flags & FR_HO_POWER_KNOWN) != 0u, ho->power_state, true,
			    ho->battery[0], &b);
	} else {
		power_known((ho->flags & FR_HO_POWER_KNOWN) != 0u, ho->power_state, false, 0u,
			    NULL);
	}
}

/*
 * The stream's pacing (S-09.005): the board's ops behind these. After a
 * byte the line is sampled through the window, which stands in for the
 * settle time and is never shorter; the sample after it decides the next
 * byte, as in a normal build. That deciding sample, when it finds the line
 * high, starts the wait that the first low sample ends. Only a stream is
 * counted: the first byte is the opening one, and when the slave does not
 * answer it as S-09.004 says, what follows is S-09.007's, where PA6 is
 * waited for high.
 *
 * The split (struct boot_time_split) brackets each op with the counter:
 * a byte costs two counter reads more than the pacing's own (one before
 * its transfer, one before its deciding sample), three 64-bit sums, two
 * compares for the shortest and longest transfer, two counts and the block
 * walk's test, with no division and no call to the log. A chip-select
 * change costs two reads. A counter read's cost is taken at the start, so
 * that the report can take it off.
 */
static struct s09_boot_ops stream_ops;
static bool stream_first, stream_waiting, stream_begun, stream_open, stream_selected;
static uint32_t stream_sent;
static uint64_t stream_from, stream_byte_end;
static struct boot_time_watch stream_watch;
static struct boot_time_walk stream_walk;

#define SPLIT (opxy_boot_time.split)

#define STREAM_WINDOW_US \
	(OPXY_BOOT_TIMING_WINDOW_US > OPXY_BOOT_LINK_SETTLE_US ? \
	 OPXY_BOOT_TIMING_WINDOW_US : OPXY_BOOT_LINK_SETTLE_US)
#define STREAM_WINDOW_CYCLES ((uint64_t)(STREAM_WINDOW_US) * OPXY_CCLK_MHZ)

static bool stream_reached(unsigned id)
{
	return (opxy_boot_time.set >> id & 1u) != 0u;
}

static bool stream_ready(void *ctx)
{
	const uint64_t at = wb_core_cycles();
	const bool low = stream_ops.ready(ctx);
	const uint64_t now = wb_core_cycles();

	if (!stream_selected) {
		/* S-09.003's wait, before the opening byte */
		if (!stream_reached(BOOT_TIME_STREAM_WAIT))
			boot_time_mark(&opxy_boot_time, BOOT_TIME_STREAM_WAIT, at);
		if (low)
			boot_time_mark(&opxy_boot_time, BOOT_TIME_STREAM_READY, now);
		return low;
	}
	SPLIT.reads++;
	boot_time_watch_sample(&opxy_boot_time, &stream_watch, !low, now - stream_byte_end,
			       false);
	if (stream_first && !low) {
		SPLIT.sample += now - at;
		opxy_boot_time.stream_high++;
		stream_from = now;
		stream_waiting = true;
	} else if (stream_waiting && low) {
		opxy_boot_time.stream_wait += now - stream_from;
		boot_time_gap(&opxy_boot_time, now - stream_from, SPLIT.bytes);
		stream_waiting = false;
	} else if (!stream_waiting) {
		SPLIT.sample += now - at;
	}
	stream_first = false;
	return low;
}

static void stream_select(void *ctx, bool on)
{
	const uint64_t at = wb_core_cycles();

	stream_ops.select(ctx, on);
	SPLIT.select += wb_core_cycles() - at;
	SPLIT.reads += 2u;
	if (on && stream_begun && stream_open)
		SPLIT.sessions++;
	stream_selected = true;
}

static uint8_t stream_xfer(void *ctx, uint8_t tx)
{
	const bool paced = stream_ops.send != NULL;	/* by the controller: no window */
	const uint64_t at = wb_core_cycles();
	const uint8_t rx = paced ? stream_ops.xfer(ctx, tx) : opxy_boot_byte(ctx, tx);
	uint64_t since;

	stream_byte_end = wb_core_cycles();
	boot_time_xfer(&opxy_boot_time, stream_byte_end - at);
	SPLIT.reads++;
	if (!stream_begun) {
		stream_begun = true;
		stream_open = rx == 0xFFu;
		boot_time_mark(&opxy_boot_time, BOOT_TIME_STREAM_OPENED, stream_byte_end);
	} else {
		stream_sent++;
		if (stream_open) {
			boot_time_walk_byte(&opxy_boot_time, &stream_walk, tx);
			if (SPLIT.bytes == 16u)
				boot_time_mark(&opxy_boot_time, BOOT_TIME_STREAM_HEADER,
					       stream_byte_end);
		}
	}
	stream_first = stream_open;
	stream_waiting = false;
	stream_watch.active = false;
	if (paced)
		return rx;
	if (stream_open)
		boot_time_watch_byte(&stream_watch, stream_sent);
	do {
		const bool low = stream_ops.ready(ctx);

		since = wb_core_cycles() - stream_byte_end;
		boot_time_watch_sample(&opxy_boot_time, &stream_watch, !low, since, true);
	} while (since < STREAM_WINDOW_CYCLES);
	SPLIT.window += since;
	return rx;
}

/* A byte's cycles at the link's clock. */
#define STREAM_WIRE_BYTE \
	(8u * (opxy_spi_boot_link.clk + 1u) * (OPXY_CCLK_MHZ * 1000000u / OPXY_SCLK0_HZ))

#if OPXY_BOOT_LINK_FC
/* Paced by the controller, a byte is not seen: the line at a session's
 * start, the session's whole time, and its time beyond its bytes' own at
 * the link's rate, which is where the slave held them off. */
static bool stream_send(void *ctx, const uint8_t *d, size_t n)
{
	const uint64_t wire = (uint64_t)n * STREAM_WIRE_BYTE;
	const uint64_t at = wb_core_cycles();
	const uint32_t before = SPLIT.bytes;
	/* fed by DMA, the guards are the stream's own pauses, not the slave's */
	const uint32_t guard_was = opxy_boot_fast()->st.guard_us;
	uint64_t from, took, guard;
	bool sent;

	if (!stream_ops.ready(ctx))
		opxy_boot_time.stream_high++;
	from = wb_core_cycles();
	sent = stream_ops.send(ctx, d, n);
	stream_byte_end = wb_core_cycles();
	took = stream_byte_end - from;
	guard = (uint64_t)(opxy_boot_fast()->st.guard_us - guard_was) * OPXY_CCLK_MHZ;
	took = took > guard ? took - guard : 0u;
	SPLIT.sample += from - at;
	SPLIT.send += took;
	SPLIT.reads++;
	if (took > SPLIT.send_max)
		SPLIT.send_max = took > UINT32_MAX ? UINT32_MAX : (uint32_t)took;
	if (sent)
		boot_time_walk(&opxy_boot_time, &stream_walk, d, (uint32_t)n);
	if (took > wire) {
		opxy_boot_time.stream_wait += took - wire;
		boot_time_gap(&opxy_boot_time, took - wire, before);
	}
	if (before < 16u && SPLIT.bytes >= 16u)
		boot_time_mark(&opxy_boot_time, BOOT_TIME_STREAM_HEADER, stream_byte_end);
	opxy_boot_time.flags |= BOOT_TIME_LINK_FC;
	return sent;
}
#endif

void opxy_timing_stream(struct s09_boot_ops *ops)
{
	unsigned i;

	stream_ops = *ops;
	opxy_boot_time.line_window = (uint32_t)STREAM_WINDOW_CYCLES;
	SPLIT.xfer_min = UINT32_MAX;
	SPLIT.wire_byte = STREAM_WIRE_BYTE;
	/* a counter read's cost: the least of a few pairs back to back */
	SPLIT.read = UINT32_MAX;
	for (i = 0; i < 8u; i++) {
		const uint64_t a = wb_core_cycles();
		const uint64_t b = wb_core_cycles();

		if (b - a < SPLIT.read)
			SPLIT.read = (uint32_t)(b - a);
	}
	ops->ready = stream_ready;
	ops->select = stream_select;
	ops->xfer = stream_xfer;
#if OPXY_BOOT_LINK_FC
	if (ops->send != NULL)
		ops->send = stream_send;
#endif
}

void opxy_timing_stream_end(void)
{
#if OPXY_BOOT_LINK_FC
	const struct s09_fast_stats *f = &opxy_boot_fast()->st;
	struct opxy_boot_fcs fcs;

	if (opxy_boot_way() == 2u && f->sessions != 0u) {
		opxy_boot_time.flags |= BOOT_TIME_LINK_DMA;
		opxy_boot_time.dma_parts = f->parts;
		opxy_boot_time.dma_guard = OPXY_BOOT_LINK_GUARD_US;
		opxy_boot_time.dma_guard_us = f->guard_us;
		opxy_boot_time.dma_high = f->high;
		opxy_boot_time.dma_fell = f->fell;
		opxy_boot_time.dma_fell_session = f->fell_session;
		opxy_boot_time.dma_fell_at = f->fell_at;
		opxy_boot_time.dma_slow_bytes = f->slow_bytes;
		opxy_boot_time.dma_held = f->held ? 1u : 0u;
		opxy_boot_fcs_stats(&fcs);
		opxy_boot_time.dma_fcs_samples = fcs.samples;
		opxy_boot_time.dma_fcs_held = fcs.held;
		opxy_boot_time.dma_fcs_parts = fcs.parts;
		opxy_boot_time.dma_fcs_first = fcs.first;
	}
#endif
	if (stream_begun)
		boot_time_mark(&opxy_boot_time, BOOT_TIME_STREAM_LAST, stream_byte_end);
	OPXY_TIME(BOOT_TIME_STREAM_END);
}

static bool slot_was_fast;

void opxy_timing_slot_read(const struct s02_image *im)
{
	slot_was_fast = im->by_fast;
	if (im->fast == NULL || im->len == 0u)
		return;
	if (im->by_fast)
		opxy_boot_time.slot_fast++;
	else
		opxy_boot_time.slot_fast_failed++;
}

void opxy_timing_slot_check(const struct s02_image *im)
{
	if (opxy_crc_fast_used)
		opxy_boot_time.flags |= BOOT_TIME_SLOT_CRC_HW;
	if (slot_was_fast && im->by_fast && im->trial)
		opxy_boot_time.slot_trial++;
	if (!slot_was_fast || im->by_fast)
		return;
	opxy_boot_time.slot_reread++;
	if (im->good)
		opxy_boot_time.slot_reread_good++;
}

void opxy_timing_battery(bool on)
{
	OPXY_TIME(on ? BOOT_TIME_BATTERY_START : BOOT_TIME_BATTERY_END);
	opxy_boot_time.flags |= BOOT_TIME_BATTERY_RAN;
}

/* The copy's CRC (S-03.044), its end stamped: the peripheral's first, then
 * the software's when that one missed. */
static s03_crc32_fn copy_crc_of, copy_fast_of;
static bool copy_fast_ran;

static uint32_t copy_crc(uint32_t crc, const uint8_t *buf, uint32_t len)
{
	const uint32_t c = copy_crc_of(crc, buf, len);

	if (copy_fast_ran)
		opxy_boot_time.flags |= BOOT_TIME_COPY_CRC_AGAIN;
	OPXY_TIME(BOOT_TIME_COPY_CRC);
	return c;
}

static uint32_t copy_fast(uint32_t crc, const uint8_t *buf, uint32_t len)
{
	const uint32_t c = copy_fast_of(crc, buf, len);

	copy_fast_ran = true;
	if (opxy_crc_fast_used)
		opxy_boot_time.flags |= BOOT_TIME_COPY_CRC_HW;
	OPXY_TIME(BOOT_TIME_COPY_CRC);
	return c;
}

void opxy_timing_copy(struct opxy_master *m, bool on)
{
	static uint32_t fell;

	copying = on;
	if (on) {
		opxy_boot_time.bytes = 0u;
		opxy_boot_time.copy_link = m->bridge.rung | (uint32_t)m->link_ceiling << 16;
		opxy_boot_time.flags |= BOOT_TIME_COPY_LINK;
		fell = m->bridge.fell;
		copy_crc_of = m->emmc.crc32;
		m->emmc.crc32 = copy_crc;
		copy_fast_ran = false;
		copy_fast_of = m->emmc.crc32_fast;
		if (copy_fast_of != NULL)
			m->emmc.crc32_fast = copy_fast;
		OPXY_TIME(BOOT_TIME_COPY_START);
		return;
	}
	OPXY_TIME(BOOT_TIME_STAGED);
	m->emmc.crc32 = copy_crc_of;
	m->emmc.crc32_fast = copy_fast_of;
	opxy_boot_time.copy_link = (opxy_boot_time.copy_link & ~0xFF00u) |
				   (uint32_t)m->bridge.rung << 8;
	opxy_boot_time.copy_fell = m->bridge.fell - fell;
	{
		const struct s09_qdata_rec *q = s09_master_qdata(&m->bridge);

		opxy_boot_time.copy_quad = q->replies;
		opxy_boot_time.copy_qbytes = q->bytes;
		opxy_boot_time.copy_qstate = q->state | (uint32_t)q->why << 8 |
					     (uint32_t)q->test_first << 16;
		opxy_boot_time.copy_qtest_us = q->test_us;
	}
}

void opxy_timing_reread(void)
{
	opxy_boot_time.flags |= BOOT_TIME_COPY_REREAD;
}

void opxy_timing_pa6(bool start, bool high)
{
	static bool low_seen, high_seen;

	if (start) {
		low_seen = false;
		high_seen = false;
		opxy_boot_time.flags |= BOOT_TIME_PA6_POLLED;
	}
	if (!high && !low_seen) {
		low_seen = true;
		OPXY_TIME(BOOT_TIME_SERVER_STOP);
	} else if (high && low_seen && !high_seen) {
		high_seen = true;
		OPXY_TIME(BOOT_TIME_SLAVE_OS);
	}
}

void opxy_timing_read(uint32_t bytes)
{
	if (!copying)
		return;
	opxy_boot_time.bytes += bytes;
	OPXY_TIME(BOOT_TIME_COPY_LAST);
}

void opxy_timing_leave(void)
{
	uint8_t *rec = (uint8_t *)(uintptr_t)OPXY_BOOT_TIME_RECORD;

	boot_time_pack(&opxy_boot_time, opxy_time.cycles_per_us, rec);
	wb_dcache_flush_range(OPXY_BOOT_TIME_RECORD, BOOT_TIME_RECORD);
}

#define LINE_GAP_US   100000u
#define REPORT_GAP_US 2000000u

void opxy_timing_held(struct opxy_master *m)
{
	static unsigned k;
	static uint64_t next;
	struct s14_text t;

	if (next != 0u && !wb_time_passed(next))
		return;
	while (k < BOOT_TIME_LINES && !boot_time_line(&opxy_boot_time, k, opxy_time.cycles_per_us, &t))
		k++;
	if (k < BOOT_TIME_LINES) {
		s14_log_text(&m->sv.log, S14_INFO, S14_TAG("time"), &t);
		k++;
	}
	if (k < BOOT_TIME_LINES) {
		next = wb_time_deadline(&opxy_time, LINE_GAP_US);
	} else {
		k = 0;
		next = wb_time_deadline(&opxy_time, REPORT_GAP_US);
	}
}

#elif defined(OPXY_BOOT_TIMING_ON) && defined(OPXY_TEST_OS)
#include "waffle/boot_time.h"
#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_cplb.h"
#include "waffle/testos.h"

static uint8_t raw[BOOT_TIME_RECORD];
static uint64_t entry_cycles;

/* At entry, before anything can reuse the memory: the record, then its
 * magic cleared, so a later boot under another bootloader cannot find it. */
void opxy_testos_timing_capture(void)
{
	volatile uint8_t *rec = (volatile uint8_t *)(uintptr_t)OPXY_BOOT_TIME_RECORD;
	unsigned i;

	entry_cycles = wb_core_cycles();
	wb_dcache_invalidate_range(OPXY_BOOT_TIME_RECORD, BOOT_TIME_RECORD);
	for (i = 0; i < BOOT_TIME_RECORD; i++)
		raw[i] = rec[i];
	for (i = 0; i < 4u; i++)
		rec[i] = 0u;
	wb_dcache_flush_range(OPXY_BOOT_TIME_RECORD, BOOT_TIME_RECORD);
}

void opxy_testos_timing_log(struct opxy_master *m)
{
	struct boot_time bt;
	struct s14_text t;
	uint32_t cpu;
	unsigned k;

	s14_text_init(&t);
	if (!boot_time_unpack(raw, &bt, &cpu)) {
		s14_text_str(&t, "time: no record from the loading bootloader");
		opxy_testos_emit(m, &t);
		return;
	}
	/* The counter runs on from the bootloader's; a count below the last
	 * stamp means the record was left by an earlier boot. */
	if ((bt.set >> BOOT_TIME_RELEASED & 1u) && entry_cycles < bt.at[BOOT_TIME_RELEASED]) {
		s14_text_str(&t, "time: the record is from an earlier boot");
		opxy_testos_emit(m, &t);
		return;
	}
	boot_time_mark(&bt, BOOT_TIME_OS_ENTRY, entry_cycles);
	for (k = 0; k < BOOT_TIME_LINES; k++) {
		struct s14_text line;

		if (!boot_time_line(&bt, k, cpu, &line))
			continue;
		s14_text_init(&t);
		s14_text_str(&t, "time ");
		s14_text_bytes(&t, line.buf, line.len);
		opxy_testos_emit(m, &t);
	}
}
#else
typedef int opxy_timing_absent;	/* a translation unit is not empty */
#endif
