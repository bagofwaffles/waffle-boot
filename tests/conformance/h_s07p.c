/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host program for S-07's PSoC link (src/s07_psoc.c, src/s07_program.c) on
 * the unit tests' virtual clock and fake port (tests/unit/s07_test.h),
 * against a PSoC the Python driver plays with s07_model.
 *
 * Unlike the other programs this one is a conversation. Before the run, one
 * line each, then "go":
 *   setup BOOT OSLOADS TAIL_MS END_MS [PROMPT]  the transcript's setup (END_MS -1:
 *                                       none; PROMPT 1: a build with the boot
 *                                       prompt, S-05.025)
 *   cyacd BYTES                         the backplane file
 *   unlock BYTES                        the PSoC unlock (none: no unlock)
 *   action T_MS backplane               s07_program at T_MS
 *   action T_MS key_test_start          the key test starts 100 ms after T_MS
 *                                       (S-07.045)
 *   action T_MS continue_os             Continue to the OS at T_MS with the
 *                                       slave present (S-05.033): a key test's
 *                                       end, the view's 750 ms hold with no
 *                                       message handled, the shutdown reports
 *                                       (S-07.072), and the run ends
 *   P T_US f ROUTE ID PAYLOAD           a PSoC frame arriving at T_US
 *   P T_US r BYTES                      PSoC bytes arriving at T_US
 * The run then follows S-07.023 as the unit tests' driver does: the start-up,
 * held mode (S-04), the actions. Every write of the master is printed,
 *   M T_US f ROUTE ID PAYLOAD | M T_US r BYTES
 * and the program reads the PSoC's answer to it, P lines up to a line ".".
 * At the end: "end T_US RC OK" (RC the last S-07 call's, OK the backplane
 * result, -1 without one).
 */
#include <stdio.h>
#include <string.h>

#include "hlib.h"
#include "waffle/s07_program.h"
#include "waffle/s15_prompt.h"

#include "../unit/s07_test.h"

#define MAX_ACTIONS 8

static int boot_allowed, os_loads, boot_prompt;
static long tail_ms, end_ms;
static struct s15_prompt *prompt_shown;
static uint8_t cyacd[H_MAXLINE / 2];
static size_t cyacd_len;
static uint8_t bl_unlock[S07_CY_UNLOCK_LEN];
static bool have_unlock;
static uint32_t action_ms[MAX_ACTIONS];
enum act { ACT_BACKPLANE, ACT_KEY_TEST, ACT_CONTINUE };
static enum act action_kind[MAX_ACTIONS];
static size_t nactions, next_action;
static int result = -1;
static bool restarted;			/* Continue to the OS ran */

/* A P line into the port; 0 if it is not one. */
static int psoc_item(const struct h_cmd *c)
{
	uint8_t d[256];
	size_t n;

	if (c->n == 4 && strcmp(c->tok[2], "r") == 0) {
		n = h_hex(c, 3, d, sizeof d);
		t_port_inject(&rig.port, (uint32_t)h_num(c, 1), d, n);
		return 1;
	}
	if (c->n == 6 && strcmp(c->tok[2], "f") == 0) {
		n = h_hex(c, 5, d, sizeof d);
		t_rig_from_psoc((uint32_t)h_num(c, 1), (uint8_t)h_num(c, 3),
				(uint8_t)h_num(c, 4), d, n);
		return 1;
	}
	return 0;
}

static void put_hex(const uint8_t *d, size_t n)
{
	if (!n)
		fputs(" -", stdout);
	else
		putchar(' ');
	for (size_t i = 0; i < n; i++)
		printf("%02x", d[i]);
}

/* Every write: shown to the driver, which answers with the PSoC's items. */
static void on_item(const struct t_rig *r, const struct t_item *it)
{
	struct h_cmd c;

	(void)r;
	if (it->raw) {
		printf("M %u r", (unsigned)it->t);
	} else {
		printf("M %u f 0x%02x 0x%02x", (unsigned)it->t, it->route,
		       it->id);
	}
	put_hex(it->d, it->len);
	putchar('\n');
	fflush(stdout);
	while (h_next(&c)) {
		if (c.n == 1 && strcmp(c.tok[0], ".") == 0)
			return;
		if (strcmp(c.tok[0], "P") != 0 || !psoc_item(&c))
			h_error(&c, "PSoC item");
	}
	fprintf(stderr, "h_s07p: input ended during the run\n");
	exit(1);
}

static int actions_due(void)
{
	int rc = S07_OK;

	while (!rc && !restarted && next_action < nactions &&
	       rig.sim.now >= S06_MS(action_ms[next_action])) {
		const enum act a = action_kind[next_action++];
		bool ok;

		if (a == ACT_KEY_TEST) {
			s07_sleep(&rig.psoc, S06_MS(100));
			s07_key_test_start(&rig.psoc);
			continue;
		}
		if (a == ACT_CONTINUE) {
			/* S-05.033, as tests/unit/test_s07_transcripts.c */
			if (rig.psoc.key_test)
				s07_key_test_stop(&rig.psoc);
			s07_sleep(&rig.psoc, S06_MS(750));
			s07_shutdown_reports(&rig.psoc);
			restarted = true;
			continue;
		}
		rc = s07_program(&rig.psoc, (const char *)cyacd, cyacd_len,
				 have_unlock ? bl_unlock : 0,
				 &ok);
		result = ok;
	}
	return rc;
}

