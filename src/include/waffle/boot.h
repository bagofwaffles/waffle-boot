/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_BOOT_H
#define WAFFLE_BOOT_H

/*
 * The bootloader's boot flow: the order of the master's and the slave's
 * steps (S-01.001, S-01.002, S-05.001, S-05.018, S-05.020, S-07.023,
 * S-08.001, S-09.001, S-09.015, S-13a.021). Each step is an op, so the
 * order is tested on the host; the board (src/board/) fills the ops from
 * the S-xx logic and the drivers.
 */

#include <stdbool.h>
#include <stdint.h>

#include "waffle/device.h"
#include "waffle/s03_emmc.h"
#include "waffle/s05_handoff.h"
#include "waffle/s09_master.h"
#include "waffle/s15_lines.h"
#include "waffle/testos.h"

/* S-05.025: the boot prompt is a property of the build (BOOT_PROMPT=1,
 * waffle/board/opxy_boot.h); its op exists only then. */
#ifndef OPXY_BOOT_PROMPT
#define OPXY_BOOT_PROMPT 0
#endif

/* S-04.004: the reason of a boot with no reboot record. */
#define BOOT_REASON_NONE 20u

/*
 * What a failed start knew beyond its cause (S-05.006, S-05.007, S-05.021,
 * S-05.032), kept for the boot: S-15.163's notice, the log and a developer
 * host read it. All zero when nothing more is known.
 */
struct boot_start {
	struct s03_load load;		/* the master's stage (S-05.005) */
	uint32_t boot_size;		/* B, as the stage took it (S-16.136) */
	enum s05_refusal refusal;	/* S05_FAIL_REFUSED: S-05.024's rule */
	bool replied;			/* S05_FAIL_SLAVE: the release got a reply */
	uint8_t reply;			/* and its value */
	bool lost;			/* the bridge lost the slave (S-09.008) */
	struct s09_lost why;		/* how, when lost */
	/* S-05.035: a file start's, which read /yaffs2/os.ldr and no boot
	 * partition (load all zero but its length): what the read found, and
	 * the CRC-32 of the master's copy */
	bool file;
	struct s15_os_file os_file;
	uint32_t file_crc;
};

/* Why held mode was entered, for its entry steps and S-15's screens. */
struct boot_held {
	uint16_t reason;	/* the reboot reason (S-04) */
	bool load_attempted;	/* S-05.006: a load ran and failed */
	enum s05_fail cause;	/* its cause, S05_FAIL_NONE without a load */
	struct boot_start start;	/* the failed load's account */
	bool slave_absent;	/* S-09.008 */
	bool interrupted;	/* S-11.080: the state was 0xAB0D */
};

