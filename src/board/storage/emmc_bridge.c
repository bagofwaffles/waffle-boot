/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Storage across the bridge (see waffle/board/emmc_bridge.h). */
#include "waffle/board/emmc_bridge.h"

#include "waffle/board/boot.h"

#include "waffle/util/bytes.h"
#include "waffle/host/devhost.h"
#include "waffle/drivers/bf70x/spi.h"
#include "waffle/drivers/bf70x/time.h"
#include "waffle/bridge/records.h"

/* S-09.023: the most sectors one 0x12, 0x13 or 0x16 request moves. */
#define MAX_SECTORS 128u

/* EXT_CSD[32] FLUSH_CACHE (JESD84-B51 7.4.112). */
#define EXT_CSD_FLUSH_CACHE 32u

/* CMD38's plain erase argument (JESD84-B51). */
#define CMD38_ERASE 0u

static int32_t call(struct s09_master *m, uint8_t cmd, uint32_t arg0,
		    uint32_t arg1, const uint8_t *payload, uint32_t plen,
		    uint8_t *data, uint32_t max, uint32_t *got)
{
	struct s09_req r;
	uint32_t err;

	r.cmd = cmd;
	r.slot = 0;
	r.len = (uint16_t)(plen & 0xFFFFu);	/* S-09.023: mod 2^16 */
	r.arg0 = arg0;
	r.arg1 = arg1;
	return s09_master_call(m, &r, payload, plen, data, max, got, &err);
}

static int bridge_select(void *ctx, unsigned part)
{
	return call(ctx, S09_SELECT, part, 0, NULL, 0, NULL, 0, NULL) == 0 ?
		       0 : -1;
}

static int bridge_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	while (count > 0u) {
		uint32_t n = count < MAX_SECTORS ? count : MAX_SECTORS;
		uint32_t bytes = n * S03_SECTOR, got = 0;
		int32_t rc;

		if (n == 1u)
			rc = call(ctx, S09_READ_SECTOR, sector, 0, NULL, 0, buf,
				  bytes, &got);
		else
			rc = call(ctx, S09_READ_SECTORS, sector, n, NULL, 0,
				  buf, bytes, &got);
		if (rc != 0 || got != bytes)
			return -1;
		OPXY_TIMING_READ(bytes);
		sector += n;
		count -= n;
		buf += bytes;
	}
	return 0;
}

static int bridge_write(void *ctx, uint32_t sector, uint32_t count,
			const uint8_t *buf)
{
	while (count > 0u) {
		uint32_t n = count < MAX_SECTORS ? count : MAX_SECTORS;
		uint32_t bytes = n * S03_SECTOR;
		int32_t rc;

		if (n == 1u)
			rc = call(ctx, S09_WRITE_SECTOR, sector, 0, buf, bytes,
				  NULL, 0, NULL);
		else
			rc = call(ctx, S09_WRITE_SECTORS, sector, n, buf, bytes,
				  NULL, 0, NULL);
		if (rc != 0)
			return -1;
		sector += n;
		count -= n;
		buf += bytes;
	}
	return 0;
}

/* S-09.040: every 0x14 a master sends carries CMD38 argument 0, the
 * erase, the one that removes S-03.004's whole group. */
static int bridge_erase(void *ctx, uint32_t first_sector)
{
	return call(ctx, S09_ERASE, first_sector, CMD38_ERASE, NULL, 0, NULL,
		    0, NULL) == 0 ? 0 : -1;
}

/* The flush is the one EXT_CSD byte a bridge request sets (S-09.040,
 * S-03.068): the slave owns its eMMC's features (S-03.009). */
static int bridge_switch(void *ctx, uint8_t index, uint8_t value)
{
	if (index != EXT_CSD_FLUSH_CACHE || value != 1u)
		return -1;
	return call(ctx, S09_FLUSH, 0, 0, NULL, 0, NULL, 0, NULL) == 0 ? 0 : -1;
}

struct s03_emmc_ops opxy_bridge_emmc(struct s09_master *m)
{
	const struct s03_emmc_ops ops = {
		m, bridge_select, bridge_read, bridge_write, bridge_erase, bridge_switch,
	};

	return ops;
}

