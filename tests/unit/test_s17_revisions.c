/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-17 revisions: S-17.001, S-17.003, S-17.010, S-17.011, S-17.012,
 * S-17.013, S-17.014, S-17.015, S-17.020, S-17.021, S-17.022, S-17.023,
 * S-17.024, S-17.032, S-17.034, S-17.042.
 */
#include <stdint.h>
#include <string.h>

#include "waffle/s11_version.h"
#include "waffle/s17_revisions.h"

#include "s11_check.h"
#include "vectors/s17_vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static const uint8_t *u8(const char *s)
{
	return (const uint8_t *)s;
}

static int version_is(const struct s11_version *v, const char *want)
{
	char buf[S11_VERSION_TEXT_MAX];
	size_t n = s11_version_text(v, buf, sizeof(buf));

	return s11_text_eq(buf, n, want);
}

static int state_is(const struct s17_copro *c, const struct s17v_state *s)
{
	return version_is(&c->psoc, s->psoc) &&
	       s11_text_eq(c->psoc_serial, c->psoc_serial_len, s->serial) &&
	       version_is(&c->nordic_app, s->nordic_app) &&
	       version_is(&c->nordic_bl, s->nordic_bl);
}

static struct s11_version version_of(const char *text)
{
	struct s11_version v = { 0, 0, 0, 0 };

	(void)s17_version_value(u8(text), strlen(text), &v);
	return v;
}

static void state_set(struct s17_copro *c, const struct s17v_state *s)
{
	size_t n = strlen(s->serial);

	c->psoc = version_of(s->psoc);
	c->nordic_app = version_of(s->nordic_app);
	c->nordic_bl = version_of(s->nordic_bl);
	memcpy(c->psoc_serial, s->serial, n);
	c->psoc_serial_len = (uint8_t)n;
}

/* S-17.001, S-17.003 */
static void test_matrix(void)
{
	for (size_t i = 0; i < N(s17v_matrix); i++) {
		const struct s17v_matrix *m = &s17v_matrix[i];
		struct s17_rev r;

		CHECK(s17_decode_strap(m->pa8, m->pa0) == m->decoded,
		      "decode %u%u", m->pa8, m->pa0);
		if (!m->master)
			continue;	/* S-17.004: nothing of the slave reports it */
		CHECK((int)s17_panel_reset_path(m->pa8) == m->panel,
		      "panel reset %u%u", m->pa8, m->pa0);
		s17_rev_power_on(&r, m->pa8, m->pa0);
		s17_rev_metadata(&r, S17_META_READ);
		CHECK(s17_rev_reported(&r) == (unsigned)m->reported,
		      "reported %u%u", m->pa8, m->pa0);
	}
	/* a swapped reading of the two bits would give 5 for 01 */
	CHECK(s17_decode_strap(0, 1) == 4 && s17_decode_strap(1, 0) == 5,
	      "PA8 is the high bit");
}

/* S-17.010-S-17.015 */
static void test_sequences(void)
{
	for (size_t i = 0; i < N(s17v_seqs); i++) {
		struct s17_rev r = { 0, 0, S17_META_UNREAD };
		uint8_t byte;

		for (const struct s17v_event *e = s17v_seqs[i].ev;
		     e->kind != S17V_END; e++) {
			char buf[S17_HW_REV_ENTRY_MAX];
			size_t n;

			switch (e->kind) {
			case S17V_POWER_ON:
				s17_rev_power_on(&r, e->a, e->b);
				break;
			case S17V_META_READ:
				s17_rev_metadata(&r, S17_META_READ);
				break;
			case S17V_META_FAILED:
				s17_rev_metadata(&r, e->a ? S17_META_RECORD_FAILED :
							    S17_META_SERIAL_FAILED);
				break;
			case S17V_GREETING:
				if (s17_rev_greeting(&r, e->data, e->len, &byte))
					CHECK(byte == e->data[1], "%s: logged byte",
					      s17v_seqs[i].name);
				break;
			case S17V_INFO:
				n = s17_hw_rev_entry(s17_rev_reported(&r), buf,
						     sizeof(buf));
				CHECK(s11_text_eq(buf, n, e->entry), "%s: want %s, got %.*s",
				      s17v_seqs[i].name, e->entry, (int)n, buf);
				break;
			case S17V_END:
			default:
				break;
			}
		}
	}
}

