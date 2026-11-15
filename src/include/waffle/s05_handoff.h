/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-05: the OS hand-off.  The boot decision, the LDR block walk and the
 * compatibility check of a staged image, the kernel's configuration block,
 * the load function, and the master's and slave's hand-off sequences, built
 * and checked as data.  The ROM kernel call, the jump, the bridge and the
 * teardown are operations passed in.
 */
#ifndef WAFFLE_S05_HANDOFF_H
#define WAFFLE_S05_HANDOFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- The boot decision (S-05.001) --- */

enum s05_keys { S05_KEYS_NOT_HELD = 0, S05_KEYS_HELD, S05_KEYS_SILENT };

struct s05_decision {
	bool boot_os;	/* attempt the OS boot, once */
	bool interrupted;	/* state 0xAB0D: a reset fell during an install
				 * (S-11.080) */
};

/*
 * reason: the reason the boot took (S-04.004); state: the metadata state word
 * after the restore of S-02.065; keys: the PSoC's key report (S-16.008);
 * slave_present: false when the slave is absent (S-09.008), which leaves
 * boot_os as it is (S-09.008 makes the load). Nothing is written here.
 */
void s05_boot_decision(uint16_t reason, uint32_t state, enum s05_keys keys,
		       bool slave_present, struct s05_decision *out);

/* --- LDR blocks --- */

#define S05_LDR_HEADER     16u
#define S05_LDR_SIGNATURE  0xADu
#define S05_FLAG_FINAL     0x8000u
#define S05_FLAG_FIRST     0x4000u
#define S05_FLAG_INDIRECT  0x2000u
#define S05_FLAG_IGNORE    0x1000u
#define S05_FLAG_INIT      0x0800u
#define S05_FLAG_CALLBACK  0x0400u
#define S05_FLAG_QUICKBOOT 0x0200u
#define S05_FLAG_FILL      0x0100u
#define S05_FLAG_AUX       0x0020u
#define S05_FLAG_SAVE      0x0010u
/* S-05.023: the contract this implements. */
#define S05_CONTRACT_VERSION "XY-HANDOFF-1"

/* S-05.024: the flags that make an image refused. */
#define S05_FLAGS_REFUSED  (S05_FLAG_INIT | S05_FLAG_CALLBACK | S05_FLAG_INDIRECT | \
			    S05_FLAG_QUICKBOOT | S05_FLAG_AUX | S05_FLAG_SAVE)

struct s05_block {
	uint32_t offset;	/* of the header in the stream */
	uint32_t code;
	uint16_t flags;		/* code bits 15-4 */
	uint32_t target;
	uint32_t count;
	uint32_t argument;
};

/* Signature 0xAD and a zero XOR over the 16 header bytes. */
bool s05_ldr_header_ok(const uint8_t h[S05_LDR_HEADER]);
void s05_ldr_decode(const uint8_t h[S05_LDR_HEADER], struct s05_block *b);

/*
 * Walk the stream: the block at *off, then *off moves past it (a FILL block
 * has no payload).  Returns 0, or -1 on a bad signature or checksum or a
 * header or payload that does not fit in len.
 */
int s05_ldr_next(const uint8_t *img, uint32_t len, uint32_t *off, struct s05_block *b);

/* The block's destination [target, target + count), empty for IGNORE or count 0. */
bool s05_block_has_destination(const struct s05_block *b);

enum s05_refusal {
	S05_ACCEPT = 0,
	S05_REFUSE_LENGTH,	/* longer than the staging area */
	S05_REFUSE_PARSE,	/* signature, checksum, or no FINAL within the length */
	S05_REFUSE_FLAGS,	/* INIT, CALLBACK, INDIRECT, QUICKBOOT, AUX or SAVE */
	S05_REFUSE_SPACE,	/* a destination outside L1, L2 and DDR */
	S05_REFUSE_STAGING,
	S05_REFUSE_RESERVE,
	S05_REFUSE_RECORD,
	S05_REFUSE_BOOT_RESERVE	/* the ROM's boot reserve (S-01.008) */
};

/* S-05.024, S-01.017: the check of a staged image before the release or the kernel call. */
enum s05_refusal s05_check_image(const uint8_t *img, uint32_t len);

/* --- The kernel call (S-05.008, S-05.009) --- */

