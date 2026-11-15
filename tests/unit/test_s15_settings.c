/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 settings file and grays.
 * Requirements: S-15.172 (the file's form), S-15.173 (the grays), S-15.175
 * (not in the file).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "waffle/s15_console.h"
#include "waffle/s15_settings.h"
#include "s14_check.h"
#define S15V_SETTINGS_FILE
#define S15V_GRAY
#include "vectors/s15_vectors.h"

static void test_paths(void)
{
	CHECK(strcmp(S15_SETTINGS_PATH, S15V_SETTINGS_PATH) == 0, "S-15.172 path");
	CHECK(strcmp(S15_SETTINGS_NEW_PATH, S15V_SETTINGS_NEW_PATH) == 0,
	      "S-15.172 new path");
	CHECK(S15_SETTINGS_MAX == S15V_SETTINGS_MAX, "S-15.172 512 bytes");
}

static void test_read(void)
{
	for (size_t i = 0; i < S15V_N(s15v_settings_read); i++) {
		const struct s15v_settings_read *c = &s15v_settings_read[i];
		struct s15_settings s;
		bool usable;

		/* garbage in, to see every field is set */
		s.dev_mode = !c->dev_mode;
		s.color = !c->color;
		s.mtp_macos = !c->mtp_macos;
		usable = s15_settings_read((const uint8_t *)(c->bytes != NULL ?
							    c->bytes : ""), c->len, &s);
		CHECK(usable == c->usable && s.dev_mode == c->dev_mode &&
		      s.color == c->color && s.mtp_macos == c->mtp_macos,
		      "S-15.172 read %s", c->name);
	}
}

static void test_write(void)
{
	uint8_t buf[S15_SETTINGS_OUT_MAX];

	for (size_t i = 0; i < S15V_N(s15v_settings_write); i++) {
		const struct s15v_settings_write *c = &s15v_settings_write[i];
		struct s15_settings s = { c->dev_mode, c->color, c->mtp_macos }, back;
		size_t n = s15_settings_write(&s, buf, sizeof buf);

		CHECK(n == strlen(c->bytes) && memcmp(buf, c->bytes, n) == 0,
		      "S-15.172 write %zu", i);
		CHECK(s15_settings_read(buf, n, &back) &&
		      back.dev_mode == s.dev_mode && back.color == s.color &&
		      back.mtp_macos == s.mtp_macos,
		      "S-15.172 read back %zu", i);
		CHECK(s15_settings_write(&s, buf, n - 1u) == 0u,
		      "S-15.172 no room: nothing");
	}
}

/* S-15.175: start in boot menu is the boot store's, not the file's: the
 * file has no key for it, and a line for it is one this does not know. */
static void test_no_start_menu_key(void)
{
	static const char with[] = "waffle-boot settings 1\nstart_in_menu=on\ncolor=off\n";
	uint8_t buf[S15_SETTINGS_OUT_MAX];
	struct s15_settings s;
	bool found = false;
	size_t n;

	s15_settings_defaults(&s);
	n = s15_settings_write(&s, buf, sizeof buf);
	for (size_t i = 0; i + 5u <= n; i++)
		found = found || memcmp(buf + i, "start", 5) == 0;
	CHECK(n != 0u && !found, "S-15.175 not written to the settings file");
	CHECK(s15_settings_read((const uint8_t *)with, sizeof with - 1u, &s) &&
	      !s.dev_mode && !s.color && s.mtp_macos,
	      "S-15.175 such a line is ignored");
}

static void test_gray(void)
{
	for (size_t i = 0; i < S15V_N(s15v_gray); i++)
		CHECK(s15_gray(s15v_gray[i].color) == s15v_gray[i].gray,
		      "S-15.173 gray of %s", s15v_gray[i].name);
	for (uint32_t c = 0; c <= 0xffffu; c++) {
		uint16_t g = s15_gray((uint16_t)c);

		if (g >> 11 != (g & 0x1fu) || g >> 11 != ((g >> 5) & 0x3fu) >> 1) {
			CHECK(false, "S-15.173 0x%04x not drawn gray", (unsigned)c);
			break;
		}
	}
}

int main(void)
{
	test_paths();
	test_read();
	test_write();
	test_no_start_menu_key();
	test_gray();
	return check_done("test_s15_settings");
}