uint32_t opxy_bridge_boot_size(struct s09_master *m)
{
	int32_t v = call(m, S09_BOOT_SIZE, 0, 0, NULL, 0, NULL, 0, NULL);

	return v > 0 ? (uint32_t)v : 0u;
}

int opxy_bridge_release_file(struct s09_master *m, const uint8_t *copy, uint32_t len,
			     uint32_t *crc, bool peripheral, s03_crc32_fn soft)
{
	const int v = s09_master_release_file(m, len, *crc);
	uint32_t again;

	if (v != 1 || !peripheral)
		return v;
	again = soft(0u, copy, len);
	if (again == *crc)
		return v;	/* the slave's copy differs: no second try */
	*crc = again;
	return s09_master_release_file(m, len, again);
}

/* PA6 is read once more after the deadline has passed. */
bool opxy_bridge_init_ready(const struct s09_bus_ops *bus, uint64_t deadline)
{
	bool late;

	do {
		late = wb_time_passed(deadline);
		if (bus->ready(bus->ctx)) {
			OPXY_TIME(BOOT_TIME_SLAVE_READY);
			return true;
		}
	} while (!late);
	return false;
}

int32_t opxy_slave_emmc(const struct s03_emmc_ops *emmc,
			const struct opxy_emmc_more *more, uint8_t cmd,
			uint32_t arg0, uint32_t arg1, uint8_t *data,
			uint32_t n)
{
	uint32_t count = 1;

	switch (cmd) {
	case S09_FLUSH:
		/* S-03.011: S-09.040's flush is S-03's,
		 * EXT_CSD[32] FLUSH_CACHE <- 1. */
		return emmc->switch_byte(emmc->ctx, EXT_CSD_FLUSH_CACHE, 1u);
	case S09_SELECT:
		/* S-03.006: the user area, BP1 or BP2; no eMMC command else */
		if (arg0 > S03_PART_BP2)
			return -1;
		return emmc->select(emmc->ctx, arg0);
	case S09_ERASE:
		/* S-09.040: an argument other than 0 sends no eMMC command
		 * and answers -1 (errno 0, the eMMC left as it was). */
		if (arg1 != CMD38_ERASE)
			return -1;
		return more->erase_arg(emmc->ctx, arg0, arg1);
	case S09_READ_SECTORS:
	case S09_WRITE_SECTORS:
	case S09_WRITE_RELIABLE:
		count = arg1;
		break;
	case S09_READ_SECTOR:
	case S09_WRITE_SECTOR:
		break;
	default:
		return -1;
	}
	if (count == 0u || count > MAX_SECTORS || n < count * S03_SECTOR)
		return -1;
	if (cmd == S09_READ_SECTOR || cmd == S09_READ_SECTORS)
		return emmc->read(emmc->ctx, arg0, count, data);
	if (cmd == S09_WRITE_RELIABLE)
		return more->write_reliable(emmc->ctx, arg0, count, data);
	return emmc->write(emmc->ctx, arg0, count, data);
}

/* --- S-09.075: the link's rung, and its record in plain words --- */

static const char *const rung_wire[] = {
	[S09_RUNG_PLAIN] = "single I/O at 25 MHz, plain",
	[S09_RUNG_CHECKED] = "single I/O at 25 MHz, checked",
	[S09_RUNG_CHECKED_FAST] = "single I/O at 50 MHz, checked",
	[S09_RUNG_QUAD] = "quad I/O at 25 MHz, checked",
	[S09_RUNG_QUAD_FAST] = "quad I/O at 50 MHz, checked",
};

static const char *const at_text[S09_AT_COUNT] = {
	[S09_AT_NONE] = "no fault",
	[S09_AT_SEND] = "the send failed",
	[S09_AT_READY] = "the ready line never came",
	[S09_AT_REPLY_START] = "the reply never started",
	[S09_AT_REPLY_BROKE] = "the reply broke off",
	[S09_AT_FIRST_WORD] = "the reply's first word was wrong",
	[S09_AT_CHECK] = "the reply's check word was wrong",
	[S09_AT_EXEC] = "no reply within T_EXEC",
	[S09_AT_SLAVE] = "the slave's line said it fell back",
	[S09_AT_VALUE] = "the reply's value was wrong",
};

