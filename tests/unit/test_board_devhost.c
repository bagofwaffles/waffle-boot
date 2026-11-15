/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Developer host requests and the hand-over options (waffle/devhost.h,
 * src/board/opxy_devhost.c, src/board/opxy_handover.c): opxy_ho_flags = 0 is
 * the normal hand-over (S-05.023), each bit moves one piece away from it,
 * the developer requests answer only while developer mode is on, and
 * opxy_dev_mem_ok bounds the access.
 */
#include <string.h>

#include "support/check.h"
#include "support/fake_core.h"
#include "support/fake_mmr.h"

#include "waffle/board/opxy_boot.h"
#include "waffle/devhost.h"
#include "waffle/drivers/bf70x_coremmr.h"
#include "waffle/drivers/bf70x_dma.h"
#include "waffle/drivers/bf70x_dmc.h"
#include "waffle/drivers/bf70x_msi.h"
#include "waffle/drivers/bf70x_port.h"
#include "waffle/drivers/bf70x_spi.h"
#include "waffle/drivers/bf70x_sec.h"

#define CRC1_CTL 0x200B1000u

static bool msi_hook(void *ctx, enum fake_mmr_op op, unsigned w,
		     wb_mmr_addr addr, uint32_t *v)
{
	(void)ctx;
	(void)w;
	if (op == FAKE_MMR_WRITE && addr == MSI_CTL)
		*v &= ~MSI_CTL_RESETS;
	if (op == FAKE_MMR_WRITE && (addr == MSI_ISTAT || addr == MSI_IDSTS)) {
		fake_mmr_set(addr, fake_mmr_get(addr) & ~*v);
		return false;
	}
	return true;
}

static void reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
	fake_mmr_set_hook(msi_hook, NULL);
	fake_mmr_set(DMC0_STAT, DMC_STAT_IDLE);
	opxy_ho_flags = 0u;
}

static bool written(wb_mmr_addr addr)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == addr)
			return true;
	return false;
}

static struct opxy_master m;
static struct opxy_slave s;

static void slave_ready(void)
{
	memset(&s, 0, sizeof(s));
	s.hw.time.cycles_per_us = 400u;
	s.hw.msi.c = &opxy_msi;
	s.hw.msi.t = &s.hw.time;
	s.emmc_up = true;
}

/* Every DMA channel stopped (S-05.016), and the DDR controller read. */
static bool all_dma_stopped(void)
{
	unsigned ch;

	for (ch = 0; ch < WB_DMA_CHANNELS; ch++)
		if (!written(DMA_CFG(ch)) || fake_mmr_get(DMA_CFG(ch)) != 0u)
			return false;
	return true;
}

static bool read_of(wb_mmr_addr addr)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++)
		if (log[i].op == FAKE_MMR_READ && log[i].addr == addr)
			return true;
	return false;
}

/* opxy_ho_flags == 0: the normal hand-over (S-05.023). */
static void test_normal(void)
{
	reset();
	fake_mmr_set(L1IM_ICTL, 1u);				/* bit 0 as at reset */
	fake_mmr_set(L1DM_DCTL, 1u);
	opxy_teardown_master(&m);
	CHECK(all_dma_stopped());				/* S-05.016 DMA row */
	CHECK(read_of(DMC0_STAT));				/* DDR controller idle */
	CHECK(!written(CRC1_CTL));
	CHECK(opxy_handover_os.sp == 0x88A65B44u);		/* S-05.014 SP */
	CHECK(opxy_handover_os.evt[0] == 0x11A00D4Cu);		/* S-05.014 EVT2 */
	CHECK(opxy_ho_cache.on == 1u);				/* S-05.014 */
	CHECK(opxy_ho_cache.ictl == 0x207u && opxy_ho_cache.dctl == 0x207u);
	CHECK(fake_mmr_get(SEC0_SCTL(1)) == 0x5u);		/* SEC set */

	reset();
	slave_ready();
	fake_mmr_set(L1IM_ICTL, 1u);
	fake_mmr_set(L1DM_DCTL, 1u);
	opxy_teardown_slave(&s);
	CHECK(fake_mmr_get(MSI_CTL) == 0x10u);			/* MSI in FIFO mode */
	CHECK(all_dma_stopped());
	CHECK(opxy_handover_os.sp == 0x88A65B18u);
	CHECK(opxy_ho_cache.on == 1u);
	CHECK(opxy_ho_cache.ictl == 0x207u && opxy_ho_cache.dctl == 0x207u);
}

/* The page that holds a, or -1. */
static int page_of(const struct wb_cplb *t, unsigned n, uint32_t a)
{
	unsigned i;

	for (i = 0; i < n; i++)
		if (a >= t[i].addr && a - t[i].addr < wb_cplb_page_bytes(t[i].data))
			return (int)i;
	return -1;
}

/*
 * S-05.031's pages under S-05.014's control value: a valid table, the
 * reserve (where the install runs) uncached on both sides, L1 left to the
 * defaults, the display bank and the OTP uncached data pages, and the
 * install in the PRM's order, the tags initialized before ENCPLB.
 */
