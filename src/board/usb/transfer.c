/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11.050's transfer storage on the master: a host-link transfer's bytes
 * held as a file of the bootloader's own in the RAM disk's free space
 * (S-12.024), while the disk is withdrawn (S-12.018).
 *
 * The file takes a short name no entry of the root has, so no host file's
 * name is ever taken. Before it is removed its clusters are overwritten
 * with 0x00 to their end: FatFs writes a last, partial sector whole from
 * its buffer, which still holds earlier bytes past the file's end. The
 * clusters it frees then read as zeros, and the rest of the free space as
 * at the begin. The package's zip is recovered in place in this file
 * (S-11.018, opxy_install_source()), so it too is overwritten with the
 * rest.
 */
#include "waffle/board/services.h"

#include <string.h>

#include "waffle/board/boot.h"

/* "~WBX0000.TMP": one 8.3 entry, which FatFs writes without a long name */
#define NAME_TRIES 10000u

static struct {
	bool on;		/* a transfer holds the volume and its file */
	bool offered;		/* the disk was offered at the begin */
	FIL fil;
	char name[13];
	uint32_t len;		/* bytes stored */
	uint32_t chunks;	/* For the transfer's progress lines */
} tr;

static uint8_t zeros[512];

static bool new_name(void)
{
	unsigned i;

	for (i = 0; i < NAME_TRIES; i++) {
		memcpy(tr.name, "~WBX0000.TMP", sizeof(tr.name));
		tr.name[4] = (char)('0' + i / 1000u);
		tr.name[5] = (char)('0' + i / 100u % 10u);
		tr.name[6] = (char)('0' + i / 10u % 10u);
		tr.name[7] = (char)('0' + i % 10u);
		if (f_stat(tr.name, NULL) == FR_NO_FILE)
			return true;
	}
	return false;
}

/* S-12.024: the file's clusters zeroed, the file removed, FatFs
 * unmounted; open says whether tr.fil holds it open. A long write: under
 * the host hold (S-10a.049). */
static void wipe(struct opxy_master *m, bool open)
{
	FIL *f = &tr.fil;
	bool ok;

	opxy_host_hold_begin(m);
	ok = open || f_open(f, tr.name, FA_WRITE | FA_OPEN_EXISTING) == FR_OK;
	if (ok) {
		/* to the last cluster's end, which the file already holds */
		const uint32_t cl = (uint32_t)f->obj.fs->csize * FF_MAX_SS;
		uint32_t left = ((uint32_t)f_size(f) + cl - 1u) / cl * cl;
		bool zeroed = f_lseek(f, 0) == FR_OK;
		UINT w;

		while (zeroed && left > 0u) {
			const UINT n = left < sizeof(zeros) ? (UINT)left
							    : sizeof(zeros);

			zeroed = f_write(f, zeros, n, &w) == FR_OK && w == n;
			left -= n;
			opxy_usb_breathe();
		}
		ok = f_close(f) == FR_OK && zeroed;
	}
	ok = f_unlink(tr.name) == FR_OK && ok;
	if (!ok)
		s14_log_str(&m->sv.log, S14_ERROR, S14_NO_TAG,
			    "transfer: file not cleared");
	wire_fat_unmount(&m->sv.fat);
	tr.on = false;
	opxy_host_hold_end(m);
}

/* S-11.050: once no transfer is in progress the disk is offered again. */
static void give_back(struct opxy_master *m)
{
	if (tr.offered && s15_up_disk_offerable(&m->sv.menu))
		s10a_usb_disk_offer();
	tr.offered = false;
}

/*
 * S-10c.071, S-10c.088: the disk withdrawn as a scan withdraws it, its
 * volume mounted and the file created. A disk with no volume FatFs reads
 * refuses the begin unwritten (S-12.020, S-12.021).
 */
bool opxy_transfer_start(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;

	if (tr.on) {			/* S-11.050: a begin discards it */
		wipe(m, true);
		give_back(m);
	}
	tr.offered = opxy_usb_up() && s10a_usb_disk()->offered;
	tr.len = tr.chunks = 0;
	if (!opxy_disk_ready(m) ||
	    (tr.offered && !opxy_disk_take(m, S10A_WITHDRAW_SCAN))) {
		give_back(m);
		return false;
	}
	if (!wire_fat_mount(&sv->fat, &sv->disk.dev)) {
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG, "transfer: no volume");
		give_back(m);
		return false;
	}
	if (!new_name() ||
	    f_open(&tr.fil, tr.name, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG, "transfer: no file");
		wire_fat_unmount(&sv->fat);
		give_back(m);
		return false;
	}
	tr.on = true;
	return true;
}

/* S-10c.072, S-10c.089: a short write (the volume full) ends the transfer,
 * which S-10c then discards. */
bool opxy_transfer_append(struct opxy_master *m, const uint8_t *p, size_t n)
{
	UINT w = 0;

	if (!tr.on)
		return false;
	if (n > 0u && (n > UINT32_MAX - tr.len ||
		       f_write(&tr.fil, p, (UINT)n, &w) != FR_OK || w != n))
		return false;
	tr.len += (uint32_t)n;
	tr.chunks++;
	s15_up_transfer_chunk(&m->sv.menu, tr.chunks, tr.len,
			      m->sv.menu.transfer_total != 0u &&
			      tr.len >= m->sv.menu.transfer_total);
	return true;
}

void opxy_transfer_discard(struct opxy_master *m)
{
	if (!tr.on)
		return;
	wipe(m, true);
	give_back(m);
}

/*
 * S-11.050: the bytes as one body-form package, forced over the firmware
 * command (S-10c.075). S-10c offers the disk again after its final reply.
 */
static void submit(struct opxy_master *m, bool fw_command, const char *name,
		   struct s11_outcome *out)
{
	struct opxy_services *sv = &m->sv;
	struct s11_install_result res;
	struct s11_check_in in;
	struct wire_source src;
	uint32_t size = 0;

	out->kind = S11_OUTCOME_REFUSED;
	out->reason = S11_REASON_ZIP;
	out->step = 0u;
	out->result = S11_INTERNAL;
	if (!tr.on)
		return;
	if (f_close(&tr.fil) != FR_OK ||
	    !wire_fat_open_package(&sv->fat, tr.name, &src, &size)) {
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG,
			    "transfer: file unreadable");
		wipe(m, false);
		return;
	}
	in.form = name != NULL ? S11_FORM_FILE : S11_FORM_BODY;
	in.size = size;
	in.option = sv->option;		/* S-11.090: as the checks begin */
	in.file_name = name;
	in.fw_command = fw_command;
	in.dev = opxy_dev(m);
	opxy_install_source(m, &src, &in, &res);
	*out = res.outcome;
	wire_fat_close_file(&sv->fat);
	wipe(m, false);
	tr.offered = false;		/* S-10c's ramdisk_offer() follows */
}

/* S-10a.049: the host hold through the install and the wipe after it. */
void opxy_transfer_submit(struct opxy_master *m, bool fw_command,
			  struct s11_outcome *out)
{
	opxy_host_hold_begin(m);
	submit(m, fw_command, NULL, out);
	opxy_host_hold_end(m);
}

/* A host link's install of a file from the drive, as the scan's of a
 * RAM-disk file: its preamble, and its name for S-11.047. */
void opxy_transfer_submit_file(struct opxy_master *m, const char *name,
			       struct s11_outcome *out)
{
	opxy_host_hold_begin(m);
	submit(m, false, name, out);
	opxy_host_hold_end(m);
}
