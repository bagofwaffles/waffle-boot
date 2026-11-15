/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Clock Generation Unit (see waffle/drivers/bf70x_cgu.h). */
#include "waffle/drivers/bf70x_cgu.h"

#include <stddef.h>

uint32_t wb_cgu_ctl_value(const struct wb_cgu_config *c)
{
	uint32_t ctl = ((uint32_t)c->msel & 0x7Fu) << CGU_CTL_MSEL_SHIFT;

	if (c->df)
		ctl |= CGU_CTL_DF;
	return ctl;
}

uint32_t wb_cgu_div_value(const struct wb_cgu_config *c)
{
	return ((uint32_t)c->csel << CGU_DIV_CSEL_SHIFT) |
	       ((uint32_t)c->s0sel << CGU_DIV_S0SEL_SHIFT) |
	       ((uint32_t)c->syssel << CGU_DIV_SYSSEL_SHIFT) |
	       ((uint32_t)c->s1sel << CGU_DIV_S1SEL_SHIFT) |
	       ((uint32_t)c->dsel << CGU_DIV_DSEL_SHIFT) |
	       ((uint32_t)c->osel << CGU_DIV_OSEL_SHIFT);
}

static bool in(uint32_t v, uint32_t lo, uint32_t hi)
{
	return v >= lo && v <= hi;
}

static bool over(uint32_t f, uint32_t max)
{
	return max != 0 && f > max;
}

int wb_cgu_freqs(const struct wb_cgu_config *c, struct wb_cgu_freqs *f)
{
	const struct wb_cgu_limits *l = &c->limits;
	struct wb_cgu_freqs r;
	uint64_t vco;

	if (c->clkin_hz == 0 || !in(c->msel, 1, 128) || !in(c->csel, 1, 31) ||
	    !in(c->syssel, 1, 31) || !in(c->s0sel, 1, 7) ||
	    !in(c->s1sel, 1, 7) || !in(c->dsel, 1, 31) ||
	    !in(c->osel, 1, 127) || c->clkoutsel > 15)
		return WB_CGU_ERR_ARG;
	vco = (uint64_t)c->clkin_hz * c->msel / (c->df ? 2u : 1u);
	if (vco > UINT32_MAX)
		return WB_CGU_ERR_LIMIT;
	r.vco = (uint32_t)vco;
	r.cclk = r.vco / c->csel;
	r.sysclk = r.vco / c->syssel;
	r.sclk0 = r.sysclk / c->s0sel;
	r.sclk1 = r.sysclk / c->s1sel;
	r.dclk = r.vco / c->dsel;
	r.oclk = r.vco / c->osel;
	if (r.vco < l->vco_min || over(r.vco, l->vco_max) ||
	    over(r.cclk, l->cclk_max) || over(r.sysclk, l->sysclk_max) ||
	    over(r.sclk0, l->sclk0_max) || over(r.sclk1, l->sclk1_max) ||
	    over(r.dclk, l->dclk_max) || over(r.oclk, l->oclk_max))
		return WB_CGU_ERR_LIMIT;
	if (f != NULL)
		*f = r;
	return WB_CGU_OK;
}

#define MHZ(x) ((x) * 1000000u)

int wb_cgu_check_part(const struct wb_cgu_config *c,
		      const struct wb_cgu_part *p)
{
	struct wb_cgu_freqs f;
	uint32_t cmax, smax;
	int rc = wb_cgu_freqs(c, &f);

	if (rc != WB_CGU_OK)
		return rc;
	if (!in(c->msel, 8, 41) || !in(f.vco, 230200000u, MHZ(800u)))
		return WB_CGU_ERR_LIMIT;
	if (f.vco == MHZ(800u)) {
		cmax = MHZ(400u);
		smax = MHZ(200u);
	} else if (f.vco >= MHZ(600u)) {
		cmax = MHZ(390u);
		smax = MHZ(195u);
	} else if (f.vco >= MHZ(380u)) {
		cmax = MHZ(380u);
		smax = MHZ(190u);
	} else {
		cmax = f.vco;
		smax = f.vco / 2u;
	}
	if (p->cclk_grade_hz != 0 && p->cclk_grade_hz < cmax)
		cmax = p->cclk_grade_hz;
	if (f.cclk > cmax || f.sysclk > smax || f.cclk < f.sysclk ||
	    f.sclk0 > MHZ(100u) || f.sclk1 > MHZ(200u) || f.oclk > MHZ(50u))
		return WB_CGU_ERR_LIMIT;
	if (p->usb && (f.sysclk < MHZ(60u) || f.sclk0 < MHZ(30u)))
		return WB_CGU_ERR_LIMIT;
	if (p->dmc != WB_CGU_DMC_NONE &&
	    (f.dclk > f.sysclk || f.dclk > MHZ(200u) ||
	     f.dclk < (p->dmc == WB_CGU_DMC_DDR2 ? MHZ(125u) : MHZ(10u))))
		return WB_CGU_ERR_LIMIT;
	return WB_CGU_OK;
}