static void test_cache_table(void)
{
	const struct wb_cplb *it = opxy_ho_icplb, *dt = opxy_ho_dcplb;
	const unsigned ni = opxy_ho_n_icplb, nd = opxy_ho_n_dcplb;
	int i, d;

	CHECK(wb_cplb_check(it, ni, false) == WB_CPLB_OK);
	CHECK(wb_cplb_check(dt, nd, true) == WB_CPLB_OK);
	i = page_of(it, ni, 0x0803C000u);
	d = page_of(dt, nd, 0x0803C000u);
	CHECK(i >= 0 && (it[i].data & ICPLB_CPROPS_MASK) == ICPLB_NOCACHE);
	CHECK(d >= 0 && (dt[d].data & DCPLB_CPROPS_MASK) == DCPLB_NOCACHE);
	CHECK(page_of(it, ni, 0x11A00000u) < 0 && page_of(dt, nd, 0x11800000u) < 0);
	d = page_of(dt, nd, 0x70000000u);
	CHECK(d >= 0 && (dt[d].data & DCPLB_CPROPS_MASK) == DCPLB_NOCACHE &&
	      wb_cplb_page_bytes(dt[d].data) == 4096u);
	d = page_of(dt, nd, 0x38000000u);
	CHECK(d >= 0 && (dt[d].data & DCPLB_CPROPS_MASK) == DCPLB_NOCACHE);
	CHECK(page_of(dt, nd, 0x8FFFFFFCu) >= 0 && page_of(it, ni, 0x8FFFFFFCu) >= 0);
	CHECK(page_of(dt, nd, 0x04000000u) >= 0 && page_of(it, ni, 0x44000000u) >= 0);

	reset();
	opxy_teardown_master(&m);
	fake_mmr_clear_log();
	opxy_ho_finish();
	{
		const struct fake_mmr_access *log = fake_mmr_log();
		size_t k, first_ictl = 0, last_ictl = 0, entry = 0;

		for (k = 0; k < fake_mmr_count(); k++) {
			if (log[k].op != FAKE_MMR_WRITE)
				continue;
			if (log[k].addr == L1IM_ICTL) {
				if (first_ictl == 0)
					first_ictl = k + 1;
				last_ictl = k + 1;
			}
			if (log[k].addr == L1IM_ICPLB_ADDR(0))
				entry = k + 1;
		}
		CHECK(first_ictl != 0 && entry > first_ictl && last_ictl > entry);
		CHECK((log[first_ictl - 1].value & L1IM_ICTL_ENCPLB) == 0u &&
		      (log[first_ictl - 1].value & L1IM_ICTL_CFG) != 0u);
	}
	CHECK(fake_mmr_get(L1IM_ICTL) == opxy_ho_cache.ictl);
	CHECK(fake_mmr_get(L1DM_DCTL) == opxy_ho_cache.dctl);
	CHECK(fake_mmr_get(L1IM_ICPLB_DFLT) == 0x300u && fake_mmr_get(L1DM_DCPLB_DFLT) == 0xF00u);
	CHECK(fake_mmr_get(L1DM_DCPLB_ADDR(12)) == 0x38000000u &&
	      fake_mmr_get(L1DM_DCPLB_DATA(13)) == 0u);		/* unused: invalid */
	CHECK(read_of(DMC0_STAT));
}

/* OPXY_HO_TRACE records a milestone through the teardown; off, it stays 0. */
static void test_trace(void)
{
	reset();
	opxy_ho_trace = 0x99u;
	opxy_ho_flags = 0u;			/* trace off */
	opxy_teardown_master(&m);
	CHECK(opxy_ho_trace == 0x99u);		/* untouched */

	reset();
	opxy_ho_trace = 0u;
	opxy_ho_flags = OPXY_HO_TRACE;
	opxy_teardown_master(&m);
	CHECK(opxy_ho_trace == OPXY_HO_M_ENTRY);

	reset();
	slave_ready();
	opxy_ho_flags = OPXY_HO_TRACE;
	opxy_teardown_slave(&s);
	CHECK(opxy_ho_trace == OPXY_HO_M_ENTRY);
}

/* The "as found" options leave each piece as waffle-boot had it. */
static void test_asfound(void)
{
	reset();
	fake_mmr_set(SEC0_SCTL(1), SEC_SCTL_IEN | SEC_SCTL_SEN | 0x300u);
	opxy_ho_flags = OPXY_HO_SEC_ASFOUND | OPXY_HO_NO_SPFP | OPXY_HO_NO_EVT |
			OPXY_HO_DMA_KEEP | OPXY_HO_NO_USB_STOP | OPXY_HO_NO_TIMER_STOP;
	opxy_teardown_master(&m);
	CHECK(!written(DMA_CFG(20u)));			/* DMA left running */
	CHECK(opxy_handover_os.sp != 0x88A65B44u);	/* the kernel's SP */
	CHECK(opxy_handover_os.fp == 0u);
	CHECK(!written(SEC0_SCTL(1)));			/* SEC left as found */

	/* MSI as found, slave teardown skipped. */
	reset();
	slave_ready();
	opxy_ho_flags = OPXY_HO_MSI_ASFOUND;
	opxy_teardown_slave(&s);
	CHECK(!written(MSI_CTL));

	reset();
	slave_ready();
	opxy_ho_flags = OPXY_HO_NO_SLAVE_TEAR;
	opxy_teardown_slave(&s);
	CHECK(!written(MSI_CTL) && !written(SEC0_SCTL(1)));	/* teardown skipped */
	CHECK(opxy_handover_os.sp == 0x88A65B18u);		/* entry still set */
}

/* OPXY_HO_CRC1_STOCK writes S-05.031's value; cache off and DMA keep leave
 * those pieces as contract XY-HANDOFF-1 has them. */