static const char *const why_text[S09_WHY_COUNT] = {
	[S09_WHY_NONE] = "no fall-back yet",
	[S09_WHY_CHECK] = "a check word wrong",
	[S09_WHY_SHORT] = "a session shorter than its request",
	[S09_WHY_IGNORED] = "a header it ignores",
	[S09_WHY_RECEIVE] = "its receive failed",
	[S09_WHY_TRIAL] = "its trial ran out",
	[S09_WHY_NUMBER] = "a number or check in conflict",
	[S09_WHY_ON_TRIAL] = "a request other than a test on trial",
	[S09_WHY_KAT] = "its check failed its known answers",
	[S09_WHY_REPLY_END] = "a reply it saw no end of",
};

/* The tests' patterns, in bridge/master.c's order. */
static const char *const pat_text[] = {
	"0x00", "0xFF", "0xAA", "0x55", "0x0F", "0xF0", "walking one", "walking zero",
	"count", "random",
};

/* A rung, with its wire when wire is set; a fall-back's "to" may be none. */
static void t_rung(struct s14_text *t, unsigned r, bool wire)
{
	if (r > S09_RUNG_TOP) {
		s14_text_str(t, "nowhere (the slave is absent)");
		return;
	}
	s14_text_str(t, "rung ");
	s14_text_dec(t, r);
	if (r == S09_RUNG_PLAIN)
		s14_text_str(t, " (plain)");
	if (!wire)
		return;
	s14_text_str(t, " (");
	s14_text_str(t, rung_wire[r]);
	s14_text_str(t, ")");
}

/* The SPI driver's stages (WB_SPI_STAGE_..., waffle/drivers/bf70x/spi.h), short. */
static const char *const stage_text[] = {
	"none", "arg", "tx room", "tx end", "rx word", "dma start", "dma stall",
	"dma err", "overrun", "master ready", "first word", "next word",
	"master end", "tx err", "reply bound", "refused",
};

/* The driver's own account of a failed send or receive. */
static void t_words(struct s14_text *t, const uint32_t w[4])
{
	const uint32_t stage = w[3] & WB_SPI_STAGE_MASK;

	s14_text_str(t, " [");
	s14_text_str(t, stage < sizeof stage_text / sizeof stage_text[0] ?
			stage_text[stage] : "stage ?");
	if (stage == WB_SPI_STAGE_DMA_ERR) {
		s14_text_str(t, " errc ");
		s14_text_dec(t, w[3] >> WB_SPI_STAGE_ERRC_SHIFT & 7u);
	}
	s14_text_str(t, " rc 0x");
	s14_text_hex(t, w[0], 1);
	s14_text_str(t, " st 0x");
	s14_text_hex(t, w[1], 1);
	s14_text_str(t, " left ");
	s14_text_dec(t, w[2]);
	s14_text_str(t, "]");
}

static void t_fault(struct s14_text *t, const struct s09_fault *f)
{
	s14_text_str(t, f->at < S09_AT_COUNT ? at_text[f->at] : "?");
	if (f->at != S09_AT_SEND && f->at != S09_AT_REPLY_BROKE)
		return;
	t_words(t, f->words);
	if ((f->more[0] | f->more[1] | f->more[2] | f->more[3]) == 0u)
		return;
	/* the transmit DMA channel's status and set-up, SPI_TXCTL, SPI_TWC */
	s14_text_str(t, " [dma 0x");
	s14_text_hex(t, f->more[0], 1);
	s14_text_str(t, " cfg 0x");
	s14_text_hex(t, f->more[1], 1);
	s14_text_str(t, " txctl 0x");
	s14_text_hex(t, f->more[2], 1);
	s14_text_str(t, " twc ");
	s14_text_dec(t, f->more[3]);
	s14_text_str(t, "]");
}

static void t_status(struct s14_text *t, const struct s09_link_status *st)
{
	if (!st->told) {
		s14_text_str(t, "; the slave did not say");
		return;
	}
	s14_text_str(t, "; the slave: ");
	s14_text_str(t, st->why < S09_WHY_COUNT ? why_text[st->why] : "?");
	if (st->why != S09_WHY_NONE) {
		s14_text_str(t, " on 0x");
		s14_text_hex(t, st->cmd, 2);
		s14_text_str(t, " of ");
		s14_text_dec(t, st->len);
		s14_text_str(t, " bytes, ");
		s14_text_dec(t, st->got);
		s14_text_str(t, " in");
	}
	if (st->why == S09_WHY_RECEIVE)
		t_words(t, st->words);
	s14_text_str(t, ", its fall-backs ");
	s14_text_dec(t, st->fell);
}

