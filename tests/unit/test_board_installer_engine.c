/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The installer's write engine (src/testos/installer_engine.c,
 * wiki/Installer-Reference.md): its write gate, a trial and a commit against a RAM flash,
 * and every power cut in the trial's selector write and the first stage's
 * writes after it. The source is built here in the installer variant, its
 * SPI flash calls on the RAM flash below.
 */
#define OPXY_TEST_OS 1
#define OPXY_TESTOS_CHECK 1
#define OPXY_TESTOS_INSTALLER 1
#define wb_spiflash_read ram_read
#define wb_spiflash_erase ram_erase
#define wb_spiflash_program ram_program
#define wb_spiflash_status ram_status
/* The carried image, its length and CRC through pointers, so that a test
 * can carry another image than the build's. */
#define opxy_install_image (*inst_image_p)
#define opxy_install_len (*inst_len_p)
#define opxy_install_crc (*inst_crc_p)
#include "../../src/testos/installer_engine.c"
#include "../../src/testos/check_logic.c"
#include "../../src/testos/installer_logic.c"

#include <stdio.h>

#include "waffle/board/opxy_ops.h"
#include "waffle/boot/device.h"

#include "support/check.h"

/* A stream S-01 boots from a slot: an INIT block of 4 bytes, an empty block
 * and the FINAL block. */
static const uint8_t base_image[52] = {
	0x04, 0x00, 0x1c, 0xad, 0x00, 0x00, 0xa0, 0x11, 0x04, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x10, 0x17, 0x1e, 0x25, 0x04, 0x08, 0x10, 0xad, 0x00, 0x00,
	0xa0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x80, 0x98,
	0xad, 0x00, 0x00, 0xa0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static uint8_t img[S02_SLOT_CAPACITY + 1u];
static uint32_t img_len, img_crc;
const uint8_t (*inst_image_p)[] = (const uint8_t (*)[])img;
const uint32_t *inst_len_p = &img_len;
const uint32_t *inst_crc_p = &img_crc;

/* The stream of 52 bytes, the build's image here. */
static void image_base(void)
{
	memcpy(img, base_image, sizeof base_image);
	img_len = sizeof base_image;
	img_crc = 0xDD3F022Cu;
}

static uint8_t mem[S02_FLASH_SIZE], before[S02_FLASH_SIZE];
static unsigned writes;

/* The erases and programs that reach the selector's two bytes, in order. */
struct sel_op {
	bool erase;
	uint8_t at;		/* a program's first byte: 0 index, 1 verified */
	uint8_t n;
	uint8_t b[2];
};
#define SEL_OPS 8u
static struct sel_op sel_log[SEL_OPS];
static unsigned sel_nlog;

/*
 * Faults on the selector: selector op fail_op (1-based) leaves the selector
 * reading fail_left and reports failure, or success with fail_ok; the first
 * read of the selector once fail_read_op ops are logged fails.
 */
static unsigned fail_op, fail_read_op;
static unsigned fail_more;	/* so many selector ops after fail_op fail the same way */
static bool fail_ok;
static bool fail_rec;		/* a record program reports success, length corrupted */
static uint8_t fail_left[2];
static bool right_crc_seen;	/* a record in flash held the image's crc */
static bool sel_read_fails;	/* every read of the selector fails */
static bool spoil_on_record;	/* a record program changes a byte of its slot */

/* Byte 0 of slot t's last sector: its start log's first byte (S-02.095). */
static uint32_t log_byte(unsigned t)
{
	return s02_slot_base(t) + S02_SLOT_CAPACITY - S02_SECTOR_SIZE;
}

/* Programs of a log's byte 0: how many, the write number of the last, and a
 * fault: 1 fails, 2 reports success; either stores nothing. */
static unsigned log_prog_n, log_prog_at, fail_log;
static unsigned sel_first_at;	/* the write number of the first selector op */

static bool sel_fault(void)
{
	if (sel_nlog == 1u)
		sel_first_at = writes;
	if (fail_op == 0u || sel_nlog < fail_op || sel_nlog > fail_op + fail_more)
		return false;
	if (sel_nlog == fail_op + fail_more)
		fail_op = 0u;
	mem[S02_SELECTOR_OFF] = fail_left[0];
	mem[S02_SELECTOR_OFF + 1u] = fail_left[1];
	return !fail_ok;
}

int ram_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if ((uint64_t)off + len > S02_FLASH_SIZE)
		return -1;
	if (sel_read_fails && off <= S02_SELECTOR_OFF && off + len > S02_SELECTOR_OFF)
		return -1;
	if (fail_read_op != 0u && sel_nlog == fail_read_op && off <= S02_SELECTOR_OFF &&
	    off + len > S02_SELECTOR_OFF) {
		fail_read_op = 0u;
		return -1;
	}
	memcpy(buf, mem + off, len);
	return 0;
}

