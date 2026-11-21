/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_S10A_USB_H
#define WAFFLE_S10A_USB_H

/*
 * S-10a on TinyUSB's device core: the descriptor callbacks, one class
 * driver for every function of the sets (registered through
 * usbd_app_driver_get_cb()), the chapter 9 filter for the device controller
 * driver, and the CDC and MIDI byte paths to S-10c.
 *
 * Everything here runs in the context that calls wb_usb_task() and
 * s10a_usb_poll(), except s10a_usb_setup_filter() (interrupt context) and
 * the bus-event hook. Class callbacks only move bytes between the endpoints
 * and rings; S-10c is called from s10a_usb_poll() only.
 *
 * The board, in held mode:
 *   s10a_usb_init(&board, &host, dev);
 *   wb_usb_set_setup_filter(s10a_usb_setup_filter, NULL);
 *   wb_usb_init(&cfg);
 * and for each state of S-10a.010 that starts a set
 * (s10a_set_for_state() true):
 *   wb_usb_detach();                      (if attached)
 *   s10a_usb_select(set, serial, disk);
 *   wb_usb_attach();
 * then in its loop:
 *   wb_usb_task(); s10a_usb_poll();
 * For the MTP sets, once after s10a_usb_init():
 *   s10b_init(&mtp, &cfg); s10a_usb_mtp(&mtp);
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blockdev.h"
#include "waffle/drivers/bf70x_usb.h"
#include "waffle/s10a_desc.h"
#include "waffle/s10a_msc.h"

struct s10b;
struct s10c_host;
struct wb_device;

struct s10a_usb_board {
	void *ctx;
	/* S-07.057's input-current target in mA: 2 x bMaxPower when a
	 * configuration is selected, 100 after SET_CONFIGURATION(0), a bus
	 * reset or the loss of VBUS (S-10a.032). */
	void (*input_current)(void *ctx, unsigned ma);
	/* The bus suspended (true) or resumed (false). What a suspend sets is
	 * open (S-10a.081 item 3); nothing else changes here. May be NULL. */
	void (*suspend)(void *ctx, bool suspended);
	/* The host released the disk (S-10a.050), for S-11.045's scan. May be
	 * NULL. */
	void (*disk_released)(void *ctx);
	/* USB activity for S-07.054's charger rule: a bus
	 * reset, a suspend, a resume, the loss of VBUS. usbd passes on a
	 * suspend or resume only once the bus has been reset. May be NULL. */
	void (*activity)(void *ctx);
	/* Whether S-10c may be handed the next SysEx message now; until it
	 * is, the MIDI bytes wait in the receive ring and the host is held
	 * by flow control. NULL: always. */
	bool (*midi_ready)(void *ctx);
};

/* host may be NULL: CDC and MIDI bytes then go nowhere. dev is the device
 * whose identity the sets carry (S-10a.001, S-18.030). */
void s10a_usb_init(const struct s10a_usb_board *board,
		   struct s10c_host *host, const struct wb_device *dev);

/*
 * The set the next wb_usb_attach() presents (S-10a.010), with the serial
 * number text (S-10a.004; NULL or "" for none) and, for the disk sets, the
 * disk's blocks (S-10a.052). Call it while detached. The disk starts not
 * offered (S-10a.049).
 */
void s10a_usb_select(enum s10a_set set, const char *serial,
		     const struct wb_blockdev *disk);

/*
 * The MTP responder the MTP sets carry (S-10b), NULL for none: their
 * endpoints then stay idle and the Still Image class requests get a STALL.
 * Its DeviceInfo strings become the set's (s10b_identity()). It is driven
 * from s10a_usb_poll() only, so the volume and device ops are called from
 * there.
 */
void s10a_usb_mtp(struct s10b *m);

/* The wb_usb_setup_filter of S-10a.031 to S-10a.034 (interrupt context). */
enum wb_usb_setup_action s10a_usb_setup_filter(void *ctx,
		const uint8_t setup[8], uint8_t *reply, uint16_t *len);

/*
 * Moves pending bytes and notifications, and hands S-10c what arrived:
 * each SET_CONTROL_LINE_STATE (s10c_host_cdc_line_state) and each whole
 * SysEx message the board is ready for (s10c_host_midi_message); runs the
 * MTP responder. Bus events that reach the responder while one of its
 * requests runs (the bus serviced from a wait inside it) are recorded and
 * acted on when the request returns (S-10b.005, S-10b.007). Called
 * again from inside S-10c (a long submission that keeps the bus serviced),
 * it only moves bytes.
 */
void s10a_usb_poll(void);

/* --- The disk (S-10a.049) ---------------------------------------------- */

void s10a_usb_disk_offer(void);
/* Takes the disk from the host, for a scan or a host-link transfer (a
 * release when the host holds it) or for an own write (not one). Before
 * writing the blocks itself the board waits until s10a_usb_disk_busy() is
 * false (a command may be running). */
void s10a_usb_disk_withdraw(enum s10a_withdrawal why);
bool s10a_usb_disk_busy(void);
/* The unit as it stands: offered, its sense data, releases (S-10a.050). */
const struct s10a_lu *s10a_usb_disk(void);

/* --- The CDC port (S-10c.030 to S-10c.033) ------------------------------ */

/* Whether the port is open: configured, DTR or RTS set (S-10c.031). The
 * ctx argument lets it serve as s10c_ops.cdc_open; it is not used. */
bool s10a_usb_cdc_open(void *ctx);
/* Received bytes, at most n; returns how many. It stops where a
 * SET_CONTROL_LINE_STATE arrived until s10a_usb_poll() has restarted
 * S-10c's receiver there (S-10c.032). */
size_t s10a_usb_cdc_read(uint8_t *buf, size_t n);
/* Queues bytes for the host; returns how many fit (S-10c.033's wait is the
 * caller's). Nothing is queued while the port is not configured. */
size_t s10a_usb_cdc_write(const uint8_t *buf, size_t n);
/* Bytes queued and not yet handed to the controller. */
size_t s10a_usb_cdc_pending(void);
/* What s10a_usb_cdc_write() would take now; 0 while not configured. */
size_t s10a_usb_cdc_room(void);

/* --- MIDI (S-10c.003) --------------------------------------------------- */

/*
 * One whole SysEx message as event packets on cable 0; usable as
 * s10c_ops.send_sysex. A message that does not fit in what is queued is
 * dropped whole, never in part; so is any while MIDI is not configured.
 */
void s10a_usb_send_sysex(void *ctx, const uint8_t *msg, size_t n);
/*
 * The queue's free bytes, 0 while MIDI is not configured, and what one
 * message of n bytes takes of them: a caller sending several messages that
 * belong together sends them only when their sizes add up to the room.
 */
size_t s10a_usb_midi_room(void);
size_t s10a_usb_sysex_size(size_t n);
/* Whether the set's MIDI function is configured. */
bool s10a_usb_midi_open(void);

#endif /* WAFFLE_S10A_USB_H */
