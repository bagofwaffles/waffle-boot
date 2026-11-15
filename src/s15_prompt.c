/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-15.180: the boot prompt's lines and keys (see waffle/s15_prompt.h).
 */
#include "waffle/s15_prompt.h"

const char s15_prompt_title[] = "Boot";

static bool bit(const uint8_t *set, unsigned k)
{
	return (((unsigned)set[k / 8u] >> (k % 8u)) & 1u) != 0u;
}

static void set_bit(uint8_t *set, unsigned k, bool v)
{
	if (v)
		set[k / 8u] = (uint8_t)(set[k / 8u] | (1u << (k % 8u)));
	else
		set[k / 8u] = (uint8_t)(set[k / 8u] & ~(1u << (k % 8u)));
}

/* `Slot <n>` for the slot index, or `Unknown` for a slot that is not known
 * (S-15.180). */
static void slot_text(struct s15_buf *b, int slot)
{
	if (slot < 0) {
		s15_buf_str(b, "Unknown");
		return;
	}
	s15_buf_str(b, "Slot ");
	s15_buf_dec(b, s15_slot_number((unsigned)slot));
}

/* v as n upper-case hexadecimal digits. */
static void hex(struct s15_buf *b, uint32_t v, unsigned n)
{
	static const char digits[] = "0123456789ABCDEF";

	while (n-- > 0u) {
		const char c[2] = { digits[(v >> (4u * n)) & 0xFu], '\0' };

		s15_buf_str(b, c);
	}
}

void s15_prompt_lines(const struct s15_prompt_values *v,
		      struct s15_buf out[S15_PROMPT_LINES],
		      enum s15_style style[S15_PROMPT_LINES])
{
	unsigned i;

	for (i = 0; i < S15_PROMPT_LINES; i++) {
		s15_buf_init(&out[i]);
		style[i] = S15_ST_TEXT;
	}
	style[1] = S15_ST_ITEM;
	s15_buf_str(&out[1], "1 Start the OS");
	style[2] = S15_ST_ITEM;
	s15_buf_str(&out[2], "2 Bootloader menus");
	style[4] = S15_ST_KV;
	s15_buf_str(&out[4], "Running\t");
	slot_text(&out[4], v->running);
	s15_buf_str(&out[4], "\tSelector\t");
	slot_text(&out[4], v->selector);
	style[5] = S15_ST_KV;
	s15_buf_str(&out[5], "Verified\t");
	if (v->verified < 0)
		s15_buf_str(&out[5], "Unknown");
	else
		hex(&out[5], (uint32_t)v->verified & 0xFFu, 2u);
	s15_buf_str(&out[5], "\tEntry SP\t");
	if (v->sp_kept) {
		s15_buf_str(&out[5], "0x");
		hex(&out[5], v->entry_sp, 8u);
	} else {
		s15_buf_str(&out[5], "Not kept");
	}
	style[6] = S15_ST_KV;
	s15_buf_str(&out[6], "Debug key\t");
	s15_buf_str(&out[6], v->debug ? "Held" : "Not held");
}

void s15_prompt_show(struct s15_menu *m, const struct s15_prompt_values *v)
{
	struct s15_buf lines[S15_PROMPT_LINES];
	enum s15_style st[S15_PROMPT_LINES];
	unsigned i;

	s15_prompt_lines(v, lines, st);
	/* as the menus open a screen: the title, the console cleared */
	m->title = s15_prompt_title;
	m->changes++;
	s15_console_clear(&m->con);
	for (i = 0; i < S15_PROMPT_LINES; i++)
		s15_console_add(&m->con, st[i], lines[i].s);
}

void s15_prompt_open(struct s15_prompt *p, const uint8_t *down_now)
{
	unsigned i;

	for (i = 0; i < sizeof p->stale; i++)
		p->stale[i] = down_now != NULL ? down_now[i] : 0u;
	p->choice = S15_PROMPT_WAIT;
}

/* S-15.180: a key down when the screen was shown acts only after it has
 * gone up and down again; the first digit 1 or 2 to go down chooses, and
 * every later event changes nothing (the choice is acted on at once). */
void s15_prompt_key(struct s15_prompt *p, unsigned key, bool down)
{
	unsigned d;

	if (key >= S15_NKEYS)
		return;
	if (bit(p->stale, key)) {
		if (!down)
			set_bit(p->stale, key, false);
		return;
	}
	if (!down || p->choice != S15_PROMPT_WAIT)
		return;
	d = s15_digit(key);			/* S-15.011 */
	if (d == 1u)
		p->choice = S15_PROMPT_START;
	else if (d == 2u)
		p->choice = S15_PROMPT_HELD;
}

enum s15_prompt_choice s15_prompt_choice(const struct s15_prompt *p)
{
	return p->choice;
}
