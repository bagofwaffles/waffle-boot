/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-11.045-S-11.049 on the master: the RAM disk's scan for a package, which
 * S-15 starts (S-15.105). The disk is taken from the host for the scan
 * (S-10a.049); the package is read from the FAT volume, its zip recovered
 * in place in the package's own clusters (S-11.018: no package is too
 * large, and the largest is bigger than the bootloader's own memory,
 * S-01.016), and staged over S-09 into the slave's /yaffs2/upgrade/
 * (bridge/files.h).
 */
#include "waffle/board/opxy_services.h"

#include "waffle/board/opxy_boot.h"
#include "waffle/board/opxy_ops.h"
#include "waffle/install/content.h"
#include "waffle/install/zip.h"

static struct wire_package pkg;
static struct wire_zip zip;
static struct s09_files_zip staged;

/* S-04.024: true when the root asks for the CDC port. */
static bool cdc_request(struct wire_fat *fat)
{
	struct s04_cdc_scan cdc;
	char name[S11_PATH_MAX];
	bool dir;
	size_t i;

	s04_cdc_scan_init(&cdc);
	for (i = 0; wire_fat_root(fat, i, name, sizeof(name), &dir); i++)
		s04_cdc_scan_entry(&cdc, name, !dir);
	return s04_cdc_scan_result(&cdc);
}

void opxy_install_source(struct opxy_master *m, const struct wire_source *src,
			 const struct s11_check_in *in,
			 struct s11_install_result *res)
{
	struct s11_package_ops pops;
	struct s11_zip_ops reader, zops;
	const bool was = m->sv.working;

	/* S-11.018: the zip is decrypted in place, wherever src keeps the
	 * package (wire_fat_open_package) */
	wire_package_init(&pkg, &pops, src, &opxy_unit_ops, NULL, NULL, 0u);
	wire_zip_init(&zip, &reader, &pkg, NULL, 0, NULL);
	reader.crc32 = opxy_crc32;	/* the members' CRC-32s, S-11.030 */
	s09_files_zip_ops(&staged, &m->sv.files, &reader, &zops);
	m->sv.working = true;		/* a host-link transfer's install too */
	opxy_install_package(m, &pops, &zops, in, res);
	m->sv.working = was;
	wire_zip_close(&zip);
}

static void scan(struct opxy_master *m)
{
	struct opxy_services *sv = &m->sv;
	struct wire_source src;
	struct s11_check_in in;
	struct s11_install_result res;
	char name[S11_PATH_MAX];
	uint32_t size = 0;
	const bool offered = opxy_usb_up() && s10a_usb_disk()->offered;
	bool taken = false;

	if (!opxy_disk_ready(m) ||
	    (offered && !opxy_disk_take(m, S10A_WITHDRAW_SCAN)) ||
	    !wire_fat_mount(&sv->fat, &sv->disk.dev)) {
		s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG, "scan: no volume");
		s15_up_not_found(&sv->menu);
	} else if (cdc_request(&sv->fat)) {
		wire_fat_unmount(&sv->fat);
		opxy_reboot(m, S04_W4, 0);	/* S-04.024: reason 10, hard */
		return;
	} else if (!s11_disk_pick(wire_fat_root, &sv->fat, name) ||
		   !wire_fat_open_package(&sv->fat, name, &src, &size)) {
		s15_up_not_found(&sv->menu);	/* S-11.046 */
		wire_fat_unmount(&sv->fat);
	} else {
		taken = true;
		in.form = S11_FORM_FILE;
		in.size = size;
		in.option = sv->option;		/* S-11.090: as the checks begin */
		in.file_name = name;		/* S-11.047 */
		in.fw_command = false;
		in.dev = opxy_dev(m);
		opxy_install_source(m, &src, &in, &res);
		/* S-10c.106: a disk package's last report, on CDC */
		s10c_host_outcome(&sv->host, &res.outcome);
		wire_fat_close_file(&sv->fat);
		wire_fat_unmount(&sv->fat);
	}
	/* S-11.049: a package taken leaves a freshly formatted disk */
	if (taken) {
		if (wire_fat_format(&sv->fat, &sv->disk.dev, opxy_dev(m)))
			wire_fat_unmount(&sv->fat);
		else
			s14_log_str(&sv->log, S14_ERROR, S14_NO_TAG,
				    "ramdisk format failed");
	}
	/* S-11.049, S-15.115: offered again unless the zip was read */
	if (offered && s15_up_disk_offerable(&sv->menu))
		s10a_usb_disk_offer();
}

/* S-10a.049: the host hold from the take to the offer, every way. */
void opxy_scan(struct opxy_master *m)
{
	opxy_host_hold_begin(m);
	scan(m);
	opxy_host_hold_end(m);
}