int ram_erase(void *ctx, uint32_t sector_off)
{
	(void)ctx;
	if (sector_off % S02_SECTOR_SIZE != 0u || sector_off >= S02_FLASH_SIZE)
		return -1;
	memset(mem + sector_off, 0xFF, S02_SECTOR_SIZE);
	writes++;
	if (sector_off == S02_SELECTOR_OFF && sel_nlog < SEL_OPS) {
		sel_log[sel_nlog++] = (struct sel_op){ true, 0u, 0u, { 0xFFu, 0xFFu } };
		if (sel_fault())
			return -1;
	}
	return 0;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int ram_program(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
	(void)ctx;
	if ((uint64_t)off + len > S02_FLASH_SIZE)
		return -1;
	if (off == log_byte(0u) || off == log_byte(1u)) {
		log_prog_at = writes + 1u;
		log_prog_n++;
		if (fail_log != 0u) {			/* stores nothing */
			writes++;
			return fail_log == 1u ? -1 : 0;
		}
	}
	for (uint32_t i = 0; i < len; i++)
		mem[off + i] &= buf[i];
	if (fail_rec && (off == S02_RECORD0_OFF || off == S02_RECORD1_OFF) && len > 4u)
		mem[off + 4u] &= 0x0Fu;
	if (spoil_on_record && (off == S02_RECORD0_OFF || off == S02_RECORD1_OFF))
		mem[s02_slot_base(off == S02_RECORD0_OFF ? 0u : 1u) + 20u] ^= 0x01u;
	if (off >= S02_SELECTOR_OFF && off < S02_SELECTOR_OFF + 2u && sel_nlog < SEL_OPS) {
		struct sel_op *o = &sel_log[sel_nlog++];

		*o = (struct sel_op){ false, (uint8_t)(off - S02_SELECTOR_OFF), 0u, { 0xFFu, 0xFFu } };
		for (uint32_t i = 0; i < len && (unsigned)o->at + o->n < 2u; i++) {
			o->b[o->at + o->n] = buf[i];
			o->n++;
		}
		writes++;
		return sel_fault() ? -1 : 0;
	}
	if (get32(mem + S02_RECORD0_OFF) == opxy_install_crc ||
	    get32(mem + S02_RECORD1_OFF) == opxy_install_crc)
		right_crc_seen = true;
	writes++;
	return 0;
}

int ram_status(const struct wb_spiflash *f, uint8_t *sr)
{
	(void)f;
	*sr = 0x00u;
	return 0;
}

static struct opxy_master m;
static uint8_t scratch[S02_SECTOR_SIZE];

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* An erased flash, slot s good with other bytes than the image, the selector
 * {s, verified}. */
static void setup(unsigned s, bool good)
{
	uint8_t *rec = mem + s02_record_off(s);
	uint8_t *slot = mem + s02_slot_base(s);
	const uint32_t n = 0x1800u;

	image_base();
	memset(mem, 0xFF, sizeof mem);
	memset(mem, 0x5A, S02_MIRROR_OFF);	/* the first stage */
	for (uint32_t i = 0; i < n; i++)
		slot[i] = (uint8_t)(i * 7u + 3u);
	put32(rec, opxy_crc32_long(0u, slot, n) ^ (good ? 0u : 1u));
	put32(rec + 4, n);
	put32(rec + 8, S02_RECORD_TAG);
	mem[S02_SELECTOR_OFF] = (uint8_t)s;
	mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	memcpy(before, mem, sizeof mem);
	writes = 0;
	sel_nlog = 0;
	fail_op = 0u;
	fail_more = 0u;
	fail_read_op = 0u;
	fail_ok = false;
	fail_rec = false;
	right_crc_seen = false;
	held_until_restart = false;
	sel_read_fails = false;
	spoil_on_record = false;
	log_prog_n = 0u;
	log_prog_at = 0u;
	fail_log = 0u;
	sel_first_at = 0u;
	memset(&m, 0, sizeof m);
	m.flash.crc32 = opxy_crc32_long;
	m.flash.scratch = scratch;
	m.flash.scratch_len = sizeof scratch;
}

/* Every byte that changed lies in slot t, its record's sector or the selector's. */
static bool only_allowed_changed(unsigned t)
{
	for (uint32_t off = 0; off < S02_FLASH_SIZE; off++) {
		if (mem[off] == before[off])
			continue;
		if (off >= s02_slot_base(t) && off < s02_slot_base(t) + S02_SLOT_CAPACITY)
			continue;
		if (off >= s02_record_off(t) && off < s02_record_off(t) + S02_SECTOR_SIZE)
			continue;
		if (off >= S02_SELECTOR_OFF && off < S02_SELECTOR_OFF + S02_SECTOR_SIZE)
			continue;
		printf("  byte 0x%05lx changed\n", (unsigned long)off);
		return false;
	}
	return true;
}

static void test_image(void)
{
	CHECK(opxy_crc32_long(0u, opxy_install_image, opxy_install_len) == opxy_install_crc);
}

static void test_gate(void)
{
	struct gate g = { &m.hw.flash, 0u, 0u, false, false, false, 0u, false, false };

	CHECK(!gate_allows(&g, s02_slot_base(1), S02_SECTOR_SIZE));	/* not armed */
	g.slot = s02_slot_base(1);
	g.record = s02_record_off(1);
	g.armed = true;
	CHECK(gate_allows(&g, s02_slot_base(1), S02_SECTOR_SIZE));
	CHECK(gate_allows(&g, s02_slot_base(1) + S02_SLOT_CAPACITY - S02_SECTOR_SIZE,
			  S02_SECTOR_SIZE));
	CHECK(!gate_allows(&g, s02_slot_base(1) + S02_SLOT_CAPACITY - 1u, 2u));
	CHECK(gate_allows(&g, s02_record_off(1), S02_RECORD_SIZE));
	CHECK(!gate_allows(&g, s02_record_off(0), S02_RECORD_SIZE));
	CHECK(!gate_allows(&g, s02_slot_base(0), 1u));
	CHECK(!gate_allows(&g, 0u, 1u));			/* the first stage */
	CHECK(!gate_allows(&g, S02_MIRROR_OFF, 1u));
	CHECK(!gate_allows(&g, S02_META_OFF, 1u));
	CHECK(!gate_allows(&g, S02_SELECTOR_OFF, 2u));	/* not open yet */
	CHECK(!gate_allows(&g, 0xFFFFFFF0u, 0x20u));		/* wraps */
	g.selector_open = true;
	CHECK(gate_allows(&g, S02_SELECTOR_OFF, 2u));
	CHECK(!gate_allows(&g, S02_SELECTOR_OFF + S02_SECTOR_SIZE - 1u, 2u));

	/* Sector by sector, for either target: its slot, its record, the selector. */
	for (unsigned t = 0; t < 2u; t++) {
		g.slot = s02_slot_base(t);
		g.record = s02_record_off(t);
		for (uint32_t off = 0; off < S02_FLASH_SIZE; off += S02_SECTOR_SIZE) {
			const bool want = (off >= g.slot && off < g.slot + S02_SLOT_CAPACITY) ||
					  off == g.record || off == S02_SELECTOR_OFF;

			CHECK(gate_allows(&g, off, S02_SECTOR_SIZE) == want);
		}
	}
	g.slot = s02_slot_base(1);
	g.record = s02_record_off(1);

	/* After the trial's upgrade: of slot 1 and its record, byte 0 of its log
	 * alone, one byte; the selector once open. */
	g.log_only = true;
	for (uint32_t off = 0; off < S02_FLASH_SIZE; off += S02_SECTOR_SIZE)
		CHECK(gate_allows(&g, off, S02_SECTOR_SIZE) == (off == S02_SELECTOR_OFF));
	CHECK(gate_allows(&g, log_byte(1u), 1u));
	CHECK(!gate_allows(&g, log_byte(1u), 2u));
	CHECK(!gate_allows(&g, log_byte(1u) + 1u, 1u));
	CHECK(!gate_allows(&g, log_byte(1u) - 1u, 1u));
	CHECK(!gate_allows(&g, log_byte(0u), 1u));
	CHECK(!gate_allows(&g, s02_record_off(1), S02_RECORD_SIZE));
	CHECK(!gate_allows(&g, s02_slot_base(1), 1u));
	CHECK(gate_allows(&g, S02_SELECTOR_OFF, 2u));
	g.log_only = false;

	/* Refused writes reach nothing and are counted. */
	g.selector_open = false;
	setup(0u, true);
	CHECK(gate_erase(&g, S02_SELECTOR_OFF) != 0);
	CHECK(g.at_selector);
	CHECK(gate_erase(&g, s02_slot_base(0)) != 0);
	CHECK(gate_program(&g, 0u, scratch, 4u) != 0);
	CHECK(g.refused == 2u);
	CHECK(writes == 0u);
}

/* Slot t's last sector reads a trial pending alone: 0xFD, then 0xFF. */
static bool log_pending(unsigned t)
{
	if (mem[log_byte(t)] != 0xFDu)
		return false;
	for (uint32_t i = 1; i < S02_SECTOR_SIZE; i++)
		if (mem[log_byte(t) + i] != 0xFFu)
			return false;
	return true;
}

/*
 * A front of version 7 or later in slot k, in a trial start of slot k
 * (S-02.095, the trial start's mark): slot k is the slot the selector does
 * not name, its record has the tag and a length of 1 to 0x77000, and its log
 * reads a trial pending alone; when it establishes that it runs from slot k
 * it programs byte 0 to 0xFC. With both slots' streams byte for byte the
 * same, slot k's front is the one that runs whichever slot was read, and
 * it marks the same way (S-02.095). An older front makes no mark.
 */
static void front_trial_start(unsigned k)
{
	const uint8_t *rk = mem + s02_record_off(k);
	const uint32_t n = get32(rk + 4);

	if (s02_other_slot(mem[S02_SELECTOR_OFF]) != k || get32(rk + 8) != S02_RECORD_TAG ||
	    n < 1u || n > S02_SLOT_CAPACITY - S02_SECTOR_SIZE || !log_pending(k))
		return;
	mem[log_byte(k)] = 0xFCu;
}

/* The main image of that start at its boot menu (opxy_trial_reached()):
 * 0xFC to 0xF0 where the front marked the start. */
static void menu_reached(unsigned k)
{
	if (mem[log_byte(k)] == OPXY_TRIAL_STARTED_BYTE)
		mem[log_byte(k)] = OPXY_TRIAL_REACHED_BYTE;
}

/* A trial start that reached the boot menu. */
static void trial_start(unsigned k)
{
	front_trial_start(k);
	menu_reached(k);
}

/* A trial from slot 0 writes slot 1, the record with the wrong crc, byte 0 of
 * slot 1's log 0xFD before the selector, and the selector {0xFF, 0xFF}, and
 * nothing else; after the trial start's mark the commit makes the record
 * right and leaves the log byte as it was. */
static void test_trial_and_commit(void)
{
	uint8_t out[512];
	uint8_t raw[S02_RECORD_SIZE];
	struct s02_record rec;

	setup(0u, true);
	memset(out, 0xEE, sizeof out);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK);
	CHECK(out[2] == 0xFFu && out[3] == 0xFFu);		/* the selector */
	CHECK(sel_nlog == 1u && sel_log[0].erase);		/* the erase alone */
	CHECK(out[5] == (OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_PENDING));
	CHECK(out[6] == 1u);
	CHECK(out[7] == 0u);					/* nothing refused */
	CHECK(memcmp(mem + s02_slot_base(1), opxy_install_image, opxy_install_len) == 0);
	s02_record_decode(mem + s02_record_off(1), &rec);
	CHECK(rec.crc32 == (opxy_install_crc ^ TRIAL_CRC_XOR));
	CHECK(rec.length == opxy_install_len);
	CHECK(only_allowed_changed(1u));
	CHECK(!right_crc_seen);		/* the record never held the right crc */
	CHECK(log_pending(1u) && log_prog_n == 1u);
	CHECK(log_prog_at != 0u && log_prog_at < sel_first_at);	/* before the selector */

	/* The trial start: the first stage writes {0, 0x01}; slot 1's front marks. */
	mem[S02_SELECTOR_OFF] = 0x00u;
	mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	front_trial_start(1u);
	CHECK(mem[log_byte(1u)] == 0xFCu);
	/* the front ran, the boot menu not reached: no commit, nothing written */
	memcpy(before, mem, sizeof mem);
	writes = 0;
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == OPXY_INSTALL_REFUSED_UNREACHED);
	CHECK(writes == 0u && memcmp(mem, before, sizeof mem) == 0);
	menu_reached(1u);
	CHECK(mem[log_byte(1u)] == 0xF0u);
	memcpy(before, mem, sizeof mem);
	(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
	CHECK(out[5] == (OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_STARTED |
			 OPXY_INSTALL_SLOT_REACHED));
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == S02_OK);
	CHECK(out[2] == 1u && out[3] == 0x00u);
	CHECK((out[5] & OPXY_INSTALL_SLOT_COMMITTED) != 0u);
	CHECK((out[5] & (OPXY_INSTALL_SLOT_PENDING | OPXY_INSTALL_SLOT_STARTED |
			 OPXY_INSTALL_SLOT_SPENT)) == 0u);
	memcpy(raw, mem + s02_record_off(1), sizeof raw);
	s02_record_decode(raw, &rec);
	CHECK(rec.crc32 == opxy_install_crc);
	CHECK(mem[log_byte(1u)] == 0xF0u);	/* the log byte as it was */
	CHECK(only_allowed_changed(1u));
}

/*
 * The commit screen's 2 (inst_act_of()): the same trial written again over a
 * trial that reached the boot menu, by the same form and checks as the
 * first. A first trial start spent on recovery leaves the entry started
 * without the boot menu's mark; a second, on the boot menu, then lets the
 * commit go. The selected slot, the way back, is never written, on the
 * OP-XY and on the OP-1F, where the unit's own bootloader stays in it.
 */
