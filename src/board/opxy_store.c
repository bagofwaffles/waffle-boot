/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The boot store on the master's flash (waffle/board/opxy_services.h). */
#include "waffle/board/opxy_boot.h"

void opxy_boot_store_load(struct opxy_master *m)
{
	/* S-02.097: the defaults when unusable, unreadable or not valid */
	(void)s02_store_read(&m->flash, &m->sv.store);
}

enum s15_store_save opxy_boot_store_save(struct opxy_master *m,
					 const struct s02_store *s,
					 struct s02_store *now)
{
	enum s02_store_rc rc;

	/* S-13b.017: a frame that ends with a controller error is sent again,
	 * as for the other writers; the save itself is made once */
	opxy_flash_writer(&m->hw, true);
	rc = s02_store_write(&m->flash, s);
	opxy_flash_writer(&m->hw, false);

	/* S-15.175: the item shows what a start would read */
	(void)s02_store_read(&m->flash, now);
	switch (rc) {
	case S02_STORE_OK:
		return S15_STORE_SAVED;
	case S02_STORE_UNUSABLE:
		s14_log_str(&m->sv.log, S14_WARNING, S14_NO_TAG,
			    "boot store: slot 2's image covers its sector");
		return S15_STORE_UNUSABLE;
	case S02_STORE_IO:
	case S02_STORE_VERIFY:
	default:
		s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG,
			    "boot store: write failed");
		return S15_STORE_FAILED;
	}
}