static void test_stock(void)
{
	reset();
	opxy_ho_flags = OPXY_HO_CRC1_STOCK;
	opxy_teardown_master(&m);
	CHECK(fake_mmr_get(CRC1_CTL) == OPXY_HO_CRC1_CTL);

	/* OPXY_HO_CACHE_OFF: the CPLBs and caches stay off at the jump */
	reset();
	opxy_ho_flags = OPXY_HO_CACHE_OFF;
	opxy_teardown_master(&m);
	CHECK(opxy_ho_cache.on == 0u);
	fake_mmr_clear_log();
	opxy_ho_finish();
	CHECK(!written(L1IM_ICTL) && !written(L1DM_DCTL));

	/* OPXY_HO_DMA_KEEP: the display's channels left running */
	reset();
	opxy_ho_flags = OPXY_HO_DMA_KEEP;
	opxy_teardown_master(&m);
	CHECK(!written(DMA_CFG(20u)) && !written(DMA_CFG(21u)) && written(DMA_CFG(16u)));
}

static void test_mem_ok(void)
{
	CHECK(opxy_dev_mem_ok(0x20004000u, false));	/* MMR */
	CHECK(opxy_dev_mem_ok(0x20004000u, true));
	CHECK(opxy_dev_mem_ok(0x1FC01004u, true));	/* core MMR */
	CHECK(!opxy_dev_mem_ok(0x20004001u, false));	/* misaligned */
	CHECK(!opxy_dev_mem_ok(0x20075000u, false));	/* OTP: never */
	CHECK(!opxy_dev_mem_ok(0x20075000u, true));
	CHECK(opxy_dev_mem_ok(0x20055000u + 0x2000u, false));	/* SPI2: read ok */
	CHECK(!opxy_dev_mem_ok(0x20055000u + 0x2000u, true));	/* SPI2: no write */
	CHECK(!opxy_dev_mem_ok(0x00000000u, false));	/* outside */
	CHECK(opxy_dev_mem_ok(OPXY_IMAGE_START, true));	/* this image */
	CHECK(opxy_dev_mem_ok(0x8FFFFF00u, false));	/* display record: read */
	CHECK(opxy_dev_mem_ok(0x8FFFFF28u, false));
	CHECK(!opxy_dev_mem_ok(0x8FFFFF00u, true));	/* but not write */
	CHECK(!opxy_dev_mem_ok(0x8FFFFF2Cu, false));	/* just past it */
}

/*
 * The gate: a request is served with developer mode on and refused with it
 * off, and nothing is written when refused. The reply's payload is decoded
 * from the wire so the status byte can be read.
 */
static uint8_t wire[256];
static size_t wire_n;

static size_t port_room(void *ctx) { (void)ctx; return sizeof(wire) - wire_n; }
static void port_write(void *ctx, const uint8_t *data, size_t n)
{
	(void)ctx;
	if (wire_n + n <= sizeof(wire)) {
		memcpy(wire + wire_n, data, n);
		wire_n += n;
	}
}
static const struct s06_port_ops port = { NULL, port_room, port_write };

static uint8_t crc8(const uint8_t *p, size_t n)
{
	uint8_t c = 0;

	while (n-- != 0u)
		c = (uint8_t)(c ^ *p++);
	return c;
}

static uint32_t reply_word;	/* the payload's bytes 1-4, of reply_status() */
static uint8_t reply[S06_MAX_PAYLOAD];	/* the payload, of reply_status() */

/* The first payload byte of the single frame on the wire (COBS over the
 * frame; byte 0 is the length, the payload begins after id and route). */
static uint8_t reply_status(size_t *plen)
{
	uint8_t dec[S06_MAX_PAYLOAD + 8u] = {0};
	size_t dn = 0, i;

	for (i = 0; i < wire_n && wire[i] != 0u; i++)	/* to the delimiter */
		;
	/* COBS-decode wire[0..i). */
	{
		size_t j = 0;

		while (j < i) {
			uint8_t code = wire[j++];
			uint8_t k;

			for (k = 1u; k < code && j < i; k++)
				dec[dn++] = wire[j++];
			if (code < 0xFFu && j < i)
				dec[dn++] = 0u;
		}
	}
	/* dec: route, seq, id, len, payload..., crc (S-06 frame, S06_HEADER 4). */
	*plen = dn >= 5u ? dec[3] : 0u;
	memset(reply, 0, sizeof(reply));
	if (dn >= 5u && *plen <= dn - 4u)
		memcpy(reply, dec + 4, *plen);
	reply_word = dn >= 9u ? (uint32_t)dec[5] | (uint32_t)dec[6] << 8 |
		     (uint32_t)dec[7] << 16 | (uint32_t)dec[8] << 24 : 0u;
	return dn >= 5u ? dec[4] : 0xEEu;
}

static void dev_frame(uint8_t op, const uint8_t *extra, size_t en)
{
	struct s06_frame f;

	memset(&f, 0, sizeof(f));
	f.id = OPXY_DEV_REQ_ID;
	f.route = s06_route(S06_ADDR_HOST, S06_ADDR_MASTER);
	f.payload[0] = op;
	if (en != 0u)
		memcpy(f.payload + 1, extra, en);
	f.len = (uint8_t)(1u + en);
	wire_n = 0;
	CHECK(opxy_dev_host(&m, &f));
}

