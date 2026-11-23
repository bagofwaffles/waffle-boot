/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The check suite's table, verdicts, detail lines, summary pages, report
 * and the Nordic link's gate (waffle/board/testos_check.h). Pure: the host's
 * unit tests include this file with OPXY_TESTOS_CHECK defined. Nothing is
 * compiled without it.
 */
#include "waffle/board/testos_check.h"

#if defined(OPXY_TESTOS_CHECK)

#include <string.h>

#include "waffle/s06_frame.h"
#include "waffle/s11_version.h"
#include "waffle/s17_revisions.h"

const struct tchk_info tchk_tests[TCHK_COUNT] = {
	[TCHK_OS_PARTITION] = { "os-partition", "OS partition", TCHK_KIND_OS },
	[TCHK_CLOCKS] = { "clocks", "Clocks", TCHK_KIND_AUTO },
	[TCHK_DDR] = { "ddr", "DDR", TCHK_KIND_AUTO },
	[TCHK_CORE_STATE] = { "core-state", "Core state", TCHK_KIND_AUTO },
	[TCHK_FLASH_ID] = { "flash-id", "Flash ID", TCHK_KIND_AUTO },
	[TCHK_FLASH_LAYOUT] = { "flash-layout", "Flash layout", TCHK_KIND_AUTO },
	[TCHK_PSOC] = { "psoc", "PSoC", TCHK_KIND_AUTO },
	[TCHK_SLAVE] = { "slave", "Second processor", TCHK_KIND_AUTO },
	[TCHK_BRIDGE] = { "bridge", "Bridge", TCHK_KIND_AUTO },
	[TCHK_EMMC_CONFIG] = { "emmc-config", "eMMC setup", TCHK_KIND_AUTO },
	[TCHK_EMMC_BOOT] = { "emmc-boot", "eMMC boot areas", TCHK_KIND_AUTO },
	[TCHK_EMMC_VOLUME] = { "emmc-volume", "eMMC volume", TCHK_KIND_AUTO },
	[TCHK_EMMC_FILE] = { "emmc-file", "eMMC file", TCHK_KIND_AUTO },
	[TCHK_PANEL] = { "panel", "Display", TCHK_KIND_AUTO },
	[TCHK_RTC] = { "rtc", "Clock (RTC)", TCHK_KIND_AUTO },
	[TCHK_NORDIC] = { "nordic", "Nordic", TCHK_KIND_AUTO },
	[TCHK_TIMING] = { "timing", "Timing", TCHK_KIND_AUTO },
	[TCHK_KEYS] = { "keys", "Keys", TCHK_KIND_GUIDED },
};

/* --- the formatters --- */

void tchk_text_init(struct tchk_text *t, uint16_t cap)
{
	t->n = 0;
	t->cap = cap < TCHK_MORE_MAX ? cap : TCHK_MORE_MAX;
	t->cut = false;
	t->s[0] = '\0';
}

static void put_char(struct tchk_text *t, char c)
{
	if (t->n >= t->cap) {
		t->cut = true;
		return;
	}
	t->s[t->n++] = c;
	t->s[t->n] = '\0';
}

void tchk_put_str(struct tchk_text *t, const char *fixed)
{
	while (*fixed != '\0')
		put_char(t, *fixed++);
}

