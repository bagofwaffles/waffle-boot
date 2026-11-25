/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tests for the CEC and SEC driver (src/drivers/bf70x/sec.c). */
#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/drivers/bf70x/coremmr.h"
#include "waffle/drivers/bf70x/sec.h"

/* The driver's "configured" state lasts for the process, as on hardware. */
static void reset_fakes(void)
{
	fake_mmr_reset();
	fake_core_reset();
}

static unsigned calls;
static uint32_t last_sid;
static void *last_ctx;

static void handler(void *ctx, uint32_t sid)
{
	calls++;
	last_sid = sid;
	last_ctx = ctx;
	/* The source is acknowledged before the handler runs. */
	CHECK(fake_mmr_get(SEC0_CSID0) == sid);
}

static bool any_reset_write(void)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++) {
		if (log[i].op != FAKE_MMR_WRITE)
			continue;
		if ((log[i].addr == SEC0_GCTL || log[i].addr == SEC0_FCTL ||
		     log[i].addr == SEC0_CCTL0) &&
		    (log[i].value & SEC_CTL_RESET) != 0)
			return true;
	}
	return false;
}

static void test_cec(void)
{
	reset_fakes();
	fake_core_set_imask(0x801Fu);
	wb_cec_enable(WB_IVG_TMR);
	CHECK(fake_core_imask() == 0x805Fu);
	wb_cec_disable(15);
	CHECK(fake_core_imask() == 0x005Fu);
	wb_cec_set_vector(6, 0x11A00100u);
	CHECK(fake_mmr_get(ICU_EVT(6)) == 0x11A00100u);
	CHECK(fake_mmr_get(0x1FC02018u) == 0x11A00100u);	/* S-01.012 */
	CHECK(fake_mmr_get(0x1FE02018u) == 0);	/* not the PRM's misprint */
}

static void test_init(void)
{
	const uint32_t entry = (uint32_t)(uintptr_t)wb_sec_entry;
	static struct fake_mmr_access want[] = {
		FAKE_W32(SEC0_GCTL, SEC_CTL_RESET),
		FAKE_SYNC,
		FAKE_W32(SEC0_GCTL, SEC_CTL_EN),
		FAKE_W32(SEC0_CCTL0, SEC_CTL_EN),
		FAKE_SYNC,
		FAKE_W32(0x1FC0202Cu, 0),	/* EVT11, patched below */
		FAKE_CSYNC,
	};

	want[5].value = entry;
	reset_fakes();
	fake_core_set_imask(0x801Fu);
	CHECK(wb_sec_init() == WB_SEC_OK);
	CHECK(fake_mmr_expect("init", want, ARRAY_LEN(want)));
	/* S-13a.042: prediction on once, after the EVT11 write. */
	CHECK(fake_core_bp_ons() == 1u);
	CHECK(fake_core_bp_on_at() == ARRAY_LEN(want));
	CHECK(fake_core_imask() == 0x881Fu);
	CHECK(fake_mmr_errors() == 0);
}

static void test_attach_and_dispatch(void)
{
	int ctx = 0;
	static const struct fake_mmr_access want[] = {
		FAKE_R32(SEC0_CSID0, 12u),
		FAKE_W32(SEC0_CSID0, 12u),
		FAKE_W32(SEC0_END, 12u),
		FAKE_SYNC,
	};

	reset_fakes();
	CHECK(wb_sec_attach(12u, 5, false, handler, &ctx) == WB_SEC_OK);
	CHECK(fake_mmr_get(SEC0_SCTL(12)) == ((5u << 8) | SEC_SCTL_IEN));
	CHECK(fake_core_imask() == 0);	/* restored after the update */
	CHECK(wb_sec_enable(12u) == WB_SEC_OK);
	CHECK(fake_mmr_get(SEC0_SCTL(12)) ==
	      ((5u << 8) | SEC_SCTL_IEN | SEC_SCTL_SEN));
	CHECK(wb_sec_attach(WB_SID_TIMER0_TMR(3), 0, true, handler, NULL) ==
	      WB_SEC_OK);
	CHECK(fake_mmr_get(SEC0_SCTL(14)) == (SEC_SCTL_ES | SEC_SCTL_IEN));

	fake_mmr_clear_log();
	calls = 0;
	fake_mmr_set(SEC0_CSID0, 12u);
	wb_sec_dispatch();
	CHECK(calls == 1 && last_sid == 12u && last_ctx == &ctx);
	CHECK(fake_mmr_expect("dispatch", want, ARRAY_LEN(want)));

	CHECK(wb_sec_disable(12u) == WB_SEC_OK);
	CHECK((fake_mmr_get(SEC0_SCTL(12)) & SEC_SCTL_SEN) == 0);
}