static void test_second_trial(void)
{
	uint8_t out[OPXY_INSTALL_REPLY];

	for (unsigned d = 0; d < 2u; d++) {
		setup(0u, true);
		m.dev = d == 0u ? NULL : &wb_device_op1f;
		(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
		CHECK(out[0] == S02_OK && out[6] == 1u);
		/* the first trial start reaches the boot menu: the commit is offered */
		mem[S02_SELECTOR_OFF] = 0x00u;
		mem[S02_SELECTOR_OFF + 1u] = 0x01u;
		trial_start(1u);
		(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
		CHECK(inst_trial_of(out[5]) == INST_TRIAL_REACHED);
		/* the trial again instead: slot 1 alone, a trial pending again */
		memcpy(before, mem, sizeof mem);
		(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
		CHECK(out[0] == S02_OK && out[6] == 1u && out[7] == 0u);
		CHECK(out[2] == 0xFFu && out[3] == 0xFFu);
		CHECK(out[5] == (OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_PENDING));
		CHECK(inst_trial_of(out[5]) == INST_TRIAL_WAITING);
		CHECK(log_pending(1u) && only_allowed_changed(1u));
		CHECK(memcmp(mem + s02_slot_base(0), before + s02_slot_base(0), S02_SLOT_CAPACITY) == 0);
		CHECK(memcmp(mem + s02_record_off(0), before + s02_record_off(0), S02_SECTOR_SIZE) == 0);
		/* that start goes to recovery: the front's mark, no boot menu */
		mem[S02_SELECTOR_OFF] = 0x00u;
		mem[S02_SELECTOR_OFF + 1u] = 0x01u;
		front_trial_start(1u);
		(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
		CHECK(out[0] == OPXY_INSTALL_REFUSED_UNREACHED);
		/* once more, and that start reaches the boot menu: kept */
		(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
		CHECK(out[0] == S02_OK && log_pending(1u));
		mem[S02_SELECTOR_OFF] = 0x00u;
		mem[S02_SELECTOR_OFF + 1u] = 0x01u;
		trial_start(1u);
		(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
		CHECK(out[0] == S02_OK && out[2] == 1u && out[3] == 0x00u);
		CHECK((out[5] & OPXY_INSTALL_SLOT_COMMITTED) != 0u);
	}
}

/* Commit with each trial entry: refused, writing nothing, unless started and
 * the boot menu's mark after it. */
static void test_commit_needs_start(void)
{
	static const struct {
		uint8_t entry;
		uint8_t bits;	/* the status's entry bits */
		int rc;
	} cases[] = {
		{ 0xFFu, 0u, OPXY_INSTALL_REFUSED_UNSTARTED },	/* no entry */
		{ 0xFDu, OPXY_INSTALL_SLOT_PENDING, OPXY_INSTALL_REFUSED_UNSTARTED },
		{ 0xF9u, OPXY_INSTALL_SLOT_SPENT, OPXY_INSTALL_REFUSED_UNSTARTED },
		{ 0xF1u, OPXY_INSTALL_SLOT_SPENT, OPXY_INSTALL_REFUSED_UNSTARTED },
		{ 0x01u, OPXY_INSTALL_SLOT_SPENT, OPXY_INSTALL_REFUSED_UNSTARTED },
		{ 0xFEu, OPXY_INSTALL_SLOT_ODD, OPXY_INSTALL_REFUSED_UNSTARTED },
		{ 0xFBu, OPXY_INSTALL_SLOT_ODD, OPXY_INSTALL_REFUSED_UNSTARTED },
		{ 0x7Fu, OPXY_INSTALL_SLOT_ODD, OPXY_INSTALL_REFUSED_UNSTARTED },
		{ 0xFCu, OPXY_INSTALL_SLOT_STARTED, OPXY_INSTALL_REFUSED_UNREACHED },
		{ 0x00u, OPXY_INSTALL_SLOT_STARTED, OPXY_INSTALL_REFUSED_UNREACHED },
		{ 0xF4u, OPXY_INSTALL_SLOT_STARTED, OPXY_INSTALL_REFUSED_UNREACHED },
		{ 0xF0u, OPXY_INSTALL_SLOT_STARTED | OPXY_INSTALL_SLOT_REACHED, S02_OK },
	};
	uint8_t out[512];

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		for (unsigned k = 0; k < 2u; k++) {
			setup(1u - k, true);
			(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
			CHECK(out[0] == S02_OK);
			/* The trial start's selector, {old, 0x01}; the entry as given. */
			mem[S02_SELECTOR_OFF] = (uint8_t)(1u - k);
			mem[S02_SELECTOR_OFF + 1u] = 0x01u;
			mem[log_byte(k)] = cases[i].entry;
			memcpy(before, mem, sizeof mem);
			writes = 0;
			(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
			CHECK(out[4u + k] == (OPXY_INSTALL_SLOT_TRIAL | cases[i].bits));
			CHECK(out[4u + (1u - k)] == OPXY_INSTALL_SLOT_GOOD);
			(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
			CHECK(out[0] == (uint8_t)cases[i].rc);
			CHECK(mem[log_byte(k)] == cases[i].entry);
			if (cases[i].rc == S02_OK) {
				CHECK((out[4u + k] & OPXY_INSTALL_SLOT_COMMITTED) != 0u);
			} else {
				CHECK(writes == 0u && memcmp(mem, before, sizeof mem) == 0);
				CHECK(out[2] == (uint8_t)(1u - k) && out[3] == 0x01u);
			}
		}
	}
}

/*
 * A trial of the image the selected slot holds goes as any other, for
 * either slot order: two slots may hold the same image. The trial start
 * marks the entry, the commit takes it, and both slots are then good with
 * the same bytes.
 */
static void test_trial_same_image(void)
{
	uint8_t out[512];

	for (unsigned s = 0; s < 2u; s++) {
		const unsigned k = 1u - s;

		setup(s, true);
		memset(mem + s02_slot_base(s), 0xFF, 0x1800u);
		memcpy(mem + s02_slot_base(s), opxy_install_image, opxy_install_len);
		put32(mem + s02_record_off(s), opxy_install_crc);
		put32(mem + s02_record_off(s) + 4, opxy_install_len);
		memcpy(before, mem, sizeof mem);
		memset(out, 0xEE, sizeof out);
		(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
		CHECK(out[0] == S02_OK && out[6] == (uint8_t)k && log_pending(k));
		CHECK(out[4u + s] == (OPXY_INSTALL_SLOT_GOOD | OPXY_INSTALL_SLOT_COMMITTED));
		CHECK(out[4u + k] == (OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_PENDING));
		CHECK(only_allowed_changed(k));
		/* a commit before the trial start is refused, as for any image */
		mem[S02_SELECTOR_OFF] = (uint8_t)s;
		mem[S02_SELECTOR_OFF + 1u] = 0x01u;
		(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
		CHECK(out[0] == OPXY_INSTALL_REFUSED_UNSTARTED);
		front_trial_start(k);
		CHECK(mem[log_byte(k)] == 0xFCu);
		menu_reached(k);
		(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
		CHECK(out[0] == S02_OK);
		CHECK(out[2] == (uint8_t)k && out[3] == 0x00u);
		CHECK(out[4] == (OPXY_INSTALL_SLOT_GOOD | OPXY_INSTALL_SLOT_COMMITTED));
		CHECK(out[5] == (OPXY_INSTALL_SLOT_GOOD | OPXY_INSTALL_SLOT_COMMITTED));
		CHECK(memcmp(mem + s02_slot_base(0), mem + s02_slot_base(1),
			     opxy_install_len) == 0);
	}
}

/*
 * The cases around the trial start's mark:
 * - an older front in the old slot ignores the other slot's byte: the entry
 *   stays pending until slot k's own front starts, and a commit is refused;
 * - both slots holding the same bytes: the mark is made and the commit goes;
 * - a recovery trial of this image (the same record, entry and selector):
 *   the installer's commit takes it only once started;
 * - a front in the old slot that spends the entry (0xF9): refused, spent.
 */
static void test_trial_mark_cases(void)
{
	uint8_t out[512];
	uint32_t n;

	/* An older front in slot 0 starts, writing nothing to slot 1's log. */
	setup(0u, true);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	mem[S02_SELECTOR_OFF] = 0x00u;
	mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == OPXY_INSTALL_REFUSED_UNSTARTED);
	CHECK(out[5] == (OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_PENDING));
	trial_start(1u);			/* slot 1's own front, later */
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == S02_OK);

	/* Both slots the same bytes, slot 1 as a trial left it: the trial
	 * start marks it, and the commit goes. */
	setup(0u, true);
	n = opxy_install_len;
	memset(mem + s02_slot_base(0), 0xFF, 0x1800u);
	memcpy(mem + s02_slot_base(0), opxy_install_image, n);
	put32(mem + s02_record_off(0), opxy_install_crc);
	put32(mem + s02_record_off(0) + 4, n);
	memcpy(mem + s02_slot_base(1), opxy_install_image, n);
	put32(mem + s02_record_off(1), ~opxy_install_crc);
	put32(mem + s02_record_off(1) + 4, n);
	put32(mem + s02_record_off(1) + 8, S02_RECORD_TAG);
	mem[log_byte(1u)] = 0xFDu;
	front_trial_start(1u);
	CHECK(mem[log_byte(1u)] == 0xFCu);
	menu_reached(1u);
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == S02_OK);

	/* What recovery's trial write leaves (S-02.084): the image in slot 1, the
	 * record with the complement of its crc, entry 0xFD, the selector, here
	 * after the trial start's first-stage write. */
	setup(0u, true);
	memcpy(mem + s02_slot_base(1), opxy_install_image, opxy_install_len);
	put32(mem + s02_record_off(1), ~opxy_install_crc);
	put32(mem + s02_record_off(1) + 4, opxy_install_len);
	put32(mem + s02_record_off(1) + 8, S02_RECORD_TAG);
	mem[log_byte(1u)] = 0xFDu;
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == OPXY_INSTALL_REFUSED_UNSTARTED);
	trial_start(1u);
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == S02_OK && mem[log_byte(1u)] == 0xF0u);

	/* A front in the old slot spent the entry, 0xFD to 0xF9. */
	setup(0u, true);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	mem[S02_SELECTOR_OFF] = 0x00u;
	mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	mem[log_byte(1u)] = 0xF9u;
	front_trial_start(1u);			/* not pending: no mark */
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == OPXY_INSTALL_REFUSED_UNSTARTED);
	CHECK(out[5] == (OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_SPENT));
	/* Another trial erases the sector and starts over. */
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK && log_pending(1u));
}

/* The log byte's program fails, or reads back wrong: the error, nothing of
 * the selector written. */
static void test_trial_log_faults(void)
{
	uint8_t out[512];

	for (unsigned k = 0; k < 2u; k++) {
		for (unsigned f = 1; f <= 2u; f++) {
			setup(1u - k, true);
			fail_log = f;
			(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
			CHECK(out[0] == (uint8_t)(f == 1u ? S02_ERR_IO : S02_ERR_VERIFY));
			CHECK(sel_nlog == 0u);
			CHECK(out[2] == (uint8_t)(1u - k) && out[3] == 0x01u);
			CHECK(out[4u + k] == OPXY_INSTALL_SLOT_TRIAL);	/* no entry */
			CHECK(only_allowed_changed(k));
			CHECK(!right_crc_seen);
		}
	}
}

/* A trial from slot 1 writes slot 0 and the selector {0x00, 0xFE}: the erase,
 * verified, then the index; the commit writes {0x00, 0x00}. */
static void test_trial_slot0(void)
{
	uint8_t out[512];

	setup(1u, true);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK);
	CHECK(out[2] == 0x00u && out[3] == 0xFEu);
	CHECK(out[4] == (OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_PENDING));
	CHECK(out[6] == 0u);
	CHECK(sel_nlog == 3u);
	CHECK(sel_log[0].erase);
	CHECK(!sel_log[1].erase && sel_log[1].at == 1u && sel_log[1].n == 1u &&
	      sel_log[1].b[1] == 0xFEu);
	CHECK(!sel_log[2].erase && sel_log[2].at == 0u && sel_log[2].n == 1u &&
	      sel_log[2].b[0] == 0x00u);
	CHECK(only_allowed_changed(0u));
	CHECK(log_pending(0u) && log_prog_n == 1u && log_prog_at < sel_first_at);

	mem[S02_SELECTOR_OFF] = 0x01u;		/* the first stage's {1, 0x01} */
	mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	front_trial_start(0u);
	CHECK(mem[log_byte(0u)] == 0xFCu);
	menu_reached(0u);
	memcpy(before, mem, sizeof mem);
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == S02_OK);
	CHECK(out[2] == 0x00u && out[3] == 0x00u);
	CHECK((out[4] & OPXY_INSTALL_SLOT_COMMITTED) != 0u);
	CHECK(only_allowed_changed(0u));
}

/*
 * Power cuts, on the selector's two bytes as a 16-bit value v: index v & 0xFF,
 * verified v >> 8. A torn erase leaves any value with at least the old 1 bits;
 * a torn program any value between the old one and the old one AND the new.
 * The trial's own writes are cut at most once (a cut ends the installer); the
 * first stage (S-02.020 to S-02.023) then runs on every reset, and any of its
 * writes may be torn, any number of times. Slot k fails its check (its
 * record's crc is wrong), the other slot passes. A value trusts slot k when
 * the first stage boots slot k from it with no check.
 */
#define CUT_INST_ERASE  0x01u
#define CUT_INST_PROG   0x02u
#define CUT_FS_ERASE    0x04u
#define CUT_FS_PROG_001 0x08u	/* the first stage's program of {0x00, 0x01} */
#define CUT_FS_PROG     0x10u	/* its program of any other value */
#define CUT_CAUSES      5u

static uint8_t cut_seen[0x10000], cut_why[0x10000];
static uint16_t cut_queue[0x10000];
static unsigned cut_nq;

struct cut_counts {
	unsigned states;		/* values reachable */
	unsigned trusted;		/* of them, those that trust slot k */
	unsigned by[CUT_CAUSES];	/* trusted, reached by each CUT_* cause */
	unsigned other;			/* trusted, reached by a cause not allowed */
};

static bool trusts(uint32_t v, unsigned k)
{
	return (v >> 8) == 0x01u && s02_selected_slot((uint8_t)v) == k;
}

static void cut_reach(uint32_t v, unsigned k, uint8_t cause)
{
	if (trusts(v, k))
		cut_why[v] |= cause;
	if (cut_seen[v] == 0u) {
		cut_seen[v] = 1u;
		cut_queue[cut_nq++] = (uint16_t)v;
	}
}

/* Every result of a torn erase of s: its supersets, s and 0xFFFF included. */
static void cut_erase(uint32_t s, unsigned k, uint8_t cause)
{
	for (uint32_t r = s;; r = ((r + 1u) | s) & 0xFFFFu) {
		cut_reach(r, k, cause);
		if (r == 0xFFFFu)
			break;
	}
}

/* Every result of a torn program of w over s, s and s & w included. */
static void cut_program(uint32_t s, uint32_t w, unsigned k, uint8_t cause)
{
	const uint32_t lo = s & w, free_bits = s & ~w & 0xFFFFu;

	for (uint32_t sub = free_bits;; sub = (sub - 1u) & free_bits) {
		cut_reach(lo | sub, k, cause);
		if (sub == 0u)
			break;
	}
}

/*
 * The installer's ops from each of the starts, then the first stage to
 * closure. One closure over all starts reaches exactly the union of what each
 * start reaches, so a start set costs no more than one start.
 */
static void cut_model(const struct sel_op *ops, unsigned n, const uint32_t *starts,
		      unsigned nstarts, unsigned k, uint8_t allowed, struct cut_counts *c)
{
	memset(cut_seen, 0, sizeof cut_seen);
	memset(cut_why, 0, sizeof cut_why);
	memset(c, 0, sizeof *c);
	cut_nq = 0;
	for (unsigned st = 0; st < nstarts; st++) {
		uint32_t s = starts[st];

		cut_reach(s, k, 0u);
		for (unsigned i = 0; i < n; i++) {
			if (ops[i].erase) {
				cut_erase(s, k, CUT_INST_ERASE);
				s = 0xFFFFu;
			} else {
				uint32_t w = 0xFFFFu;

				for (unsigned j = ops[i].at; j < (unsigned)ops[i].at + ops[i].n; j++)
					w &= ~(0xFFu << (8u * j)) | ((uint32_t)ops[i].b[j] << (8u * j));
				cut_program(s, w, k, CUT_INST_PROG);
				s &= w;
			}
			cut_reach(s, k, 0u);
		}
	}
	for (unsigned q = 0; q < cut_nq; q++) {
		const uint32_t v = cut_queue[q];
		const uint8_t idx = (uint8_t)v;
		uint32_t w;

		if ((v >> 8) == 0x01u)
			continue;			/* S-02.020: no check, no write */
		/* S-02.021, S-02.022: a pass keeps the index, a fail names the other. */
		if (s02_selected_slot(idx) != k)
			w = idx;
		else
			w = idx == 0x00u ? 0x01u : 0x00u;
		w |= 0x0100u;
		cut_erase(v, k, CUT_FS_ERASE);
		cut_program(0xFFFFu, w, k, w == 0x0100u ? CUT_FS_PROG_001 : CUT_FS_PROG);
	}
	c->states = cut_nq;
	for (uint32_t v = 0; v < 0x10000u; v++) {
		if (cut_seen[v] == 0u || !trusts(v, k))
			continue;
		c->trusted++;
		for (unsigned b = 0; b < CUT_CAUSES; b++)
			if ((cut_why[v] & (1u << b)) != 0u)
				c->by[b]++;
		if (cut_why[v] == 0u || (cut_why[v] & (uint8_t)~allowed) != 0u)
			c->other++;
	}
}

static void cut_print(const char *what, const struct cut_counts *c)
{
	printf("  %-32s %5u values, %5u trust slot k: installer erase %u, program %u;"
	       " first stage erase %u, program {00, 01} %u, other program %u\n",
	       what, c->states, c->trusted, c->by[0], c->by[1], c->by[2], c->by[3], c->by[4]);
}

static void test_trial_cuts(void)
{
	static const struct sel_op old0[2] = {
		{ true, 0u, 0u, { 0xFFu, 0xFFu } }, { false, 0u, 2u, { 0x00u, 0x00u } } };
	static const struct sel_op old1[2] = {
		{ true, 0u, 0u, { 0xFFu, 0xFFu } }, { false, 0u, 2u, { 0x01u, 0x00u } } };
	static const uint32_t start1 = 0x0100u;
	const uint8_t windows = CUT_INST_ERASE | CUT_FS_PROG_001;
	struct sel_op ops[SEL_OPS];
	uint32_t starts0[255];
	struct cut_counts c;
	uint8_t out[512];
	unsigned n = 0;

	memset(ops, 0, sizeof ops);
	/* k = 0:the selector before is {x, 0x01} for every nonzero x, as the
	 * first stage keeps it (S-02.022); the installer's own writes from each. */
	for (unsigned x = 1; x < 256u; x++) {
		setup(1u, true);
		mem[S02_SELECTOR_OFF] = (uint8_t)x;
		memcpy(before, mem, sizeof mem);
		(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
		CHECK(out[0] == S02_OK && out[6] == 0u);
		if (x == 1u) {
			memcpy(ops, sel_log, sizeof ops);
			n = sel_nlog;
		}
		CHECK(sel_nlog == n && memcmp(ops, sel_log, sizeof ops) == 0);
		CHECK(!right_crc_seen);
		starts0[x - 1u] = 0x0100u | x;
	}
	cut_model(ops, n, starts0, 255u, 0u, 0u, &c);
	cut_print("slot index 0, {00, FE}:", &c);
	CHECK(c.trusted == 0u);

	cut_model(old0, 2u, starts0, 255u, 0u, 0u, &c);
	cut_print("slot index 0, {00, 00} (control):", &c);
	CHECK(c.trusted > 0u);

	/* k = 1: the selector before is {0x00, 0x01}. */
	setup(0u, true);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK && out[6] == 1u);
	CHECK(!right_crc_seen);
	memcpy(ops, sel_log, sizeof ops);
	cut_model(ops, sel_nlog, &start1, 1u, 1u, windows, &c);
	cut_print("slot index 1, {FF, FF}:", &c);
	CHECK(c.other == 0u);
	CHECK(c.by[0] > 0u && c.by[3] > 0u);	/* the two windows, and only they */

	cut_model(old1, 2u, &start1, 1u, 1u, windows, &c);
	cut_print("slot index 1, {01, 00} (control):", &c);
	CHECK(c.by[1] > 0u);			/* its own program tears to {01, 01} */
}

/*
 * Faults in the trial's selector write: an erase or program that fails, or
 * that reports success and reads back wrong, and a read-back that fails. Each
 * returns its own error and leaves a selector that does not trust slot k.
 */
struct fault_case {
	unsigned running;	/* the slot selected before; k is the other */
	unsigned op;		/* selector op that fails, 0 none */
	bool ok;		/* it reports success */
	uint8_t left[2];
	unsigned read_op;	/* the read-back after this op fails, 0 none */
	int rc;
	uint8_t sel[2];		/* the selector after */
	unsigned nops;		/* selector ops issued */
};

static void test_trial_faults(void)
{
	static const struct fault_case cases[] = {
		/* k = 1, from {00, 01}: where verified has bit 0 set, every
		 * failure ends settled on slot 0, {00, 01}, by programs alone
		 * (S-02.102 item 6); where it is clear, nothing is written. */
		{ 0u, 1u, false, { 0x03u, 0x01u }, 0u, S02_ERR_IO, { 0x00u, 0x01u }, 2u },
		{ 0u, 1u, false, { 0xFFu, 0xFFu }, 0u, S02_ERR_IO, { 0x00u, 0x01u }, 3u },
		{ 0u, 1u, false, { 0x00u, 0x01u }, 0u, S02_ERR_IO, { 0x00u, 0x01u }, 1u },
		{ 0u, 1u, true, { 0x05u, 0x01u }, 0u, S02_ERR_VERIFY, { 0x00u, 0x01u }, 2u },
		{ 0u, 1u, true, { 0xFFu, 0x03u }, 0u, S02_ERR_VERIFY, { 0x00u, 0x01u }, 3u },
		{ 0u, 1u, true, { 0x00u, 0xFFu }, 0u, S02_ERR_VERIFY, { 0x00u, 0x01u }, 2u },
		{ 0u, 1u, true, { 0x00u, 0x00u }, 0u, S02_ERR_VERIFY, { 0x00u, 0x00u }, 1u },
		{ 0u, 1u, true, { 0x05u, 0xFEu }, 0u, S02_ERR_VERIFY, { 0x05u, 0xFEu }, 1u },
		{ 0u, 1u, false, { 0xFFu, 0xFEu }, 0u, S02_ERR_IO, { 0xFFu, 0xFEu }, 1u },
		{ 0u, 0u, false, { 0u, 0u }, 1u, S02_ERR_IO, { 0x00u, 0x01u }, 3u },
		/* k = 0, from {01, 01}: nothing is written after a failure. */
		{ 1u, 1u, false, { 0x03u, 0x05u }, 0u, S02_ERR_IO, { 0x03u, 0x05u }, 1u },
		{ 1u, 1u, true, { 0x01u, 0x01u }, 0u, S02_ERR_VERIFY, { 0x01u, 0x01u }, 1u },
		{ 1u, 0u, false, { 0u, 0u }, 1u, S02_ERR_IO, { 0xFFu, 0xFFu }, 1u },
		{ 1u, 2u, false, { 0xFFu, 0xFFu }, 0u, S02_ERR_IO, { 0xFFu, 0xFFu }, 2u },
		{ 1u, 2u, true, { 0xFFu, 0xFFu }, 0u, S02_ERR_VERIFY, { 0xFFu, 0xFFu }, 2u },
		{ 1u, 0u, false, { 0u, 0u }, 2u, S02_ERR_IO, { 0xFFu, 0xFEu }, 2u },
		{ 1u, 3u, false, { 0x7Fu, 0xFEu }, 0u, S02_ERR_IO, { 0x7Fu, 0xFEu }, 3u },
		{ 1u, 3u, false, { 0x00u, 0xFEu }, 0u, S02_ERR_IO, { 0x00u, 0xFEu }, 3u },
		{ 1u, 3u, true, { 0x10u, 0xFEu }, 0u, S02_ERR_VERIFY, { 0x10u, 0xFEu }, 3u },
		{ 1u, 0u, false, { 0u, 0u }, 3u, S02_ERR_IO, { 0x00u, 0xFEu }, 3u },
	};
	uint8_t out[512];

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		const struct fault_case *f = &cases[i];
		const unsigned k = 1u - f->running;

		setup(f->running, true);
		fail_op = f->op;
		fail_ok = f->ok;
		memcpy(fail_left, f->left, 2u);
		fail_read_op = f->read_op;
		(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
		CHECK(out[0] == (uint8_t)f->rc);
		CHECK(out[2] == f->sel[0] && out[3] == f->sel[1]);
		CHECK(mem[S02_SELECTOR_OFF] == f->sel[0] && mem[S02_SELECTOR_OFF + 1u] == f->sel[1]);
		CHECK(sel_nlog == f->nops);
		CHECK(!trusts((uint32_t)f->sel[0] | (uint32_t)f->sel[1] << 8, k));
		CHECK(!right_crc_seen);
		CHECK(only_allowed_changed(k));
		/* no erase after the step's own; the index first, then verified */
		for (unsigned j = 1; j < sel_nlog; j++)
			CHECK(!sel_log[j].erase);
		if (k == 1u && f->nops >= 2u) {
			const bool index = (f->read_op != 0u ? 0xFFu : f->left[0]) != 0x00u;

			CHECK(!index || (sel_log[1].at == 0u && sel_log[1].n == 1u &&
					 sel_log[1].b[0] == 0x00u));
			CHECK(f->nops < (index ? 3u : 2u) ||
			      (sel_log[f->nops - 1u].at == 1u && sel_log[f->nops - 1u].b[1] == 0x01u));
		}
		/* a selector left as it was holds every later write */
		CHECK(held_until_restart == (k == 1u && f->sel[1] != 0x01u));
	}
}

/*
 * The settling after a failed selector step for slot 1 (S-02.102 item 6),
 * cut at each of its operations: before it, torn (every result the torn
 * rules of cut_erase() and cut_program() allow) and after the last. It is
 * made only where the step left `verified` with bit 0 set, by programs
 * alone. What one start then reaches, under the factory first stage, slot
 * 0 (A) good and running, slot 1 (B) whole but tried (its record's check
 * fails):
 *   A+   verified 0x01, index 0x00: slot 0 with no check
 *   A    another verified, index 0x00: slot 0 checked, passes
 *   B1   another verified, a nonzero index: slot 1 checked, fails; it starts
 *        once, the trial start, and every later start reaches slot 0
 *   B+   verified 0x01, a nonzero index: slot 1 with no check, on every start
 * Each row prints the outcomes reachable. The settling makes no selector
 * that names slot 1 where the selector it started from did not: every
 * value cut before or inside the index program keeps the starting
 * `verified` and a subset of the starting index's bits, and from that
 * program's end every value names slot 0.
 */
#define OUT_A_TRUSTED 0x01u
#define OUT_A_CHECKED 0x02u
#define OUT_B_ONCE    0x04u
#define OUT_B_TRUSTED 0x08u

static unsigned outcome_of(uint32_t v)
{
	const bool trusted = (v >> 8) == 0x01u;

	if (s02_selected_slot((uint8_t)v) == 0u)
		return trusted ? OUT_A_TRUSTED : OUT_A_CHECKED;
	return trusted ? OUT_B_TRUSTED : OUT_B_ONCE;
}

static void outcomes_print(const char *cut, unsigned out)
{
	printf("    %-30s %s%s%s%s\n", cut, (out & OUT_A_TRUSTED) != 0u ? " A+" : "",
	       (out & OUT_A_CHECKED) != 0u ? " A" : "", (out & OUT_B_ONCE) != 0u ? " B1" : "",
	       (out & OUT_B_TRUSTED) != 0u ? " B+" : "");
}

/* Every value a cut can leave: one value, or every torn result of op over
 * s; *inherited false when one before the index program's end is not the
 * starting verified with a subset of the starting index's bits. */
static unsigned settle_cut(uint32_t s, const struct sel_op *op, uint32_t s0, bool before_index_end,
			   bool *inherited)
{
	unsigned out = 0;

	memset(cut_seen, 0, sizeof cut_seen);
	cut_nq = 0;
	if (op == NULL) {
		cut_reach(s, 1u, 0u);
	} else if (op->erase) {
		cut_erase(s, 1u, 0u);
	} else {
		uint32_t w = 0xFFFFu;

		for (unsigned j = op->at; j < (unsigned)op->at + op->n; j++)
			w &= ~(0xFFu << (8u * j)) | ((uint32_t)op->b[j] << (8u * j));
		cut_program(s, w, 1u, 0u);
	}
	for (unsigned q = 0; q < cut_nq; q++) {
		const uint32_t v = cut_queue[q];

		out |= outcome_of(v);
		if (before_index_end &&
		    ((v >> 8) != (s0 >> 8) || ((v & 0xFFu) & ~(s0 & 0xFFu)) != 0u))
			*inherited = false;
	}
	return out;
}

static void test_settle_cuts(void)
{
	static const uint8_t starts[][2] = {
		{ 0x03u, 0x01u }, { 0xFFu, 0x01u }, { 0x00u, 0x01u }, { 0xFFu, 0xFFu },
		{ 0x05u, 0x03u }, { 0x00u, 0xFFu },
	};
	uint8_t out[512];

	printf("  settling after a failed selector step for slot 1, cut at each op:\n");
	for (size_t c = 0; c < ARRAY_LEN(starts); c++) {
		const uint32_t s0 = (uint32_t)starts[c][0] | (uint32_t)starts[c][1] << 8;
		struct sel_op ops[SEL_OPS];
		unsigned n, index_op = 0u;
		uint32_t s = s0;
		bool inherited = true, index_done = (s0 & 0xFFu) == 0u;
		char what[48];

		setup(0u, true);
		fail_op = 1u;
		/* the erase reports success and reads back wrong; {FF, FF} would
		 * read right, so there it reports a failure */
		fail_ok = s0 != 0xFFFFu;
		fail_left[0] = starts[c][0];
		fail_left[1] = starts[c][1];
		(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
		CHECK(out[0] == (uint8_t)(fail_ok ? S02_ERR_VERIFY : S02_ERR_IO));
		CHECK(mem[S02_SELECTOR_OFF] == 0x00u && mem[S02_SELECTOR_OFF + 1u] == 0x01u);
		CHECK(only_allowed_changed(1u) && !held_until_restart);
		n = sel_nlog - 1u;		/* the settling's ops, after the failed erase */
		memcpy(ops, sel_log + 1, n * sizeof ops[0]);
		for (unsigned i = 0; i < n; i++) {
			CHECK(!ops[i].erase);
			if (ops[i].at == 0u)
				index_op = i + 1u;
		}
		printf("   from {%02x, %02x}, programs alone:\n", starts[c][0], starts[c][1]);
		for (unsigned i = 0; i < n; i++) {
			unsigned o;

			snprintf(what, sizeof what, "before op %u (%s)", i + 1u,
				 ops[i].at == 0u ? "index 00" : "verified 01");
			o = settle_cut(s, NULL, s0, !index_done, &inherited);
			outcomes_print(what, o);
			if (index_done)
				CHECK((o & (OUT_B_ONCE | OUT_B_TRUSTED)) == 0u);
			snprintf(what, sizeof what, "op %u torn", i + 1u);
			o = settle_cut(s, &ops[i], s0, !index_done && i + 1u <= index_op, &inherited);
			outcomes_print(what, o);
			if (index_done)
				CHECK((o & (OUT_B_ONCE | OUT_B_TRUSTED)) == 0u);
			for (unsigned j = ops[i].at; j < (unsigned)ops[i].at + ops[i].n; j++)
				s &= ~(0xFFu << (8u * j)) | ((uint32_t)ops[i].b[j] << (8u * j));
			if (i + 1u == index_op)
				index_done = true;
		}
		outcomes_print("after the last", outcome_of(s));
		CHECK(s == 0x0100u);
		CHECK(outcome_of(s) == OUT_A_TRUSTED);
		CHECK(inherited);
	}
}

/*
 * S-02.102 item 6, the case where the settling writes nothing: the failed
 * step left `verified` with bit 0 clear. The selector stays as it is, the
 * log says so, and every later trial or commit is refused, with nothing
 * written, until a restart (here: the engine's state cleared); the status
 * form still answers.
 */
static void test_settle_held(void)
{
	static const uint8_t lefts[][2] = { { 0x00u, 0x00u }, { 0x05u, 0xFEu }, { 0xFFu, 0xFEu },
					    { 0x00u, 0xFEu } };
	uint8_t out[512];

	for (size_t c = 0; c < ARRAY_LEN(lefts); c++) {
		setup(0u, true);
		fail_op = 1u;
		fail_ok = true;
		fail_left[0] = lefts[c][0];
		fail_left[1] = lefts[c][1];
		(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
		CHECK(out[0] == (uint8_t)S02_ERR_VERIFY && sel_nlog == 1u);
		CHECK(out[2] == lefts[c][0] && out[3] == lefts[c][1]);
		CHECK(held_until_restart);
		memcpy(before, mem, sizeof mem);
		writes = 0;
		sel_nlog = 0;
		for (unsigned form = OPXY_INSTALL_TRIAL; form <= OPXY_INSTALL_COMMIT; form++) {
			(void)opxy_installer_write(&m, form, true, out);
			CHECK(out[0] == OPXY_INSTALL_REFUSED_HELD);
			CHECK(writes == 0u && sel_nlog == 0u && memcmp(mem, before, sizeof mem) == 0);
		}
		(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
		CHECK(out[0] == S02_OK && writes == 0u);
		/* after a restart, whose first stage settles the selector */
		held_until_restart = false;
		mem[S02_SELECTOR_OFF] = 0x00u;
		mem[S02_SELECTOR_OFF + 1u] = 0x01u;
		(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
		CHECK(out[0] == S02_OK && !held_until_restart);
	}
}

/*
 * Slot index 1's rescue, after an erase that failed reading {03, 01}: the
 * index program read back and made again when it does not read 0x00. One
 * failed rescue program is made good by the second. When both fail, nothing
 * more is tried: {03, 01} stays, trusting slot 1 until a reset's first stage,
 * which then boots the trial once with no check, as a torn erase here would.
 */
static void test_trial_rescue_faults(void)
{
	uint8_t out[512];

	setup(0u, true);
	fail_op = 1u;
	fail_more = 1u;			/* the erase and the first rescue program */
	fail_left[0] = 0x03u;
	fail_left[1] = 0x01u;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == (uint8_t)S02_ERR_IO);
	CHECK(sel_nlog == 3u && !sel_log[2].erase && sel_log[2].b[0] == 0x00u);
	CHECK(out[2] == 0x00u && out[3] == 0x01u);

	setup(0u, true);
	fail_op = 1u;
	fail_more = 2u;			/* both rescue programs too */
	fail_left[0] = 0x03u;
	fail_left[1] = 0x01u;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == (uint8_t)S02_ERR_IO);
	CHECK(sel_nlog == 3u);
	CHECK(out[2] == 0x03u && out[3] == 0x01u);
	CHECK(trusts(0x0103u, 1u));		/* what is left */
	CHECK(!right_crc_seen);
}

/*
 * The record: a program that reports success but stores a wrong length is
 * caught by the upgrade's read-back through the gate, before the log byte
 * and the selector; trial_crc is cleared after it, so a later read through
 * the gate gives the flash's bytes. trial_finish's own check refuses a
 * record that does not read as the trial's.
 */
static void test_trial_record_faults(void)
{
	struct gate g = { &m.hw.flash, 0u, 0u, false, false, false, 0u, false, false };
	const struct s02_flash_ops ops = { &g, gate_read, gate_erase, gate_program };
	struct s02_env env;
	uint8_t out[512], raw[S02_RECORD_SIZE] = { 0 };
	unsigned target = 2u;

	setup(0u, true);
	fail_rec = true;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == (uint8_t)S02_ERR_VERIFY);
	CHECK(sel_nlog == 0u && log_prog_n == 0u);
	CHECK(out[2] == 0x00u && out[3] == 0x01u);
	CHECK(out[5] == 0u);			/* slot 1: not good, no trial */
	CHECK(!right_crc_seen);

	setup(0u, true);
	fail_rec = true;
	env = m.flash;
	env.flash = &ops;
	g.slot = s02_slot_base(1);
	g.record = s02_record_off(1);
	g.armed = true;
	CHECK(trial_upgrade(&env, &g, &target) == S02_ERR_VERIFY && !g.trial_crc);
	CHECK(gate_read(&g, s02_record_off(1), raw, sizeof raw) == 0);
	CHECK(memcmp(raw, mem + s02_record_off(1), sizeof raw) == 0);	/* not inverted */
	CHECK(get32(raw) == ~opxy_install_crc);

	/* The record as the trial's but for its crc: trial_finish refuses. */
	setup(0u, true);
	memcpy(mem + s02_slot_base(1), opxy_install_image, opxy_install_len);
	put32(mem + s02_record_off(1), opxy_install_crc);
	put32(mem + s02_record_off(1) + 4, opxy_install_len);
	put32(mem + s02_record_off(1) + 8, S02_RECORD_TAG);
	memcpy(before, mem, sizeof mem);
	g.selector_open = false;
	g.log_only = false;
	CHECK(trial_finish(&env, &g, 1u) == S02_ERR_VERIFY);
	CHECK(sel_nlog == 0u && log_prog_n == 0u && memcmp(mem, before, sizeof mem) == 0);
	put32(mem + s02_record_off(1), ~opxy_install_crc);
	put32(mem + s02_record_off(1) + 4, opxy_install_len - 1u);	/* its length */
	CHECK(trial_finish(&env, &g, 1u) == S02_ERR_VERIFY);
	CHECK(sel_nlog == 0u && log_prog_n == 0u);
}

/* One reset of the factory first stage on mem, its write whole (S-02.020 to
 * S-02.023). */
static void first_stage_reset(void)
{
	const uint8_t idx = mem[S02_SELECTOR_OFF];
	const unsigned s = s02_selected_slot(idx);
	const uint8_t *r = mem + s02_record_off(s);
	bool pass;

	if (mem[S02_SELECTOR_OFF + 1u] == 0x01u)
		return;
	pass = get32(r + 8) == S02_RECORD_TAG && get32(r + 4) <= S02_SLOT_CAPACITY &&
	       opxy_crc32_long(0u, mem + s02_slot_base(s), get32(r + 4)) == get32(r);
	memset(mem + S02_SELECTOR_OFF, 0xFF, S02_SECTOR_SIZE);
	mem[S02_SELECTOR_OFF] = pass ? idx : (idx == 0x00u ? 0x01u : 0x00u);
	mem[S02_SELECTOR_OFF + 1u] = 0x01u;
}

/* The installer run again after a cut: its start condition (a verified
 * selector naming a good slot) refuses or starts over, never writes from a
 * selector that names the trial slot. */
static void test_rerun_after_cut(void)
{
	uint8_t out[512];

	/* k = 0, cut after the selector erase: {FF, FF}. Run again at once:
	 * not verified, nothing written. */
	setup(1u, true);
	fail_op = 1u;
	fail_left[0] = 0xFFu;
	fail_left[1] = 0xFFu;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[2] == 0xFFu && out[3] == 0xFFu);
	writes = 0;
	sel_nlog = 0;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == (uint8_t)S02_ERR_START && writes == 0u);

	/* After a reset: slot 1 passes, {FF, 01}; the run starts over, k = 0. */
	first_stage_reset();
	CHECK(mem[S02_SELECTOR_OFF] == 0xFFu && mem[S02_SELECTOR_OFF + 1u] == 0x01u);
	sel_nlog = 0;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK && out[6] == 0u);
	CHECK(out[2] == 0x00u && out[3] == 0xFEu && out[4] == (OPXY_INSTALL_SLOT_TRIAL | OPXY_INSTALL_SLOT_PENDING));

	/* k = 0, cut in the index program: {7F, FE}. A reset checks slot 1,
	 * passes and writes {7F, 01}; the run starts over. */
	setup(1u, true);
	fail_op = 3u;
	fail_left[0] = 0x7Fu;
	fail_left[1] = 0xFEu;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	first_stage_reset();
	CHECK(mem[S02_SELECTOR_OFF] == 0x7Fu && mem[S02_SELECTOR_OFF + 1u] == 0x01u);
	sel_nlog = 0;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK && out[2] == 0x00u && out[3] == 0xFEu);

	/* k = 1, the window: the erase torn to {03, 01}, trusting the trial slot.
	 * Run again: the selected slot is slot 1, bad, so nothing is written. */
	setup(0u, true);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	mem[S02_SELECTOR_OFF] = 0x03u;
	mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	writes = 0;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == (uint8_t)S02_ERR_START && writes == 0u);
}