/* S-17.011, S-17.014: a short greeting sets nothing and logs nothing. */
static void test_greeting_logging(void)
{
	static const uint8_t one[1] = { 0xBB }, two[2] = { 0xBB, 0x5A };
	struct s17_rev r;
	uint8_t byte = 0;

	s17_rev_power_on(&r, 1, 1);
	CHECK(!s17_rev_greeting(&r, two, 2, &byte), "before the metadata read");
	s17_rev_metadata(&r, S17_META_READ);
	CHECK(!s17_rev_greeting(&r, one, 1, &byte), "1-byte greeting");
	CHECK(!s17_rev_greeting(&r, two, 0, &byte), "empty greeting");
	CHECK(s17_rev_greeting(&r, two, 2, &byte) && byte == 0x5A, "2 bytes");
	CHECK(s17_rev_greeting(&r, two, 2, &byte), "a confirming greeting logs");
	for (size_t i = 0; i < N(s17v_logs); i++) {
		char hex[2];

		s17_hex2((uint8_t)s17v_logs[i].byte, hex);
		CHECK(s11_text_eq(hex, 2, s17v_logs[i].hex), "hex %u",
		      s17v_logs[i].byte);
	}
}

/* S-17.013: no padding, no sign, the whole 0-255 range. */
static void test_info_entry(void)
{
	char buf[S17_HW_REV_ENTRY_MAX];

	for (unsigned v = 0; v < 256; v++) {
		char want[16];
		size_t n = s17_hw_rev_entry((uint8_t)v, buf, sizeof(buf));

		snprintf(want, sizeof(want), "hw_rev:%u", v);
		CHECK(s11_text_eq(buf, n, want), "entry %u", v);
	}
	CHECK(s17_hw_rev_entry(255, buf, 9) == 0, "short buffer");
}

/* S-17.024 */
static void test_values(void)
{
	for (size_t i = 0; i < N(s17v_values); i++) {
		const struct s17v_value *t = &s17v_values[i];
		struct s11_version v;
		bool ok = s17_version_value(u8(t->text), strlen(t->text), &v);

		if (t->version == NULL)
			CHECK(!ok, "'%s' is not a version", t->text);
		else
			CHECK(ok && version_is(&v, t->version), "'%s' reads as %s",
			      t->text, t->version);
	}
}

/* S-17.023: exact key, first match, empty and colon-less entries. */
static void test_kv(void)
{
	static const char p[] = "sw_versionx:9;;nokey;sw_version:1.2.3;"
				"sw_version:4.5.6;serial:";
	const uint8_t *val;
	size_t vlen;

	CHECK(s17_kv_lookup(u8(p), strlen(p), "sw_version", &val, &vlen) &&
	      vlen == 5 && memcmp(val, "1.2.3", 5) == 0, "first exact match");
	CHECK(!s17_kv_lookup(u8(p), strlen(p), "sw_vers", &val, &vlen),
	      "no prefix match");
	CHECK(s17_kv_lookup(u8(p), strlen(p), "serial", &val, &vlen) && vlen == 0,
	      "empty value at the end");
	CHECK(!s17_kv_lookup(u8(p), strlen(p), "nokey", &val, &vlen),
	      "an entry with no ':' has no key");
	CHECK(!s17_kv_lookup(u8(""), 0, "mode", &val, &vlen), "empty payload");
}

/* S-17.020-S-17.023 */
static void test_replies(void)
{
	struct s17_copro c;

	s17_copro_init(&c);
	CHECK(state_is(&c, &s17v_initial), "initial values");
	for (size_t i = 0; i < N(s17v_replies); i++) {
		const struct s17v_reply *t = &s17v_replies[i];

		state_set(&c, &t->before);
		if (t->payload == NULL) {
			/* no reply within 100 ms: nothing changes (S-17.022) */
		} else if (t->step == 1) {
			s17_copro_held_reply(&c, t->peer ? S17_PEER_PSOC :
							   S17_PEER_NORDIC,
					     u8(t->payload), strlen(t->payload));
		} else if (t->mode_reply != NULL &&
			   s17_reply_in_bootloader(u8(t->mode_reply),
						   strlen(t->mode_reply)) &&
			   t->payload != NULL) {
			s17_copro_dfu_reply(&c, u8(t->payload), strlen(t->payload));
		}
		CHECK(state_is(&c, &t->after), "reply %s", t->name);
	}
}

