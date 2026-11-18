/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 settings' defaults and grays.
 * Requirements: S-15.172 (the defaults), S-15.173 (the grays).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "waffle/s15_console.h"
#include "waffle/s15_settings.h"
#include "s14_check.h"
#define S15V_GRAY
#include "vectors/s15_vectors.h"

/* S-15.172: the defaults a start without saved settings takes */
static void test_defaults(void)
{
	struct s15_settings s = { true, false, false, true };

	s15_settings_defaults(&s);
	CHECK(!s.dev_mode && s.color && s.mtp_macos && !s.start_menu, "S-15.172 defaults");
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
	test_defaults();
	test_gray();
	return check_done("test_s15_settings");
}