/* A carried image of n bytes: the 52-byte stream, then other bytes after
 * its FINAL block; stream false spoils its first header's checksum. Its
 * CRC is its own either way. */
static void image_of(uint32_t n, bool stream)
{
	image_base();
	for (uint32_t i = sizeof base_image; i < n; i++)
		img[i] = (uint8_t)(i * 13u + 5u);
	if (!stream)
		img[2] ^= 0x01u;
	img_len = n;
	img_crc = opxy_crc32_long(0u, img, n);
}

/* S-02.102 item 4, S-02.104: the image's checks, all in memory, come
 * before the selector is read, and a mismatch at any of them refuses with
 * nothing written: a CRC that is not the carried one, a length over
 * 0x76000, a stream the boot ROM would not start (S-02.091), for a commit
 * as for a trial. With the selector unreadable, the image's refusal is the
 * one reported. */
static void test_image_checks_first(void)
{
	static const struct {
		uint32_t n;
		bool stream, crc_ok;
		uint8_t rc;
	} cases[] = {
		{ 52u, true, false, (uint8_t)S02_ERR_VERIFY },
		{ 0x76001u, true, true, (uint8_t)S02_ERR_SIZE },
		{ 0x77000u, true, true, (uint8_t)S02_ERR_SIZE },
		{ 0x78000u, true, true, (uint8_t)S02_ERR_SIZE },
		{ 52u, false, true, (uint8_t)S02_ERR_STREAM },
		{ 0x1000u, false, true, (uint8_t)S02_ERR_STREAM },
	};
	uint8_t out[512];

	for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
		for (unsigned form = 0; form < 2u; form++) {
			for (int unreadable = 0; unreadable < 2; unreadable++) {
				setup(0u, true);
				if (form == 1u) {
					/* a trial of the build's image, started and reached */
					(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
					CHECK(out[0] == S02_OK);
					mem[S02_SELECTOR_OFF] = 0x00u;
					mem[S02_SELECTOR_OFF + 1u] = 0x01u;
					trial_start(1u);
					memcpy(before, mem, sizeof mem);
				}
				image_of(cases[i].n, cases[i].stream);
				if (!cases[i].crc_ok)
					img_crc ^= 0x10u;
				writes = 0;
				sel_nlog = 0;
				sel_read_fails = unreadable != 0;
				(void)opxy_installer_write(&m, form == 0u ? OPXY_INSTALL_TRIAL :
						   OPXY_INSTALL_COMMIT, true, out);
				CHECK(out[0] == cases[i].rc);
				CHECK(writes == 0u && sel_nlog == 0u);
				CHECK(memcmp(mem, before, sizeof mem) == 0);
			}
		}
	}
	/* a good image with the selector unreadable: the read's failure */
	setup(0u, true);
	sel_read_fails = true;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == (uint8_t)S02_ERR_IO && writes == 0u);
}