struct boot_master_ops {
	void *ctx;
	/*
	 * S-13b: the pins, SEC and the master's buses. The caller has kept
	 * DMA9_XMOD/YMOD (S-01.006) and read the strap (S-13b.001) already,
	 * since the strap gives the role.
	 */
	void (*bringup)(void *ctx);
	/* S-13a.021: false when 0x84000000 aliases 0x80000000. */
	bool (*ddr_check)(void *ctx);
	/* S-13a.021, S-16.023: the log line, the fatal record, the halt.
	 * Returns only with test ops. */
	void (*ddr_fatal)(void *ctx);
	/*
	 * S-02.065 then S-04: the metadata record, restored from its mirror,
	 * gives the state word and whether sw_version is not 0.0.0.0.
	 */
	void (*metadata)(void *ctx, uint32_t *state, bool *sw_version_nonzero);
	/* S-04.004: the reboot record's reason, consumed; 20 when missing. */
	uint16_t (*reboot_reason)(void *ctx);
	/* S-07.023 steps 1-6: the key report, S05_KEYS_SILENT unanswered.
	 * Before the slave's stream (S-09.001). */
	enum s05_keys (*psoc_start)(void *ctx);
	/*
	 * S-09.001-S-09.008 (the running slot's stream) then S-09.015 (the
	 * bridge start). False: the slave is absent (S-09.008).
	 */
	bool (*slave_start)(void *ctx);
	/*
	 * S-05.018: the hand-off. Its held op enters held mode with the
	 * failed load's cause (boot_master_load_failed()).
	 */
	const struct s05_master_ops *handoff;
	const uint8_t *staging;		/* S-01.014 */
	uint32_t config_addr;		/* S-05.008, in the handover reserve */
	/*
	 * Held mode (S-01.016): its entry steps (boot_held_enter()) and then
	 * the service loop. Does not return on the target.
	 */
	void (*held)(void *ctx, const struct boot_held *h);
#if OPXY_BOOT_PROMPT
	/*
	 * S-05.025: the boot prompt of a build with it on, shown once the
	 * decision is to boot and the slave is present, before the
	 * hand-off's steps; waits without a
	 * timeout for digit 1 (BOOT_PROMPT_START: the hand-off goes on
	 * unchanged) or digit 2 (BOOT_PROMPT_HELD: held mode as a boot
	 * that did not attempt the OS). NULL: the build has it off, and
	 * the boot behaves as if S-05.025 did not exist.
	 */
	int (*boot_prompt)(void *ctx);
#endif
	/*
	 * S-15.175, S-05.001: the running slot's "start in boot menu" setting, asked
	 * once the decision is to boot and the slave is present, before the
	 * boot prompt; true is held mode as a boot that attempted no load.
	 * NULL: never set.
	 */
	bool (*start_in_menu)(void *ctx);
	/* The device (S-18): with second-cpu slave_start, without it
	 * storage_start and S-05.036's order. */
	const struct wb_device *dev;
	/*
	 * Without second-cpu, in slave_start's place (S-01.001 without step
	 * 5, S-13b.060): the main CPU brings up its own eMMC and volume. No
	 * slave can be absent there (S-05's Devices), so its failure is the
	 * load's (S-05.006). NULL: nothing to start.
	 */
	void (*storage_start)(void *ctx);
};

#define BOOT_PROMPT_START 1
#define BOOT_PROMPT_HELD 2

/*
 * The master's boot. Returns only into the caller's tests: on the target
 * it ends in the OS, in held mode or in a halt.
 */
void boot_master(const struct boot_master_ops *ops);

/* The held op of a board's s05_master_ops: the boot's state, then held.
 * start is the failed load's account, NULL when there is none. */
void boot_master_load_failed(const struct boot_master_ops *ops,
			     enum s05_fail cause, const struct boot_start *start);

/* The held-mode entry steps, in order (S-08.001, S-07.023 step 8). */
struct boot_held_ops {
	void *ctx;
	/* S-08.001, S-08.002: the Nordic link greeting, first on UART1. */
	void (*nordic_greeting)(void *ctx);
	/* S-17.021: the PSoC info request 0x09. */
	void (*psoc_info)(void *ctx);
	/* S-04.061: 0x51 {3C FF 02}. */
	void (*psoc_held_state)(void *ctx);
	/* S-07.043 and the rest of held mode: the service loop. Does not
	 * return on the target. */
	void (*service)(void *ctx, const struct boot_held *h);
};

void boot_held_enter(const struct boot_held_ops *ops, const struct boot_held *h);

/* The slave's boot (S-01.002, S-05.020-S-05.022, S-09). */
struct boot_slave_ops {
	void *ctx;
	/* S-13b: the slave's pins, the time base, SEC and its bridge end. */
	void (*bringup)(void *ctx);
	bool (*ddr_check)(void *ctx);
	void (*ddr_fatal)(void *ctx);
	/*
	 * S-09.011: serve bridge sessions until the release's reply has been
	 * clocked out completely. Returns the reply's value: 0 (staged and
	 * accepted), or 1 (no OS, S-05.021) after which serving goes on.
	 * The board stages inside S-09's os_status (S-05.020 step 1).
	 */
	uint8_t (*serve_until_release)(void *ctx);
	/* S-05.020 step 3: built on the staged result (boot_slave()). */
	const struct s05_slave_ops *handoff;
	const uint8_t *staging;
	uint32_t config_addr;
};

/* The slave's boot: returns only into the caller's tests. */
void boot_slave(const struct boot_slave_ops *ops);

#endif /* WAFFLE_BOOT_H */
