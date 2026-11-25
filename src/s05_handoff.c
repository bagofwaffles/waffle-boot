/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-05 OS hand-off: the boot decision, the LDR walk and compatibility check,
 * the kernel's configuration block, the load function and the master's and
 * slave's sequences.
 */
#include "waffle/s05_handoff.h"

#include "waffle/bytes.h"
#include "waffle/s01_memmap.h"

#define REASON_CDC   10u
#define REASON_MSC   30u
#define REASON_MIDI  40u
#define REASON_STAY  60u
#define STATE_UPGRADE 0xAB0Du

/* S-05.001, S-16.008. An install a reset interrupted (state 0xAB0D) is
 * not resumed: held mode, where the state is cleared (S-11.080). */
void s05_boot_decision(uint16_t reason, uint32_t state, enum s05_keys keys,
		       bool slave_present, struct s05_decision *out)
{
	bool stay_reason = reason == REASON_CDC || reason == REASON_MSC ||
			   reason == REASON_MIDI || reason == REASON_STAY;
	bool upgrade = state == STATE_UPGRADE;
	bool held = keys != S05_KEYS_NOT_HELD;	/* a silent PSoC counts as held */

	(void)slave_present;	/* S-09.008 makes the load, not the decision */
	out->boot_os = !stay_reason && !upgrade && !held;
	out->interrupted = upgrade;
}

/* --- LDR --- */

bool s05_ldr_header_ok(const uint8_t h[S05_LDR_HEADER])
{
	uint8_t x = 0;
	unsigned i;

	if (h[3] != S05_LDR_SIGNATURE)
		return false;
	for (i = 0; i < S05_LDR_HEADER; i++)
		x = (uint8_t)(x ^ h[i]);
	return x == 0u;
}

void s05_ldr_decode(const uint8_t h[S05_LDR_HEADER], struct s05_block *b)
{
	b->code = wb_get_le32(h);
	b->flags = (uint16_t)(b->code & 0xFFF0u);
	b->target = wb_get_le32(h + 4);
	b->count = wb_get_le32(h + 8);
	b->argument = wb_get_le32(h + 12);
}

int s05_ldr_next(const uint8_t *img, uint32_t len, uint32_t *off, struct s05_block *b)
{
	uint64_t next;

	if ((uint64_t)*off + S05_LDR_HEADER > len)
		return -1;
	if (!s05_ldr_header_ok(img + *off))
		return -1;
	s05_ldr_decode(img + *off, b);
	b->offset = *off;
	next = (uint64_t)*off + S05_LDR_HEADER;
	if (!(b->flags & S05_FLAG_FILL))
		next += b->count;	/* data and IGNORE blocks carry their payload */
	if (next > len)
		return -1;
	*off = (uint32_t)next;
	return 0;
}

/* Every block with a nonzero count that is not IGNORE; FILL counts. */
bool s05_block_has_destination(const struct s05_block *b)
{
	return b->count != 0u && !(b->flags & S05_FLAG_IGNORE);
}

/* S-05.024, S-01.017.  Every criterion is a fixed range or flag set. */
enum s05_refusal s05_check_image(const uint8_t *img, uint32_t len)
{
	struct s05_block b;
	uint32_t off = 0;
	enum s05_refusal dest = S05_ACCEPT;

	if (len > S01_STAGING_MAX)
		return S05_REFUSE_LENGTH;
	for (;;) {
		if (s05_ldr_next(img, len, &off, &b) != 0)
			return S05_REFUSE_PARSE;
		if (b.flags & S05_FLAGS_REFUSED)
			return S05_REFUSE_FLAGS;
		if (dest == S05_ACCEPT && s05_block_has_destination(&b)) {
			switch (s01_os_destination(b.target, b.count)) {
			case S01_OS_SPACE: dest = S05_REFUSE_SPACE; break;
			case S01_OS_STAGING: dest = S05_REFUSE_STAGING; break;
			case S01_OS_RESERVE: dest = S05_REFUSE_RESERVE; break;
			case S01_OS_RECORD: dest = S05_REFUSE_RECORD; break;
			case S01_OS_BOOT_RESERVE: dest = S05_REFUSE_BOOT_RESERVE; break;
			case S01_OS_OK: break;
			default: dest = S05_REFUSE_SPACE; break;
			}
		}
		if (b.flags & S05_FLAG_FINAL)
			return dest;
	}
}

const char *s05_contract(const struct wb_device *dev)
{
	return dev->contract;
}

/* --- The kernel call --- */

/* S-05.008 */
void s05_config_build(uint8_t cfg[S05_CONFIG_SIZE], uint32_t header_buffer, uint32_t load_fn)
{
	unsigned i;

	for (i = 0; i < S05_CONFIG_SIZE; i++)
		cfg[i] = 0;
	wb_put_le32(cfg + S05_CFG_PHEADER, header_buffer);
	wb_put_le32(cfg + S05_CFG_PLOADFN, load_fn);
}