static void log_rung(struct opxy_master *m, const struct s09_rec_rung *e)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "bridge: tried ");
	t_rung(&t, e->rung, true);
	s14_text_str(&t, ": ");
	if (e->result == S09_RR_TEST) {
		s14_text_str(&t, "the test of ");
		s14_text_dec(&t, e->len);
		s14_text_str(&t, " bytes (");
		s14_text_str(&t, e->pat < sizeof pat_text / sizeof pat_text[0] ?
			     pat_text[e->pat] : "?");
		s14_text_str(&t, ") failed: ");
		t_fault(&t, &e->fault);
	} else if (e->result == S09_RR_ASK || e->result == S09_RR_KEEP) {
		s14_text_str(&t, e->result == S09_RR_ASK ? "0x19 on trial failed: " :
							   "0x19 keep failed: ");
		t_fault(&t, &e->fault);
	} else if (e->result == S09_RR_CHECK) {
		s14_text_str(&t, "the check failed its known answers");
	} else {
		s14_text_str(&t, "tests passed");
	}
	if (e->plain_us != 0u) {
		s14_text_str(&t, "; 64 KiB sessions ");
		for (unsigned k = 0; k < S09_GATE_TIMES; k++) {
			if (k)
				s14_text_str(&t, "/");
			s14_text_dec(&t, e->big_us[k]);
		}
		s14_text_str(&t, " us, plain wire ");
		s14_text_dec(&t, e->plain_us);
		s14_text_str(&t, " us");
	}
	s14_text_str(&t, e->result == S09_RR_KEPT ? ": kept" :
			 e->result == S09_RR_GATE ? ": left, no faster than plain" :
			 ": failed");
	if (e->st.told)
		t_status(&t, &e->st);
	s14_log_text(&m->sv.log, e->result == S09_RR_KEPT || e->result == S09_RR_GATE ?
				 S14_INFO : S14_WARNING,
		     S14_NO_TAG, &t);
}

static void log_fall(struct opxy_master *m, const struct s09_rec_fall *f)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "bridge: request 0x");
	s14_text_hex(&t, f->cmd, 2);
	s14_text_str(&t, " of ");
	s14_text_dec(&t, f->len);
	s14_text_str(&t, f->tries != 0u ? " bytes (sent again) fell back from " :
					  " bytes (its first send) fell back from ");
	t_rung(&t, f->from, false);
	s14_text_str(&t, " to ");
	t_rung(&t, f->to, false);
	s14_text_str(&t, ": ");
	t_fault(&t, &f->fault);
	t_status(&t, &f->st);
	s14_log_text(&m->sv.log, S14_ERROR, S14_NO_TAG, &t);
}

void opxy_bridge_end_note(struct opxy_master *m)
{
	if (s09_master_end_changed(&m->bridge))
		s14_log_str(&m->sv.log, S14_WARNING, S14_NO_TAG,
			    "bridge: the slave saw no end to a reply; it now ends its "
			    "single-I/O replies by their word count for the rest of the run");
}

/* What the record holds that the log has not had yet: the last
 * negotiation's rungs, then the fall-backs since. */
static void log_record(struct opxy_master *m)
{
	struct s09_record *r = &m->bridge.rec;

	if (r->negotiations != r->shown_negotiations) {
		r->shown_negotiations = r->negotiations;
		for (unsigned i = 0; i < r->nrungs; i++)
			log_rung(m, &r->rung[i]);
	}
	if (r->falls - r->shown_falls > S09_REC_FALLS)
		r->shown_falls = r->falls - S09_REC_FALLS;	/* the oldest are gone */
	for (; r->shown_falls != r->falls; r->shown_falls++)
		log_fall(m, &r->fall[r->shown_falls % S09_REC_FALLS]);
	opxy_bridge_end_note(m);
}

