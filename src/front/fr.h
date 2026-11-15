/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The frozen front's shared pieces: the hardware it reaches through small
 * ops (so the host's tests can stand in for each), its byte helpers, CRC-32
 * and SHA-256. Everything here is the front's own (fr_handoff.h).
 */
#ifndef FR_H
#define FR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The time base: microseconds since the counter started, and a wait. */
struct fr_clock {
	uint64_t (*now_us)(void *ctx);
	void (*delay_us)(void *ctx, uint32_t us);
	void *ctx;
};

/* A byte link (the PSoC's UART, the recovery serial port). read takes
 * what has arrived, write what fits and says how much it took. */
struct fr_bytes {
	size_t (*read)(void *ctx, uint8_t *buf, size_t max);
	size_t (*write)(void *ctx, const uint8_t *buf, size_t n);
	bool (*idle)(void *ctx);	/* every byte written is on the wire */
	void *ctx;
};

/* The SPI flash: 0 on success. program stays inside one 256-byte page. */
struct fr_flash {
	int (*read)(void *ctx, uint32_t off, uint8_t *buf, uint32_t len);
	int (*erase)(void *ctx, uint32_t sector_off);
	int (*program)(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len);
	void *ctx;
};

/* The memory the stream was loaded into: [addr, addr + len) where it may be
 * read, else NULL. */
struct fr_mem {
	const uint8_t *(*map)(void *ctx, uint32_t addr, uint32_t len);
	void *ctx;
};

/* The panel bus (S-13c.001), write only. */
struct fr_panel_bus {
	void (*reset)(void *ctx, bool high);
	void (*select)(void *ctx, bool on);
	void (*cmd)(void *ctx, uint8_t c);
	void (*data)(void *ctx, const uint8_t *p, size_t n);
	void *ctx;
};

/* Little-endian fields. */
static inline uint32_t fr_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static inline uint16_t fr_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static inline void fr_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void fr_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

void fr_copy(void *dst, const void *src, size_t n);
void fr_fill(void *dst, uint8_t v, size_t n);
bool fr_same(const void *a, const void *b, size_t n);

/* The PSoC's link, served from every loop that may run long (a flash
 * erase, program or read, a CRC or SHA-256 over many bytes, a frame to the
 * panel), at least every FR_SERVE_BYTES bytes: the board points
 * fr_serve_hook at the link's reading (fr_psoc_take()), so that no byte is
 * lost to the controller's FIFO meanwhile. NULL: nothing is done. */
#define FR_SERVE_BYTES 1024u
extern void (*fr_serve_hook)(void);
void fr_serve(void);

/* CRC-32 (zlib, ISO-HDLC; S-02.015): fr_crc32(0, p, n) is the CRC of p;
 * pass a previous result to go on. Serves the link (fr_serve()). */
uint32_t fr_crc32(uint32_t crc, const uint8_t *p, size_t n);

/* SHA-256. */
#define FR_SHA256_LEN 32u
struct fr_sha256 {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	unsigned fill;
};
void fr_sha256_init(struct fr_sha256 *s);
/* Serves the link (fr_serve()). */
void fr_sha256_add(struct fr_sha256 *s, const uint8_t *p, size_t n);
void fr_sha256_end(struct fr_sha256 *s, uint8_t out[FR_SHA256_LEN]);

/* Place a function in L1 code on the target (front.ld's .fr_fast). */
#if defined(__bfin__)
#define FR_FAST __attribute__((section(".fr_fast")))
#else
#define FR_FAST
#endif

#endif /* FR_H */