/* S-05.009: the count is used exactly, never rounded. */
void s05_load_step(uint8_t cfg[S05_CONFIG_SIZE], const uint8_t *staging,
		   s05_copy_fn copy, void *ctx)
{
	uint32_t src = wb_get_le32(cfg + S05_CFG_PSOURCE);
	uint32_t dest = wb_get_le32(cfg + S05_CFG_PDEST);
	uint32_t count = wb_get_le32(cfg + S05_CFG_COUNT);

	if (count == 0u)
		return;
	copy(ctx, dest, staging + src, count);
	wb_put_le32(cfg + S05_CFG_PSOURCE, src + count);
}

/* --- The sequences --- */

/*
 * S-05.006, S-05.007: a failed load leaves the device in the bootloader.  The
 * software version is cleared only when the OS is absent or unusable; a failed
 * select or read says nothing about the image, and a file start's image
 * nothing about the installed OS (S-05.035).
 */
static void failed_load(const struct s05_master_ops *ops, const struct s05_master *m,
			enum s05_fail cause)
{
	bool unusable = !m->file &&
			(cause == S05_FAIL_NO_OS || cause == S05_FAIL_ENCRYPTED ||
			 cause == S05_FAIL_REFUSED);

	if (m->sw_version_nonzero && unusable)	/* S-16.111, S-04.045 M6 */
		ops->clear_sw_version(ops->ctx);
	ops->held(ops->ctx, cause);
}

/* S-05.003-.008, S-05.013, S-05.018, S-05.019, S-05.024, S-05.032, S-05.036 */
void s05_master_handoff(const struct s05_master_ops *ops, const struct s05_master *m)
{
	enum s05_fail f;
	uint32_t length = 0;
	uint8_t reply = 0xFF;

	ops->pre_load(ops->ctx);
	/* Step 1: staged and its CRC checked. */
	f = ops->stage(ops->ctx, &length);
	if (f != S05_FAIL_NONE) {
		failed_load(ops, m, f);		/* no release, no teardown, no kernel call */
		return;
	}
	if (s05_check_image(m->staging, length) != S05_ACCEPT) {
		failed_load(ops, m, S05_FAIL_REFUSED);
		return;
	}
	if (!wb_has(m->dev, WB_F_SECOND_CPU)) {
		/* S-05.036: one CPU, one copy; no release, no ready line */
		if (ops->accepted != NULL)
			ops->accepted(ops->ctx);
		ops->teardown(ops->ctx);
		ops->kernel_call(ops->ctx, m->config_addr);
		ops->jump(ops->ctx, ops->read_svect0(ops->ctx));
		return;
	}
	/* Steps 2 and 3: exactly one release; only a reply of 0 goes on
	 * (S-09.052), and then only once the slave's OS has started (S-05.032):
	 * held mode otherwise, with no further bridge request. */
	if (ops->release(ops->ctx, &reply) != 0)
		reply = 0xFF;			/* no reply */
	if (reply == S05_REPLY_NO_START) {
		ops->held(ops->ctx, S05_FAIL_SLAVE_START);
		return;
	}
	if (reply != 0u) {
		/*
		 * S-05.021: the master's own load succeeded, so S-05.006's
		 * software-version reset does not apply; no metadata write.
		 */
		ops->held(ops->ctx, S05_FAIL_SLAVE);
		return;
	}
	/* Steps 4 and 5, then the entry the ROM left. */
	ops->teardown(ops->ctx);
	ops->kernel_call(ops->ctx, m->config_addr);
	ops->jump(ops->ctx, ops->read_svect0(ops->ctx));
}

enum s05_fail s05_master_ram_start(const struct s05_master_ops *ops, const struct s05_master *m)
{
	enum s05_fail f;
	uint32_t length = 0;
	uint8_t reply = 0xFF;

	ops->pre_load(ops->ctx);
	f = ops->stage(ops->ctx, &length);
	if (f != S05_FAIL_NONE)
		return f;
	if (s05_check_image(m->staging, length) != S05_ACCEPT)
		return S05_FAIL_REFUSED;
	if (wb_has(m->dev, WB_F_SECOND_CPU)) {
		if (ops->release(ops->ctx, &reply) != 0)
			reply = 0xFF;
		if (reply == S05_REPLY_NO_START)
			return S05_FAIL_SLAVE_START;
		if (reply != 0u)
			return S05_FAIL_SLAVE;
	}
	ops->teardown(ops->ctx);
	ops->kernel_call(ops->ctx, m->config_addr);
	ops->jump(ops->ctx, ops->read_svect0(ops->ctx));
	return S05_FAIL_NONE;
}

/* S-05.020, S-05.021, S-05.022: after a reply of 0 the slave waits on nothing
 * from the master. */
bool s05_slave_release(const struct s05_slave_ops *ops, const uint8_t *staging,
		       uint32_t config_addr)
{
	uint32_t length = 0;
	bool ok;

	/* Step 1: stage and check while the server still runs. */
	ok = ops->stage(ops->ctx, &length) == S05_FAIL_NONE &&
	     s05_check_image(staging, length) == S05_ACCEPT;
	/* Step 2: the reply. */
	ops->reply(ops->ctx, (uint8_t)(ok ? 0u : 1u));
	if (!ok)
		return false;		/* keep serving (S-09.052) */
	/* Step 3. */
	ops->stop_server(ops->ctx);
	ops->teardown(ops->ctx);
	ops->kernel_call(ops->ctx, config_addr);
	ops->jump(ops->ctx, ops->read_svect0(ops->ctx));
	return true;
}
