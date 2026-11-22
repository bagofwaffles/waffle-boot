/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Held mode's settings' defaults (waffle/s15_settings.h). */
#include "waffle/s15_settings.h"

void s15_settings_defaults(struct s15_settings *s)
{
	s->dev_mode = false;
	s->color = false;
	s->mtp_macos = true;
	s->start_menu = false;
	s->host_link = true;
}
