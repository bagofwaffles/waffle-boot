/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The slave's stream source on the board (src/board/boot/master.c): the two
 * steps the slave start takes, called as it calls them, on a fake flash.
 * A normal start; a trial start (the tried slot runs, is established, and
 * is sent, its trial start: S-09.002); byte-identical slots (established: the slot
 * later starts settle on, with no log line), a trial of the image the kept
 * slot holds among them; a selector that cannot be read (not established:
 * the log line); a slot that runs while the selector names the other; a slot that is not the one read first; and a faster read
 * that returns wrong bytes.
 */
#include <string.h>

#include "s02_check.h"
#include "s02_fake_flash.h"
#include "waffle/board/boot.h"
#include "waffle/boot/handoff.h"
#include "waffle/util/crc.h"

#define MEMB 0x88040000u	/* the image's code and constant data */
#define MEML 0x4000u

static struct opxy_master m;
static struct fake_flash g_f;
static struct s02_flash_ops g_ops;
static uint8_t g_mem[MEML];
static uint8_t g_a[0x2000], g_b[0x2000];
static uint32_t g_alen, g_blen;

/* The memory compares made, and the slots the faster read touched. */
static unsigned g_mem_reads, g_fast_slots;

static bool mem_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	g_mem_reads++;
	if (addr < MEMB || (uint64_t)addr + len > (uint64_t)MEMB + MEML)
		return false;
	memcpy(buf, g_mem + (addr - MEMB), len);
	return true;
}

static const struct s02_mem_ops g_mops = { NULL, mem_read };

/* The faster read: counted; told to, it returns wrong bytes silently. */
static unsigned g_fast;
static bool g_fast_wrong;

static int fast_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	const struct fake_flash *f = ctx;

	g_fast++;
	g_fast_slots |= 1u << (off >= s02_slot_base(1) ? 1u : 0u);
	memcpy(buf, f->mem + off, len);
	if (g_fast_wrong)
		for (uint32_t i = 0x40u; i < len; i += 0x100u)
			buf[i] ^= 0x80u;
	return 0;
}

static const struct opxy_stream_src g_src = {
	&g_mops, { MEMB + 0x40u, 8u }, fast_read, &g_f,
};

/* The log: the lines that say the running slot was not established. */
static unsigned g_warned;

/* And every line's bytes, as they went out. */
static uint8_t g_log[1024];
static size_t g_log_n;

static bool log_send(void *ctx, uint8_t route, uint8_t id, const uint8_t *p, size_t n)
{
	static const char text[] = "running slot not established";

	(void)ctx;
	(void)route;
	(void)id;
	for (size_t i = 0; i + sizeof text - 1u <= n; i++)
		g_warned += memcmp(p + i, text, sizeof text - 1u) == 0 ? 1u : 0u;
	for (size_t i = 0; i < n && g_log_n < sizeof g_log; i++)
		g_log[g_log_n++] = p[i];
	return true;
}

static bool log_has(const char *s)
{
	const size_t n = strlen(s);

	for (size_t i = 0; i + n <= g_log_n; i++)
		if (memcmp(g_log + i, s, n) == 0)
			return true;
	return false;
}

static uint32_t log_now(void *ctx)
{
	(void)ctx;
	return 0;
}

static const struct s14_link_ops log_link = { log_send, NULL };
static const struct s14_clock_ops log_clock = { log_now, NULL };

/* A stream: a block of code, FINAL. */
static uint32_t make_stream(uint8_t *s, uint32_t seed)
{
	static uint8_t code[3000];
	uint8_t x = 0;
	uint32_t o;

	ff_fill_image(code, sizeof code, seed);
	ff_put32(s, 0xAD000000u);
	ff_put32(s + 4, MEMB);
	ff_put32(s + 8, sizeof code);
	ff_put32(s + 12, 0);
	for (unsigned i = 0; i < 16; i++)
		x = (uint8_t)(x ^ s[i]);
	s[2] = x;
	memcpy(s + 16, code, sizeof code);
	o = 16u + (uint32_t)sizeof code;
	ff_put32(s + o, 0xAD000000u | S05_FLAG_FINAL);
	ff_put32(s + o + 4, MEMB);
	ff_put32(s + o + 8, 0);
	ff_put32(s + o + 12, 0);
	x = 0;
	for (unsigned i = 0; i < 16; i++)
		x = (uint8_t)(x ^ s[o + i]);
	s[o + 2] = x;
	return o + 16u;
}

