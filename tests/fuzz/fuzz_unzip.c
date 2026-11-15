/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * minizip on an untrusted zip archive in memory (the upgrade zip,
 * S-11.030): every member's information and up to 1 MiB of its data.
 */
#include <stddef.h>
#include <stdint.h>

#include "unzip_mem.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_MEMBERS 256
#define MAX_READ (1 << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static uint8_t buf[16384];
	struct wb_unzip_mem mem;
	unz_global_info64 global;
	unz_file_info64 info;
	char name[512];
	unzFile uf;
	int ret, members = 0;

	uf = wb_unzip_open_mem(&mem, data, size);
	if (!uf)
		return 0;
	unzGetGlobalInfo64(uf, &global);
	for (ret = unzGoToFirstFile(uf); ret == UNZ_OK && members < MAX_MEMBERS;
	     ret = unzGoToNextFile(uf), members++) {
		int n, total = 0;

		if (unzGetCurrentFileInfo64(uf, &info, name, sizeof(name), NULL,
					    0, NULL, 0) != UNZ_OK)
			break;
		if (unzOpenCurrentFile(uf) != UNZ_OK)
			continue;
		while (total < MAX_READ &&
		       (n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0)
			total += n;
		unzCloseCurrentFile(uf);
	}
	unzClose(uf);
	return 0;
}