/* "requests ran in" (or "run in", now) the rung. */
static void log_way(struct opxy_master *m, const char *verb)
{
	const struct s09_master *b = &m->bridge;
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "bridge: requests ");
	s14_text_str(&t, verb);
	s14_text_str(&t, " in ");
	s14_text_str(&t, rung_wire[s09_master_rung(b)]);
	s14_log_text(&m->sv.log, S14_INFO, S14_NO_TAG, &t);
}

const char *opxy_bridge_end_line(unsigned end)
{
	return end == WB_SPI_END_COUNT ?
		       "bridge: the slave's replies end at their word count (the count end)" :
		       "bridge: the slave's replies end on TF";
}

/* A link to a slave there is: with second-cpu alone; the OP-1F's client
 * reaches its own storage, with no link to raise, lower or report. */
static bool linked(const struct opxy_master *m)
{
	return m->slave_present && wb_has(opxy_dev(m), WB_F_SECOND_CPU);
}

void opxy_bridge_report(struct opxy_master *m)
{
	if (linked(m))
		log_record(m);
}

void opxy_bridge_fast(struct opxy_master *m)
{
	if (!linked(m))
		return;
	(void)s09_master_link_fast(&m->bridge, m->link_ceiling);
	m->sv.files.link = m->link_ceiling;
	if (s09_master_check_bad(&m->bridge))	/* S-09.072 */
		s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG,
			    "bridge: the check failed its own test; requests stay plain");
	log_record(m);
	log_way(m, "run");
}

void opxy_bridge_plain(struct opxy_master *m)
{
	bool lowered;

	m->sv.files.link = 0;
	if (!linked(m))
		return;
	log_record(m);
	lowered = s09_master_rung(&m->bridge) != S09_RUNG_PLAIN;
	if (lowered)
		log_way(m, "ran");
	(void)s09_master_link_plain(&m->bridge);
	log_record(m);
	if (lowered)
		s14_log_str(&m->sv.log, S14_INFO, S14_NO_TAG,
			    "bridge: the link was lowered to plain when it ended");
	if (!s09_master_settled(&m->bridge))	/* S-09.074 */
		s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG,
			    "bridge: the slave could not be told the link is plain; "
			    "restart the unit");
}

/* --- S-09.081: the OS image's copy with quad data replies --- */

static const char *const qd_why_text[S09_QW_COUNT] = {
	[S09_QW_NONE] = "",
	[S09_QW_NO_RECV] = "the bus has no quad receive",
	[S09_QW_TEST] = "the quad reply test failed",
	[S09_QW_NO_RUNG] = "the link is on no checked rung",
	[S09_QW_REFUSED] = "the slave did not take mode 3",
	[S09_QW_LOST] = "the mode's exchange did not get through",
	[S09_QW_DATA] = "a data session broke",
	[S09_QW_DATA_CHECK] = "a data session's check word was wrong",
	[S09_QW_FELL] = "another request fell back",
	[S09_QW_ASKED] = "the copy is read again",
	[S09_QW_STRUCK] = "dropped earlier",
};

const char *opxy_bridge_qdata_why(unsigned why)
{
	return why < S09_QW_COUNT ? qd_why_text[why] : "?";
}

void opxy_bridge_copy_link(struct opxy_master *m, bool on)
{
	const struct s09_qdata_rec *q = s09_master_qdata(&m->bridge);
	struct s14_text t;

	if (!linked(m))
		return;
	if (on) {
		(void)s09_master_link_qdata(&m->bridge, m->link_ceiling);
		return;
	}
	log_record(m);
	s14_text_init(&t);
	s14_text_str(&t, "bridge: the os copy's sector data ");
	if (q->state == S09_QD_OFF) {
		s14_text_str(&t, "went in single I/O");
	} else if (q->state == S09_QD_NOT) {
		s14_text_str(&t, "went in single I/O: ");
		s14_text_str(&t, opxy_bridge_qdata_why(q->why));
	} else {
		s14_text_str(&t, "went in quad I/O at 50 MHz, ");
		s14_text_dec(&t, q->replies);
		s14_text_str(&t, " sessions, ");
		s14_text_dec(&t, q->bytes);
		s14_text_str(&t, " bytes");
		if (q->state == S09_QD_DROPPED) {
			s14_text_str(&t, "; then single I/O: ");
			s14_text_str(&t, opxy_bridge_qdata_why(q->why));
		}
	}
	s14_log_text(&m->sv.log, q->state == S09_QD_DROPPED && q->why != S09_QW_ASKED ?
				 S14_WARNING : S14_INFO, S14_NO_TAG, &t);
	opxy_bridge_plain(m);
}

