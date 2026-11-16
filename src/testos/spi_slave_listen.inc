/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The test OS's armed listen for S-13b Q-11, which wb_spi_slave_listen()
 * takes when the monitor has asked for one.
 *
 * Part of src/drivers/bf70x/spi.c in the test OS's build (waffle/testos.h,
 * wiki/Test-OS.md), included there where it stands rather than compiled
 * alone: it uses the driver's static functions and state, and the order of
 * the driver's functions places the test OS image's code.
 */

#define TESTOS_REQ_QUAD_CTL 0x00222510u	/* S-13b.044: a quad request's SPI2_CTL */
#define TESTOS_LISTEN_US    2000000u

/* S-13b Q-11 (1g), (1h): a listen under wb_spi_testos_req's bits, with the
 * request set-up of S-13b.083 and EN set first or last. MIOM is written only
 * while EN is clear (S-13b.040). */
static int testos_listen(const struct wb_spi_slave *s, uint32_t start_us)
{
	const struct wb_spi *p = &s->spi;
	const uint32_t req = wb_spi_testos_req;
	const uint32_t left = (req >> OPXY_TESTOS_REQ_COUNT_SHIFT & 0xFFu) - 1u;
	const bool en_last = (req & OPXY_TESTOS_REQ_EN_LAST) != 0u;
	const bool asked = (req & OPXY_TESTOS_REQ_QUAD) != 0u;
	const bool quad = asked && (dir_ctl(p, false) | SPI_CTL_MIOM_QUAD) == TESTOS_REQ_QUAD_CTL;
	const uint32_t ctl = dir_ctl(p, false) | (quad ? SPI_CTL_MIOM_QUAD : 0u);
	unsigned k;
	int rc;

	wb_spi_testos_req = left != 0u ? (req & ~0xFF00u) | left << OPXY_TESTOS_REQ_COUNT_SHIFT : 0u;
	wb_spi_testos_reqstat[OPXY_REQ_BITS] = (req & 0xFFFFu) | (asked && !quad ? 0x80000000u : 0u);
	wb_spi_testos_reqstat[OPXY_REQ_RC] = OPXY_TESTOS_Q11_NONE;
	for (k = OPXY_REQ_HDR0; k <= OPXY_REQ_HDR2; k++)
		wb_spi_testos_reqstat[k] = 0u;
	request_setup(p, ctl, !en_last);
	listen_mark(p);
	rc = hand_over(s, WB_SPI_SLAVE_SESSION, start_us);
	if (rc != WB_SPI_OK) {
		wb_spi_testos_reqstat[OPXY_REQ_RC] = (uint32_t)rc;
		return slave_end(s, rc, false);
	}
	testos_armed_listen = true;
	testos_armed_quad = quad;
	testos_listen_end = wb_time_deadline(p->t, TESTOS_LISTEN_US);
	return WB_SPI_OK;
}