/* crc: 0 good, 1 wrong, 2 the trial mark (S-02.016) */
static void put_slot(unsigned slot, const uint8_t *s, uint32_t len, unsigned crc)
{
	const uint32_t c = ref_crc32(0, s, len);
	struct s02_record r = { crc == 0u ? c : crc == 1u ? c ^ 1u : ~c, len, S02_RECORD_TAG };
	uint8_t raw[S02_RECORD_SIZE];

	memcpy(g_f.mem + s02_slot_base(slot), s, len);
	s02_record_encode(&r, raw);
	memcpy(g_f.mem + s02_record_off(slot), raw, sizeof raw);
}

static void set_up(unsigned selected, const uint8_t *loaded)
{
	ff_init(&g_f);
	g_f.mem[S02_SELECTOR_OFF] = (uint8_t)selected;
	g_f.mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	memset(&m, 0, sizeof m);
	m.flash = ff_env(&g_f, &g_ops);
	s14_log_init(&m.sv.log, S14_MASTER, &log_clock, NULL, &log_link, NULL);
	m.sv.log.cdc_on = true;
	memset(g_mem, 0x5A, sizeof g_mem);
	memcpy(g_mem, loaded + 16, 3000);
	g_warned = 0;
	g_fast = 0;
	g_mem_reads = 0;
	g_fast_slots = 0;
	g_fast_wrong = false;
}

/* A hand-off record's bytes (S-01.023, S-18.005): version 3's layout with
 * the version and size fields given, its CRC at size - 4, and the device
 * OP-1F decided by the table. */
static void ho_bytes(uint8_t rec[FR_HO_SIZE], unsigned version, unsigned size, uint32_t flags,
		     uint8_t run_slot)
{
	memset(rec, 0, FR_HO_SIZE);
	ff_put32(rec, FR_HO_MAGIC);
	rec[4] = (uint8_t)version;
	rec[6] = (uint8_t)size;
	ff_put32(rec + 8, flags);
	rec[45] = 1u;			/* log_slot */
	rec[46] = 0x34u;		/* log_entry */
	rec[47] = 0x12u;
	rec[80] = 0x77u;		/* the first stamp */
	rec[144] = run_slot;
	rec[145] = 9u;			/* te_misses */
	rec[146] = 0x02u;		/* link_overruns */
	rec[147] = 0x01u;
	ff_put32(rec + 148, 1234u);	/* link_gap_max_us */
	rec[152] = FR_HO_DEV_OP1F;
	rec[153] = FR_HO_HOW_TABLE;
	if (size >= 8u && size <= FR_HO_SIZE)
		ff_put32(rec + size - 4u, wire_crc32(0, rec, size - 4u));
}

/* S-18.005: version 3 of 160 bytes accepted, and every other layout
 * refused; the device from offset 152, how from 153. */