/* S-02.103, S-02.104: an image of 0x76000 bytes is taken, and the trial
 * erases slot k's settings sector with its last sector. */
static void test_trial_settings_sector(void)
{
	const uint32_t set = s02_slot_base(1u) + S02_STORE_SLOT_MAX;
	uint8_t out[512];

	setup(0u, true);
	image_of(S02_STORE_SLOT_MAX, true);
	memset(mem + set, 0x00, S02_SECTOR_SIZE);	/* another image's settings */
	memcpy(before, mem, sizeof mem);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK && out[2] == 0xFFu && out[3] == 0xFFu);
	CHECK(memcmp(mem + s02_slot_base(1), img, img_len) == 0);
	for (uint32_t i = 0; i < S02_SECTOR_SIZE; i++)
		CHECK(mem[set + i] == 0xFFu);
	CHECK(only_allowed_changed(1u));
	/* slot 0's settings sector, the running slot's, untouched */
	CHECK(memcmp(mem + s02_slot_base(0u) + S02_STORE_SLOT_MAX,
		     before + s02_slot_base(0u) + S02_STORE_SLOT_MAX, S02_SECTOR_SIZE) == 0);
}

/* S-02.102 item 7, S-02.047: the commit reads slot k's n bytes back after
 * the record's program; bytes that no longer match refuse it there, with
 * the selector untouched. */
