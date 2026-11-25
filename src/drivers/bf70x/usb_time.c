/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TinyUSB's millisecond clock. With OPT_OS_NONE the port provides
 * tusb_time_millis_api() (tusb.c references it). It has an object of its
 * own so that a test's controller fake can link usbd without the driver.
 */
#include "waffle/drivers/bf70x/usb.h"

#include "tusb.h"

static const struct wb_time *usb_clock;

void wb_usb_set_clock(const struct wb_time *t)
{
	usb_clock = t;
}

uint32_t tusb_time_millis_api(void)
{
	return usb_clock ? (uint32_t)wb_time_ms(usb_clock) : 0u;
}
