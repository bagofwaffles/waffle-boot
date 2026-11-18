/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_BOARD_OPXY_BRIDGE_H
#define WAFFLE_BOARD_OPXY_BRIDGE_H

/*
 * Storage across the bridge (S-09.040): on the master every eMMC access
 * is a bridge request (S-05.005), so its S-03 eMMC is an s03_emmc_ops
 * over s09_master_call(); on the slave the eMMC commands run on its own
 * eMMC through the driver's s03_emmc_ops.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "waffle/s03_emmc.h"
#include "waffle/s09_master.h"

/* The master's eMMC: ctx is the struct s09_master. With the slave absent
 * every operation fails at once (S-09.008). */
struct s03_emmc_ops opxy_bridge_emmc(struct s09_master *m);

/* S-03.008 over the bridge (0x05); 0 when the request fails. */
uint32_t opxy_bridge_boot_size(struct s09_master *m);

/*
 * S-09.018, S-09.060: wait for PA6 low at the start of the init request
 * session until deadline, a cycle count T_INIT after the stream's last
 * byte. False when it never came: the slave is absent.
 */
bool opxy_bridge_init_ready(const struct s09_bus_ops *bus, uint64_t deadline);

/*
 * The two eMMC commands of S-09.040 that s03_emmc_ops does not carry, on
 * the same ctx: 0x14's erase with a CMD38 argument (S-16.093) and 0x13's
 * reliable write (the driver's wb_msi_emmc_erase_arg() and
 * wb_msi_emmc_write_reliable()).
 */
struct opxy_emmc_more {
	int (*erase_arg)(void *ctx, uint32_t first_sector, uint32_t arg);
	int (*write_reliable)(void *ctx, uint32_t sector, uint32_t count,
			      const uint8_t *buf);
};

/*
 * The slave's s09_store_ops.emmc: one eMMC command of S-09.040 on emmc
 * and more. Returns 0 on success, nonzero on failure.
 */
int32_t opxy_slave_emmc(const struct s03_emmc_ops *emmc,
			const struct opxy_emmc_more *more, uint8_t cmd,
			uint32_t arg0, uint32_t arg1, uint8_t *data,
			uint32_t n);

/*
 * S-09.075: the link brought to the best rung up to the master's
 * link_ceiling that passes its tests and its gate, once a master has work
 * that is worth it (an install's extraction); nothing with the slave absent.
 * A staging file's open brings it back up to the same ceiling. The log gets
 * a line for each rung the negotiation tried, then where requests run and
 * how they are sent.
 */
struct opxy_master;
void opxy_bridge_fast(struct opxy_master *m);
/*
 * S-09.075: the board's ceiling, the field link_ceiling of struct
 * opxy_master. Single I/O at 50 MHz for now: the quad request rungs fail
 * the link's tests at 64 bytes. The quad rungs stay defined, and the link
 * test and the quad reply test still run them.
 */
#define OPXY_LINK_CEILING S09_RUNG_CHECKED_FAST
/* The link's record that the log has not had yet: a line for each rung of
 * a negotiation since, and one for each fall-back of a request. */
void opxy_bridge_report(struct opxy_master *m);
/*
 * S-13b.083: the end the slave's replies start on, the build's
 * (WB_SPI_REPLY_END_BUILD), as the line the master logs once the slave's
 * server has started. The slave emits no line (S-14.026).
 */
const char *opxy_bridge_end_line(unsigned end);
/*
 * S-13b.083, S-09.079 cause 9: one line, the first time a link status
 * showed that the slave changed to the count end; nothing after.
 */
void opxy_bridge_end_note(struct opxy_master *m);
/*
 * S-09.078: the link lowered to the plain mode with the handshake, and not
 * raised again by a staging file's open, when an install's run or
 * dev-extract ends: held mode, MTP and the hand-over work on the plain
 * link. The log says where requests ran and that the link was lowered.
 * Nothing with the slave absent.
 */
void opxy_bridge_plain(struct opxy_master *m);
/*
 * Part `part` of the link's record for a developer host (OPXY_DEV_LINK_RECORD,
 * waffle/devhost.h), at out; its length.
 */
size_t opxy_bridge_record(const struct s09_master *b, unsigned part, uint8_t *out);

/*
 * S-09.081: before the OS image's copy (on), the quad reply test, the link
 * raised to link_ceiling and the quad data replies taken up when the test
 * passed; after it (off), a line on how its sector data went, then the
 * link lowered as opxy_bridge_plain() lowers it. Nothing with the slave
 * absent.
 */
void opxy_bridge_copy_link(struct opxy_master *m, bool on);
/* S-09.081: the quad data replies dropped for a read again of the copy in
 * single I/O, the link left on its rung. */
void opxy_bridge_copy_single(struct opxy_master *m);
/*
 * S-05.005, S-09.081: the OS image's copy (s03_load_os() on m->emmc); one
 * whose CRC fails after any of its sector data came in quad I/O is read
 * once more with the quad data dropped, every reply in single I/O. True
 * when it was read again; out is the last load's.
 */
bool opxy_bridge_load_os(struct opxy_master *m, uint8_t *staging, uint32_t cap,
			 struct s03_load *out);
/* S-09.081: why the quad data replies were not taken up or were dropped
 * (S09_QW_...), in words. */
const char *opxy_bridge_qdata_why(unsigned why);
/* OPXY_DEV_QUAD_DATA's answer after its status byte (waffle/devhost.h). */
size_t opxy_bridge_qdata_report(const struct s09_master *b, uint8_t *out);

#endif /* WAFFLE_BOARD_OPXY_BRIDGE_H */