static void test_spurious(void)
{
	reset_fakes();
	fake_mmr_set(SEC0_SCTL(40), SEC_SCTL_IEN | SEC_SCTL_SEN);
	fake_mmr_set(SEC0_CSID0, 40u);
	calls = 0;
	wb_sec_dispatch();
	CHECK(calls == 0);
	CHECK(fake_mmr_get(SEC0_SCTL(40)) == SEC_SCTL_IEN);
	CHECK(fake_mmr_get(SEC0_END) == 40u);
	/* An ID past the last source is ended, never used as an index. */
	fake_mmr_set(SEC0_CSID0, 0xFFu);
	wb_sec_dispatch();
	CHECK(fake_mmr_get(SEC0_END) == 0xFFu);
	CHECK(fake_mmr_errors() == 0);
}

static void test_bad_sid(void)
{
	reset_fakes();
	CHECK(wb_sec_attach(WB_SEC_SOURCES, 0, false, handler, NULL) ==
	      WB_SEC_ERR_SID);
	CHECK(wb_sec_enable(WB_SEC_SOURCES) == WB_SEC_ERR_SID);
	CHECK(wb_sec_disable(200u) == WB_SEC_ERR_SID);
	CHECK(wb_sec_fault_reset(WB_SEC_SOURCES) == WB_SEC_ERR_SID);
	CHECK(fake_mmr_count() == 0);
}

/* S-13a.050: no RESET once a source has been configured. */
static void test_no_reset_after_configuration(void)
{
	reset_fakes();
	CHECK(wb_sec_init() == WB_SEC_ERR_AGAIN);
	CHECK(fake_core_bp_ons() == 0u);	/* no vector written */
	CHECK(!any_reset_write());
	CHECK(fake_mmr_get(SEC0_GCTL) == SEC_CTL_EN);
	CHECK(fake_mmr_get(SEC0_CCTL0) == SEC_CTL_EN);
}

/* S-13a.061: the watchdog's expiry becomes a system reset request. */
static void test_fault_reset(void)
{
	reset_fakes();
	fake_mmr_set(SEC0_SCTL(WB_SID_WDOG0), SEC_SCTL_IEN);
	CHECK(wb_sec_fault_reset(WB_SID_WDOG0) == WB_SEC_OK);
	CHECK(fake_mmr_get(SEC0_SCTL(3)) ==
	      (SEC_SCTL_IEN | SEC_SCTL_SEN | SEC_SCTL_FEN));
	CHECK(fake_mmr_get(SEC0_FCTL) == (SEC_FCTL_EN | SEC_FCTL_SREN));
	CHECK(fake_mmr_get(SEC0_GCTL) & SEC_CTL_EN);
	CHECK(!any_reset_write());
}

int main(void)
{
	RUN(test_cec);
	/* test_init must run before anything configures a source. */
	RUN(test_init);
	RUN(test_attach_and_dispatch);
	RUN(test_spurious);
	RUN(test_bad_sid);
	RUN(test_no_reset_after_configuration);
	RUN(test_fault_reset);
	return check_summary("test_drv_sec");
}