static void prompt_key(void *ctx, uint8_t key, bool down)
{
	t_rig_key(ctx, key, down);
	if (prompt_shown != NULL)
		s15_prompt_key(prompt_shown, key, down);
}

/* S-05.025, S-07.023 step 6b (S-15.180): the waiting bitmaps first, then
 * the PSoC's messages until a digit chooses, or end_ms. */
static enum s15_prompt_choice prompt_wait(int *rc)
{
	struct s15_prompt p;
	uint32_t end = end_ms >= 0 ? S06_MS((uint32_t)end_ms) :
				     rig.sim.now + S06_MS(200);

	rig.ops.key_event = prompt_key;
	*rc = s07_poll(&rig.psoc);
	if (*rc)
		return S15_PROMPT_WAIT;
	s15_prompt_open(&p, rig.psoc.keys);
	prompt_shown = &p;
	while (s15_prompt_choice(&p) == S15_PROMPT_WAIT && rig.sim.now < end) {
		*rc = s07_poll(&rig.psoc);
		if (*rc)
			break;
		s07_sleep(&rig.psoc, S06_MS(1));
	}
	prompt_shown = NULL;
	return s15_prompt_choice(&p);
}

/* S-07.023, as tests/unit/test_s07_transcripts.c drives it. */
static int drive(void)
{
	struct s07_key_status ks;
	uint8_t info[256];
	uint32_t end;
	int rc, st;

	rc = s07_startup(&rig.psoc, &ks);
	if (rc)
		return rc;
	if (boot_allowed && !ks.hold) {
		bool held = false;

		if (boot_prompt) {
			enum s15_prompt_choice c = prompt_wait(&rc);

			if (rc)
				return rc;
			if (c == S15_PROMPT_WAIT) {
				fprintf(stderr, "h_s07p: the prompt got no digit\n");
				exit(1);
			}
			held = c == S15_PROMPT_HELD;
		}
		if (!held) {
			rc = s07_handover(&rig.psoc);
			if (rc)
				return rc;
			if (os_loads) {	/* S-07.071: then only log lines */
				s07_sleep(&rig.psoc, S06_MS((uint32_t)tail_ms));
				return S07_OK;
			}
		}
	}
	(void)s07_info_request(&rig.psoc, info, sizeof info, &st);
	s07_report(&rig.psoc, S07_REPORT_HELD);
	end = end_ms >= 0 ? S06_MS((uint32_t)end_ms) : rig.sim.now + S06_MS(200);
	while (rig.sim.now < end) {
		rc = actions_due();
		if (rc || restarted)
			return rc;
		rc = s07_service_pass(&rig.psoc);
		if (rc)
			return rc;
		s07_sleep(&rig.psoc, S06_MS(10));
	}
	return s07_poll(&rig.psoc);
}

int main(void)
{
	struct h_cmd c;
	int rc;

	(void)t_failures;
	(void)t_case;
	t_rig_init(S06_MS(120000));
	while (h_next(&c)) {
		const char *k = c.tok[0];

		if (strcmp(k, "go") == 0 && c.n == 1)
			break;
		if (strcmp(k, "setup") == 0 && (c.n == 5 || c.n == 6)) {
			boot_allowed = (int)h_num(&c, 1);
			os_loads = (int)h_num(&c, 2);
			tail_ms = (long)h_num(&c, 3);
			end_ms = strcmp(c.tok[4], "-1") == 0 ? -1 : (long)h_num(&c, 4);
			boot_prompt = c.n == 6 ? (int)h_num(&c, 5) : 0;
		} else if (strcmp(k, "cyacd") == 0 && c.n == 2) {
			cyacd_len = h_hex(&c, 1, cyacd, sizeof cyacd);
		} else if (strcmp(k, "unlock") == 0 && c.n == 2) {
			if (h_hex(&c, 1, bl_unlock, sizeof bl_unlock) !=
			    sizeof bl_unlock)
				h_error(&c, "unlock length");
			have_unlock = true;
		} else if (strcmp(k, "action") == 0 && c.n == 3 &&
			   (strcmp(c.tok[2], "backplane") == 0 ||
			    strcmp(c.tok[2], "key_test_start") == 0 ||
			    strcmp(c.tok[2], "continue_os") == 0) &&
			   nactions < MAX_ACTIONS) {
			action_kind[nactions] =
				strcmp(c.tok[2], "continue_os") == 0 ? ACT_CONTINUE :
				strcmp(c.tok[2], "key_test_start") == 0 ? ACT_KEY_TEST :
				ACT_BACKPLANE;
			action_ms[nactions++] = (uint32_t)h_num(&c, 1);
		} else if (strcmp(k, "P") != 0 || !psoc_item(&c)) {
			h_error(&c, "unknown command");
		}
	}
	if (h_status())
		return h_status();
	rig.on_item = on_item;
	rc = drive();
	printf("end %u %d %d\n", (unsigned)rig.sim.now, rc, result);
	return h_status();
}