static void test_accept(void)
{
	static const struct { unsigned version, size; } refused[] = {
		{ 1, 148 }, { 2, 156 }, { 3, 156 }, { 2, 160 }, { 1, 160 }, { 4, 160 }, { 3, 148 },
	};
	uint8_t rec[FR_HO_SIZE];
	struct fr_handoff ho;
	enum wb_dev_id id;
	enum wb_dev_how how;
	unsigned slot = 9;

	ho_bytes(rec, 3, FR_HO_SIZE, FR_HO_RUNNING | FR_HO_START_LOGGED, 1);
	rec[14] = 1u;			/* role_master */
	ff_put32(rec + FR_HO_SIZE - 4u, wire_crc32(0, rec, FR_HO_SIZE - 4u));
	CHECK(opxy_front_accept(rec, &ho));
	CHECK(ho.version == 3u && ho.size == FR_HO_SIZE && ho.log_slot == 1u &&
	      ho.log_entry == 0x1234u && ho.stamps[0] == 0x77u);
	CHECK(ho.run_slot == 1u && ho.te_misses == 9u && ho.link_overruns == 0x0102u &&
	      ho.link_gap_max_us == 1234u);
	CHECK(ho.device == 1u && ho.device_how == 0u && ho.zero == 0u);
	CHECK(ho.crc32 == wire_crc32(0, rec, FR_HO_SIZE - 4u));
	CHECK(opxy_front_device(&ho, &id, &how) && id == WB_DEV_OP1F && how == WB_DEV_BY_TABLE);
	CHECK(opxy_front_running(&ho, &slot) && slot == 1u);
	/* versions 1 and 2, and version 3 at another size, each CRC right for
	 * its size */
	for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
		ho_bytes(rec, refused[i].version, refused[i].size, 0, 0);
		CHECK(!opxy_front_accept(rec, &ho));
	}
	/* a wrong CRC, a wrong magic, and any one byte changed */
	ho_bytes(rec, 3, FR_HO_SIZE, 0, 0);
	rec[FR_HO_SIZE - 1u] ^= 0x01u;
	CHECK(!opxy_front_accept(rec, &ho));
	ho_bytes(rec, 3, FR_HO_SIZE, 0, 0);
	rec[0] ^= 0x01u;
	ff_put32(rec + FR_HO_SIZE - 4u, wire_crc32(0, rec, FR_HO_SIZE - 4u));
	CHECK(!opxy_front_accept(rec, &ho));
	for (unsigned i = 0; i < FR_HO_SIZE; i++) {
		ho_bytes(rec, 3, FR_HO_SIZE, 0, 0);
		rec[i] ^= 0x40u;
		CHECK(!opxy_front_accept(rec, &ho));
	}
	/* every device and how code on either role: the record is whole, the
	 * codes known or not; the OP-1F only on the master, never decided as
	 * the second CPU */
	for (unsigned k = 0; k < 32u; k++) {
		const unsigned d = k % 4u, h = (k / 4u) % 4u, master = k / 16u;
		const bool known = d <= 1u && h <= 2u &&
				   (d == 0u || (master != 0u && h != 1u));

		ho_bytes(rec, 3, FR_HO_SIZE, 0, 0);
		rec[14] = (uint8_t)master;
		rec[152] = (uint8_t)d;
		rec[153] = (uint8_t)h;
		ff_put32(rec + FR_HO_SIZE - 4u, wire_crc32(0, rec, FR_HO_SIZE - 4u));
		CHECK(opxy_front_accept(rec, &ho));
		id = WB_DEV_OP1F;
		how = WB_DEV_SECOND_CPU;
		CHECK(opxy_front_device(&ho, &id, &how) == known);
		if (known)
			CHECK(id == (enum wb_dev_id)d && how == (enum wb_dev_how)h);
	}
	/* the flag with no slot that exists; the trial mark */
	ho_bytes(rec, 3, FR_HO_SIZE, FR_HO_RUNNING, 2);
	CHECK(opxy_front_accept(rec, &ho) && !opxy_front_running(&ho, &slot));
	ho_bytes(rec, 3, FR_HO_SIZE, 0, 1);
	CHECK(opxy_front_accept(rec, &ho) && !opxy_front_running(&ho, &slot));
	ho_bytes(rec, 3, FR_HO_SIZE, FR_HO_RUNNING | FR_HO_TRIAL_MARKED, 0);
	CHECK(opxy_front_accept(rec, &ho) && opxy_front_running(&ho, &slot) && slot == 0u);
	CHECK(!opxy_front_no_trial(&ho, &slot));
	ho_bytes(rec, 3, FR_HO_SIZE, FR_HO_RUNNING, 0);
	CHECK(opxy_front_accept(rec, &ho) && opxy_front_no_trial(&ho, &slot) && slot == 0u);
}

/* The front's record for the stream tests: taken, version 3. */
static void give_record(uint32_t flags, uint8_t run_slot)
{
	uint8_t rec[FR_HO_SIZE];

	ho_bytes(rec, 3, FR_HO_SIZE, flags, run_slot);
	m.front = opxy_front_accept(rec, &m.ho);
	CHECK(m.front);
}

