/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The stub's unpack (unpack.c) on the host, over a build's stream: the
 * cross build's check that the stub's own code gives the image back
 * (tools/mkldr.py --unpacked compares).
 *
 *     unpack-host STREAM.ldr OUT.bin
 *
 * Finds the one data block the stream loads at WB_PACK_START, unpacks it
 * with the stub's own map of where a run may go, and writes the entry
 * (u32) and each run as {u32 address, u32 length, bytes}. Exit status 1 on
 * any failure, with the refusal's code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unpack.h"

#define RUNS_MAX WB_PACK_RUNS

struct host_map {
	uint32_t addr[RUNS_MAX], len[RUNS_MAX];
	uint8_t *buf[RUNS_MAX];
	unsigned n;
};

static uint8_t *host_dest(void *ctx, uint32_t addr, uint32_t len)
{
	struct host_map *m = ctx;

	if (!wb_unpack_writable(addr, len) || m->n == RUNS_MAX)
		return NULL;
	m->buf[m->n] = malloc(len);
	if (m->buf[m->n] == NULL)
		return NULL;
	m->addr[m->n] = addr;
	m->len[m->n] = len;
	return m->buf[m->n++];
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(FILE *f, uint32_t v)
{
	const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };

	(void)fwrite(b, 1, 4, f);
}

int main(int argc, char **argv)
{
	static uint64_t arena[WB_UNPACK_ARENA / 8u];
	static uint8_t img[0x80000];
	struct host_map map = { { 0 }, { 0 }, { NULL }, 0 };
	const struct wb_unpack_ops ops = { host_dest, &map, arena, sizeof arena };
	const uint8_t *pk = NULL;
	uint32_t pk_len = 0, off = 0, entry = 0;
	enum wb_unpack_rc rc;
	size_t n;
	unsigned found = 0, i;
	FILE *f;

	if (argc != 3) {
		fprintf(stderr, "usage: unpack-host STREAM.ldr OUT.bin\n");
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (f == NULL) {
		perror(argv[1]);
		return 1;
	}
	n = fread(img, 1, sizeof img, f);
	fclose(f);
	/* the blocks: {code, target, count, argument}; FILL carries no payload */
	while (off + 16u <= n) {
		const uint32_t code = get32(img + off), target = get32(img + off + 4u);
		const uint32_t count = get32(img + off + 8u);

		off += 16u;
		if ((code >> 24) != 0xADu)
			break;
		if (!(code & 0x0100u)) {
			if (count > n - off)
				break;
			if (!(code & 0x1000u) && target == WB_PACK_START && count != 0u) {
				pk = img + off;
				pk_len = count;
				found++;
			}
			off += count;
		}
		if (code & 0x8000u)
			break;
	}
	if (found != 1u) {
		fprintf(stderr, "unpack-host: %u packed blocks in %s\n", found, argv[1]);
		return 1;
	}
	rc = wb_unpack(pk, pk_len, &ops, &entry);
	if (rc != WB_UNPACK_OK) {
		fprintf(stderr, "unpack-host: refused, code %d\n", (int)rc);
		return 1;
	}
	f = fopen(argv[2], "wb");
	if (f == NULL) {
		perror(argv[2]);
		return 1;
	}
	put32(f, entry);
	for (i = 0; i < map.n; i++) {
		put32(f, map.addr[i]);
		put32(f, map.len[i]);
		(void)fwrite(map.buf[i], 1, map.len[i], f);
		free(map.buf[i]);
	}
	if (fclose(f) != 0) {
		perror(argv[2]);
		return 1;
	}
	printf("unpack-host: %u runs, entry 0x%08x\n", map.n, (unsigned)entry);
	return 0;
}
