/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Held mode's settings' defaults (waffle/ui/settings.h). */
#include "waffle/ui/settings.h"

#include <stddef.h>

bool s15_color_default(const struct wb_device *dev)
{
	return dev != NULL && dev->id == WB_DEV_OP1F;
}

void s15_settings_defaults(struct s15_settings *s, const struct wb_device *dev)
{
	s->dev_mode = false;
	s->color = s15_color_default(dev);
	s->mtp_macos = true;
	s->start_menu = false;
	s->host_link = true;
}
