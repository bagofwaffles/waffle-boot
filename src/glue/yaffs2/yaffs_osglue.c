/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The operating-system layer yaffs direct needs (yaffs_osglue.h): no
 * threads, the C library's heap, and a clock the bootloader sets.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wb_yaffs.h"
#include "yaffs_osglue.h"
#include "yaffs_trace.h"

unsigned int yaffs_trace_mask;

unsigned int wb_yaffs_bug_count;
const char *wb_yaffs_bug_file;
int wb_yaffs_bug_line;

static uint32_t (*clock_now)(void);
static int last_error;

void wb_yaffs_set_clock(uint32_t (*now)(void))
{
	clock_now = now;
}

/* The bootloader is single-threaded: yaffs needs no lock. */
void yaffsfs_Lock(void)
{
}

void yaffsfs_Unlock(void)
{
}

u32 yaffsfs_CurrentTime(void)
{
	return clock_now ? clock_now() : 0;
}

void yaffsfs_SetError(int err)
{
	last_error = err;
}

int yaffsfs_GetLastError(void)
{
	return last_error;
}

/*
 * S-13b.054: yaffs2 hands its chunk buffers (the temporary buffers, the
 * cache's and the checkpoint's) to the eMMC's DMA from their start, so
 * every block starts on a 32-byte boundary and covers whole lines. The C
 * library's block is taken larger, and its address kept in the word just
 * before the one returned, outside the returned block's lines.
 */
#define BLOCK_ALIGN 32u

void *yaffsfs_malloc(size_t size)
{
	const size_t lead = sizeof(void *) + BLOCK_ALIGN - 1u;
	size_t whole = (size + BLOCK_ALIGN - 1u) & ~(size_t)(BLOCK_ALIGN - 1u);
	void *raw;
	uintptr_t at;

	if (whole < size || whole > SIZE_MAX - lead)
		return NULL;
	WB_YAFFS_TIME(WB_YAFFS_T_ALLOC, true, 0u);
	raw = malloc(whole + lead);
	if (raw == NULL) {
		WB_YAFFS_TIME(WB_YAFFS_T_ALLOC, false, 0u);
		return NULL;
	}
	at = ((uintptr_t)raw + sizeof(void *) + BLOCK_ALIGN - 1u) &
	     ~(uintptr_t)(BLOCK_ALIGN - 1u);
	memcpy((void *)(at - sizeof(void *)), &raw, sizeof(raw));
	WB_YAFFS_TIME(WB_YAFFS_T_ALLOC, false, at);
	/* cppcheck-suppress memleak ; raw is kept just before the block */
	return (void *)at;
}

/* cppcheck-suppress constParameterPointer ; yaffs2 fixes the type */
void yaffsfs_free(void *ptr)
{
	void *raw;

	if (ptr == NULL)
		return;
	WB_YAFFS_TIME(WB_YAFFS_T_FREE, true, 0u);
	memcpy(&raw, (const uint8_t *)ptr - sizeof(raw), sizeof(raw));
	free(raw);
	WB_YAFFS_TIME(WB_YAFFS_T_FREE, false, (uintptr_t)ptr);
}

void yaffsfs_get_malloc_values(unsigned *current, unsigned *high_water)
{
	*current = 0;
	*high_water = 0;
}

int yaffsfs_CheckMemRegion(const void *addr, size_t size, int write_request)
{
	(void)size;
	(void)write_request;
	return addr ? 0 : -1;
}

void yaffsfs_OSInitialisation(void)
{
}

void yaffs_bug_fn(const char *file_name, int line_no)
{
	wb_yaffs_bug_count++;
	wb_yaffs_bug_file = file_name;
	wb_yaffs_bug_line = line_no;
}