static void test_commit_read_back(void)
{
	uint8_t out[512];

	setup(0u, true);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK);
	mem[S02_SELECTOR_OFF] = 0x00u;
	mem[S02_SELECTOR_OFF + 1u] = 0x01u;
	trial_start(1u);
	sel_nlog = 0;
	spoil_on_record = true;
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == (uint8_t)S02_ERR_VERIFY);
	CHECK(sel_nlog == 0u && mem[S02_SELECTOR_OFF] == 0x00u &&
	      mem[S02_SELECTOR_OFF + 1u] == 0x01u);
	/* the record was programmed: the read-back came after it */
	CHECK(get32(mem + s02_record_off(1)) == opxy_install_crc);
}

/* No write when the selected slot is not the way back, or the carried image
 * did not pass its check. */
static void test_refusals(void)
{
	uint8_t out[512];

	setup(0u, true);				/* the image not verified */
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, false, out);
	CHECK(out[0] == OPXY_INSTALL_REFUSED_IMAGE);
	CHECK(writes == 0u && memcmp(mem, before, sizeof mem) == 0);
	trial_start(1u);
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, false, out);
	CHECK(out[0] == OPXY_INSTALL_REFUSED_IMAGE);
	CHECK(writes == 0u);
	(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, false, out);
	CHECK(out[0] == S02_OK);

	setup(1u, false);				/* the selected slot is bad */
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == (uint8_t)S02_ERR_START);
	CHECK(writes == 0u);
	CHECK(memcmp(mem, before, sizeof mem) == 0);

	setup(0u, true);
	mem[S02_SELECTOR_OFF + 1u] = 0x00u;		/* not verified */
	memcpy(before, mem, sizeof mem);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == (uint8_t)S02_ERR_START);
	CHECK(writes == 0u);

	setup(0u, true);				/* a commit with no trial */
	(void)opxy_installer_write(&m, OPXY_INSTALL_COMMIT, true, out);
	CHECK(out[0] == (uint8_t)S02_ERR_START);
	CHECK(writes == 0u);

	setup(0u, true);				/* the status form writes nothing */
	(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
	CHECK(out[0] == S02_OK);
	CHECK(out[4] == OPXY_INSTALL_SLOT_GOOD);
	CHECK(writes == 0u);
}

