/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The hard reset (src/board/boot/reset.c, S-13a.061): before the write that
 * enables the watchdog, every RCU0_MSG bit cleared, 1s to RCU0_STAT's
 * reset-cause bits, RCU0_BCODE.NOMEMINIT, the fault route; after it, no
 * register write.
 */
#include <setjmp.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/boot.h"
#include "waffle/drivers/bf70x/rcu.h"
#include "waffle/drivers/bf70x/sec.h"
#include "waffle/drivers/bf70x/timer.h"

static jmp_buf idled;

/* The reset's last loop: on a unit the watchdog ends it. */
static void leave(void *ctx)
{
	(void)ctx;
	longjmp(idled, 1);
}

/* The index + 1 of the first write of value to addr (any value when
 * any), or 0. */
static size_t first_write(wb_mmr_addr addr, uint32_t value, bool any)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == addr &&
		    (any || log[i].value == value))
			return i + 1u;
	return 0;
}

static void run_reset(void)
{
	fake_core_set_idle_hook(leave, NULL);
	if (setjmp(idled) == 0)
		opxy_hard_reset();
}

static void test_hard_reset(void)
{
	const struct fake_mmr_access *log;
	size_t i, arm = 0, msg, stat;

	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set(RCU0_MSG, RCU_MSG_C0L1INIT | 0x5u);
	fake_mmr_set(RCU0_STAT, 0x10Bu);
	fake_mmr_set(RCU0_BCODE, RCU_BCODE_NOKERNEL | RCU_BCODE_HALT);
	run_reset();
	log = fake_mmr_log();
	/* The arming write: WDOG0_CTL with WDEN other than 0xAD. */
	for (i = 0; i < fake_mmr_count() && !arm; i++)
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == WDOG0_CTL &&
		    (log[i].value & (0xFFu << 4)) != WDOG_CTL_DISABLE)
			arm = i + 1u;
	msg = first_write(RCU0_MSG_CLR, 0xFFFFFFFFu, false);
	stat = first_write(RCU0_STAT, 0x0000000Fu, false);
	CHECK(arm != 0u);
	CHECK(msg != 0u && msg < arm);
	CHECK(stat != 0u && stat < arm);
	/* Nothing but the arming write's sync after it. */
	for (i = arm; i < fake_mmr_count(); i++)
		CHECK(log[i].op != FAKE_MMR_WRITE);
	/* The other steps, before it. */
	CHECK(fake_mmr_get(RCU0_BCODE) == RCU_BCODE_NOMEMINIT);
	CHECK(first_write(RCU0_BCODE, 0, true) < arm);
	CHECK(fake_mmr_get(RCU0_CTL) & RCU_CTL_SRSTREQEN);
	CHECK(fake_mmr_get(SEC0_SCTL(WB_SID_WDOG0)) & SEC_SCTL_FEN);
	CHECK(fake_mmr_get(SEC0_FCTL) & SEC_FCTL_SREN);
	CHECK(fake_mmr_get(WDOG0_CNT) == OPXY_WDOG_COUNT);
	CHECK(fake_core_idles() == 1u);
}

int main(void)
{
	RUN(test_hard_reset);
	return check_summary("test_board_reset");
}