#define S05_CONFIG_SIZE     600u
#define S05_CFG_PSOURCE     0x00u
#define S05_CFG_PDEST       0x04u
#define S05_CFG_COUNT       0x08u
#define S05_CFG_PHEADER     0x7Cu
#define S05_CFG_PLOADFN     0xC0u

/* S-05.008: zero except the header buffer and load function addresses. */
void s05_config_build(uint8_t cfg[S05_CONFIG_SIZE], uint32_t header_buffer, uint32_t load_fn);

/*
 * Copy count bytes from src to the target address dest.  The target's copy
 * honors S-05.010 (L1 instruction memory) and S-05.011 (caches).
 */
typedef void (*s05_copy_fn)(void *ctx, uint32_t dest, const uint8_t *src, uint32_t count);

/* S-05.009: one call of the load function on the configuration block. */
void s05_load_step(uint8_t cfg[S05_CONFIG_SIZE], const uint8_t *staging,
		   s05_copy_fn copy, void *ctx);

/* --- The hand-off sequences (S-05.005 to S-05.007, S-05.018 to S-05.021) --- */

enum s05_fail {
	S05_FAIL_NONE = 0,
	S05_FAIL_NO_OS,		/* no active partition, a header or CRC failure (S-05.006) */
	S05_FAIL_ENCRYPTED,	/* the image marked encrypted (S-03.048): the reinstall line */
	S05_FAIL_SELECT,	/* a failed partition select or read (S-05.007) */
	S05_FAIL_REFUSED,	/* S-05.024 */
	S05_FAIL_SLAVE,		/* the release reply was not 0 (S-05.018, S-05.021) */
	S05_FAIL_SLAVE_START	/* reply 0, but the slave's OS did not start (S-05.032) */
};

struct s05_master_ops {
	void *ctx;
	/* S-05.003, S-05.004: the PMIC, display and PSoC steps before the load. */
	void (*pre_load)(void *ctx);
	/*
	 * S-05.005: stage the active partition's image (S-03) at the staging
	 * area, CRC checked, and select the user area again.  Returns
	 * S05_FAIL_NONE with *length set, or why it failed.
	 */
	enum s05_fail (*stage)(void *ctx, uint32_t *length);
	/*
	 * S-05.019: send the one release; 0 when a reply came, with *reply set.
	 * After a reply of 0 it also runs S-05.018 step 3, the wait for the
	 * slave's OS (S-05.032), and sets *reply to S05_REPLY_NO_START when
	 * that wait ran out.
	 */
	int (*release)(void *ctx, uint8_t *reply);
	/* S-05.016 */
	void (*teardown)(void *ctx);
	/* S-05.008: the call of 0x04000008 with the configuration block's address. */
	void (*kernel_call)(void *ctx, uint32_t config_addr);
	/* S-05.013 */
	uint32_t (*read_svect0)(void *ctx);
	void (*jump)(void *ctx, uint32_t entry);
	/* S-05.006, S-04.045 M6: the metadata write of sw_version 0.0.0.0. */
	void (*clear_sw_version)(void *ctx);
	/* Held mode with the cause (S-15.161 shows the encrypted one only). */
	void (*held)(void *ctx, enum s05_fail cause);
};

#define S05_REPLY_NO_START 0xFEu

struct s05_master {
	const uint8_t *staging;	/* where stage() puts the image (0x8A000000 on the target) */
	uint32_t config_addr;	/* the configuration block, in the handover reserve */
	bool sw_version_nonzero;	/* S-04.045 M6's condition on the record read */
};

/* S-05.018 on the master.  Returns only into held mode, or with test ops. */
void s05_master_handoff(const struct s05_master_ops *ops, const struct s05_master *m);

struct s05_slave_ops {
	void *ctx;
	enum s05_fail (*stage)(void *ctx, uint32_t *length);	/* own eMMC, no bridge */
	void (*reply)(void *ctx, uint8_t value);		/* the release reply */
	void (*stop_server)(void *ctx);
	void (*teardown)(void *ctx);
	void (*kernel_call)(void *ctx, uint32_t config_addr);
	uint32_t (*read_svect0)(void *ctx);
	void (*jump)(void *ctx, uint32_t entry);
};

/*
 * S-05.020, S-05.021: the slave's answer to the release.  Returns false when
 * the load failed and the slave keeps serving bridge requests.
 */
bool s05_slave_release(const struct s05_slave_ops *ops, const uint8_t *staging,
		       uint32_t config_addr);

#endif /* WAFFLE_S05_HANDOFF_H */
