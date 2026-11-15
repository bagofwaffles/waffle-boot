/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Core event controller and System Event Controller (see
 * waffle/drivers/bf70x_sec.h).
 */
#include "waffle/drivers/bf70x_sec.h"

#include <stddef.h>

#include "waffle/drivers/bf70x_core.h"
#include "waffle/drivers/bf70x_coremmr.h"
#include "waffle/drivers/mmr.h"

struct handler {
	wb_irq_handler fn;
	void *ctx;
};

static struct handler handlers[WB_SEC_SOURCES];
static bool configured;		/* an SEC_SCTL has been written */

void wb_cec_set_vector(unsigned ivg, uint32_t addr)
{
	wb_mmr_write32(ICU_EVT(ivg), addr);
	wb_core_csync();
}

void wb_cec_enable(unsigned ivg)
{
	uint32_t mask = wb_core_cli();

	wb_core_sti(mask | (1u << ivg));
}

void wb_cec_disable(unsigned ivg)
{
	uint32_t mask = wb_core_cli();

	wb_core_sti(mask & ~(1u << ivg));
}

int wb_sec_init(void)
{
	unsigned i;

	if (configured) {
		wb_mmr_set32(SEC0_GCTL, SEC_CTL_EN);
		wb_mmr_set32(SEC0_CCTL0, SEC_CTL_EN);
		wb_mmr_sync();
		return WB_SEC_ERR_AGAIN;
	}
	wb_cec_disable(WB_IVG_SEC);
	for (i = 0; i < WB_SEC_SOURCES; i++) {
		handlers[i].fn = NULL;
		handlers[i].ctx = NULL;
	}
	/* Returns every SEC register, SREN included, to its reset value. */
	wb_mmr_write32(SEC0_GCTL, SEC_CTL_RESET);
	wb_mmr_sync();
	wb_mmr_write32(SEC0_GCTL, SEC_CTL_EN);
	wb_mmr_write32(SEC0_CCTL0, SEC_CTL_EN);
	wb_mmr_sync();
	wb_cec_set_vector(WB_IVG_SEC, (uint32_t)(uintptr_t)wb_sec_entry);
	/*
	 * S-13a.042: prediction goes on again once the image has written its
	 * own event vectors in its start-up; this is the last of them
	 * (start.S wrote the others). The OS's vectors, written at the
	 * hand-over, are not meant. Every SEC interrupt is taken at IVG11
	 * without nesting (S-13a.051, sec_entry.S).
	 */
	wb_core_bp_on();
	wb_cec_enable(WB_IVG_SEC);
	return WB_SEC_OK;
}

int wb_sec_attach(uint32_t sid, uint8_t prio, bool edge, wb_irq_handler fn,
		  void *ctx)
{
	uint32_t ctl = ((uint32_t)prio << SEC_SCTL_PRIO_SHIFT) | SEC_SCTL_IEN;
	uint32_t mask;

	if (sid >= WB_SEC_SOURCES)
		return WB_SEC_ERR_SID;
	if (edge)
		ctl |= SEC_SCTL_ES;
	mask = wb_core_cli();
	handlers[sid].fn = fn;
	handlers[sid].ctx = ctx;
	configured = true;
	wb_mmr_write32(SEC0_SCTL(sid), ctl);
	wb_mmr_sync();
	wb_core_sti(mask);
	return WB_SEC_OK;
}

int wb_sec_enable(uint32_t sid)
{
	if (sid >= WB_SEC_SOURCES)
		return WB_SEC_ERR_SID;
	configured = true;
	wb_mmr_set32(SEC0_SCTL(sid), SEC_SCTL_SEN);
	wb_mmr_sync();
	return WB_SEC_OK;
}

int wb_sec_disable(uint32_t sid)
{
	if (sid >= WB_SEC_SOURCES)
		return WB_SEC_ERR_SID;
	configured = true;
	wb_mmr_clear32(SEC0_SCTL(sid), SEC_SCTL_SEN);
	wb_mmr_sync();
	return WB_SEC_OK;
}

void wb_sec_dispatch(void)
{
	uint32_t sid = wb_mmr_read32(SEC0_CSID0) & 0xFFu;

	wb_mmr_write32(SEC0_CSID0, sid);	/* acknowledge */
	if (sid < WB_SEC_SOURCES && handlers[sid].fn != NULL) {
		handlers[sid].fn(handlers[sid].ctx, sid);
	} else if (sid < WB_SEC_SOURCES) {
		wb_mmr_clear32(SEC0_SCTL(sid), SEC_SCTL_SEN);
	}
	wb_mmr_write32(SEC0_END, sid);
	wb_mmr_sync();
}

int wb_sec_fault_reset(uint32_t sid)
{
	if (sid >= WB_SEC_SOURCES)
		return WB_SEC_ERR_SID;
	configured = true;
	wb_mmr_set32(SEC0_SCTL(sid), SEC_SCTL_FEN | SEC_SCTL_SEN);
	wb_mmr_set32(SEC0_FCTL, SEC_FCTL_EN | SEC_FCTL_SREN);
	wb_mmr_set32(SEC0_GCTL, SEC_CTL_EN);
	wb_mmr_sync();
	return WB_SEC_OK;
}
