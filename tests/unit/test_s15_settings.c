/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15 settings' defaults and grays.
 * Requirements: S-15.172 (the defaults), S-15.173 (the grays).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "waffle/ui/console.h"
#include "waffle/ui/settings.h"
#include "s14_check.h"
#define S15V_GRAY
#include "vectors/s15_vectors.h"

/* S-15.172: the defaults a start without saved settings takes; S-15.173:
 * the color's by device, on for the OP-1F alone */
static void test_defaults(void)
{
	struct s15_settings s = { true, true, false, true, false };

	s15_settings_defaults(&s, &wb_device_opxy);
	CHECK(!s.dev_mode && !s.color && s.mtp_macos && !s.start_menu && s.host_link,
	      "S-15.172 defaults");
	s15_settings_defaults(&s, &wb_device_op1f);
	CHECK(!s.dev_mode && s.color && s.mtp_macos && !s.start_menu && s.host_link,
	      "S-15.173 the OP-1F's default: color on");
	s15_settings_defaults(&s, NULL);
	CHECK(!s.color, "S-15.173 no device: gray");
	CHECK(!s15_color_default(&wb_device_opxy) && s15_color_default(&wb_device_op1f),
	      "S-15.173 by device");
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