/* No flash read of the slot not named, and no memory compared. */
static bool only_slot_read(unsigned slot)
{
	const uint32_t lo = s02_slot_base(1u - slot), hi = lo + S02_SLOT_CAPACITY;

	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
		if (g_f.log[i].off >= lo && g_f.log[i].off < hi)
			return false;
	return g_mem_reads == 0u && g_fast_slots == 1u << slot;
}

/*
 * S-01.023: the running slot from the record with its flag, read
 * from no stream; its stream alone read, for the slave, and judged as
 * always (S-02.016). Without the flag, the streams.
 */
static void test_from_record(void)
{
	const uint8_t *stream = NULL;
	bool established = false;
	uint32_t len = 0;
	unsigned slot;

	/* the record names the slot memory would not: memory holds Slot 1's
	 * image, the selector names Slot 1, the record Slot 2 */
	for (unsigned named = 0; named < 2u; named++) {
		set_up(0, g_a);
		put_slot(0, g_a, g_alen, 0);
		put_slot(1, g_b, g_blen, 0);
		give_record(FR_HO_RUNNING, (uint8_t)named);
		slot = opxy_stream_slot(&m, &g_src, &established);
		CHECK(established);
		CHECK_EQ(slot, named);
		CHECK_EQ(g_warned, 0u);
		CHECK(only_slot_read(named));
		CHECK(opxy_stream_checked(&m, slot, established, &stream, &len));
		CHECK_EQ(len, named ? g_blen : g_alen);
		CHECK(memcmp(stream, named ? g_b : g_a, len) == 0);
		CHECK_EQ(g_fast, 1u);		/* read once */
		CHECK(only_slot_read(named));
	}
	/* a trial start the front established: the trial slot's record still
	 * read, and its stream sent on the trial mark (S-02.016) */
	set_up(0, g_b);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_b, g_blen, 2);
	give_record(FR_HO_RUNNING | FR_HO_TRIAL_MARKED, 1);
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established && slot == 1u);
	CHECK(only_slot_read(1));
	CHECK(opxy_stream_trial(1));
	CHECK(opxy_stream_checked(&m, slot, established, &stream, &len));
	CHECK_EQ(len, g_blen);
	/* a bad CRC in the slot named: established, not sent */
	set_up(0, g_a);
	put_slot(0, g_a, g_alen, 1);
	put_slot(1, g_b, g_blen, 0);
	give_record(FR_HO_RUNNING, 0);
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established && slot == 0u);
	CHECK(!opxy_stream_checked(&m, slot, established, &stream, &len));
	CHECK_EQ(len, 0u);
	/* without the flag, with no slot that exists: the streams, as before;
	 * memory holds Slot 2's image */
	for (unsigned c = 0; c < 2u; c++) {
		set_up(0, g_b);
		put_slot(0, g_a, g_alen, 0);
		put_slot(1, g_b, g_blen, 0);
		if (c == 0u)
			give_record(0, 0);
		else
			give_record(FR_HO_RUNNING, 2);
		slot = opxy_stream_slot(&m, &g_src, &established);
		CHECK(established);
		CHECK_EQ(slot, 1u);
		CHECK(g_mem_reads != 0u);
	}
	/* no record taken at all: the streams */
	set_up(0, g_b);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_b, g_blen, 0);
	give_record(FR_HO_RUNNING, 0);
	m.front = false;
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established && slot == 1u && g_mem_reads != 0u);
}

/* S-18.004 to S-14: a first stage the table does not hold, with n and c;
 * S-18.002: a master by the fallback sample, and a main CPU whose PA2
 * read 0. None of them: no line. */
