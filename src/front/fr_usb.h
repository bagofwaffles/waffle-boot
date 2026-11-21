/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Recovery's USB device: one CDC ACM serial port on the BF70x USB
 * controller, polled, with S-10a's serial set's identity (vendor 0x2367,
 * the device's product) and its own product string, which the host tool
 * looks for (S-02.093): the product text followed by " Recovery". The
 * descriptors are built by portable code (fr_usb_desc.c); the controller's
 * driver is target only (fr_hw_usb.c).
 */
#ifndef FR_USB_H
#define FR_USB_H

#include "fr.h"
#include "fr_device.h"

#define FR_USB_VID        0x2367u
#define FR_USB_PID        0x0021u		/* the OP-XY's */
#define FR_USB_PRODUCT    "OP-XY Recovery"
#define FR_USB_PID_OP1F     0x0102u
#define FR_USB_PRODUCT_OP1F "OP-1 Recovery"
#define FR_USB_EP0        64u
#define FR_USB_BULK       64u	/* full speed; 512 at high speed */
#define FR_USB_BULK_HS    512u
#define FR_USB_NOTIFY_MPS 16u	/* S-10a.021 */
/* The serial set's endpoints (S-10a.013, S-10a.019). */
#define FR_USB_EP_NOTIFY  0x81u
#define FR_USB_EP_IN      0x89u
#define FR_USB_EP_OUT     0x0Au
#define FR_USB_MAX_MA     500u
#define FR_USB_CONFIG_LEN 67u	/* wTotalLength, as S-10a.019's */

/* The descriptors: the device's 18 bytes; the configuration (its whole
 * length is returned, at most max copied) for high or full speed; string
 * index i as a USB string descriptor (0: the language list). Each returns
 * the bytes it wrote (0: none such). serial is the device's serial text
 * or NULL. dev names the product (NULL: the default device's). */
size_t fr_usb_desc_device(const struct fr_dev *dev, uint8_t *out, size_t max);
size_t fr_usb_desc_config(bool high_speed, uint8_t *out, size_t max);
size_t fr_usb_desc_qualifier(uint8_t *out, size_t max);
size_t fr_usb_desc_string(const struct fr_dev *dev, unsigned i, const char *serial, uint8_t *out,
			  size_t max);

/* The controller (target only). The device whose identity it gives, set
 * before fr_usb_start() (NULL: the default device's). */
void fr_usb_device(const struct fr_dev *dev);
void fr_usb_start(const struct fr_clock *clock);
/* Serves the controller: reset, enumeration, the class requests, the bulk
 * endpoints. Called on every pass of recovery's loop. */
void fr_usb_poll(void);
/* The configured state, and the current the configuration asks for. */
bool fr_usb_configured(void);
uint32_t fr_usb_ma(void);
/* The serial port's bytes, as an fr_bytes. */
extern const struct fr_bytes fr_usb_serial;

#endif /* FR_USB_H */
