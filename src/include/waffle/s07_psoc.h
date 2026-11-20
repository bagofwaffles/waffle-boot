/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-07: the master's side of the PSoC link. The exchanges of S-07 are
 * functions the bootloader's phases call in the order of S-07.023 (or
 * s07_startup() for steps 1-6a); what the bootloader decides between them
 * (S-05's boot decision, S-15's menus, S-04's shutdown) stays with their
 * owners and reaches this module through struct s07_ops.
 *
 * Every function that waits returns S07_OK, or S07_POWEROFF when the PSoC
 * reported a power-off during it (S-07.051). After S07_POWEROFF the
 * shutdown has run, 0x32 {s} has been sent, the module sends nothing more,
 * and the caller is to wait for the PSoC to remove the power.
 */
#ifndef WAFFLE_S07_PSOC_H
#define WAFFLE_S07_PSOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s06_link.h"
#include "waffle/s07_cypress.h"
#include "waffle/s07_rules.h"

#define S07_OK 0
#define S07_POWEROFF 1

/* Log events for S-14 (the text is S-14's); a and b as noted. */
#define S07_LOG_GREET_REJECTED 1	/* a: the 0x06 first byte */
#define S07_LOG_GREET_FAILED 2		/* the sequence ended ungreeted */
#define S07_LOG_WARNING_00 3		/* a 0x00 message */
#define S07_LOG_BATTERY 4		/* a: charge, b: mV */
#define S07_LOG_BATTERY_REMOVED 5
#define S07_LOG_KEY 6			/* a: key, b: 1 down / 0 up */
#define S07_LOG_KEYS_NO_REPLY 7		/* no 0x41 reply */
#define S07_LOG_KEY_STATUS 8		/* a: hold|debug<<1|precomp<<2 */
#define S07_LOG_VBUS 9			/* a: st, b: n x 10 mA */
#define S07_LOG_PORT 10			/* a: payload bytes 0-2 */
#define S07_LOG_REPLY_ID 11		/* a waited 0x30/0x36 got id a */
#define S07_LOG_BACKPLANE 12		/* a: result (1 ok), b: at<<8|why */
#define S07_LOG_POWER 13		/* a: state */

struct s07_ops {
	void *ctx;
	/* S-14 log line for an event. */
	void (*log)(void *ctx, int event, uint32_t a, uint32_t b);
	/* S-07.031: a key change, for S-15's menus. */
	void (*key_event)(void *ctx, uint8_t key, bool down);
	/*
	 * S-07.051 step 1: S-04.063's shutdown, which must call
	 * s07_shutdown_reports() (S-07.072).
	 */
	void (*shutdown)(void *ctx);
	/* S-07.054: start (true) or stop USB mass storage (S-10). */
	void (*usb_msc)(void *ctx, bool on);
	/*
	 * S-05.004: draw and light the boot logo. Called once by
	 * s07_startup(), after the battery wait's first requests and any
	 * charge wait, and before the subscription. NULL: no logo here.
	 */
	void (*early_logo)(void *ctx);
	/*
	 * Called on every round of a wait loop, to run USB and the screen.
	 * It must not apply the 1 s charger rule S-07.057 permits (S-16.192):
	 * in the battery wait the 3 s rule is the only one (S-07.054).
	 */
	void (*service)(void *ctx);
	/*
	 * S-07.076: a PSoC message with an id outside table 2 (other than the
	 * ping, which this module answers): S-10c's common command set. NULL:
	 * logged nothing, nothing sent.
	 */
	void (*common)(void *ctx, const struct s06_frame *f);
	/* S-07.082's CRC variant (CRC-16/X-25). */
	s07_crc16_fn crc16;
};

struct s07_psoc {
	struct s06_node *node;
	unsigned link;			/* the node's PSoC link */
	const struct s07_ops *ops;

	bool greeted;			/* S-07.020 */
	bool greeting;			/* a greeting sequence is running */
	bool started;			/* S-07.025 */
	bool off;			/* S-07.051 step 3: nothing more */
	bool shut;			/* S-07.072: the shutdown ran */
	bool first_pass_done;		/* S-07.043 */
	unsigned state;			/* S-07.050, S07_POWER_NONE at first */
	struct s07_battery batt;
	uint32_t applied_ma;		/* S-07.055 */
	uint32_t target_ma;		/* S-07.057 */
	bool usb_configured;
	uint32_t last_usb_us;
	uint8_t keys[S07_BITMAP];	/* S-07.031: known state, from S-07.032's reply on */

	/* S-07.041: last values sent */
	uint8_t led_key[S07_KEYS][2];
	uint8_t led_rgb[3];
	int led_bar;			/* -1: none sent */
	bool charge_led_on;		/* false while the key test runs */

	/* S-07.045 key test */
	bool key_test;
	unsigned kt_down;
	unsigned kt_lone;		/* S07_KEYS: none */
	unsigned kt_presses;
	uint8_t kt_tested[S07_BITMAP];
};

void s07_init(struct s07_psoc *p, struct s06_node *node, unsigned link,
	      const struct s07_ops *ops);

/* Handle the PSoC messages received so far (table 2). */
int s07_poll(struct s07_psoc *p);

/*
 * A power message (id 0x00, 0x31, 0x32, 0x35 or 0x37) received from another
 * peer, handled as table 2 handles it from the PSoC: S-08.004 sends the
 * Nordic's here. Other ids are ignored. Returns S07_POWEROFF when it started
 * a power-off (on the PSoC link, S-07.051).
 */
int s07_power_message(struct s07_psoc *p, const struct s06_frame *f);

/* Wait us, receiving (and forwarding) but handling nothing. */
void s07_sleep(struct s07_psoc *p, uint32_t us);

/* S-07.020: the greeting sequence; *greeted says how it ended. */
int s07_greet(struct s07_psoc *p, bool *greeted);

/* S-07.020-S-07.022: greeting (with S-07.021's fallback, a wait that the
 * PSoC's greeting ends), power queries. */
int s07_open(struct s07_psoc *p);

/* S-07.054: the battery wait's first two requests and any charge wait.
 * Its loop is s07_battery_rest(), after the key status (S-07.023). */
int s07_battery_wait(struct s07_psoc *p);

/* S-07.025: the first key subscription; the link is started. */
void s07_subscribe(struct s07_psoc *p);

/* S-07.032: the key-status request. Its reply is the known key state from
 * then on (S-07.031), keys held at power-on included, set without events. */
int s07_key_status(struct s07_psoc *p, struct s07_key_status *ks);

/*
 * S-01.023: the link as the front left it. s07_resume() takes its greeting,
 * its subscription (the link is started), the last power state and battery
 * report it saw; s07_battery_rest() is S-07.054's wait loop alone, without
 * the first requests, the charge wait, the early logo and the subscription,
 * which the front made; s07_key_status_given() reads the front's key-status
 * reply (pl NULL: none came) as s07_key_status() would.
 */
void s07_resume(struct s07_psoc *p, bool greeted, bool state_known, uint8_t state,
		const uint8_t *batt, size_t batt_len);
int s07_battery_rest(struct s07_psoc *p);
void s07_key_status_given(struct s07_psoc *p, const uint8_t *pl, size_t n,
			  struct s07_key_status *ks);

/*
 * S-07.023 steps 1-6a: s07_open, the start report (S-07.066; the caller
 * reads the display handshake first), the battery wait's first requests and
 * any charge wait, the early logo, the subscription, the key status, then
 * the battery wait's loop.
 */
int s07_startup(struct s07_psoc *p, struct s07_key_status *ks);

/* S-07.061: the PMIC sequence (boot path only). */
int s07_pmic(struct s07_psoc *p);

/* S-07.065 phase reports. */
#define S07_REPORT_START 0	/* {00 00 00} */
#define S07_REPORT_HELD 1	/* {3C FF 02} */
#define S07_REPORT_BOOT 2	/* {3C FF 32} */
#define S07_REPORT_SHUTDOWN 3	/* {00 00 02} */
void s07_report(struct s07_psoc *p, int which);

/* S-07.071: the PMIC sequence then {3C FF 32}; then only log lines. */
int s07_handover(struct s07_psoc *p);

/*
 * S-07.077: the coprocessor info request 0x09 (held mode, once). Copies
 * the reply payload (S-17 reads it) and returns its length, or -1 without
 * a reply within 100 ms.
 */
int s07_info_request(struct s07_psoc *p, uint8_t *buf, size_t max,
		     int *status);

/*
 * One pass of the service loop: on the first pass 0x40 {01} and the bar
 * (S-07.043); the PSoC's messages; the limit step (S-07.056).
 */
int s07_service_pass(struct s07_psoc *p);

/* S-07.072: {00 00 02} then 0x40 {01}, once. */
void s07_shutdown_reports(struct s07_psoc *p);

/* S-07.070: 0x97 {DE AD BE EF} before a soft reboot. */
void s07_reset_request(struct s07_psoc *p);

/* S-07.078: 0x48 {01}. */
void s07_volcal(struct s07_psoc *p);

/* S-07.057 events from S-10a. */
void s07_usb_configured(struct s07_psoc *p, uint32_t ma);
void s07_usb_reset(struct s07_psoc *p);	/* SET_CONFIGURATION(0), reset, VBUS loss */
void s07_usb_activity(struct s07_psoc *p);

/* S-07.040, S-07.041: LEDs through the change filter. */
void s07_led_key(struct s07_psoc *p, uint8_t key, uint8_t a, uint8_t b);
void s07_led_bar(struct s07_psoc *p, uint8_t n);
void s07_led_rgb(struct s07_psoc *p, const uint8_t rgb[3], bool forced);
void s07_led_all_off(struct s07_psoc *p);
void s07_update_charge_led(struct s07_psoc *p);

/* S-07.045 */
void s07_key_test_start(struct s07_psoc *p);
void s07_key_test_stop(struct s07_psoc *p);

/*
 * S-07.080-S-07.085: the backplane step. *ok is the last run's result
 * (S-07.084); the return wait of S-07.085 follows either way. No exit
 * packet is written between runs or after the last failed run; the only
 * ones are those a run ends with and the return wait's (S-07.084).
 */
int s07_backplane(struct s07_psoc *p, const struct s07_cyacd *f,
		  const uint8_t unlock[S07_CY_UNLOCK_LEN], bool *ok);

#endif