void tchk_put_dec(struct tchk_text *t, uint32_t v)
{
	char d[10];
	unsigned n = 0;

	do {
		d[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u);
	while (n > 0u)
		put_char(t, d[--n]);
}

void tchk_put_hex(struct tchk_text *t, uint32_t v, unsigned digits)
{
	static const char hex[] = "0123456789abcdef";
	unsigned n = 8u;

	while (n > 1u && (v >> (4u * (n - 1u))) == 0u && n > digits)
		n--;
	while (n > 0u) {
		n--;
		put_char(t, hex[(v >> (4u * n)) & 15u]);
	}
}

/* v tenths as "N.N" */
static void put_tenths(struct tchk_text *t, uint64_t tenths)
{
	tchk_put_dec(t, (uint32_t)(tenths / 10u));
	put_char(t, '.');
	put_char(t, (char)('0' + tenths % 10u));
}

void tchk_put_rate(struct tchk_text *t, uint64_t bytes, uint64_t us)
{
	if (us == 0u) {
		tchk_put_str(t, "? MB/s");
		return;
	}
	/* bytes per us is MB/s */
	put_tenths(t, bytes * 10u / us);
	tchk_put_str(t, " MB/s");
}

void tchk_put_ms(struct tchk_text *t, uint32_t us)
{
	if (us < 100000u)
		put_tenths(t, us / 100u);
	else
		tchk_put_dec(t, us / 1000u);
	tchk_put_str(t, " ms");
}

/* --- the table and the verdict --- */

void tchk_table_init(struct tchk_table *t)
{
	unsigned i;

	for (i = 0; i < TCHK_COUNT; i++) {
		t->r[i].state = TCHK_NOT_RUN;
		tchk_text_init(&t->r[i].detail, TCHK_DETAIL_COLS);
		tchk_text_init(&t->r[i].more, TCHK_MORE_MAX);
	}
	for (i = 0; i < 2u; i++)
		tchk_text_init(&t->emmc_fault[i], TCHK_MORE_MAX);
}

struct tchk_text *tchk_set(struct tchk_table *t, enum tchk_id id, enum tchk_state s)
{
	t->r[id].state = (uint8_t)s;
	tchk_text_init(&t->r[id].detail, TCHK_DETAIL_COLS);
	tchk_text_init(&t->r[id].more, TCHK_MORE_MAX);
	return &t->r[id].detail;
}

enum tchk_verdict tchk_verdict(const struct tchk_table *t)
{
	bool gap = false;
	unsigned i;

	for (i = 0; i < TCHK_COUNT; i++) {
		const uint8_t s = t->r[i].state;

		if (tchk_tests[i].kind == TCHK_KIND_OS || s == TCHK_NOT_ON_DEVICE)
			continue;
		if (s == TCHK_FAIL || s == TCHK_NOT_PASSED)
			return TCHK_VERDICT_FAIL;
		if (tchk_tests[i].kind == TCHK_KIND_AUTO && s != TCHK_PASS)
			gap = true;
	}
	return gap ? TCHK_VERDICT_GAPS : TCHK_VERDICT_PASS;
}

const char *tchk_verdict_word(enum tchk_verdict v)
{
	return v == TCHK_VERDICT_PASS ? "PASS" : v == TCHK_VERDICT_GAPS ? "PASS WITH GAPS" : "FAIL";
}

const char *tchk_state_word(enum tchk_state s)
{
	switch (s) {
	case TCHK_PASS:
		return "pass";
	case TCHK_FAIL:
		return "FAIL";
	case TCHK_NOT_TESTED:
		return "not tested";
	case TCHK_NOT_PASSED:
		return "not passed";
	case TCHK_NOT_ON_DEVICE:
		return "n/a";
	case TCHK_NOT_RUN:
	default:
		return "not run";
	}
}

/* --- the Nordic link's gate --- */

bool tchk_nordic_allowed(const uint8_t *wire, size_t n, unsigned *id)
{
	uint8_t raw[S06_MAX_RAW];
	size_t i = 0, k = 0, len;

	*id = 0xFFFFu;
	/* COBS (S-06.004): the frame, then one 0x00, nothing after it */
	if (n < 2u || n > S06_MAX_WIRE || wire[n - 1u] != 0x00u)
		return false;
	n--;
	while (i < n) {
		const uint8_t code = wire[i++];
		uint8_t j;

		if (code == 0u)
			return false;
		for (j = 1; j < code; j++) {
			if (i >= n || k >= sizeof raw || wire[i] == 0u)
				return false;
			raw[k++] = wire[i++];
		}
		if (code != 0xFFu && i < n) {
			if (k >= sizeof raw)
				return false;
			raw[k++] = 0x00u;
		}
	}
	/* S-06.001: route, seq, id, len, payload, check */
	if (k < S06_HEADER + 1u)
		return false;
	len = raw[3];
	if (k != S06_HEADER + len + 1u)
		return false;
	*id = raw[2];
	switch (raw[2]) {
	case 0x05u:		/* S-08.002 */
		return len == 2u && raw[4] == 0x01u && raw[5] == 0x03u;
	case 0x06u:		/* S-08.011's answer in state 0 */
		return len == 1u && raw[4] == 0x40u;
	case 0x0Du:		/* S-08.011's other answer in state 0: leave the
				 * bootloader; every other 0x0D form is held back */
		return len == 1u && raw[4] == 0x18u;
	case 0x09u:		/* S-08.020 */
		return len == 0u;
	case 0x02u:		/* a ping's answer (S-07.076's shape) */
		return true;
	default:
		return false;
	}
}

/* --- the first stage --- */

#define STAGE_MAX 0xB000u	/* S-02.002 */
#define BLOCK_FINAL 0x8000u	/* S-05 Terms */
#define BLOCK_FILL 0x0100u

void tchk_first_stage(int (*read)(void *ctx, uint32_t off, uint8_t *buf, uint32_t len),
		      void *ctx, struct tchk_stage *out)
{
	uint32_t off = 0;

	memset(out, 0, sizeof(*out));
	out->codes_ok = true;
	while (off + 16u <= STAGE_MAX) {
		uint8_t h[16];
		uint8_t x = 0;
		uint32_t code, count;
		unsigned i;

		if (read(ctx, off, h, sizeof h) != 0)
			return;
		for (i = 0; i < sizeof h; i++)
			x ^= h[i];
		code = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 |
		       (uint32_t)h[3] << 24;
		count = (uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 |
			(uint32_t)h[11] << 24;
		if ((code >> 24) != 0xADu || x != 0u)
			return;
		out->blocks++;
		if ((code & 0xFu) != 4u)	/* S-01.020 */
			out->codes_ok = false;
		off += 16u;
		if ((code & BLOCK_FILL) == 0u) {
			if (count > STAGE_MAX - off)
				return;
			off += count;
		}
		if ((code & BLOCK_FINAL) != 0u) {
			out->parsed = true;
			out->len = off;
			return;
		}
	}
}

/* --- the registers at entry (S-13a's OSENTRY rows) --- */

unsigned tchk_reg_group(const char *req)
{
	unsigned v = 0, i;

	/* "S-13a.NNN" */
	if (strncmp(req, "S-13a.", 6) != 0)
		return TCHK_REGS_CORE;
	for (i = 6; req[i] >= '0' && req[i] <= '9'; i++)
		v = v * 10u + (unsigned)(req[i] - '0');
	if (v >= 2u && v <= 6u)
		return TCHK_REGS_CLOCKS;
	if (v >= 8u && v <= 17u)
		return TCHK_REGS_DDR;
	if (v == 80u)
		return TCHK_REGS_USB_PLL;
	return TCHK_REGS_CORE;
}

static uint32_t field_of(uint32_t v, unsigned msb, unsigned lsb)
{
	const unsigned width = msb - lsb + 1u;

	if (width >= 32u)
		return v;
	return (v >> lsb) & ((1u << width) - 1u);
}

static bool holds(const struct opxy_testos_reg *r, uint32_t f)
{
	switch (r->rule) {
	case OPXY_TESTOS_RULE_EQ:
		return f == r->lo;
	case OPXY_TESTOS_RULE_NE:
		return f != r->lo;
	case OPXY_TESTOS_RULE_GE:
		return f >= r->lo;
	case OPXY_TESTOS_RULE_LE:
		return f <= r->lo;
	case OPXY_TESTOS_RULE_IN:
		return r->lo <= f && f <= r->hi;
	case OPXY_TESTOS_RULE_ANY:
	default:
		return true;
	}
}

void tchk_regs_judge(const struct opxy_testos_reg *rows, unsigned n, const uint32_t *values,
		     struct tchk_regs out[TCHK_REGS_GROUPS])
{
	unsigned i;

	memset(out, 0, sizeof(*out) * TCHK_REGS_GROUPS);
	for (i = 0; i < n; i++) {
		const struct opxy_testos_reg *r = &rows[i];
		struct tchk_regs *g = &out[tchk_reg_group(r->req)];
		const uint32_t f = field_of(values[i], r->msb, r->lsb);

		if (r->rule == OPXY_TESTOS_RULE_ANY)
			continue;
		g->checked++;
		if (holds(r, f))
			continue;
		if (g->differ++ == 0u) {
			g->first = r;
			g->first_value = f;
		}
	}
}

/* --- the judges --- */

static bool ran(const struct tchk_facts *f, enum tchk_id id)
{
	return (f->ran >> id & 1u) != 0u;
}

static const struct wb_device *dev_of(const struct tchk_facts *f)
{
	return f->dev != NULL ? f->dev : &wb_device_opxy;
}

/* The slave and its bridge are parts of the OP-XY alone (S-18.020). */
static bool two_cpus(const struct tchk_facts *f)
{
	return wb_has(dev_of(f), WB_F_SECOND_CPU);
}

/* An eMMC test that could not run because the eMMC is out of reach. */
static const char *no_emmc(const struct tchk_facts *f)
{
	return two_cpus(f) ? "no slave" : "the eMMC did not come up";
}

static void not_on_device(struct tchk_table *t, enum tchk_id id)
{
	tchk_put_str(tchk_set(t, id, TCHK_NOT_ON_DEVICE), "not on this device");
}

/* An automatic test whose facts are not in: why. */
static void not_tested(struct tchk_table *t, enum tchk_id id, const char *why)
{
	tchk_put_str(tchk_set(t, id, TCHK_NOT_TESTED), why);
}

static const char *look_word(uint8_t look)
{
	switch (look) {
	case TCHK_LOOK_ZERO:
		return "all zero";
	case TCHK_LOOK_FF:
		return "all FF";
	case TCHK_LOOK_INACTIVE:
		return "a header, inactive";
	case TCHK_LOOK_ACTIVE:
		return "a header, active";
	case TCHK_LOOK_OTHER:
		return "other";
	default:
		return "not read";
	}
}

/*
 * What the other boot partition holds, reported apart from the hardware: an
 * OS there or none is no fault, and the eMMC's own faults are emmc-boot's.
 * Only a header write that failed is one.
 */
static void judge_os(const struct tchk_facts *f, struct tchk_table *t)
{
	const struct tchk_switch *sw = &f->sw;
	struct tchk_text *d;

	switch (sw->result) {
	case TCHK_SW_RESTORED:
		d = tchk_set(t, TCHK_OS_PARTITION, TCHK_PASS);
		tchk_put_str(d, sw->unverified ? "an image stored encrypted, made active" :
				 "an OS, checked and made active");
		break;
	case TCHK_SW_READY:
		d = tchk_set(t, TCHK_OS_PARTITION, TCHK_PASS);
		tchk_put_str(d, sw->unverified ? "an image stored encrypted: made active at the end" :
				 "an OS, checked: made active at the end");
		break;
	case TCHK_SW_NO_OTHER_OS:
		d = tchk_set(t, TCHK_OS_PARTITION, TCHK_PASS);
		tchk_put_str(d, "no OS header in the other partition");
		break;
	case TCHK_SW_OTHER_BAD:
		d = tchk_set(t, TCHK_OS_PARTITION, TCHK_PASS);
		tchk_put_str(d, "the other partition's image: its CRC differs");
		break;
	case TCHK_SW_WRITE_FAILED:
		d = tchk_set(t, TCHK_OS_PARTITION, TCHK_FAIL);
		tchk_put_str(d, "a header write failed: the installer still starts");
		break;
	case TCHK_SW_NOT_ACTIVE:
		d = tchk_set(t, TCHK_OS_PARTITION, TCHK_PASS);
		tchk_put_str(d, "the installer's own header not found active");
		break;
	case TCHK_SW_NO_SLAVE:
		d = tchk_set(t, TCHK_OS_PARTITION, TCHK_NOT_TESTED);
		tchk_put_str(d, "not read: the eMMC could not be reached");
		break;
	case TCHK_SW_IO:
	default:
		d = tchk_set(t, TCHK_OS_PARTITION, TCHK_NOT_TESTED);
		tchk_put_str(d, "not read: a boot partition read failed");
		break;
	}
	d = &t->r[TCHK_OS_PARTITION].more;
	tchk_put_str(d, "other partition's header: ");
	tchk_put_str(d, look_word(sw->look));
	tchk_put_str(d, sw->unverified ? ", stored encrypted, its CRC not checked" : "");
	if (sw->mine != 0u) {
		tchk_put_str(d, "; suite in BP");
		tchk_put_dec(d, sw->mine);
	}
	tchk_put_str(d, sw->wrote_a ? "; OS header set active" : "");
	tchk_put_str(d, sw->wrote_b ? "; suite header set inactive" : "");
	tchk_put_str(d, sw->b_failed ? "; the suite's header could not be cleared" : "");
	tchk_put_str(d, sw->undo_failed ? "; the undo failed" : "");
	tchk_put_str(d, sw->select0_failed ? "; user area not selected again" : "");
}

static void judge_regs(const struct tchk_facts *f, struct tchk_table *t, enum tchk_id id,
		       unsigned group)
{
	const struct tchk_regs *g = &f->regs[group];
	struct tchk_text *d;

	if (g->differ == 0u) {
		d = tchk_set(t, id, TCHK_PASS);
		tchk_put_dec(d, g->checked);
		tchk_put_str(d, " fields as S-13a states");
	} else {
		d = tchk_set(t, id, TCHK_FAIL);
		tchk_put_dec(d, g->differ);
		tchk_put_str(d, " of ");
		tchk_put_dec(d, g->checked);
		tchk_put_str(d, " fields differ from S-13a");
		d = &t->r[id].more;
		tchk_put_str(d, "first: ");
		tchk_put_str(d, g->first->reg);
		tchk_put_str(d, " ");
		tchk_put_str(d, g->first->field);
		tchk_put_str(d, " ");
		tchk_put_hex(d, g->first_value, 1u);
		tchk_put_str(d, ", wanted ");
		tchk_put_hex(d, g->first->lo, 1u);
		if (g->first->rule == OPXY_TESTOS_RULE_IN) {
			tchk_put_str(d, "..");
			tchk_put_hex(d, g->first->hi, 1u);
		}
		tchk_put_str(d, " (");
		tchk_put_str(d, g->first->req);
		tchk_put_str(d, ")");
	}
	if (id == TCHK_CORE_STATE && f->regs[TCHK_REGS_USB_PLL].differ != 0u) {
		const struct tchk_regs *u = &f->regs[TCHK_REGS_USB_PLL];

		/*
		 * SPEC-Q: S-13a.080 states USB0_PLL_OSC holds its reset value at
		 * OS entry, and no stage of waffle-boot writes it; the suite
		 * reads it at the OS's entry. A unit read another value while
		 * USB worked. Whether a stage before waffle-boot sets it, or
		 * the part's reset value differs from the HRM's, is not known:
		 * reported here, not judged.
		 */
		tchk_put_str(&t->r[id].detail, "; USB PLL differs, noted");
		d = &t->r[id].more;
		tchk_put_str(d, d->n != 0u ? "; " : "");
		tchk_put_str(d, u->first->reg);
		tchk_put_str(d, " ");
		tchk_put_str(d, u->first->field);
		tchk_put_str(d, " ");
		tchk_put_hex(d, u->first_value, 1u);
		tchk_put_str(d, ", reset value ");
		tchk_put_hex(d, u->first->lo, 1u);
		tchk_put_str(d, " (");
		tchk_put_str(d, u->first->req);
		tchk_put_str(d, ", not judged)");
	}
	if (id == TCHK_DDR) {
		d = &t->r[id].detail;
		tchk_put_str(d, f->ddr_aliased ? "; 0x84000000 aliases" : "; no alias");
		if (f->ddr_aliased)
			t->r[id].state = TCHK_FAIL;
	}
}

static void judge_flash(const struct tchk_facts *f, struct tchk_table *t)
{
	struct tchk_text *d;
	unsigned sel;

	if (ran(f, TCHK_FLASH_ID)) {
		const bool zero = f->jedec[0] == 0u && f->jedec[1] == 0u && f->jedec[2] == 0u;
		const bool all_ff = f->jedec[0] == 0xFFu && f->jedec[1] == 0xFFu && f->jedec[2] == 0xFFu;
		const bool ok = f->flash_id_read && !zero && !all_ff;
		unsigned i;

		d = tchk_set(t, TCHK_FLASH_ID, ok ? TCHK_PASS : TCHK_FAIL);
		tchk_put_str(d, f->flash_id_read ? "JEDEC id" : "RDID not answered");
		for (i = 0; f->flash_id_read && i < 3u; i++) {
			tchk_put_str(d, " ");
			tchk_put_hex(d, f->jedec[i], 2u);
		}
		/* S-13b.018: informative, no expected value */
		if (f->flash_sr_read) {
			/* BP3-BP0, bits 5-2: 0000 and 1111 protect nothing on
			 * the part the id names (its data sheet's table) */
			const unsigned bp = ((unsigned)f->flash_sr >> 2) & 0x0Fu;

			tchk_put_str(d, "; status ");
			tchk_put_hex(d, f->flash_sr, 2u);
			tchk_put_str(d, bp == 0u || bp == 0x0Fu ? ", no block protected" :
					", block protect bits set");
		}
	} else {
		not_tested(t, TCHK_FLASH_ID, "not run");
	}

	if (!ran(f, TCHK_FLASH_LAYOUT)) {
		not_tested(t, TCHK_FLASH_LAYOUT, "not run");
		return;
	}
	if (!f->flash_read) {
		tchk_put_str(tchk_set(t, TCHK_FLASH_LAYOUT, TCHK_FAIL), "the flash could not be read");
		return;
	}
	/* S-02.010: index 0 is slot 0, anything else slot 1 */
	sel = f->sel_index == 0u ? 0u : 1u;
	d = tchk_set(t, TCHK_FLASH_LAYOUT, f->slot_good[sel] ? TCHK_PASS : TCHK_FAIL);
	tchk_put_str(d, "selected slot ");
	tchk_put_dec(d, sel + 1u);
	tchk_put_str(d, f->slot_good[sel] ? " good" : " not good");
	tchk_put_str(d, ", other ");
	tchk_put_str(d, f->slot_good[sel ^ 1u] ? "good" : "not good");
	d = &t->r[TCHK_FLASH_LAYOUT].more;
	tchk_put_str(d, f->meta_magic_ok ? "metadata present" :
			f->flash_late ? "reads one byte late" : "metadata differs");
	tchk_put_str(d, "; selector index ");
	tchk_put_hex(d, f->sel_index, 2u);
	tchk_put_str(d, ", verified ");
	tchk_put_hex(d, f->sel_verified, 2u);
	tchk_put_str(d, "; first stage ");
	tchk_put_str(d, f->fs.parsed && f->fs.codes_ok ? "present" : "differs");
	tchk_put_str(d, "; read at ");
	tchk_put_rate(d, f->flash_bytes, f->flash_us);
}

/* A coprocessor's version from its raw info reply: digits alone (S-17.024). */
static void put_version(struct tchk_text *d, const uint8_t *reply, size_t len)
{
	const uint8_t *v;
	size_t vlen;
	struct s11_version ver;

	if ((!s17_kv_lookup(reply, len, "sw_version", &v, &vlen) &&
	     !s17_kv_lookup(reply, len, "app_sw_version", &v, &vlen)) ||
	    !s17_version_value(v, vlen, &ver)) {
		tchk_put_str(d, "version unread");
		return;
	}
	tchk_put_str(d, "version ");
	tchk_put_dec(d, ver.major);
	tchk_put_str(d, ".");
	tchk_put_dec(d, ver.minor);
	tchk_put_str(d, ".");
	tchk_put_dec(d, ver.patch);
}

static void judge_psoc(const struct tchk_facts *f, struct tchk_table *t)
{
	const bool greeted = f->psoc_greeted || f->psoc_asked;
	/* the ping 0x01 is pmic-pass-through's (S-07 Devices): not sent elsewhere */
	const bool pinged = !wb_has(dev_of(f), WB_F_PMIC_PASS_THROUGH) || f->psoc_ping;
	const bool ok = greeted && pinged && f->psoc_keys && f->psoc_battery;
	struct tchk_text *d;

	if (!ran(f, TCHK_PSOC)) {
		not_tested(t, TCHK_PSOC, "not run");
		return;
	}
	d = tchk_set(t, TCHK_PSOC, ok ? TCHK_PASS : TCHK_FAIL);
	if (ok) {
		tchk_put_str(d, pinged && f->psoc_ping ? "greeting, ping, keys, battery answered" :
				"greeting, keys, battery answered");
	} else {
		tchk_put_str(d, "no answer:");
		tchk_put_str(d, greeted ? "" : " greeting");
		tchk_put_str(d, pinged ? "" : " ping");
		tchk_put_str(d, f->psoc_keys ? "" : " keys");
		tchk_put_str(d, f->psoc_battery ? "" : " battery");
	}
	put_version(&t->r[TCHK_PSOC].more, f->psoc_info, f->psoc_info_len);
	/* the greeting's history, for a unit that needed a greeting again */
	d = &t->r[TCHK_PSOC].more;
	if (!f->psoc_start_greeted)
		tchk_put_str(d, "; start-up greeting not answered");
	if (f->psoc_greet_failed != 0u) {
		tchk_put_str(d, "; greetings not answered since: ");
		tchk_put_dec(d, f->psoc_greet_failed);
	}
	if (!f->psoc_greeted)
		tchk_put_str(d, f->psoc_asked ? "; greeted again at the test" :
						"; the test's greeting not answered");
}

static void judge_slave(const struct tchk_facts *f, struct tchk_table *t)
{
	if (!two_cpus(f)) {
		not_on_device(t, TCHK_SLAVE);
		return;
	}
	if (!f->slave) {
		tchk_put_str(tchk_set(t, TCHK_SLAVE, TCHK_FAIL), "no answer over the bridge");
		return;
	}
	if (!ran(f, TCHK_SLAVE)) {
		not_tested(t, TCHK_SLAVE, "not run");
		return;
	}
	if (!f->slave_echo) {
		tchk_put_str(tchk_set(t, TCHK_SLAVE, TCHK_FAIL), "the echo was not answered (S-09.015)");
		return;
	}
	if (f->slave_failed_row >= 0) {
		struct tchk_text *d = tchk_set(t, TCHK_SLAVE, TCHK_FAIL);

		tchk_put_str(d, "start-up step ");
		tchk_put_dec(d, (uint32_t)f->slave_failed_row);
		tchk_put_str(d, " failed, code ");
		tchk_put_dec(d, f->slave_failed_code);
		return;
	}
	tchk_put_str(tchk_set(t, TCHK_SLAVE, TCHK_PASS), "present, echo answered, every start-up step done");
}

static void judge_bridge(const struct tchk_facts *f, struct tchk_table *t)
{
	struct tchk_text *d;
	const bool ok = f->link_bad == 0u && f->link_err == 0u && f->link_bytes >= 0x800000u;

	if (!two_cpus(f)) {
		not_on_device(t, TCHK_BRIDGE);
		return;
	}
	if (!f->slave) {
		not_tested(t, TCHK_BRIDGE, "no slave");
		return;
	}
	if (!ran(f, TCHK_BRIDGE)) {
		not_tested(t, TCHK_BRIDGE, "not run");
		return;
	}
	d = tchk_set(t, TCHK_BRIDGE, ok ? TCHK_PASS : TCHK_FAIL);
	tchk_put_dec(d, (uint32_t)(f->link_bytes >> 20));
	tchk_put_str(d, " MiB at ");
	tchk_put_rate(d, f->link_bytes, f->link_us);
	if (f->link_bad != 0u || f->link_err != 0u) {
		tchk_put_str(d, ", ");
		tchk_put_dec(d, f->link_bad);
		tchk_put_str(d, " wrong, ");
		tchk_put_dec(d, f->link_err);
		tchk_put_str(d, " errors");
	}
	if (f->boot.run && f->boot.us != 0u) {
		d = &t->r[TCHK_BRIDGE].more;
		tchk_put_str(d, "eMMC sector reads ");
		tchk_put_rate(d, f->boot.bytes, f->boot.us);
	}
}

/* emmc-config: the first value S-03 fixes that differs, or none. */
static void judge_ext(const struct tchk_facts *f, struct tchk_table *t)
{
	const struct wb_device *dev = dev_of(f);
	struct tchk_text *d;
	const char *what = NULL;
	/* S-13b.051, S-13b.061: the bus width switched to, 2 for 8 lines,
	 * 1 for 4 */
	const unsigned width = dev->mem.emmc_width == 4u ? 1u : 2u;

	if (!f->slave) {
		not_tested(t, TCHK_EMMC_CONFIG, no_emmc(f));
		return;
	}
	if (!ran(f, TCHK_EMMC_CONFIG) || !f->ext_read) {
		not_tested(t, TCHK_EMMC_CONFIG, "EXT_CSD not read");
		return;
	}
	/* S-03.001: device.emmc_sectors, exactly or at least */
	if (dev->mem.emmc_sectors_at_least ? f->sec_count < dev->mem.emmc_sectors :
					     f->sec_count != dev->mem.emmc_sectors)
		what = "SEC_COUNT";				/* S-03.001 */
	else if (f->erase_group != S03_GROUP_SECTORS)
		what = "erase group";				/* S-03.001 */
	else if (f->ext265 == 0u || 8u % f->ext265 != 0u)
		what = "OPTIMAL_WRITE_SIZE [265]";		/* S-03.001 */
	else if (f->ext183 != width)
		what = "BUS_WIDTH [183]";			/* S-13b.051, S-13b.061 */
	/*
	 * S-03.009's bytes 33, 34 and 163 are set by switches before the
	 * first user-area read, after the bring-up's EXT_CSD read; 33 and 34
	 * reset at every power-up (JESD84-B51, their type R/W/E_P). So the
	 * bring-up's copy shows the power-up values, and the switches are
	 * judged by the slave's record that each was made and passed its
	 * busy end (S-13b.055).
	 */
	d = tchk_set(t, TCHK_EMMC_CONFIG, what == NULL && f->features ? TCHK_PASS : TCHK_FAIL);
	if (what == NULL && f->features) {
		tchk_put_str(d, "as S-03 fixes; boot areas ");
		tchk_put_dec(d, (uint32_t)f->ext226 * 128u);
		tchk_put_str(d, " KiB");
	} else if (what == NULL) {
		tchk_put_str(d, "S-03.009's feature switches not made");
	} else {
		tchk_put_str(d, what);
		tchk_put_str(d, " differs from S-03");
	}
	d = &t->r[TCHK_EMMC_CONFIG].more;
	tchk_put_str(d, "SEC_COUNT ");
	tchk_put_hex(d, f->sec_count, 1u);
	tchk_put_str(d, ", erase group ");
	tchk_put_hex(d, f->erase_group, 1u);
	tchk_put_str(d, ", [33] ");
	tchk_put_hex(d, f->ext33, 2u);
	tchk_put_str(d, " [34] ");
	tchk_put_hex(d, f->ext34, 2u);
	tchk_put_str(d, " [163] ");
	tchk_put_hex(d, f->ext163, 2u);
	tchk_put_str(d, " [179] ");
	tchk_put_hex(d, f->ext179, 2u);
	tchk_put_str(d, " [183] ");
	tchk_put_hex(d, f->ext183, 2u);
	tchk_put_str(d, " [226] ");
	tchk_put_hex(d, f->ext226, 2u);
	tchk_put_str(d, " [265] ");
	tchk_put_hex(d, f->ext265, 2u);
	tchk_put_str(d, " at bring-up; features ");
	tchk_put_str(d, f->features ? "set since" : "not set");
}

static const char *const fault_step[TCHK_FS_COUNT] = {
	[TCHK_FS_NONE] = "no driver fault",
	[TCHK_FS_ARG] = "refused (argument or buffer)",
	[TCHK_FS_OFF] = "after the power-off notice",
	[TCHK_FS_CMD] = "no good response",
	[TCHK_FS_R1] = "R1 error",
	[TCHK_FS_BUSY] = "DAT0 busy",
	[TCHK_FS_SWITCH] = "SWITCH_ERROR",
	[TCHK_FS_STATE] = "not ready in tran",
	[TCHK_FS_DATA] = "data error",
	[TCHK_FS_DATA_TIME] = "data timeout",
};

static void put_part(struct tchk_text *d, uint8_t part)
{
	tchk_put_str(d, part == 0u ? "user" : part == 1u ? "BP1" : part == 2u ? "BP2" : "?");
}

static void put_pc(struct tchk_text *d, const struct tchk_emmc_fault *e, uint8_t bit, uint8_t v)
{
	if ((e->pc_flags & bit) != 0u)
		tchk_put_hex(d, v, 2u);
	else
		tchk_put_str(d, "-");
}

/* One fault line: where the request stopped, then the driver's record. */
static void put_fault(struct tchk_text *d, const struct tchk_emmc_fault *e, bool select)
{
	tchk_put_str(d, select ? "select " : "read ");
	put_part(d, e->part);
	if (!select) {
		tchk_put_str(d, " ");
		tchk_put_hex(d, e->sector, 1u);
		tchk_put_str(d, "+");
		tchk_put_dec(d, e->count);
	}
	tchk_put_str(d, ": ");
	if (e->why == TCHK_FW_UNKNOWN) {
		tchk_put_str(d, "no partition known since a failed select");
		return;
	}
	if (e->why == TCHK_FW_REFUSED && e->step == TCHK_FS_NONE) {
		tchk_put_str(d, "refused by the store");
		return;
	}
	if (e->why == TCHK_FW_DOWN)
		tchk_put_str(d, "eMMC not up; ");
	tchk_put_str(d, "CMD");
	tchk_put_dec(d, e->cmd);
	tchk_put_str(d, " ");
	tchk_put_hex(d, e->arg, 8u);
	tchk_put_str(d, ", at CMD");
	tchk_put_dec(d, e->at);
	tchk_put_str(d, " ");
	tchk_put_str(d, e->step < TCHK_FS_COUNT ? fault_step[e->step] : "?");
	tchk_put_str(d, ", rc ");
	if (e->rc < 0) {
		tchk_put_str(d, "-");
		tchk_put_dec(d, (uint32_t)(-(int32_t)e->rc));
	} else {
		tchk_put_dec(d, (uint32_t)e->rc);
	}
	tchk_put_str(d, ", r1 ");
	tchk_put_hex(d, e->r1, 8u);
	tchk_put_str(d, ", state ");
	if (e->state == 0xFFu)
		tchk_put_str(d, "-");
	else
		tchk_put_dec(d, e->state);
	tchk_put_str(d, ", is ");
	tchk_put_hex(d, e->istat, 8u);
	tchk_put_str(d, ", ids ");
	tchk_put_hex(d, e->idsts, 8u);
	tchk_put_str(d, ", busy ");
	tchk_put_dec(d, e->busy_us);
	tchk_put_str(d, " us");
	if (select) {
		tchk_put_str(d, ", [179] ");
		put_pc(d, e, TCHK_PC_BEFORE, e->pc_before);
		tchk_put_str(d, " then ");
		put_pc(d, e, TCHK_PC_AFTER, e->pc_after);
	}
}

static void fault_put32(uint8_t *o, uint32_t v)
{
	o[0] = (uint8_t)v;
	o[1] = (uint8_t)(v >> 8);
	o[2] = (uint8_t)(v >> 16);
	o[3] = (uint8_t)(v >> 24);
}

void tchk_fault_bytes(const struct tchk_emmc_fault *e, uint8_t out[TCHK_FAULT_BYTES])
{
	const uint32_t w[7] = { e->sector, e->count, e->arg, e->r1, e->istat, e->idsts, e->busy_us };
	unsigned i;

	memset(out, 0, TCHK_FAULT_BYTES);
	if (!e->seen)
		return;
	out[0] = 1u;
	out[1] = e->part;
	out[2] = e->why;
	out[3] = e->pc_flags;
	out[4] = e->pc_before;
	out[5] = e->pc_after;
	out[6] = e->step;
	out[7] = e->cmd;
	out[8] = e->at;
	out[9] = e->state;
	out[10] = (uint8_t)e->rc;
	for (i = 0; i < 7u; i++)
		fault_put32(out + 12u + 4u * i, w[i]);
}

static void judge_emmc(const struct tchk_facts *f, struct tchk_table *t)
{
	struct tchk_text *d;

	if (!f->slave) {
		not_tested(t, TCHK_EMMC_BOOT, no_emmc(f));
		not_tested(t, TCHK_EMMC_VOLUME, no_emmc(f));
		not_tested(t, TCHK_EMMC_FILE, no_emmc(f));
		return;
	}
	if (!ran(f, TCHK_EMMC_BOOT) || !f->boot.run) {
		not_tested(t, TCHK_EMMC_BOOT, "not run");
	} else {
		const uint32_t errors = f->boot.errors[0] + f->boot.errors[1];
		const bool crc_bad = f->boot.mine_checked && !f->boot.mine_ok;

		d = tchk_set(t, TCHK_EMMC_BOOT, errors == 0u && !crc_bad && !f->boot.select0_failed ?
				 TCHK_PASS : TCHK_FAIL);
		if (errors != 0u) {
			tchk_put_dec(d, errors);
			tchk_put_str(d, " reads failed");
		} else {
			tchk_put_str(d, "every sector reads");
		}
		if (f->boot.mine_checked)
			tchk_put_str(d, f->boot.mine_ok ? "; active image good" :
					 "; active image differs");
		else if (f->boot.unchecked == TCHK_UNCHECKED_ENCRYPTED)
			tchk_put_str(d, "; image stored encrypted");
		else
			tchk_put_str(d, "; image not checked");
		tchk_put_str(d, f->boot.select0_failed ? "; user area not selected" : "");
		d = &t->r[TCHK_EMMC_BOOT].more;
		for (unsigned p = 0; p < 2u; p++) {
			tchk_put_str(d, p == 0u ? "BP1 " : "; BP2 ");
			tchk_put_dec(d, f->boot.sectors[p]);
			tchk_put_str(d, " read");
			if (f->boot.errors[p] != 0u) {
				tchk_put_str(d, ", ");
				tchk_put_dec(d, f->boot.errors[p]);
				tchk_put_str(d, " failed from ");
				tchk_put_dec(d, f->boot.first_bad[p]);
			}
		}
		if (!f->boot.mine_checked && f->boot.unchecked == TCHK_UNCHECKED_READ)
			tchk_put_str(d, "; image not checked: a read inside it failed");
		else if (!f->boot.mine_checked && f->boot.unchecked == TCHK_UNCHECKED_NO_IMAGE)
			tchk_put_str(d, "; image not checked: no sound active header");
	}
	for (unsigned k = 0; k < 2u; k++) {
		tchk_text_init(&t->emmc_fault[k], TCHK_MORE_MAX);
		if (f->emmc_fault[k].seen)
			put_fault(&t->emmc_fault[k], &f->emmc_fault[k], k == 0u);
	}

	if (!ran(f, TCHK_EMMC_VOLUME)) {
		not_tested(t, TCHK_EMMC_VOLUME, "not run");
	} else {
		d = tchk_set(t, TCHK_EMMC_VOLUME, f->mounted ? TCHK_PASS : TCHK_FAIL);
		tchk_put_str(d, f->mounted ? "yaffs2 mounted" : "yaffs2 not mounted");
		if (f->space_read) {
			tchk_put_str(d, ", ");
			tchk_put_dec(d, (uint32_t)(f->free_bytes >> 20));
			tchk_put_str(d, " MiB free");
		}
	}

	if (!ran(f, TCHK_EMMC_FILE)) {
		not_tested(t, TCHK_EMMC_FILE, f->mounted ? "not run" : "no volume");
	} else {
		static const char *const step[] = {
			[TCHK_FILE_OK] = "", [TCHK_FILE_LEFTOVER] = "removing an old file",
			[TCHK_FILE_CREATE] = "create", [TCHK_FILE_WRITE] = "write",
			[TCHK_FILE_CLOSE] = "close", [TCHK_FILE_OPEN] = "open to read",
			[TCHK_FILE_READ] = "read", [TCHK_FILE_COMPARE] = "compare",
			[TCHK_FILE_UNLINK] = "unlink", [TCHK_FILE_GONE] = "still there after unlink",
		};
		const uint8_t s = f->file.failed <= TCHK_FILE_GONE ? f->file.failed : TCHK_FILE_GONE;

		d = tchk_set(t, TCHK_EMMC_FILE, s == TCHK_FILE_OK ? TCHK_PASS : TCHK_FAIL);
		if (s == TCHK_FILE_OK) {
			tchk_put_dec(d, f->file.compared >> 10);
			tchk_put_str(d, " KiB written, read back, removed");
		} else {
			tchk_put_str(d, "failed at: ");
			tchk_put_str(d, step[s]);
		}
		tchk_put_str(&t->r[TCHK_EMMC_FILE].more,
			     f->file.leftover ? "a file of an earlier run was removed first" : "");
	}
}

static void judge_panel(const struct tchk_facts *f, struct tchk_table *t)
{
	struct tchk_text *d;
	const bool ok = f->panel_up && f->pattern_drawn && f->te_seen &&
			f->te_misses == 0u && f->frame_timeouts == 0u;

	if (!ran(f, TCHK_PANEL)) {
		not_tested(t, TCHK_PANEL, "not run");
		return;
	}
	d = tchk_set(t, TCHK_PANEL, ok ? TCHK_PASS : TCHK_FAIL);
	if (!f->panel_up) {
		tchk_put_str(d, "the panel did not start");
		return;
	}
	if (f->te_seen) {
		tchk_put_str(d, "TE every ");
		tchk_put_ms(d, f->te_us);
		/* S-13c.018: 15.4 to 20 ms, a best guess: reported, not judged */
		tchk_put_str(d, f->te_us >= 15400u && f->te_us <= 20000u ? "" : " (S-13c.018: 15.4-20)");
	} else {
		tchk_put_str(d, "no TE edge");
	}
	if (f->te_misses != 0u || f->frame_timeouts != 0u) {
		tchk_put_str(d, "; waits cut short: ");
		tchk_put_dec(d, f->te_misses + f->frame_timeouts);
	}
	d = &t->r[TCHK_PANEL].more;
	if (f->panel_id) {
		unsigned i;

		tchk_put_str(d, "RDDID");
		for (i = 0; i < 3u; i++) {
			tchk_put_str(d, " ");
			tchk_put_hex(d, f->rddid[i], 2u);
		}
		tchk_put_str(d, ", RDID4");
		for (i = 0; i < 3u; i++) {
			tchk_put_str(d, " ");
			tchk_put_hex(d, f->rdid4[i], 2u);
		}
	} else {
		tchk_put_str(d, "ID bytes not read");
	}
	tchk_put_str(d, f->pattern_drawn ? "; full-frame pattern drawn" : "; pattern not drawn");
}

static void judge_rtc(const struct tchk_facts *f, struct tchk_table *t)
{
	struct tchk_text *d;
	/* one second in about one second */
	const bool ok = f->rtc_read && f->rtc_ticked && f->rtc_us >= 800000u && f->rtc_us <= 1200000u;

	if (!ran(f, TCHK_RTC)) {
		not_tested(t, TCHK_RTC, "not run");
		return;
	}
	d = tchk_set(t, TCHK_RTC, ok ? TCHK_PASS : TCHK_FAIL);
	if (!f->rtc_read) {
		tchk_put_str(d, "could not be read");
	} else if (!f->rtc_ticked) {
		tchk_put_str(d, "did not advance within 2 s");
	} else {
		tchk_put_str(d, "one second in ");
		tchk_put_ms(d, f->rtc_us);
	}
}

static void judge_nordic(const struct tchk_facts *f, struct tchk_table *t)
{
	struct tchk_text *d;
	const bool dfu = f->nordic_info &&
			 s17_reply_in_bootloader(f->nordic_reply, f->nordic_reply_len);
	const bool ok = f->nordic_greeting && f->nordic_info && !dfu;

	if (!ran(f, TCHK_NORDIC)) {
		not_tested(t, TCHK_NORDIC, "not run");
		return;
	}
	d = tchk_set(t, TCHK_NORDIC, ok ? TCHK_PASS : TCHK_FAIL);
	if (!f->nordic_greeting)
		tchk_put_str(d, "the link greeting was not answered");
	else if (!f->nordic_info)
		tchk_put_str(d, "the info request was not answered");
	else if (dfu)
		tchk_put_str(d, "it is in its bootloader (DFU)");
	else
		put_version(d, f->nordic_reply, f->nordic_reply_len);
	d = &t->r[TCHK_NORDIC].more;
	tchk_put_str(d, f->nordic_start_mode == 2u ? "in its bootloader at the suite's start; " :
			f->nordic_start_mode == 1u ? "in its application at the suite's start; " :
						     "no answer at the suite's start; ");
	if (f->nordic_left != 0u) {
		tchk_put_str(d, "told to leave its bootloader: ");
		tchk_put_dec(d, f->nordic_left);
		tchk_put_str(d, "; ");
	}
	tchk_put_str(d, "frames held back by the gate: ");
	tchk_put_dec(d, f->nordic_refused);
}

static void judge_timing(const struct tchk_facts *f, struct tchk_table *t)
{
	struct tchk_text *d = tchk_set(t, TCHK_TIMING, TCHK_PASS);
	static const struct {
		unsigned step;
		const char *name;
	} rows[] = {
		{ OPXY_TESTOS_STEP_EMMC, "eMMC up " },
		{ OPXY_TESTOS_STEP_FEATURES, ", features " },
		{ OPXY_TESTOS_STEP_MOUNT, ", mount " },
	};

	/* No S-13a or S-09 bound applies to these: reported */
	tchk_put_str(d, "suite started ");
	tchk_put_dec(d, f->held_ms);
	tchk_put_str(d, " ms after entry");
	d = &t->r[TCHK_TIMING].more;
	if (f->slave && two_cpus(f)) {
		tchk_put_str(d, "slave, ms after its server: ");
		for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
			tchk_put_str(d, rows[i].name);
			if (f->slave_ms[rows[i].step] == 0xFFFFu)
				tchk_put_str(d, "-");
			else
				tchk_put_dec(d, f->slave_ms[rows[i].step]);
		}
		tchk_put_str(d, "; ");
	}
	tchk_put_str(d, "flash ");
	tchk_put_rate(d, f->flash_bytes, f->flash_us);
	if (f->slave && two_cpus(f)) {
		tchk_put_str(d, ", bridge ");
		tchk_put_rate(d, f->link_bytes, f->link_us);
	}
}

static void judge_guided(const struct tchk_facts *f, struct tchk_table *t)
{
	/* com on every device; the recovery key by name on the OP-XY alone */
	const bool xy = dev_of(f)->id == WB_DEV_OPXY;
	struct tchk_text *d;

	switch (f->keys) {
	case TCHK_G_PASSED:
		tchk_put_str(tchk_set(t, TCHK_KEYS, TCHK_PASS),
			     xy ? "com and M4 pressed" : "com and the recovery key pressed");
		break;
	case TCHK_G_EARLIER:
		tchk_put_str(tchk_set(t, TCHK_KEYS, TCHK_PASS),
			     "passed before the trial, not asked again");
		break;
	case TCHK_G_ENDED:
		d = tchk_set(t, TCHK_KEYS, TCHK_NOT_TESTED);
		tchk_put_str(d, "ended with 8; ");
		tchk_put_str(d, "com");
		tchk_put_str(d, (f->keys_seen & 1u) ? " pressed, " : " not pressed, ");
		tchk_put_str(d, xy ? "M4" : "recovery key");
		tchk_put_str(d, (f->keys_seen & 2u) ? " pressed" : " not pressed");
		break;
	default:
		not_tested(t, TCHK_KEYS, "not run");
		break;
	}
}

void tchk_judge(const struct tchk_facts *f, struct tchk_table *t)
{
	judge_os(f, t);
	if (ran(f, TCHK_CLOCKS)) {
		judge_regs(f, t, TCHK_CLOCKS, TCHK_REGS_CLOCKS);
		judge_regs(f, t, TCHK_DDR, TCHK_REGS_DDR);
		judge_regs(f, t, TCHK_CORE_STATE, TCHK_REGS_CORE);
	} else {
		not_tested(t, TCHK_CLOCKS, "not run");
		not_tested(t, TCHK_DDR, "not run");
		not_tested(t, TCHK_CORE_STATE, "not run");
	}
	judge_flash(f, t);
	judge_psoc(f, t);
	judge_slave(f, t);
	judge_bridge(f, t);
	judge_ext(f, t);
	judge_emmc(f, t);
	judge_panel(f, t);
	judge_rtc(f, t);
	judge_nordic(f, t);
	judge_timing(f, t);
	judge_guided(f, t);
}

/* --- the summary --- */

const char *const tchk_first_lines[TCHK_FIRST_LINES] = {
	"Checking the hardware",
	"",
};

unsigned tchk_cover_lines(const char *out[TCHK_COVER_LINES])
{
	unsigned n = 0;

	out[n++] = "A pass does not cover:";
	out[n++] = "- the boot ROM loading waffle-boot from the flash";
	out[n++] = "- the first stage choosing a slot";
	out[n++] = "- anything that needs a flash write";
	out[n++] = "- waffle-boot's own earliest start-up";
	return n;
}

static void line(struct tchk_line *l, enum s15_style style, const char *a, const char *b,
		 const char *c)
{
	size_t n = 0;
	const char *parts[3] = { a, b, c };
	unsigned i;

	l->style = (uint8_t)style;
	for (i = 0; i < 3u; i++)
		for (const char *p = parts[i]; p != NULL && *p != '\0' && n + 1u < sizeof l->s; p++)
			l->s[n++] = *p;
	l->s[n] = '\0';
}

static enum s15_style state_style(uint8_t s)
{
	switch (s) {
	case TCHK_PASS:
		return S15_ST_STEP_OK;
	case TCHK_FAIL:
		return S15_ST_STEP_FAIL;
	default:
		return S15_ST_STEP_SKIP;
	}
}

unsigned tchk_page(const struct tchk_table *t, const struct tchk_switch *sw, unsigned page,
		   struct tchk_line out[S15_ROWS])
{
	static const char *const keys = "`2` next page, `1` previous page";
	unsigned n = 0, i;
	char num[24];
	struct tchk_text pg;

	(void)sw;
	tchk_text_init(&pg, sizeof num - 1u);
	tchk_put_str(&pg, "Page ");
	tchk_put_dec(&pg, page + 1u);
	tchk_put_str(&pg, " of ");
	tchk_put_dec(&pg, TCHK_PAGES);
	memcpy(num, pg.s, (size_t)pg.n + 1u);

	if (page == 0u) {
		const enum tchk_verdict v = tchk_verdict(t);

		line(&out[n++], v == TCHK_VERDICT_PASS ? S15_ST_OK :
				v == TCHK_VERDICT_GAPS ? S15_ST_WARN : S15_ST_ERROR,
		     "Hardware: ", tchk_verdict_word(v), NULL);
		line(&out[n++], S15_ST_DIM, "The report is check.txt on the USB disk.", NULL, NULL);
	} else if (page <= TCHK_TEST_PAGES) {
		for (i = (page - 1u) * TCHK_PER_PAGE; i < page * TCHK_PER_PAGE && i < TCHK_COUNT; i++) {
			const struct tchk_result *r = &t->r[i];

			line(&out[n++], state_style(r->state), tchk_tests[i].name, "\t",
			     tchk_state_word((enum tchk_state)r->state));
			line(&out[n++], S15_ST_DIM, r->detail.s, NULL, NULL);
		}
	} else {
		const char *c[TCHK_COVER_LINES];
		const unsigned k = tchk_cover_lines(c);

		for (i = 0; i < k; i++)
			line(&out[n++], i == 0u ? S15_ST_HEAD : S15_ST_TEXT, c[i], NULL, NULL);
	}
	while (n < S15_ROWS - 1u)
		line(&out[n++], S15_ST_TEXT, "", NULL, NULL);
	line(&out[n++], S15_ST_KEYS, num, ". ", keys);
	return n;
}

/* --- check.txt --- */

struct out {
	char *p;
	size_t n, cap;
};

static void emit(struct out *o, const char *s)
{
	while (*s != '\0' && o->n + 1u < o->cap)
		o->p[o->n++] = *s++;
	o->p[o->n] = '\0';
}

static void emit_pad(struct out *o, const char *s, size_t w)
{
	size_t k = strlen(s);

	emit(o, s);
	while (k++ < w)
		emit(o, " ");
}

size_t tchk_report(const struct tchk_table *t, const struct tchk_switch *sw, const char *version,
		   char *buf, size_t cap)
{
	struct out o = { buf, 0, cap };
	const char *l[TCHK_COVER_LINES];
	unsigned i, k;

	(void)sw;
	if (cap == 0u)
		return 0;
	buf[0] = '\0';
	emit(&o, "waffle-boot installer, checks v");
	emit(&o, version);
	emit(&o, "\n\nThe installer's hardware checks.\n\nHardware: ");
	emit(&o, tchk_verdict_word(tchk_verdict(t)));
	emit(&o, "\n\n");
	for (i = 0; i < TCHK_COUNT; i++) {
		const struct tchk_result *r = &t->r[i];

		emit_pad(&o, tchk_tests[i].id, 14u);
		emit_pad(&o, tchk_state_word((enum tchk_state)r->state), 12u);
		emit(&o, r->detail.s);
		emit(&o, "\n");
		if (r->more.n != 0u) {
			emit_pad(&o, "", 26u);
			emit(&o, r->more.s);
			emit(&o, "\n");
		}
		for (k = 0; i == TCHK_EMMC_BOOT && k < 2u; k++) {
			if (t->emmc_fault[k].n == 0u)
				continue;
			emit_pad(&o, "", 26u);
			emit(&o, t->emmc_fault[k].s);
			emit(&o, "\n");
		}
	}
	emit(&o, "\n");
	k = tchk_cover_lines(l);
	for (i = 0; i < k; i++) {
		emit(&o, l[i]);
		emit(&o, "\n");
	}
	emit(&o, "A key check ended with 8 is listed as not tested; the install is then not offered.\n");
	return o.n;
}

size_t tchk_report_pad(char *buf, size_t n, size_t cap)
{
	if (n + 1u >= TCHK_REPORT_LEN || TCHK_REPORT_LEN > cap)
		return n;
	memset(buf + n, ' ', TCHK_REPORT_LEN - 1u - n);
	buf[TCHK_REPORT_LEN - 1u] = '\n';
	return TCHK_REPORT_LEN;
}

#else

/* ISO C wants a declaration in every translation unit. */
typedef int tchk_logic_not_built;

#endif /* OPXY_TESTOS_CHECK */