void opxy_bridge_copy_single(struct opxy_master *m)
{
	if (linked(m))
		(void)s09_master_qdata_off(&m->bridge, m->link_ceiling);
}

bool opxy_bridge_load_os(struct opxy_master *m, uint8_t *staging, uint32_t cap,
			 struct s03_load *out)
{
	s03_load_os(&m->emmc, staging, cap, out);
	if (out->result != S03_LOAD_CRC || s09_master_qdata(&m->bridge)->replies == 0u)
		return false;
	opxy_bridge_copy_single(m);
	s03_load_os(&m->emmc, staging, cap, out);
	return true;
}

/* --- the record for a developer host (waffle/host/devhost.h) --- */

static size_t put_u32(uint8_t *p, uint32_t v)
{
	wb_put_le32(p, v);
	return 4u;
}

static size_t put_fault(uint8_t *p, const struct s09_fault *f)
{
	size_t o = 0;

	p[o++] = f->at;
	for (unsigned i = 0; i < 4u; i++)
		o += put_u32(p + o, f->words[i]);
	return o;
}

/* ... with the driver's second four words */
static size_t put_fault_more(uint8_t *p, const struct s09_fault *f)
{
	size_t o = put_fault(p, f);

	for (unsigned i = 0; i < 4u; i++)
		o += put_u32(p + o, f->more[i]);
	return o;
}

static size_t put_status(uint8_t *p, const struct s09_link_status *st)
{
	size_t o = 0;

	p[o++] = st->told;
	o += put_u32(p + o, st->fell);
	p[o++] = st->why;
	p[o++] = st->cmd;
	p[o++] = st->mode;
	o += put_u32(p + o, st->len);
	o += put_u32(p + o, st->got);
	for (unsigned i = 0; i < 4u; i++)
		o += put_u32(p + o, st->words[i]);
	return o;
}

_Static_assert(OPXY_RECORD_RUNG_PARTS * OPXY_RECORD_RUNGS_PER >= S09_REC_RUNGS &&
	       OPXY_RECORD_FALL_PARTS * OPXY_RECORD_FALLS_PER >= S09_REC_FALLS &&
	       (OPXY_RECORD_PARTS - 1u - OPXY_RECORD_RUNG_PARTS - OPXY_RECORD_FALL_PARTS) *
	       OPXY_RECORD_BENCH_PER >= S09_REC_BENCH &&
	       1u + 3u + OPXY_RECORD_BENCH_PER * 114u <= S06_MAX_PAYLOAD &&
	       1u + 3u + OPXY_RECORD_FALLS_PER * 73u <= S06_MAX_PAYLOAD,
	       "the record's parts hold it, each within an S-06 payload");

/* The k-th bench fault the record keeps, in the run's order. */
static const struct s09_rec_bench *bench_kept(const struct s09_record *r, uint32_t k)
{
	const uint32_t ring = S09_REC_BENCH - S09_REC_BENCH_FIRST;
	const uint32_t no = r->bench_faults <= S09_REC_BENCH || k < S09_REC_BENCH_FIRST ? k :
			    r->bench_faults - S09_REC_BENCH + k;

	return &r->bench[no < S09_REC_BENCH_FIRST ? no :
			 S09_REC_BENCH_FIRST + (no - S09_REC_BENCH_FIRST) % ring];
}

static size_t put_bench(uint8_t *p, const struct s09_rec_bench *e)
{
	size_t o = 0;

	p[o++] = (uint8_t)e->no;
	p[o++] = (uint8_t)(e->no >> 8);
	p[o++] = e->kind;
	p[o++] = e->rung;
	p[o++] = e->result;
	o += put_u32(p + o, e->len);
	p[o++] = (uint8_t)e->req;
	p[o++] = (uint8_t)(e->req >> 8);
	p[o++] = e->to;
	o += put_u32(p + o, e->fall_us);
	o += put_fault_more(p + o, &e->fault);
	o += put_fault_more(p + o, &e->echo);
	o += put_status(p + o, &e->st);
	return o;
}

