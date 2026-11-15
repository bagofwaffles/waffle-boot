/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-05.025, S-15.180: the boot prompt on the master, in a build with it on
 * (OPXY_BOOT_PROMPT, make BOOT_PROMPT=1). In every other build this file
 * compiles to nothing and the boot master's op is NULL.
 *
 * The prompt comes after the slave's init session, and before S-05.003's
 * PMIC step and the hand-over frame. Its frame is drawn in the console's
 * frame under the title Boot, lit by the early logo's 0x51 {3C FF 02}
 * (S-13c.019: the prompt sends no 0x51 of its own). While it waits the
 * PSoC's messages are handled as S-07.076 says, with the boot ops of
 * S-10c.045 (opxy_services.c, host_boot_start); nothing is written to any
 * medium and no bridge request is made.
 */
#include "waffle/board/opxy_boot.h"

#if OPXY_BOOT_PROMPT

#include "waffle/drivers/bf70x_core.h"
#include "waffle/s11_version.h"
#include "waffle/s15_prompt.h"

static struct s15_prompt prompt;
/* The board's key hook (opxy_master.c's, for the menus), kept while the
 * prompt's stands in for it. */
static void (*menu_key)(void *ctx, uint8_t key, bool down);

/* S-07.031's events while the prompt is shown: the menus keep their key
 * state as on every boot (S-15.013), and the prompt reads the event. */
static void prompt_key(void *ctx, uint8_t key, bool down)
{
	if (menu_key != NULL)
		menu_key(ctx, key, down);
	s15_prompt_key(&prompt, key, down);
}

/* S-15.180's values: the running slot as the boot established it (S-01.002,
 * kept by the slave's start), the selector and its verified byte as read
 * now (S-02; a read is no write), the INIT entry SP (S-01.007) and the
 * debug key (S-07.032). */
static void values(struct opxy_master *m, struct s15_prompt_values *v)
{
	struct s02_selector sel;

	v->running = m->running_established ? (int)m->running_slot : -1;
	if (s02_read_selector(&m->flash, &sel) == S02_OK) {
		v->selector = (int)s02_selected_slot(sel.index);
		v->verified = sel.verified;
	} else {
		v->selector = -1;
		v->verified = -1;
	}
	v->sp_kept = true;
	v->entry_sp = opxy_init_sp;
	v->debug = m->debug_key;
}

/*
 * S-15.180: the status bar shows the running slot and the version text as
 * on every screen; USB stays dark gray and the frame is in color, since
 * the settings are read only when held mode starts
 * (opxy_held_screen() fills the rest of sv->boot then).
 */
static void frame_info(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;
	static const struct s11_version v = {
		OPXY_BL_VERSION_MAJOR, OPXY_BL_VERSION_MINOR,
		OPXY_BL_VERSION_PATCH, OPXY_BL_VERSION_BUILD,
	};

	sv->bl_text[s11_version_text(&v, sv->bl_text, S11_VERSION_TEXT_MAX)] = '\0';
	sv->boot.version = sv->bl_text;
	sv->boot.running_known = m->running_established;
	sv->boot.running_slot = m->running_slot;
}

/* The frame drawn and sent: the boot path has not set held mode's draw
 * ops, so they are taken here. */
static void draw(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;
	const struct s15_geometry *g = opxy_display_geometry();
	struct s15_frame f;

	/* cppcheck-suppress knownConditionTrueFalse ; a weak default */
	if (g == NULL)
		return;
	opxy_display_draw_ops(m, &sv->draw);
	s15_menu_frame(&sv->menu, &f);
	s15_render_console(&sv->menu.con, g, &sv->draw, &f);
	opxy_display_show();
}

int opxy_boot_prompt(void *ctx)
{
	struct opxy_master *m = ctx;
	struct s15_prompt_values v;

	frame_info(m);
	values(m, &v);
	s15_prompt_show(&m->sv.menu, &v);	/* logged */
	draw(m);
	/* S-02.095: the prompt shown, alive; the one write, before the wait. */
	opxy_start_finished(m);
	/*
	 * S-15.180: every bitmap that arrived before the screen was shown is
	 * taken into account first; its events choose nothing. The keys
	 * down then act after an up and a down.
	 */
	if (s07_poll(&m->psoc) == S07_POWEROFF)
		goto off;
	s15_prompt_open(&prompt, m->psoc.keys);
	menu_key = m->psoc_ops.key_event;
	m->psoc_ops.key_event = prompt_key;
	while (s15_prompt_choice(&prompt) == S15_PROMPT_WAIT) {
		/* S-07.076: the PSoC's messages, and only their answers. */
		if (s07_poll(&m->psoc) == S07_POWEROFF)
			goto off;
		s06_idle(&m->clock, s06_now(&m->clock) + 1000u);
	}
	m->psoc_ops.key_event = menu_key;
	return s15_prompt_choice(&prompt) == S15_PROMPT_START ?
		       BOOT_PROMPT_START : BOOT_PROMPT_HELD;
off:
	/* S-07.051: the PSoC removes the power. */
	opxy_powered_off(m);
}

#else

/* No prompt in this build: nothing to compile. */
typedef int opxy_prompt_off;

#endif /* OPXY_BOOT_PROMPT */