/* A fake coprocessor link: scripted replies, a log of the exchanges. */
struct fake_link {
	const char *reply[4];
	int peer[4];
	uint32_t timeout[4];
	unsigned n;
};

static int fake_exchange(void *ctx, enum s17_peer peer, uint32_t timeout_ms,
			 uint8_t *reply, size_t cap)
{
	struct fake_link *f = ctx;
	const char *r;
	size_t len;

	if (f->n >= N(f->reply))
		return -1;
	f->peer[f->n] = (int)peer;
	f->timeout[f->n] = timeout_ms;
	r = f->reply[f->n++];
	if (r == NULL)
		return -1;
	len = strlen(r);
	if (len > cap)
		return -1;
	memcpy(reply, r, len);
	return (int)len;
}

static const struct s17_link_ops fake_ops = { fake_exchange };

/* S-17.021, S-17.022, S-17.034 */
static void test_run_end(void)
{
	for (size_t i = 0; i < N(s17v_run_ends); i++) {
		const struct s17v_run_end *t = &s17v_run_ends[i];
		struct fake_link f = { { t->nordic_reply, t->psoc_reply }, { 0 },
				       { 0 }, 0 };
		struct s17_copro c;

		state_set(&c, &t->before);
		if (t->s11_nordic_app != NULL)
			c.nordic_app = version_of(t->s11_nordic_app);
		s17_copro_refresh(&c, &fake_ops, &f);
		CHECK(state_is(&c, &t->after), "run end %s", t->name);
		CHECK(f.n == 2 && f.peer[0] == S17_PEER_NORDIC &&
		      f.peer[1] == S17_PEER_PSOC, "%s: one exchange per peer",
		      t->name);
		CHECK(f.timeout[0] == 100 && f.timeout[1] == 100,
		      "%s: 100 ms per reply, 200 ms in all (S-17.022's 2 s)",
		      t->name);
	}
}

/* S-17.021 step 2 through the link */
static void test_dfu_read(void)
{
	struct s17_copro c;
	struct fake_link app = { { "mode:application;sw_version:9.9.9" },
				 { 0 }, { 0 }, 0 };
	struct fake_link bl = { { "mode:bootloader",
				  "sw_version:1.0.1;bl_version:1000.0.10" },
				{ 0 }, { 0 }, 0 };
	struct fake_link silent = { { NULL }, { 0 }, { 0 }, 0 };

	s17_copro_init(&c);
	CHECK(!s17_copro_dfu_read(&c, &fake_ops, &app) && app.n == 1 &&
	      version_is(&c.nordic_app, "0.0.0"),
	      "application mode: one exchange, nothing read");
	CHECK(!s17_copro_dfu_read(&c, &fake_ops, &silent) && silent.n == 1,
	      "silent Nordic");
	CHECK(s17_copro_dfu_read(&c, &fake_ops, &bl) && bl.n == 2 &&
	      version_is(&c.nordic_app, "1.0.1") &&
	      version_is(&c.nordic_bl, "1000.0.10"), "bootloader mode");
}

/* S-17.032, S-17.042 */
static void test_own_version(void)
{
	for (size_t i = 0; i < N(s17v_own_accept); i++) {
		const char *t = s17v_own_accept[i];
		struct s11_version v;

		CHECK(s17_version_value(u8(t), strlen(t), &v) &&
		      s17_own_version_ok(&v), "own version %s accepted", t);
	}
	for (size_t i = 0; i < N(s17v_own_reject); i++) {
		const char *t = s17v_own_reject[i];
		struct s11_version v;

		CHECK(!s17_version_value(u8(t), strlen(t), &v) ||
		      !s17_own_version_ok(&v), "own version %s rejected", t);
	}
	for (size_t i = 0; i < N(s17v_copro_versions); i++) {
		const char *t = s17v_copro_versions[i];
		struct s11_version v;

		CHECK(s17_version_value(u8(t), strlen(t), &v) && version_is(&v, t),
		      "coprocessor version %s reads back", t);
	}
}

int main(void)
{
	test_matrix();
	test_sequences();
	test_greeting_logging();
	test_info_entry();
	test_values();
	test_kv();
	test_replies();
	test_run_end();
	test_dfu_read();
	test_own_version();
	return s11_check_report("test_s17_revisions");
}
