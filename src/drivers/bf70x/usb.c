/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * USB device port: bring-up, attach and detach (waffle/drivers/bf70x_usb.h).
 * The device controller driver is usb_dcd.c; the device stack is TinyUSB's.
 */
#include "waffle/drivers/bf70x_usb.h"

#include "waffle/drivers/bf70x_sec.h"
#include "waffle/drivers/mmr.h"

#include "tusb_option.h"
#include "device/usbd.h"

static struct wb_usb_config config;
static bool inited, stack_up, attached;
/* The cycle count from which the PHY has had WB_USB_PHY_SETTLE_US. */
static uint64_t phy_settled;

/*
 * S-13a.080: USB_PLL_OSC is never written. The frequency at USB_CLKIN is not
 * known, and the reset value is the setting known to run USB on this board.
 */
void wb_usb_phy_init(void)
{
	wb_mmr_write8(USB0_PHY_CTL, USB_PHY_CTL_EN);
	wb_mmr_sync();
}

static int stack_start(void)
{
	static const tusb_rhport_init_t dev = {
		.role = TUSB_ROLE_DEVICE,
		.speed = TUSB_SPEED_HIGH,
	};

	if (!tud_rhport_init(0, &dev))
		return WB_USB_ERR_STACK;
	stack_up = true;
	return WB_USB_OK;
}

static void stack_stop(void)
{
	if (stack_up) {
		(void)tud_deinit(0);
		stack_up = false;
	}
}

int wb_usb_init(const struct wb_usb_config *cfg)
{
	int rc;

	if (!cfg || !cfg->time)
		return WB_USB_ERR_ARG;
	wb_usb_phy_init();
	phy_settled = wb_time_deadline(cfg->time, WB_USB_PHY_SETTLE_US);
	config = *cfg;
	wb_usb_set_clock(cfg->time);
	if (wb_sec_attach(WB_SID_USB0_STAT, cfg->irq_prio, false,
			  wb_usb_irq_stat, NULL) != WB_SEC_OK ||
	    wb_sec_attach(WB_SID_USB0_DATA, cfg->irq_prio, false,
			  wb_usb_irq_data, NULL) != WB_SEC_OK)
		return WB_USB_ERR_SEC;
	attached = false;
	stack_stop();
	rc = stack_start();
	inited = rc == WB_USB_OK;
	return rc;
}

int wb_usb_attach(void)
{
	if (!inited)
		return WB_USB_ERR_ARG;
	if (attached) {
		/* Long enough for any hub to report the disconnect. */
		(void)tud_disconnect();
		wb_delay_us(config.time, WB_USB_DETACH_US);
		stack_stop();
	}
	if (!stack_up) {
		const int rc = stack_start();

		if (rc != WB_USB_OK) {
			attached = false;
			return rc;
		}
	}
	/*
	 * S-13a.080: the PHY's PLL gets its time before the pull-up, on the
	 * first attach too. The time since wb_usb_init() enabled the PHY
	 * counts, so a later attach does not wait here.
	 */
	while (!wb_time_passed(phy_settled))
		;
	(void)tud_connect();
	attached = true;
	return WB_USB_OK;
}

void wb_usb_detach(void)
{
	if (!inited)
		return;
	if (stack_up)
		(void)tud_disconnect();
	stack_stop();
	attached = false;
}

/*
 * S-10a.030, S-05.016: the pull-up off, then the PHY off. A later
 * wb_usb_init() enables the PHY again, and S-13a.080's settle counts from
 * that write. Written whether or not USB came up in this start.
 */
void wb_usb_stop(void)
{
	uint8_t v;

	wb_usb_detach();
	v = wb_mmr_read8(USB0_POWER);
	if (v & USB_POWER_SOFTCONN)
		wb_mmr_write8(USB0_POWER, (uint8_t)(v & ~USB_POWER_SOFTCONN));
	wb_mmr_sync();
	wb_mmr_write8(USB0_PHY_CTL,
		      (uint8_t)(wb_mmr_read8(USB0_PHY_CTL) & ~USB_PHY_CTL_EN));
	wb_mmr_sync();
	inited = false;
}

void wb_usb_task(void)
{
	if (stack_up)
		tud_task_ext(0, false);
}

bool wb_usb_attached(void)
{
	return attached;
}

enum wb_usb_state wb_usb_state(void)
{
	if (!attached)
		return WB_USB_DETACHED;
	if (tud_suspended())
		return WB_USB_SUSPENDED;
	if (tud_mounted())
		return WB_USB_CONFIGURED;
	/* A B device's session starts when VBUS rises above session valid. */
	if (wb_mmr_read8(USB0_DEV_CTL) & USB_DEV_CTL_SESSION)
		return WB_USB_POWERED;
	return WB_USB_ATTACHED;
}