static void test_device_log(void)
{
	memset(&m, 0, sizeof m);
	s14_log_init(&m.sv.log, S14_MASTER, &log_clock, NULL, &log_link, NULL);
	m.sv.log.cdc_on = true;
	m.dev_how = WB_DEV_UNKNOWN;
	m.fs_n = 0x8600u;
	m.fs_c = 0x0049267Eu;
	g_log_n = 0;
	opxy_device_log(&m);
	CHECK(log_has("first stage not known: n 34304, c 0049267e"));
	CHECK(!log_has("pa2"));
	m.dev_how = WB_DEV_BY_TABLE;
	m.role_fallback = true;
	g_log_n = 0;
	opxy_device_log(&m);
	CHECK(log_has("pa2 fallback: master") && !log_has("first stage"));
	m.role_disagrees = true;
	g_log_n = 0;
	opxy_device_log(&m);
	CHECK(log_has("pa2 fallback: master") && !log_has("still the master"));
	m.role_fallback = false;
	g_log_n = 0;
	opxy_device_log(&m);
	CHECK(log_has("pa2 reads 0: still the master") && !log_has("fallback"));
	m.role_disagrees = false;
	g_log_n = 0;
	opxy_device_log(&m);
	CHECK(g_log_n == 0u);
}

static unsigned fp_reads;

static int fp_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	(void)off;
	fp_reads++;
	memset(buf, 0xFF, len);
	return 0;
}

/* S-18.004: bringup()'s second read of the first stage, only for a device
 * the front did not identify and only while no n is known. */
static void test_device_fingerprint(void)
{
	memset(&m, 0, sizeof(m));
	m.dev_how = WB_DEV_BY_TABLE;
	fp_reads = 0;
	opxy_device_fingerprint(&m, fp_read, NULL);
	CHECK(fp_reads == 0u && m.fs_n == 0u);
	m.dev_how = WB_DEV_UNKNOWN;
	opxy_device_fingerprint(&m, fp_read, NULL);
	CHECK(fp_reads != 0u && m.fs_n == 0xFFFFFFFFu && m.fs_c == 0u);
	fp_reads = 0;
	opxy_device_fingerprint(&m, fp_read, NULL);	/* n known: no read */
	CHECK(fp_reads == 0u && m.fs_n == 0xFFFFFFFFu);
}

