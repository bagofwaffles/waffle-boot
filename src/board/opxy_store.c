/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The running slot's settings sector on the master's flash
 * (waffle/board/opxy_services.h). */
#include "waffle/board/opxy_boot.h"

void opxy_store_defaults(const struct opxy_master *m, struct s02_store *s)
{
	s02_store_defaults(s);
	s->color = s15_color_default(opxy_dev(m));
}

/* S-02.097: the defaults when unusable, unreadable or not valid, the
 * color's by device (S-15.173); a stored record gives every setting. */
static void store_read(struct opxy_master *m, struct s02_store *out)
{
	if (!s02_store_read(&m->flash, &m->store_slot, out))
		opxy_store_defaults(m, out);
}

void opxy_boot_store_load(struct opxy_master *m, const struct s02_store_slot *run)
{
	m->store_slot = *run;
	store_read(m, &m->sv.store);
}

static enum s15_store_save outcome(struct opxy_master *m, enum s02_store_rc rc,
				   bool clear)
{
	switch (rc) {
	case S02_STORE_OK:
		return S15_STORE_SAVED;
	case S02_STORE_UNUSABLE:
		s14_log_str(&m->sv.log, S14_WARNING, S14_TAG("settings"),
			    clear ? "not cleared: this slot keeps no settings" :
				    "not saved: this slot keeps no settings");
		return S15_STORE_UNUSABLE;
	case S02_STORE_IO:
	case S02_STORE_VERIFY:
	default:
		s14_log_str(&m->sv.log, S14_ERROR, S14_TAG("settings"),
			    clear ? "clear failed" : "save failed");
		return S15_STORE_FAILED;
	}
}

enum s15_store_save opxy_boot_store_save(struct opxy_master *m,
					 const struct s02_store *s,
					 struct s02_store *now)
{
	enum s02_store_rc rc;

	/* S-13b.017: a frame that ends with a controller error is sent again,
	 * as for the other writers; the save itself is made once */
	opxy_flash_writer(&m->hw, true);
	rc = s02_store_write(&m->flash, &m->store_slot, s);
	opxy_flash_writer(&m->hw, false);

	/* S-15.175: the items show what a start would read */
	store_read(m, now);
	return outcome(m, rc, false);
}

enum s15_store_save opxy_boot_store_clear(struct opxy_master *m)
{
	enum s02_store_rc rc;

	opxy_flash_writer(&m->hw, true);
	rc = s02_store_clear(&m->flash, &m->store_slot);
	opxy_flash_writer(&m->hw, false);
	if (rc == S02_STORE_OK)
		s14_log_str(&m->sv.log, S14_INFO, S14_TAG("settings"), "cleared");
	return outcome(m, rc, true);
}
