/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_TUSB_CONFIG_H
#define WAFFLE_TUSB_CONFIG_H

/*
 * TinyUSB configuration (src/third_party/tinyusb, 0.21.0).
 *
 * Only TinyUSB's device core is built: enumeration, control transfers and
 * endpoint management (usbd.c). The device controller
 * driver is ours (src/drivers/bf70x/usb_dcd.c, written from the ADSP-BF70x
 * HRM chapter 28 through the MMR accessors), and so are the class drivers
 * (src/core/usb/device.c, registered through usbd_app_driver_get_cb()), so every
 * TinyUSB class is off here.
 */

#define CFG_TUSB_MCU            OPT_MCU_NONE
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_DEBUG          0

/* The BF70x controller: endpoints 0-11, a high-speed PHY on chip. */
#define TUP_DCD_ENDPOINT_MAX    12
#define TUP_RHPORT_HIGHSPEED    1

#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_HIGH_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

/* SET_FEATURE(TEST_MODE) is accepted (S-10a.031) by the chapter 9 filter. */
#define CFG_TUD_TEST_MODE       0

#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_AUDIO           0
#define CFG_TUD_VENDOR          0
#define CFG_TUD_MTP             0

#endif /* WAFFLE_TUSB_CONFIG_H */