size_t opxy_bridge_record(const struct s09_master *b, unsigned part, uint8_t *out)
{
	const struct s09_record *r = s09_master_record(b);
	const unsigned bench_first = 1u + OPXY_RECORD_RUNG_PARTS + OPXY_RECORD_FALL_PARTS;
	size_t o = 3;

	out[0] = (uint8_t)part;
	out[1] = OPXY_RECORD_PARTS;
	out[2] = 0;
	if (part == 0u) {
		o += put_u32(out + o, r->falls);
		o += put_u32(out + o, r->negotiations);
		out[o++] = r->nrungs;
		out[o++] = r->kept;
		out[o++] = (uint8_t)s09_master_rung(b);
		o += put_u32(out + o, s09_master_fell(b));
		out[o++] = s09_master_settled(b);
		o += put_u32(out + o, r->bench_faults);
	} else if (part <= OPXY_RECORD_RUNG_PARTS) {
		for (unsigned i = (part - 1u) * OPXY_RECORD_RUNGS_PER;
		     i < part * OPXY_RECORD_RUNGS_PER && i < r->nrungs; i++) {
			const struct s09_rec_rung *e = &r->rung[i];

			out[2]++;
			out[o++] = e->rung;
			out[o++] = e->result;
			out[o++] = e->pat;
			o += put_u32(out + o, e->len);
			o += put_fault(out + o, &e->fault);
			for (unsigned k = 0; k < S09_GATE_TIMES; k++)
				o += put_u32(out + o, e->big_us[k]);
			o += put_u32(out + o, e->plain_us);
			o += put_status(out + o, &e->st);
		}
	} else if (part >= bench_first && part < OPXY_RECORD_PARTS) {
		/* the last bench run's faults kept, in its order */
		const uint32_t kept = r->bench_faults < S09_REC_BENCH ? r->bench_faults :
				      S09_REC_BENCH;

		for (uint32_t k = (part - bench_first) * OPXY_RECORD_BENCH_PER;
		     k < (part - bench_first + 1u) * OPXY_RECORD_BENCH_PER && k < kept; k++) {
			out[2]++;
			o += put_bench(out + o, bench_kept(r, k));
		}
	} else if (part < bench_first) {
		/* the fall-backs kept, oldest first */
		const uint32_t first = r->falls > S09_REC_FALLS ? r->falls - S09_REC_FALLS : 0u;
		const uint32_t k = part - 1u - OPXY_RECORD_RUNG_PARTS;

		for (uint32_t i = first + k * OPXY_RECORD_FALLS_PER;
		     i < first + (k + 1u) * OPXY_RECORD_FALLS_PER && i < r->falls; i++) {
			const struct s09_rec_fall *f = &r->fall[i % S09_REC_FALLS];

			out[2]++;
			out[o++] = f->from;
			out[o++] = f->to;
			out[o++] = f->cmd;
			out[o++] = f->tries;
			o += put_u32(out + o, f->len);
			o += put_fault_more(out + o, &f->fault);
			o += put_status(out + o, &f->st);
		}
	}
	return o;
}

size_t opxy_bridge_qdata_report(const struct s09_master *b, uint8_t *out)
{
	const struct s09_qdata_rec *q = s09_master_qdata(b);
	size_t o = 0;

	out[o++] = q->state;
	out[o++] = q->why;
	out[o++] = (uint8_t)s09_master_rung(b);
	out[o++] = (uint8_t)q->test_sent;
	out[o++] = (uint8_t)(q->test_sent >> 8);
	out[o++] = (uint8_t)q->test_whole;
	out[o++] = (uint8_t)(q->test_whole >> 8);
	out[o++] = q->test_first;
	o += put_u32(out + o, q->test_len);
	o += put_u32(out + o, q->test_us);
	o += put_u32(out + o, q->replies);
	o += put_u32(out + o, q->bytes);
	o += put_u32(out + o, q->failed);
	o += put_fault(out + o, &q->fault);
	return o;
}