static void test_gate(void)
{
	uint8_t wr[8];
	size_t plen;

	fake_mmr_reset();
	fake_core_reset();
	memset(&m, 0, sizeof(m));
	s06_node_init(&m.node, crc8, NULL);
	s06_link_init(&m.link[S06_LINK_CDC], &port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m.node.link[S06_LINK_CDC] = &m.link[S06_LINK_CDC];
	wr[0] = 0x00; wr[1] = 0x40; wr[2] = 0x00; wr[3] = 0x20;	/* 0x20004000 */
	wr[4] = 0x5A; wr[5] = 0; wr[6] = 0; wr[7] = 0;

	/* Developer mode off: PING is refused, and a WRITE changes nothing. */
	m.sv.menu.dev_mode = false;
	dev_frame(OPXY_DEV_PING, NULL, 0);
	CHECK(reply_status(&plen) == OPXY_DEV_REFUSED && plen == 1u);
	dev_frame(OPXY_DEV_WRITE, wr, 8);
	CHECK(reply_status(&plen) == OPXY_DEV_REFUSED);
	CHECK(!written(0x20004000u));

	/* Developer mode on: PING answers OK with the magic, WRITE takes effect. */
	m.sv.menu.dev_mode = true;
	dev_frame(OPXY_DEV_PING, NULL, 0);
	CHECK(reply_status(&plen) == OPXY_DEV_OK && plen == 5u);
	dev_frame(OPXY_DEV_WRITE, wr, 8);
	CHECK(reply_status(&plen) == OPXY_DEV_OK);
	CHECK(fake_mmr_get(0x20004000u) == 0x5Au);
}

/*
 * S-10c.110: the options apply only to a hand-over a developer request
 * starts. A flags request holds them for the next one and leaves
 * opxy_ho_flags, which the ordinary hand-over reads, at 0; a hand-over
 * request that returns leaves 0 behind, and so does developer mode going
 * off, which also drops what is held.
 */
static uint32_t seen_flags;
static unsigned stages;

static void ho_pre_load(void *ctx)
{
	(void)ctx;
}

static enum s05_fail ho_stage(void *ctx, uint32_t *length)
{
	(void)ctx;
	*length = 0u;
	seen_flags = opxy_ho_flags;
	stages++;
	return S05_FAIL_NO_OS;
}

static uint32_t flags_reply(void)
{
	size_t plen;

	return reply_status(&plen) == OPXY_DEV_OK && plen == 5u ?
	       reply_word : 0xEEEEEEEEu;
}

static void test_options_scope(void)
{
	const uint8_t set[5] = { 1u, 0x00u, 0x28u, 0u, 0u };	/* 0x2800 */
	const uint8_t ask[5] = { 0u, 0u, 0u, 0u, 0u };
	const uint8_t own[8] = { 0x00u, 0x10u, 0u, 0u, 0u, 0u, 0u, 0u };

	reset();
	memset(&m, 0, sizeof(m));
	s06_node_init(&m.node, crc8, NULL);
	s06_link_init(&m.link[S06_LINK_CDC], &port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m.node.link[S06_LINK_CDC] = &m.link[S06_LINK_CDC];
	m.bridge.absent = true;
	m.handoff.pre_load = ho_pre_load;
	m.handoff.stage = ho_stage;
	m.sv.menu.dev_mode = true;
	stages = 0;

	dev_frame(OPXY_DEV_FLAGS, set, sizeof(set));
	CHECK(flags_reply() == 0x2800u);
	CHECK(opxy_ho_flags == 0u);
	dev_frame(OPXY_DEV_FLAGS, ask, sizeof(ask));
	CHECK(flags_reply() == 0x2800u);

	/* With no options of its own, the request takes those held. */
	dev_frame(OPXY_DEV_HANDOVER, NULL, 0);
	CHECK(stages == 1u && seen_flags == 0x2800u);
	CHECK(opxy_ho_flags == 0u);
	dev_frame(OPXY_DEV_HANDOVER, own, sizeof(own));
	CHECK(stages == 2u && seen_flags == 0x1000u);
	CHECK(opxy_ho_flags == 0u);

	/* Developer mode off: nothing held once it is on again. */
	opxy_ho_flags = OPXY_HO_TRACE;		/* a write to it, say */
	opxy_dev_keys(&m);
	CHECK(opxy_ho_flags == OPXY_HO_TRACE);	/* still on: kept */
	m.sv.menu.dev_mode = false;
	opxy_dev_keys(&m);
	CHECK(opxy_ho_flags == 0u);
	m.sv.menu.dev_mode = true;
	dev_frame(OPXY_DEV_FLAGS, ask, sizeof(ask));
	CHECK(flags_reply() == 0u);
	opxy_ho_flags = 0u;
}

/* The slave's PA4 at the end of its server: a GPIO driven low by default
 * (S-05.020), its level set before it leaves SPI2; to SPI2 with
 * OPXY_HO_PA4_SPI2 (S-05.031). */
static void test_pa4(void)
{
	static const struct fake_mmr_access to_spi2[] = {
		{ FAKE_MMR_WRITE, 32, PORT_FER_SET(WB_PORT_A), 1u << 4 },
	};
	static const struct fake_mmr_access low[] = {
		{ FAKE_MMR_WRITE, 32, PORT_DATA_CLR(WB_PORT_A), 1u << 4 },
		{ FAKE_MMR_WRITE, 32, PORT_DIR_SET(WB_PORT_A), 1u << 4 },
		{ FAKE_MMR_WRITE, 32, PORT_FER_CLR(WB_PORT_A), 1u << 4 },
	};

	reset();
	opxy_ho_leave_pa4();
	CHECK(fake_mmr_expect_writes("pa4 low", low, 3));
	CHECK(!written(PORT_FER_SET(WB_PORT_A)));
	CHECK(!written(PORT_DATA_SET(WB_PORT_A)));
	reset();
	opxy_ho_flags = OPXY_HO_PA4_SPI2;
	opxy_ho_leave_pa4();
	CHECK(fake_mmr_expect_writes("pa4 to spi2", to_spi2, 1));
}

/* Each OPXY_HO_SLAVE_MARKS pulse is one PA4 set, then one clear. */
static unsigned pa4_pulses(void)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	unsigned sets = 0, clears = 0;
	size_t i;

	for (i = 0; i < fake_mmr_count(); i++) {
		if (log[i].op != FAKE_MMR_WRITE || log[i].value != (1u << 4))
			continue;
		if (log[i].addr == PORT_DATA_SET(WB_PORT_A))
			sets++;
		else if (log[i].addr == PORT_DATA_CLR(WB_PORT_A) && clears < sets)
			clears++;
	}
	return sets == clears ? sets : 0xFFu;
}

/* OPXY_HO_SLAVE_MARKS: PA4 low from stop_server, then a pulse per slave step,
 * step k lasting k x 2 ms; the master's own hand-over makes none. */
static void test_marks(void)
{
	uint64_t c0;
	unsigned before;

	reset();
	opxy_ho_flags = OPXY_HO_SLAVE_MARKS;
	opxy_ho_leave_pa4();
	CHECK(written(PORT_FER_CLR(WB_PORT_A)) && !written(PORT_FER_SET(WB_PORT_A)));

	/* the whole teardown: teardown, MSI, SEC, entry, delayed */
	reset();
	slave_ready();
	fake_core_set_cycle_step(400u);			/* 1 us a read */
	opxy_ho_flags = OPXY_HO_SLAVE_MARKS;
	c0 = fake_core_peek_cycles();
	opxy_teardown_slave(&s);
	CHECK(pa4_pulses() == 5u);
	/* at least (k x 2 + 2) ms for k = 1..5 */
	CHECK(fake_core_peek_cycles() - c0 >= (2u + 4u + 6u + 8u + 10u + 5u * 2u) * 1000u * 400u);
	/* the kernel's return and the jump (the load function's first-call mark
	 * reads a real configuration block, which a host address cannot be) */
	before = pa4_pulses();
	opxy_ho_finish();
	CHECK(pa4_pulses() == before + 2u);

	/* interrupts off alone: teardown, entry, delayed */
	reset();
	slave_ready();
	fake_core_set_cycle_step(400u);
	opxy_ho_flags = OPXY_HO_SLAVE_MARKS | OPXY_HO_NO_SLAVE_TEAR;
	opxy_teardown_slave(&s);
	CHECK(pa4_pulses() == 3u);

	/* the master: none, in its teardown, load or entry */
	reset();
	opxy_ho_flags = OPXY_HO_SLAVE_MARKS;
	opxy_teardown_master(&m);
	opxy_ho_finish();
	CHECK(pa4_pulses() == 0u);
	CHECK(!written(PORT_DATA_SET(WB_PORT_A)));
}

/* OPXY_HO_MASTER_STOCK: SPI0 off with the OPXY_HO_SPI0_* registers;
 * without it the bridge's own values stay. SPI2_CTL is S-05.016's 0x2
 * either way. */
static void test_master_stock(void)
{
	reset();
	opxy_teardown_master(&m);
	CHECK(fake_mmr_get(SPI_CTL(0)) == 0u && fake_mmr_get(SPI_RXCTL(0)) == 0u);
	CHECK(!written(SPI_SLVSEL(0)) && !written(SPI_CLK(0)));
	CHECK(fake_mmr_get(SPI_CTL(2)) == 0x2u);

	reset();
	opxy_ho_flags = OPXY_HO_MASTER_STOCK;
	opxy_teardown_master(&m);
	CHECK(fake_mmr_get(SPI_CTL(0)) == 0u);
	CHECK(fake_mmr_get(SPI_RXCTL(0)) == 0x0001001Cu);
	CHECK(fake_mmr_get(SPI_TXCTL(0)) == 0x0001001Cu);
	CHECK(fake_mmr_get(SPI_CLK(0)) == 1u);
	CHECK(fake_mmr_get(SPI_DLY(0)) == 0u && written(SPI_DLY(0)));
	CHECK(fake_mmr_get(SPI_SLVSEL(0)) == 0x0808u);
	CHECK(fake_mmr_get(SPI_CTL(2)) == 0x2u);
}

/* PINT_LATCH is W1C. */
static bool latch_hook(void *ctx, enum fake_mmr_op op, unsigned w,
		       /* cppcheck-suppress constParameterCallback */
		       wb_mmr_addr addr, uint32_t *v)
{
	(void)ctx;
	(void)w;
	if (op == FAKE_MMR_WRITE && addr == PINT_LATCH(0u)) {
		fake_mmr_set(addr, fake_mmr_get(addr) & ~*v);
		return false;
	}
	return true;
}

#define PA6 (1u << 6)
#define LATCH_PA6 (1u << 22)

static void os_wait_reset(void)
{
	reset();
	fake_mmr_set_hook(latch_hook, NULL);
	memset(&m, 0, sizeof(m));
	m.hw.time.cycles_per_us = 1u;
	fake_core_set_cycle_step(1000u);		/* 1 ms a read */
	fake_mmr_set(PINT_ASSIGN(0u), 0x00010101u);
}

/* The latch's clear comes after a low read of PA6, never before. */
static bool cleared_after_low(void)
{
	const struct fake_mmr_access *log = fake_mmr_log();
	size_t i;
	bool low = false;

	for (i = 0; i < fake_mmr_count(); i++) {
		if (log[i].op == FAKE_MMR_READ && log[i].addr == PORT_DATA(WB_PORT_A) &&
		    (log[i].value & PA6) == 0u)
			low = true;
		if (log[i].op == FAKE_MMR_WRITE && log[i].addr == PINT_LATCH(0u))
			return low;
	}
	return false;
}

/*
 * S-05.032: PA6 low, then high, within T_OSSTART; the high from PINT0's
 * latch, armed only after a low read, so a high before the server's stop
 * does not count and a pulse polling misses does.
 */
static void test_os_wait(void)
{
	/* high from the session, low at the server's stop, then a latched pulse */
	os_wait_reset();
	fake_mmr_set(PORT_DATA(WB_PORT_A), PA6);
	fake_mmr_queue_read(PORT_DATA(WB_PORT_A), PA6);
	fake_mmr_queue_read(PORT_DATA(WB_PORT_A), PA6);
	fake_mmr_queue_read(PORT_DATA(WB_PORT_A), 0u);
	fake_mmr_set(PINT_LATCH(0u), LATCH_PA6);	/* stale, from the session */
	fake_mmr_queue_read(PINT_LATCH(0u), 0u);
	fake_mmr_queue_read(PINT_LATCH(0u), 0u);
	fake_mmr_queue_read(PINT_LATCH(0u), LATCH_PA6);
	CHECK(opxy_os_wait(&m) == 0);
	CHECK(cleared_after_low());
	CHECK(fake_mmr_get(PINT_ASSIGN(0u)) == 0x00000101u);	/* PA.L on byte 2 */
	CHECK(written(PINT_EDGE_CLR(0u)) && written(PINT_INV_CLR(0u)));
	CHECK(!written(PINT_MSK_SET(0u)));			/* stays masked */

	/* never low: the bound runs out */
	os_wait_reset();
	fake_mmr_set(PORT_DATA(WB_PORT_A), PA6);
	fake_mmr_set(PINT_LATCH(0u), LATCH_PA6);
	CHECK(opxy_os_wait(&m) == -1);
	CHECK(!written(PINT_LATCH(0u)));
	CHECK(fake_core_peek_cycles() >= OPXY_T_OSSTART_US);

	/* low, never high: the stale latch was cleared, the bound runs out */
	os_wait_reset();
	fake_mmr_set(PORT_DATA(WB_PORT_A), 0u);
	fake_mmr_set(PINT_LATCH(0u), LATCH_PA6);
	CHECK(opxy_os_wait(&m) == -1);
	CHECK(cleared_after_low());
	CHECK(OPXY_T_OSSTART_US >= 10000000u && OPXY_T_OSSTART_US <= 30000000u);

	/* OPXY_HO_NO_OS_WAIT and OPXY_HO_STAY: no wait, nothing touched */
	os_wait_reset();
	fake_mmr_set(PORT_DATA(WB_PORT_A), PA6);
	opxy_ho_flags = OPXY_HO_NO_OS_WAIT;
	CHECK(opxy_os_wait(&m) == 0 && fake_mmr_count() == 0u);
	opxy_ho_flags = OPXY_HO_STAY;
	CHECK(opxy_os_wait(&m) == 0 && fake_mmr_count() == 0u);
}

/*
 * The raw yaffs2 reads: tags of any chunk of the region, and the first bytes
 * of a header (0x200) or a summary (1,540) only; a data chunk, an unused
 * chunk and anything past the region are refused.
 */
#define YD_CHUNKS 4u
static uint8_t yd_image[YD_CHUNKS * 4096u];
static unsigned yd_reads;
static bool yd_fail;

static int yd_read(void *ctx, uint32_t sector, uint32_t count, uint8_t *buf)
{
	(void)ctx;
	yd_reads++;
	if (yd_fail)
		return -1;
	while (count-- != 0u) {
		if (sector < YD_CHUNKS * 8u)
			memcpy(buf, yd_image + 512u * sector, 512u);
		else
			memset(buf, 0, 512u);
		sector++;
		buf += 512u;
	}
	return 0;
}

static void yd_tags(uint32_t chunk, uint32_t seq, uint32_t obj, uint32_t cw,
		    uint32_t bytes)
{
	uint8_t *t = yd_image + 4096u * chunk + 0xFF0u;
	const uint32_t w[4] = { seq, obj, cw, bytes };
	unsigned i, k;

	for (i = 0; i < 4u; i++)
		for (k = 0; k < 4u; k++)
			t[4u * i + k] = (uint8_t)(w[i] >> (8u * k));
}

static void test_ydump(void)
{
	const struct opxy_ydump_ops io = { NULL, yd_read };
	uint8_t out[OPXY_YMETA_MAX];
	unsigned i;

	memset(yd_image, 0, sizeof(yd_image));
	for (i = 0; i < 4096u; i++) {
		yd_image[i] = (uint8_t)i;			/* header */
		yd_image[4096u + i] = 0xD5u;			/* data */
		yd_image[3u * 4096u + i] = (uint8_t)(i * 7u);	/* summary */
	}
	yd_tags(0, 0x1234u, 0x10000100u, 0x80000001u, 77u);	/* file header */
	yd_tags(1, 0x1234u, 0x100u, 1u, 4080u);			/* data chunk */
	yd_tags(3, 0x1234u, 0x10u, 1u, 1540u);			/* summary */

	/* Tags: chunk 0, then a stride that lands on chunks 1 and 3. */
	CHECK(opxy_ydump_tags(&io, 0u, 1u, 2u, out) == OPXY_DEV_OK);
	CHECK(memcmp(out, yd_image + 0xFF0u, 16u) == 0);
	CHECK(memcmp(out + 16, yd_image + 4096u + 0xFF0u, 16u) == 0);
	CHECK(opxy_ydump_tags(&io, 1u, 2u, 2u, out) == OPXY_DEV_OK);
	CHECK(memcmp(out + 16, yd_image + 3u * 4096u + 0xFF0u, 16u) == 0);
	/* The region's last chunk is readable, the one after it is not. */
	CHECK(opxy_ydump_tags(&io, 0x8FFFFu, 0u, 1u, out) == OPXY_DEV_OK);
	CHECK(opxy_ydump_tags(&io, 0x90000u, 0u, 1u, out) == OPXY_DEV_BAD);
	CHECK(opxy_ydump_tags(&io, 0x8FF80u, 0x80u, 2u, out) == OPXY_DEV_BAD);
	CHECK(opxy_ydump_tags(&io, 0u, 0xFFFFFFFFu, 2u, out) == OPXY_DEV_BAD);
	CHECK(opxy_ydump_tags(&io, 0u, 0u, 2u, out) == OPXY_DEV_BAD);
	CHECK(opxy_ydump_tags(&io, 0u, 1u, 0u, out) == OPXY_DEV_BAD);
	CHECK(opxy_ydump_tags(&io, 0u, 1u, OPXY_YTAGS_MAX + 1u, out) == OPXY_DEV_BAD);

	/* A header's first 0x200 bytes, across a sector boundary too. */
	CHECK(opxy_ydump_meta(&io, 0u, 0u, OPXY_YMETA_MAX, out) == OPXY_DEV_OK);
	CHECK(memcmp(out, yd_image, OPXY_YMETA_MAX) == 0);
	CHECK(opxy_ydump_meta(&io, 0u, 0x200u - 8u, 8u, out) == OPXY_DEV_OK);
	CHECK(memcmp(out, yd_image + 0x1F8u, 8u) == 0);
	CHECK(opxy_ydump_meta(&io, 0u, 0x200u - 8u, 9u, out) == OPXY_DEV_REFUSED);
	/* A summary's first 1,540 bytes, across a sector boundary. */
	CHECK(opxy_ydump_meta(&io, 3u, 500u, 30u, out) == OPXY_DEV_OK);
	CHECK(memcmp(out, yd_image + 3u * 4096u + 500u, 30u) == 0);
	CHECK(opxy_ydump_meta(&io, 3u, 1540u - 4u, 4u, out) == OPXY_DEV_OK);
	CHECK(opxy_ydump_meta(&io, 3u, 1540u - 4u, 5u, out) == OPXY_DEV_REFUSED);
	/* A data chunk and an unused chunk give nothing. */
	memset(out, 0, sizeof(out));
	CHECK(opxy_ydump_meta(&io, 1u, 0u, 16u, out) == OPXY_DEV_REFUSED);
	CHECK(opxy_ydump_meta(&io, 2u, 0u, 16u, out) == OPXY_DEV_REFUSED);
	for (i = 0; i < sizeof(out); i++)
		CHECK(out[i] == 0u);
	/* Bounds and a failed read. */
	CHECK(opxy_ydump_meta(&io, 0u, 0u, 0u, out) == OPXY_DEV_BAD);
	CHECK(opxy_ydump_meta(&io, 0u, 0u, OPXY_YMETA_MAX + 1u, out) == OPXY_DEV_BAD);
	CHECK(opxy_ydump_meta(&io, 0x90000u, 0u, 4u, out) == OPXY_DEV_BAD);
	yd_fail = true;
	CHECK(opxy_ydump_tags(&io, 0u, 1u, 1u, out) == OPXY_DEV_REFUSED);
	CHECK(opxy_ydump_meta(&io, 0u, 0u, 4u, out) == OPXY_DEV_REFUSED);
	yd_fail = false;
}

/*
 * With developer mode on but no slave volume (its statfs fails), the raw
 * reads are refused before any eMMC read.
 */
static void test_ydump_gate(void)
{
	uint8_t req[9] = { 0 };
	size_t plen;

	fake_mmr_reset();
	fake_core_reset();
	memset(&m, 0, sizeof(m));
	s06_node_init(&m.node, crc8, NULL);
	s06_link_init(&m.link[S06_LINK_CDC], &port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m.node.link[S06_LINK_CDC] = &m.link[S06_LINK_CDC];
	m.bridge.absent = true;
	s09_files_init(&m.sv.files, &m.bridge);
	m.sv.menu.dev_mode = true;
	req[8] = 1u;
	/* the slave absent: not answering, which is no refusal */
	dev_frame(OPXY_DEV_YTAGS, req, 9);
	CHECK(reply_status(&plen) == OPXY_DEV_NO_SLAVE && plen == 1u);
	dev_frame(OPXY_DEV_YTAGS, req, 8);
	CHECK(reply_status(&plen) == OPXY_DEV_BAD);
	req[6] = 4u;
	dev_frame(OPXY_DEV_YMETA, req, 7);
	CHECK(reply_status(&plen) == OPXY_DEV_NO_SLAVE);
	m.sv.menu.dev_mode = false;
	dev_frame(OPXY_DEV_YMETA, req, 7);
	CHECK(reply_status(&plen) == OPXY_DEV_REFUSED);
}

static void host_reset(void)
{
	fake_mmr_reset();
	fake_core_reset();
	memset(&m, 0, sizeof(m));
	s06_node_init(&m.node, crc8, NULL);
	s06_link_init(&m.link[S06_LINK_CDC], &port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m.node.link[S06_LINK_CDC] = &m.link[S06_LINK_CDC];
	s09_master_set_absent(&m.bridge);
	s09_files_init(&m.sv.files, &m.bridge);
	m.sv.menu.dev_mode = true;
}

/*
 * Every file request while the slave is not answering says so
 * (OPXY_DEV_NO_SLAVE), with no bridge traffic; a malformed one is still
 * malformed, and with developer mode off each is refused.
 */
static void test_files_no_slave(void)
{
	static const uint8_t open[] = { OPXY_DEV_PUT_OPEN, 'o', 's', '.', 'l', 'd', 'r' };
	static const uint8_t data[] = { OPXY_DEV_PUT_DATA, 1, 2, 3 };
	static const uint8_t close[] = { OPXY_DEV_PUT_CLOSE };
	static const uint8_t bad[] = { OPXY_DEV_PUT_OPEN, 'x' };
	static const uint8_t name[] = { 'o', 's', '.', 'l', 'd', 'r' };
	size_t plen;

	host_reset();
	dev_frame(OPXY_DEV_PUT, open, sizeof(open));
	CHECK(reply_status(&plen) == OPXY_DEV_NO_SLAVE && plen == 1u);
	dev_frame(OPXY_DEV_PUT, data, sizeof(data));
	CHECK(reply_status(&plen) == OPXY_DEV_NO_SLAVE);
	dev_frame(OPXY_DEV_PUT, close, sizeof(close));
	CHECK(reply_status(&plen) == OPXY_DEV_NO_SLAVE);
	dev_frame(OPXY_DEV_SHA, name, sizeof(name));
	CHECK(reply_status(&plen) == OPXY_DEV_NO_SLAVE && plen == 1u);
	dev_frame(OPXY_DEV_PUT, bad, sizeof(bad));
	CHECK(reply_status(&plen) == OPXY_DEV_BAD);
	m.sv.menu.dev_mode = false;
	dev_frame(OPXY_DEV_SHA, name, sizeof(name));
	CHECK(reply_status(&plen) == OPXY_DEV_REFUSED);
}

/*
 * OPXY_DEV_BOOT_START: the failed start's account as held mode keeps it
 * (opxy_start_report()), and zeros after a boot that tried none.
 */
static void test_boot_start(void)
{
	size_t plen;

	host_reset();
	dev_frame(OPXY_DEV_BOOT_START, NULL, 0);
	CHECK(reply_status(&plen) == OPXY_DEV_OK &&
	      plen == 1u + OPXY_START_REPORT && reply[1] == 0u);
	m.sv.held.load_attempted = true;
	m.sv.held.cause = S05_FAIL_SELECT;
	m.sv.held.start.load.result = S03_LOAD_IO;
	m.sv.held.start.load.step = S03_STEP_IMAGE;
	m.sv.held.start.load.step_part = 2u;
	m.sv.held.start.load.sector = 1281u;
	m.sv.held.start.lost = true;
	m.sv.held.start.why.cmd = S09_READ_SECTORS;
	m.sv.held.start.why.at = S09_AT_REPLY_BROKE;
	m.sv.held.start.why.fault[3] = 0x0Eu;
	dev_frame(OPXY_DEV_BOOT_START, NULL, 0);
	CHECK(reply_status(&plen) == OPXY_DEV_OK && plen == 1u + OPXY_START_REPORT);
	CHECK(reply[1] == 1u && reply[2] == S05_FAIL_SELECT &&
	      reply[3] == S03_LOAD_IO && reply[4] == S03_STEP_IMAGE &&
	      reply[5] == 2u && reply[8] == 0x01u && reply[9] == 0x05u);
	CHECK(reply[39] == 1u && reply[40] == S09_READ_SECTORS &&
	      reply[41] == S09_AT_REPLY_BROKE && reply[58] == 0x0Eu);
	/* refused like every request with developer mode off */
	m.sv.menu.dev_mode = false;
	dev_frame(OPXY_DEV_BOOT_START, NULL, 0);
	CHECK(reply_status(&plen) == OPXY_DEV_REFUSED && plen == 1u);
}

/* OPXY_DEV_CAL_START: the answer, then S-04's W10: the shutdown, reason
 * 70 in the record, the hard reset (S-04.012). Refused with developer mode
 * off, and then nothing is written. */
static char cal_ops[64];

static void cal_op(const char *op)
{
	strncat(cal_ops, op, sizeof(cal_ops) - strlen(cal_ops) - 1u);
}

static void cal_shutdown(void *ctx)
{
	(void)ctx;
	cal_op("shutdown ");
}

static void cal_write(void *ctx, uint16_t reason)
{
	(void)ctx;
	cal_op(reason == S04_REASON_OS_FRAME_CAL ? "write:70 " : "write:? ");
}

static void cal_reset(void *ctx)
{
	(void)ctx;
	cal_op("reset");
}

static void test_cal_start(void)
{
	size_t plen;

	fake_mmr_reset();
	fake_core_reset();
	memset(&m, 0, sizeof(m));
	s06_node_init(&m.node, crc8, NULL);
	s06_link_init(&m.link[S06_LINK_CDC], &port, NULL, S06_ADDR_MASTER,
		      S06_ADDR_HOST);
	m.node.link[S06_LINK_CDC] = &m.link[S06_LINK_CDC];
	m.sv.reboot.shutdown = cal_shutdown;
	m.sv.reboot.write_record = cal_write;
	m.sv.reboot.hard_reset = cal_reset;
	cal_ops[0] = '\0';
	dev_frame(OPXY_DEV_CAL_START, NULL, 0);
	CHECK(reply_status(&plen) == OPXY_DEV_REFUSED && cal_ops[0] == '\0');
	m.sv.menu.dev_mode = true;
	dev_frame(OPXY_DEV_CAL_START, NULL, 0);
	CHECK(reply_status(&plen) == OPXY_DEV_OK && plen == 1u);
	CHECK(strcmp(cal_ops, "shutdown write:70 reset") == 0);
}

int main(void)
{
	RUN(test_normal);
	RUN(test_cache_table);
	RUN(test_os_wait);
	RUN(test_trace);
	RUN(test_asfound);
	RUN(test_stock);
	RUN(test_mem_ok);
	RUN(test_gate);
	RUN(test_options_scope);
	RUN(test_pa4);
	RUN(test_marks);
	RUN(test_master_stock);
	RUN(test_ydump);
	RUN(test_ydump_gate);
	RUN(test_files_no_slave);
	RUN(test_boot_start);
	RUN(test_cal_start);
	return check_summary("test_board_devhost");
}
