/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the test OS's slave end records for the USB monitor, and the state
 * of a reply set-up the monitor chose and of an armed listen.
 *
 * Part of src/drivers/bf70x/spi.c in the test OS's build (waffle/testos.h,
 * wiki/Test-OS.md), included there where it stands rather than compiled
 * alone: it uses the driver's static functions and state, and the order of
 * the driver's functions places the test OS image's code.
 */

/* What the last reply's send saw, for the USB monitor (wiki/Test-OS.md):
 * every STAT bit, the last STAT, then rc, words out and in, TWC; TWC
 * before the hand-over and when PB15 first read low; then core cycles after
 * the hand-over at which PB15 first read low, TWC first read 0, the FIFO first
 * read empty and the last word was received, 0 for not seen. With a reply
 * set-up chosen: only those replies are kept, slot 7 is every ILAT bit, slot 10
 * the cycle TF was first seen, and replies under mode 0 leave it all alone.
 * An armed reply fills it the same way, its slot 1 after the tight loop of
 * S-13b Q-11 (1d) the last SPI_STAT that loop read. */
volatile uint32_t wb_spi_testos_send[12];
/* The reply set-up the monitor has chosen, OPXY_TESTOS_REPLY_*; 0 the default. */
volatile uint32_t wb_spi_testos_mode;
/* S-13b Q-11's measurement 4: each change of SPI_STAT during such a reply, as pairs
 * of the core cycle and the value, with bit 31 set while PB15 reads low; the
 * first word counts the pairs. */
volatile uint32_t wb_spi_testos_trace[1u + 2u * OPXY_TESTOS_TRACE_MAX];
/* S-13b Q-11's armed replies and listens, and what they saw (waffle/testos.h). */
volatile uint32_t wb_spi_testos_arm;
volatile uint32_t wb_spi_testos_q11[OPXY_Q11_SLOTS];
volatile uint32_t wb_spi_testos_tally[2];
volatile uint32_t wb_spi_testos_req;
volatile uint32_t wb_spi_testos_reqstat[OPXY_REQ_SLOTS];
/* The set-up of the reply being sent, for dir_ctl(): 0 the default reply. */
static uint32_t testos_reply_mode;
/* An armed listen whose request header has not come yet, and its 2 s. */
static bool testos_armed_listen, testos_armed_quad;
static uint64_t testos_listen_end;
