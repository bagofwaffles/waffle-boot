/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_DRIVERS_BF70X_COREMMR_H
#define WAFFLE_DRIVERS_BF70X_COREMMR_H

/*
 * Blackfin+ core memory-mapped registers, at the addresses of S-01.012's
 * table (tests/oracle/S-01-boot-chain/memory_map.json, mmr.core_mmr): the classic
 * Blackfin core layout at 0x1FC00000 in place of 0xFFE00000, one 0x1000
 * block each for L1 data, L1 code, events and the core timer.
 *
 * The Programming Reference's event vector table (chapter 4) prints the
 * vectors at 0x1FE02000; a vector written there is never taken (S-01.012).
 * tests/unit/test_drv_coremmr.c pins every address here to that table.
 */

/* L1 data memory controller (PRM chapter 7, L1DM). */
#define L1DM_BASE              0x1FC00000u
#define L1DM_DCTL              0x1FC00004u
#define L1DM_DCPLB_DFLT        0x1FC00010u
#define L1DM_DCPLB_ADDR(n)     (0x1FC00100u + 4u * (n))
#define L1DM_DCPLB_DATA(n)     (0x1FC00200u + 4u * (n))

/* L1 instruction memory controller (PRM chapter 7, L1IM). */
#define L1IM_ICTL              0x1FC01004u
#define L1IM_ICPLB_DFLT        0x1FC01010u
#define L1IM_ICPLB_ADDR(n)     (0x1FC01100u + 4u * (n))
#define L1IM_ICPLB_DATA(n)     (0x1FC01200u + 4u * (n))

/* Core event controller (PRM chapter 4): the vector table and its masks. */
#define ICU_EVT(n)             (0x1FC02000u + 4u * (n))
#define ICU_EVT_OVERRIDE       0x1FC02100u
#define ICU_IMASK              0x1FC02104u
#define ICU_IPEND              0x1FC02108u
#define ICU_ILAT               0x1FC0210Cu

/* Core timer (PRM chapter 5, TMR). */
#define TMR_TCNTL              0x1FC03000u
#define TMR_TPERIOD            0x1FC03004u
#define TMR_TSCALE             0x1FC03008u
#define TMR_TCOUNT             0x1FC0300Cu

/* The number of CPLB register pairs per side (PRM: sixteen). */
#define WB_CPLB_ENTRIES 16u

#endif /* WAFFLE_DRIVERS_BF70X_COREMMR_H */