/* --- the devices (S-18) --- */

static size_t put_block(uint8_t *p, uint32_t flags, uint32_t target, const uint8_t *pl, uint32_t n)
{
	uint8_t x = 0;

	put32(p, 0xAD000004u | flags);
	put32(p + 4, target);
	put32(p + 8, n);
	put32(p + 12, 0u);
	for (unsigned i = 0; i < 16u; i++)
		x ^= p[i];
	p[2] = x;
	if (pl != NULL)
		memcpy(p + 16, pl, n);
	return 16u + (pl != NULL ? n : 0u);
}

/* Slot s holds a waffle-boot stream, one with the image descriptor at
 * FR_DESC_ADDR, and its right record. */
static void waffle_slot(unsigned s)
{
	uint8_t *slot = mem + s02_slot_base(s), *rec = mem + s02_record_off(s);
	uint8_t d[FR_DESC_HEAD + 8u + 4u];
	size_t n = 0;

	memset(slot, 0xFF, S02_SLOT_CAPACITY - S02_SECTOR_SIZE);
	memset(d, 0, sizeof d);
	put32(d + FR_DESC_O_MAGIC, FR_DESC_MAGIC);
	d[FR_DESC_O_VERSION] = (uint8_t)FR_DESC_VERSION;
	d[FR_DESC_O_SIZE] = (uint8_t)sizeof d;
	put32(d + FR_DESC_O_NREGIONS, 1u);
	put32(d + sizeof d - 4u, opxy_crc32_long(0u, d, sizeof d - 4u));
	n += put_block(slot + n, S05_FLAG_FIRST | S05_FLAG_IGNORE, 0x11A00000u, NULL, 0u);
	n += put_block(slot + n, 0u, FR_DESC_ADDR, d, sizeof d);
	n += put_block(slot + n, S05_FLAG_FINAL, 0x11A00000u, NULL, 0u);
	put32(rec, opxy_crc32_long(0u, slot, (uint32_t)n));
	put32(rec + 4, (uint32_t)n);
	put32(rec + 8, S02_RECORD_TAG);
	memcpy(before, mem, sizeof mem);
}