int main(void)
{
	const uint8_t *stream = NULL;
	bool established = false;
	uint32_t len = 9;
	unsigned slot;

	g_alen = make_stream(g_a, 1);
	g_blen = make_stream(g_b, 2);

	/* a normal start: the selected slot runs; its stream, read once */
	for (unsigned sel = 0; sel < 2u; sel++) {
		set_up(sel, sel ? g_b : g_a);
		put_slot(0, g_a, g_alen, 0);
		put_slot(1, g_b, g_blen, 0);
		slot = opxy_stream_slot(&m, &g_src, &established);
		CHECK(established);
		CHECK_EQ(slot, sel);
		CHECK(opxy_stream_checked(&m, slot, established, &stream, &len));
		CHECK_EQ(len, sel ? g_blen : g_alen);
		CHECK(memcmp(stream, sel ? g_b : g_a, len) == 0);
		CHECK_EQ(g_fast, 1u);
		CHECK_EQ(g_warned, 0u);
	}

	/* a trial start: the tried slot runs while the selector names the
	 * other; it is established, and with its trial mark it is sent
	 * (S-09.002), but not as any slot but the one this start runs */
	set_up(0, g_b);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_b, g_blen, 2);
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established);
	CHECK_EQ(slot, 1u);
	CHECK(!opxy_stream_checked(&m, slot, false, &stream, &len));
	CHECK_EQ(len, 0u);
	CHECK(opxy_stream_checked(&m, slot, established, &stream, &len));
	CHECK_EQ(len, g_blen);
	CHECK(memcmp(stream, g_b, len) == 0);
	CHECK_EQ(g_warned, 0u);
	/* its stream read once, by the faster read: the trial mark's CRC
	 * confirms the bytes, and no read of the whole slot follows */
	CHECK_EQ(g_fast, 1u);
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
		CHECK(g_f.log[i].len <= 2048u);
	/* the same one boot late (S-02.023): a bad CRC */
	set_up(1, g_a);
	put_slot(0, g_a, g_alen, 1);
	put_slot(1, g_b, g_blen, 0);
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established);
	CHECK_EQ(slot, 0u);
	CHECK(!opxy_stream_checked(&m, slot, established, &stream, &len));
	CHECK_EQ(len, 0u);

	/* byte-identical slots: established, the slot later starts settle on
	 * (S-02.054's tie-break): the selected slot when the selector is
	 * verified or that slot is good, otherwise the other; no log line. The
	 * selected slot's record good, with a CRC that fails, and with the
	 * trial mark: the last is a trial of the image the kept slot holds,
	 * before and after the first stage has gone back to the kept slot. */
	for (unsigned c = 0; c < 2u * 2u * 3u; c++) {
		const unsigned sel = c & 1u, rs = c / 4u;
		const bool ver = (c & 2u) == 0u;
		const unsigned want = ver || rs == 0u ? sel : 1u - sel;

		set_up(sel, g_a);
		g_f.mem[S02_SELECTOR_OFF + 1u] = ver ? 0x01u : 0x00u;
		put_slot(sel, g_a, g_alen, rs);
		put_slot(1u - sel, g_a, g_alen, 0);
		slot = opxy_stream_slot(&m, &g_src, &established);
		CHECK(established);
		CHECK_EQ(slot, want);
		CHECK_EQ(g_warned, 0u);
		/* the stream's one read is the selected slot's: its check is
		 * the image's, and nothing reads a whole slot again */
		CHECK_EQ(g_fast, 1u);
		for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
			CHECK(g_f.log[i].len <= 2048u || rs == 1u);
		/* S-09.002: sent when good, or tried in its trial start; a
		 * selected slot whose CRC fails is the answer only under a
		 * verified selector, and is not sent */
		if (want == sel && rs == 1u) {
			CHECK(!opxy_stream_checked(&m, slot, established, &stream, &len));
			CHECK_EQ(len, 0u);
		} else {
			CHECK(opxy_stream_checked(&m, slot, established, &stream, &len));
			CHECK_EQ(len, g_alen);
			CHECK(memcmp(stream, g_a, len) == 0);
		}
	}
	/* the selector cannot be read for it: not established; slot 0, its
	 * stream, and the log line */
	set_up(1, g_a);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_a, g_alen, 0);
	g_f.fail_regions = FF_R_SELECTOR;
	slot = opxy_stream_slot(&m, &g_src, &established);
	g_f.fail_regions = 0;
	CHECK(!established);
	CHECK_EQ(slot, 0u);
	CHECK_EQ(g_warned, 1u);
	CHECK(opxy_stream_checked(&m, slot, established, &stream, &len));
	CHECK_EQ(len, g_alen);

	/* slot 1 runs while the selector names slot 0: slot 1 is established
	 * and its stream sent (S-01.002) */
	set_up(0, g_b);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_b, g_blen, 0);
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established);
	CHECK_EQ(slot, 1u);
	CHECK(opxy_stream_checked(&m, slot, established, &stream, &len));
	CHECK_EQ(len, g_blen);
	CHECK(memcmp(stream, g_b, len) == 0);

	/* a slot other than the one read first: read then, and checked */
	set_up(0, g_a);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_b, g_blen, 0);
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK_EQ(slot, 0u);
	CHECK(opxy_stream_checked(&m, 1u, false, &stream, &len));
	CHECK_EQ(len, g_blen);
	CHECK(memcmp(stream, g_b, len) == 0);
	CHECK_EQ(g_fast, 2u);

	/* a faster read that returns wrong bytes: the same slot, the right
	 * bytes, in a trial-like start with the builds differing */
	set_up(0, g_b);
	put_slot(0, g_a, g_alen, 0);
	put_slot(1, g_b, g_blen, 0);
	g_fast_wrong = true;
	slot = opxy_stream_slot(&m, &g_src, &established);
	CHECK(established);
	CHECK_EQ(slot, 1u);
	CHECK(opxy_stream_checked(&m, slot, established, &stream, &len));
	CHECK_EQ(len, g_blen);
	CHECK(memcmp(stream, g_b, len) == 0);
	CHECK_EQ(g_fast, 1u);
	CHECK_EQ(g_warned, 0u);

	test_accept();
	test_from_record();

	/* the two steps write nothing */
	for (unsigned i = 0; i < g_f.nlog && i < FF_LOG_MAX; i++)
		CHECK(g_f.log[i].kind == FF_READ);
	test_device_log();
	test_device_fingerprint();
	return finish("test_board_stream");
}