/* S-02.108: the OP-1F keeps a slot whose image is not a waffle-boot
 * stream, in the four cases of the two slots; the OP-XY keeps its rule. */
static void test_op1f_keeps_factory(void)
{
	uint8_t out[OPXY_INSTALL_REPLY];

	/* neither slot waffle-boot's: the selected one stays the unit's own */
	setup(0u, true);
	m.dev = &wb_device_op1f;
	(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
	CHECK(out[0] == S02_OK && out[OPXY_INSTALL_STREAMS] == 0u);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK);
	CHECK(only_allowed_changed(1u));

	/* waffle-boot in the selected slot, the unit's own in the other: refused */
	setup(0u, true);
	waffle_slot(0u);
	m.dev = &wb_device_op1f;
	(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
	CHECK(out[0] == S02_OK && out[OPXY_INSTALL_STREAMS] == 0x01u);
	CHECK((out[4] & OPXY_INSTALL_SLOT_GOOD) != 0u);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == OPXY_INSTALL_REFUSED_FACTORY);
	CHECK(writes == 0u && memcmp(mem, before, sizeof mem) == 0);
	CHECK(out[OPXY_INSTALL_STREAMS] == 0x01u);
	/* the same flash on the OP-XY: written, as before */
	m.dev = NULL;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK);

	/* both waffle-boot's: nothing of the unit's own is left to keep */
	setup(1u, true);
	waffle_slot(1u);
	waffle_slot(0u);
	m.dev = &wb_device_op1f;
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK && out[6] == 0u);
	CHECK(only_allowed_changed(0u));

	/* waffle-boot in the target alone: the selected slot, the unit's own,
	 * stays */
	setup(0u, true);
	waffle_slot(1u);
	m.dev = &wb_device_op1f;
	(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
	CHECK(out[0] == S02_OK && out[OPXY_INSTALL_STREAMS] == 0x02u);
	(void)opxy_installer_write(&m, OPXY_INSTALL_TRIAL, true, out);
	CHECK(out[0] == S02_OK);
	CHECK(only_allowed_changed(1u));
}

/* S-02.063 at the device's record size: the OP-1F's 0x28 bytes. */
static void test_mirror_size(void)
{
	uint8_t out[OPXY_INSTALL_REPLY];
	uint8_t rec[0x28];

	setup(0u, true);
	for (unsigned i = 0; i < sizeof rec; i++)
		rec[i] = (uint8_t)(i * 3u + 1u);
	put32(rec, S02_META_MAGIC);
	memcpy(mem + S02_META_OFF, rec, sizeof rec);
	memcpy(mem + S02_MIRROR_OFF, rec, sizeof rec);
	for (unsigned i = 0; i < sizeof rec; i++)
		mem[S02_MIRROR_OFF + sizeof rec + i] = (uint8_t)~rec[i];
	m.flash.meta_size = 0x28u;
	(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
	CHECK(out[8] == OPXY_INSTALL_MIRROR_SAME);
	mem[S02_META_OFF + 0x27u] ^= 1u;
	(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
	CHECK(out[8] == OPXY_INSTALL_MIRROR_DIFFERS);
	/* the OP-XY's 0x34 bytes: that mirror is not one */
	m.flash.meta_size = 0x34u;
	(void)opxy_installer_write(&m, OPXY_INSTALL_STATUS, true, out);
	CHECK(out[8] == OPXY_INSTALL_MIRROR_INVALID);
	CHECK(writes == 0u);
}

/* The survey's flash part: read, nothing written. */
static void test_survey_read(void)
{
	struct inst_survey sv;
	struct s02_record r0;

	setup(0u, true);
	waffle_slot(1u);
	mem[log_byte(0u)] = 0xF0u;
	mem[log_byte(0u) + 1u] = 0x3Fu;
	memcpy(before, mem, sizeof mem);
	memset(&sv, 0xEE, sizeof sv);
	opxy_installer_survey_read(&m, &sv);
	CHECK(sv.sel_read && sv.sel_index == 0u && sv.sel_verified == 0x01u);
	s02_record_decode(mem + s02_record_off(0), &r0);
	CHECK(sv.slot[0].bits == (INST_SV_RECORD | INST_SV_GOOD | INST_SV_LOG));
	CHECK(sv.slot[0].crc == r0.crc32 && sv.slot[0].length == 0x1800u &&
	      sv.slot[0].tag == S02_RECORD_TAG);
	CHECK(sv.slot[0].log[0] == 0xF0u && sv.slot[0].log[1] == 0x3Fu && sv.slot[0].log[2] == 0xFFu);
	CHECK(sv.slot[1].bits == (INST_SV_RECORD | INST_SV_GOOD | INST_SV_WAFFLE | INST_SV_LOG));
	CHECK(writes == 0u && memcmp(mem, before, sizeof mem) == 0);
	/* a selector that does not read */
	sel_read_fails = true;
	opxy_installer_survey_read(&m, &sv);
	CHECK(!sv.sel_read && sv.sel_index == 0xFFu);
}

int main(void)
{
	RUN(test_image);
	RUN(test_gate);
	RUN(test_trial_and_commit);
	RUN(test_second_trial);
	RUN(test_trial_slot0);
	RUN(test_commit_needs_start);
	RUN(test_trial_same_image);
	RUN(test_trial_mark_cases);
	RUN(test_trial_log_faults);
	RUN(test_trial_record_faults);
	RUN(test_trial_rescue_faults);
	RUN(test_trial_cuts);
	RUN(test_trial_faults);
	RUN(test_settle_cuts);
	RUN(test_settle_held);
	RUN(test_rerun_after_cut);
	RUN(test_refusals);
	RUN(test_image_checks_first);
	RUN(test_trial_settings_sector);
	RUN(test_commit_read_back);
	RUN(test_op1f_keeps_factory);
	RUN(test_mirror_size);
	RUN(test_survey_read);
	return check_summary("test_board_installer_engine");
}
