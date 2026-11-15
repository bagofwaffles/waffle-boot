/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_TESTS_S13B_CHECK_H
#define WAFFLE_TESTS_S13B_CHECK_H

/*
 * The checkers of the bridge's register traces, in C, rule for rule as
 * tests/oracle/S-13b-buses-and-pins/check.py has them: check_bridge_dma
 * (S-13b.083), check_master_sessions (S-13b.040), check_reply_setup
 * (S-13b.044) and check_slave_tx (S-13b.045). Each returns the rules a trace
 * breaks as a mask of S13B_*; test_drv_spi.c runs each on every row of the
 * vectors (vectors/s13b_vectors.h) before it is used on the
 * driver's own traces.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	S13B_DMA_CORE = 1u << 0,
	S13B_DMA_CFG = 1u << 1,
	S13B_DMA_DESC = 1u << 2,
	S13B_DMA_ORDER = 1u << 3,
	S13B_DMA_ENABLE_LAST = 1u << 4,
	S13B_SEND_TWC = 1u << 5,
	S13B_SEND_TXCTL = 1u << 6,
	S13B_TF_UNMASK = 1u << 7,
	S13B_SEND_END = 1u << 8,
	S13B_RECV_RWC = 1u << 9,
	S13B_RECV_RXCTL = 1u << 10,
	S13B_RECV_PART = 1u << 11,
	S13B_RECV_END = 1u << 12,
	S13B_RECV_HEADER = 1u << 13,
	/* check_master_sessions */
	S13B_SPI0_NOT_RESET = 1u << 16,
	S13B_SPI0_RESET_IN_SESSION = 1u << 17,
	S13B_SPI0_WORD_DISABLED = 1u << 18,
	S13B_ENABLE_LAST = 1u << 19,
	S13B_MIOM_WHILE_ENABLED = 1u << 20,
};

enum s13b_kind { S13B_MASTER_SEND, S13B_MASTER_RECEIVE, S13B_SLAVE_RECEIVE, S13B_SLAVE_REPLY };
enum s13b_op { S13B_W, S13B_R, S13B_DESC, S13B_SELECT };

/* One event of a check_bridge_dma trace: a register write or read (reg its
 * name, as check.py's), a descriptor array given to channel ch (the sum of its
 * buffers' bytes, all check.py uses) or the master's PC11. */
struct s13b_ev {
	enum s13b_op op;
	const char *reg;
	uint32_t val;
	unsigned ch;
	uint64_t bytes;
	bool on;
};

#define S13B_CFG_SEND  0x00035021u
#define S13B_CFG_RECV  0x00035023u
#define S13B_CFG_MSIZE 0x00000700u
#define S13B_MASTER_XCTL 0x0001001Du
#define S13B_SLAVE_RXCTL 0x00010015u
#define S13B_TF 0x800u
#define S13B_RF 0x400u
#define S13B_PA4 0x10u

static inline bool s13b_is(const struct s13b_ev *e, const char *pre, const char *name)
{
	size_t k = strlen(pre);

	return e->reg != NULL && strncmp(e->reg, pre, k) == 0 && e->reg[k] == '_' &&
	       strcmp(e->reg + k + 1u, name) == 0;
}

static inline unsigned s13b_bridge_dma(enum s13b_kind kind, const struct s13b_ev *t, size_t n,
				uint32_t words, const uint32_t *parts, unsigned nparts)
{
	const char *spi = kind <= S13B_MASTER_RECEIVE ? "SPI0" : "SPI2";
	const char *dma = kind == S13B_MASTER_SEND ? "DMA4" : kind == S13B_MASTER_RECEIVE ? "DMA5" :
			  kind == S13B_SLAVE_RECEIVE ? "DMA9" : "DMA8";
	const unsigned ch = kind == S13B_MASTER_SEND ? 4u : kind == S13B_MASTER_RECEIVE ? 5u :
			    kind == S13B_SLAVE_RECEIVE ? 9u : 8u;
	const bool send = kind == S13B_MASTER_SEND || kind == S13B_SLAVE_REPLY;
	const uint32_t want = send ? S13B_CFG_SEND : S13B_CFG_RECV;
	uint64_t desc[64];
	unsigned out = 0, nd = 0, i;
	bool started = false;

	for (i = 0; i < n; i++) {
		const struct s13b_ev *e = &t[i];

		if ((send && e->op == S13B_W && s13b_is(e, spi, "TFIFO")) ||
		    (!send && e->op == S13B_R && s13b_is(e, spi, "RFIFO")))
			out |= S13B_DMA_CORE;
		if (e->op == S13B_W && s13b_is(e, dma, "CFG") && (e->val & 1u)) {
			started = true;
			if ((e->val & ~S13B_CFG_MSIZE) != want)
				out |= S13B_DMA_CFG;
		}
		if (e->op == S13B_DESC && e->ch == ch && nd < 64u)
			desc[nd++] = e->bytes;
	}
	if (!started)
		out |= S13B_DMA_CORE;

	if (kind == S13B_SLAVE_REPLY) {
		if (nd != 1u || desc[0] != 4u * (uint64_t)words + 8u)
			out |= S13B_DMA_DESC;
		return out;
	}

	if (kind == S13B_MASTER_SEND) {
		static const char *const order[] = { "stat", "ack", "dscptr", "cfg", "twc", "txctl" };
		const char *setup[64];
		unsigned ns = 0, stage = 0;
		bool en = false, done = false, unmasked = false, taken = false;

		if (nd != 1u || desc[0] != 4u * (uint64_t)words)
			out |= S13B_DMA_DESC;
		for (i = 0; i < n; i++) {
			const struct s13b_ev *e = &t[i];
			const char *name = NULL;

			if (e->op == S13B_DESC)
				continue;
			if (e->op == S13B_SELECT) {
				if (!e->on) {
					if (stage != 2u)
						out |= S13B_SEND_END;
					stage = 3u;
				}
				continue;
			}
			if (e->op == S13B_R) {
				if (s13b_is(e, dma, "STAT") && (e->val & 1u) && en)
					done = true;
				else if (s13b_is(e, spi, "ILAT") && (e->val & S13B_TF) && unmasked)
					taken = true;
				continue;
			}
			if (s13b_is(e, spi, "CTL")) {
				if ((e->val & 1u) && !en) {
					en = true;
					if (ns != 6u)
						out |= S13B_DMA_ORDER;
					else
						for (unsigned k = 0; k < 6u; k++)
							if (strcmp(setup[k], order[k]) != 0)
								out |= S13B_DMA_ORDER;
				} else if (!(e->val & 1u) && en) {
					en = false;
					if (!taken || stage != 1u)
						out |= S13B_SEND_END;
					stage = 2u;
				}
				continue;
			}
			if (s13b_is(e, spi, "IMSK_SET") && (e->val & S13B_TF)) {
				unmasked = true;
				if (!done)
					out |= S13B_TF_UNMASK;
				continue;
			}
			if (s13b_is(e, spi, "TXCTL") && (en || stage) && !(e->val & 1u)) {
				if (!taken)
					out |= S13B_SEND_END;
				if (stage < 1u)
					stage = 1u;
				continue;
			}
			if (s13b_is(e, spi, "STAT"))
				name = "stat";
			else if (s13b_is(e, dma, "STAT"))
				name = "ack";
			else if (s13b_is(e, dma, "DSCPTR_CUR"))
				name = "dscptr";
			else if (s13b_is(e, dma, "CFG"))
				name = "cfg";
			else if (s13b_is(e, spi, "TWC"))
				name = "twc";
			else if (s13b_is(e, spi, "TXCTL"))
				name = "txctl";
			if (name == NULL || ((s13b_is(e, dma, "STAT") || s13b_is(e, spi, "STAT")) && en))
				continue;
			if (en) {
				out |= S13B_DMA_ENABLE_LAST;
				continue;
			}
			if (s13b_is(e, spi, "STAT") && e->val != 0xFFFFFFFFu)
				continue;
			if (ns < 64u)
				setup[ns++] = name;
			if (s13b_is(e, spi, "TWC") && e->val != words)
				out |= S13B_SEND_TWC;
			if (s13b_is(e, spi, "TXCTL") && e->val != S13B_MASTER_XCTL)
				out |= S13B_SEND_TXCTL;
		}
		if (stage != 3u)
			out |= S13B_SEND_END;
		return out;
	}

	if (nd != nparts)
		out |= S13B_DMA_DESC;
	else
		for (i = 0; i < nd; i++)
			if (desc[i] != parts[i])
				out |= S13B_DMA_DESC;

	if (kind == S13B_MASTER_RECEIVE) {
		static const char *const order[] = { "dscptr", "cfg", "rwc", "rxctl" };
		const char *setup[64];
		unsigned ns = 0, p = 0, stage = 0;
		bool en = false, running = false, done = false, have_ctl = false;
		uint32_t ctl = 0;

		for (i = 0; i < n; i++) {
			const struct s13b_ev *e = &t[i];
			const char *name = NULL;

			if (e->op == S13B_DESC)
				continue;
			if (e->op == S13B_SELECT) {
				if (!e->on) {
					if (stage != 2u)
						out |= S13B_RECV_END;
					stage = 3u;
				}
				continue;
			}
			if (e->op == S13B_R) {
				if (s13b_is(e, dma, "STAT") && (e->val & 1u) && running)
					done = true;
				continue;
			}
			if (s13b_is(e, spi, "CTL")) {
				if ((e->val & 1u) && !en) {
					en = true;
					ctl = e->val;
					have_ctl = true;
					if (p == 0u && !running) {
						running = true;
						if (ns != 4u)
							out |= S13B_DMA_ORDER;
						else
							for (unsigned k = 0; k < 4u; k++)
								if (strcmp(setup[k], order[k]) != 0)
									out |= S13B_DMA_ORDER;
						ns = 0;
					} else {
						out |= S13B_RECV_PART;
					}
				} else if (!(e->val & 1u) && en) {
					en = false;
					if (p < nparts)
						out |= S13B_RECV_PART;
					else if (stage != 1u)
						out |= S13B_RECV_END;
					stage = 2u;
				} else if (en && have_ctl && e->val != ctl) {
					out |= S13B_RECV_PART;
				}
				continue;
			}
			if (s13b_is(e, spi, "RXCTL") && !(e->val & 1u)) {
				if (running && !done)
					out |= S13B_RECV_PART;
				if (running) {
					running = false;
					done = false;
					p++;
					if (p == nparts)
						stage = 1u;
				}
				continue;
			}
			if (s13b_is(e, dma, "DSCPTR_CUR"))
				name = "dscptr";
			else if (s13b_is(e, dma, "CFG"))
				name = "cfg";
			else if (s13b_is(e, spi, "RWC"))
				name = "rwc";
			else if (s13b_is(e, spi, "RXCTL"))
				name = "rxctl";
			if (name == NULL)
				continue;
			if (running && done) {
				out |= S13B_RECV_PART;
				running = false;
				done = false;
				p++;
			} else if (running) {
				out |= p == 0u ? S13B_DMA_ENABLE_LAST : S13B_RECV_PART;
				continue;
			}
			if (p >= nparts)
				continue;
			if (ns < 64u)
				setup[ns++] = name;
			if (s13b_is(e, spi, "RWC") && e->val != parts[p] / 4u)
				out |= S13B_RECV_RWC;
			if (s13b_is(e, spi, "RXCTL")) {
				if (e->val != S13B_MASTER_XCTL)
					out |= S13B_RECV_RXCTL;
				if (p != 0u) {
					bool bad_order = false;

					running = true;
					if (ns != 4u)
						bad_order = true;
					else
						for (unsigned k = 0; k < 4u; k++)
							if (strcmp(setup[k], order[k]) != 0)
								bad_order = true;
					if (bad_order)
						out |= S13B_DMA_ORDER;
					ns = 0;
				}
			}
		}
		if (stage != 3u)
			out |= S13B_RECV_END;
		return out;
	}

	/* slave_receive */
	{
		static const char *const order[] = { "off", "mask", "dscptr", "cfg", "rxctl" };
		const char *setup[64];
		unsigned ns = 0, p = 0, stage = 0;
		bool en = false, handed = false, running = false;

		if (nd != 0u && desc[0] != 12u)
			out |= S13B_RECV_HEADER;
		for (i = 0; i < n; i++) {
			const struct s13b_ev *e = &t[i];
			bool last;

			if (e->op != S13B_W) {
				if (e->op == S13B_R && s13b_is(e, dma, "STAT") && (e->val & 1u) && running) {
					running = false;
					p++;
					ns = 0;
				}
				continue;
			}
			last = p == nparts;
			if (handed && !running && !last) {		/* between two parts */
				if (s13b_is(e, dma, "STAT") || s13b_is(e, dma, "DSCPTR_CUR")) {
					if (ns < 64u)
						setup[ns++] = s13b_is(e, dma, "DSCPTR_CUR") ? "dscptr" : "ack";
				} else if (s13b_is(e, dma, "CFG")) {
					running = true;
					if (ns == 0u || strcmp(setup[ns - 1u], "dscptr") != 0)
						out |= S13B_DMA_ORDER;
				} else {
					out |= S13B_RECV_PART;
				}
				continue;
			}
			if (handed && last) {				/* the end: PA4, EN, REN */
				if (strcmp(e->reg, "PORTA_FER_CLR") == 0 && (e->val & S13B_PA4)) {
					if (stage == 0u)
						stage = 1u;
				} else if (s13b_is(e, spi, "CTL") && !(e->val & 1u)) {
					if (stage != 1u)
						out |= S13B_RECV_END;
					stage = 2u;
				} else if (s13b_is(e, spi, "RXCTL") && !(e->val & 1u)) {
					if (stage != 2u)
						out |= S13B_RECV_END;
					stage = 3u;
				}
				continue;
			}
			if (handed) {					/* a part runs */
				if (s13b_is(e, spi, "CTL") || s13b_is(e, spi, "RXCTL") ||
				    strcmp(e->reg, "PORTA_FER_CLR") == 0 || s13b_is(e, dma, "CFG") ||
				    s13b_is(e, dma, "DSCPTR_CUR"))
					out |= S13B_RECV_PART;
				continue;
			}
			if (s13b_is(e, spi, "CTL")) {
				if ((e->val & 1u) && !en) {
					en = true;
					if (ns != 0u && ns < 64u)
						setup[ns++] = "en";
				} else if (!(e->val & 1u)) {
					en = false;
				}
			} else if (s13b_is(e, dma, "CFG")) {
				if (ns < 64u)
					setup[ns++] = (e->val & 1u) ? "cfg" : "off";
			} else if (s13b_is(e, spi, "IMSK_CLR") && (e->val & S13B_RF)) {
				if (ns < 64u)
					setup[ns++] = "mask";
			} else if (s13b_is(e, dma, "DSCPTR_CUR")) {
				if (ns < 64u)
					setup[ns++] = "dscptr";
			} else if (s13b_is(e, spi, "RWC")) {
				out |= S13B_RECV_RXCTL;
			} else if (s13b_is(e, spi, "RXCTL")) {
				if (ns < 64u)
					setup[ns++] = "rxctl";
				if (e->val != S13B_SLAVE_RXCTL)
					out |= S13B_RECV_RXCTL;
			} else if (strcmp(e->reg, "PORTA_FER_SET") == 0 && (e->val & S13B_PA4)) {
				unsigned k, at = 0;
				bool bad = false, has_en = false;

				handed = true;
				running = true;
				if (!en)
					out |= S13B_DMA_ORDER;
				for (k = 0; k < ns; k++) {
					if (strcmp(setup[k], "en") == 0) {
						has_en = true;
						continue;
					}
					if (at >= 5u || strcmp(setup[k], order[at]) != 0)
						bad = true;
					at++;
				}
				if (at != 5u || (has_en && strcmp(setup[ns - 1u], "en") != 0))
					bad = true;
				if (bad)
					out |= S13B_DMA_ORDER;
			}
		}
		if (stage != 3u)
			out |= S13B_RECV_END;
		return out;
	}
}

/* check_master_sessions's events. */
enum s13b_sev_kind { S13B_EV_EN, S13B_EV_SELECT, S13B_EV_WORD, S13B_EV_SETUP };
struct s13b_sev {
	enum s13b_sev_kind ev;
	bool on;
	int miom;		/* EN: the MIOM the write gives, -1 when it leaves it */
};

static inline unsigned s13b_master_sessions(const struct s13b_sev *t, size_t n)
{
	unsigned out = 0, sessions = 0, words_here = 0;
	bool en = true, cleared = false, set_after = false, in_session = false;
	bool cleared_here = false, setup_after_en = false;
	int miom = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		const struct s13b_sev *e = &t[i];

		switch (e->ev) {
		case S13B_EV_SETUP:
			if (en && !(in_session && words_here))
				setup_after_en = true;
			break;
		case S13B_EV_EN:
			if (e->miom >= 0 && e->miom != miom) {
				if (en || e->on)
					out |= S13B_MIOM_WHILE_ENABLED;
				miom = e->miom;
			}
			if (e->on && !en)
				setup_after_en = false;
			en = e->on;
			if (!e->on) {
				cleared = true;
				set_after = false;
				if (in_session && words_here)
					cleared_here = true;
			} else if (cleared) {
				set_after = true;
			}
			break;
		case S13B_EV_SELECT:
			in_session = e->on;
			if (e->on) {
				words_here = 0;
				cleared_here = false;
			}
			break;
		case S13B_EV_WORD:
		default:
			if (!en)
				out |= S13B_SPI0_WORD_DISABLED;
			if (cleared_here) {
				out |= S13B_SPI0_RESET_IN_SESSION;
				cleared_here = false;
			}
			if (words_here == 0u) {
				if (setup_after_en)
					out |= S13B_ENABLE_LAST;
				if (sessions && !(cleared && set_after))
					out |= S13B_SPI0_NOT_RESET;
				sessions++;
			}
			cleared = false;
			set_after = false;
			words_here++;
			break;
		}
	}
	return out;
}

/* check_reply_setup's and check_slave_tx's rules: masks of their own. */
enum {
	S13B_REPLY_CTL = 1u << 0,
	S13B_REPLY_TXCTL = 1u << 1,
	S13B_REPLY_RX_QUAD = 1u << 2,
	S13B_REPLY_RX_ON = 1u << 3,
	S13B_REPLY_RX_OFF = 1u << 4,
	S13B_REPLY_NO_PAD = 1u << 5,
	S13B_REPLY_TWC = 1u << 6,
	S13B_REPLY_ENABLE_LAST = 1u << 7,
	S13B_REPLY_ORDER = 1u << 8,
	S13B_REPLY_TF_UNMASK = 1u << 9,
};
enum {
	S13B_TX_END_EARLY = 1u << 0,
	S13B_TX_END_MISSING = 1u << 1,
	S13B_TX_END_NO_COUNT = 1u << 2,
	S13B_TX_END_LATE = 1u << 3,
	S13B_TX_NO_STOP = 1u << 4,
	S13B_TX_ERROR_IGNORED = 1u << 5,
	S13B_TX_QUAD_RX_CHANNEL = 1u << 6,
};

/* check_reply_setup's `order` words. */
enum s13b_step {
	S13B_ST_STAT, S13B_ST_FEED, S13B_ST_TWC, S13B_ST_TXCTL, S13B_ST_RXCTL, S13B_ST_EN,
	S13B_ST_DMA, S13B_ST_DMA_DONE, S13B_ST_TF_UNMASK,
};
enum s13b_link { S13B_LINK_PRE, S13B_LINK_POST, S13B_LINK_QUAD };

/* check_reply_setup's `s`; tf_end is `end` = 'tf'; order NULL when it has
 * none. */
struct s13b_reply_setup {
	enum s13b_link link;
	uint32_t ctl, txctl, rxctl, twc, words, queued;
	bool tf_end;
	const enum s13b_step *order;
	size_t norder;
};

#define S13B_REPLY_TXCTL_VAL 0x0001011Du
#define S13B_REPLY_QUIET_US 500u
#define S13B_TX_END_BOUND_US 1000u

/* check_spi's 'bridge_slave_reply_' + link rules on SPI2_CTL (decode_spi). */
static inline bool s13b_reply_ctl_ok(uint32_t ctl, bool quad)
{
	return (ctl & 1u) && !(ctl >> 1 & 1u) && (ctl >> 9 & 3u) == 2u && !(ctl >> 12 & 1u) &&
	       (ctl >> 20 & 3u) == (quad ? 2u : 0u) && !(ctl >> 5 & 1u) && (ctl >> 4 & 1u) &&
	       (ctl >> 13 & 1u) && (ctl >> 14 & 1u) && (ctl >> 16 & 3u) == 0u && !(ctl >> 15 & 1u);
}

/* The first place of step k in order, or -1. */
static inline long s13b_step_at(const enum s13b_step *order, size_t n, enum s13b_step k)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (order[i] == k)
			return (long)i;
	return -1;
}

static inline unsigned s13b_reply_setup(const struct s13b_reply_setup *s)
{
	const bool quad = s->link == S13B_LINK_QUAD;
	const bool tf_end = !quad && s->tf_end;
	unsigned out = 0;

	if (!s13b_reply_ctl_ok(s->ctl, quad))
		out |= S13B_REPLY_CTL;
	if (s->txctl != S13B_REPLY_TXCTL_VAL)
		out |= S13B_REPLY_TXCTL;
	if (quad && s->rxctl)
		out |= S13B_REPLY_RX_QUAD;
	if (tf_end && (s->rxctl & 1u))
		out |= S13B_REPLY_RX_ON;
	if (!quad && !tf_end && !(s->rxctl & 1u))
		out |= S13B_REPLY_RX_OFF;
	if (s->queued != 2u)
		out |= S13B_REPLY_NO_PAD;
	if (s->twc != s->words)
		out |= S13B_REPLY_TWC;
	if ((quad || tf_end) && s->order != NULL) {
		enum s13b_step setup[64];
		size_t ns = 0, i, en_at = SIZE_MAX, nr = 0;
		enum s13b_step rest[64];
		long tf, done;

		for (i = 0; i < s->norder && ns < 64u; i++) {
			const enum s13b_step k = s->order[i] == S13B_ST_DMA ? S13B_ST_FEED : s->order[i];

			if (k <= S13B_ST_EN)
				setup[ns++] = k;
		}
		for (i = 0; i < ns; i++)
			if (setup[i] == S13B_ST_EN) {
				en_at = i;
				break;
			}
		if (en_at == SIZE_MAX || en_at != ns - 1u)
			out |= S13B_REPLY_ENABLE_LAST;
		for (i = 0; i < ns; i++)
			if (setup[i] != S13B_ST_EN)
				rest[nr++] = setup[i];
		/* REPLY_ORDER kept to those in rest, once each, must be rest */
		{
			enum s13b_step want[64] = { S13B_ST_STAT };
			size_t nw = 0, k;

			for (k = S13B_ST_STAT; k < S13B_ST_EN; k++)
				for (i = 0; i < nr; i++)
					if (rest[i] == (enum s13b_step)k) {
						want[nw++] = (enum s13b_step)k;
						break;
					}
			if (nw != nr || memcmp(want, rest, nr * sizeof rest[0]) != 0)
				out |= S13B_REPLY_ORDER;
		}
		tf = s13b_step_at(s->order, s->norder, S13B_ST_TF_UNMASK);
		done = s13b_step_at(s->order, s->norder, S13B_ST_DMA_DONE);
		if (s13b_step_at(s->order, s->norder, S13B_ST_DMA) >= 0 && tf >= 0 &&
		    (done < 0 || tf < done))
			out |= S13B_REPLY_TF_UNMASK;
	}
	return out;
}

/* check_slave_tx's events; a status's flags; rx_words < 0 and t_us < 0
 * stand for a key left out. */
enum s13b_txev {
	S13B_TX_HAND, S13B_TX_TAKE, S13B_TX_STOP, S13B_TX_PB15_HIGH, S13B_TX_START, S13B_TX_END,
	S13B_TX_STATUS, S13B_TX_DMA_DONE,
};
#define S13B_TXF_TF          (1u << 0)
#define S13B_TXF_TUR         (1u << 1)
#define S13B_TXF_TC          (1u << 2)
#define S13B_TXF_ROR         (1u << 3)
#define S13B_TXF_TWC_ZERO    (1u << 4)
#define S13B_TXF_TFIFO_EMPTY (1u << 5)
#define S13B_TXF_SPIF        (1u << 6)
struct s13b_txe {
	enum s13b_txev ev;
	uint32_t word;
	unsigned flags;
	int64_t rx_words;
	uint32_t quiet_us;
	int64_t t_us;
};

/* check_slave_tx(words, trace, io, end): quad for io 'quad', tf for end 'tf'. */
static inline unsigned s13b_slave_tx(uint32_t words, const struct s13b_txe *t, size_t n, bool quad,
				     bool tf)
{
	const bool tf_end = quad || tf;
	bool handed = false, failed = false, last_done = false, taken = false, counted = false;
	bool stopped = false, master_gone = false, ror = false;
	long err_n = -1;
	int64_t cur = -1, last_t = -1;
	unsigned out = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		const struct s13b_txe *e = &t[i];

		switch (e->ev) {
		case S13B_TX_HAND:
			handed = true;
			taken = counted = stopped = ror = false;
			break;
		case S13B_TX_PB15_HIGH:
			master_gone = true;
			break;
		case S13B_TX_START:
		case S13B_TX_END:
			if (failed && handed && !taken && err_n >= 0 &&
			    (e->ev == S13B_TX_START || (int64_t)e->word != cur))
				out |= S13B_TX_ERROR_IGNORED;
			if (e->ev == S13B_TX_START) {
				cur = e->word;
			} else if (e->word == words - 1u) {
				last_done = true;
				last_t = e->t_us;
			}
			break;
		case S13B_TX_STATUS: {
			bool quiet_end, finish, count;

			if (!handed || taken)
				break;
			ror = ror || (e->flags & S13B_TXF_ROR);
			quiet_end = !tf_end && ror && (e->flags & S13B_TXF_TWC_ZERO) &&
				    e->quiet_us >= S13B_REPLY_QUIET_US;
			count = e->rx_words >= 0 && (uint64_t)e->rx_words == words;
			finish = (e->flags & S13B_TXF_TF) || count || quiet_end;
			if (quad && e->rx_words >= 0)
				out |= S13B_TX_QUAD_RX_CHANNEL;
			if (tf_end ? (e->flags & S13B_TXF_TF) != 0u : count || quiet_end)
				counted = true;
			if ((e->flags & (S13B_TXF_TUR | S13B_TXF_TC)) && !last_done && !failed) {
				failed = true;
				err_n = (long)i;
			} else if (finish && failed && err_n >= 0) {
				out |= S13B_TX_ERROR_IGNORED;
			}
			break;
		}
		case S13B_TX_TAKE:
		case S13B_TX_STOP:
			stopped = stopped || e->ev == S13B_TX_STOP;
			if (!failed && !last_done && !master_gone)
				out |= S13B_TX_END_EARLY;
			if (e->ev == S13B_TX_TAKE) {
				if (!failed && last_done && !taken) {
					if (!counted)
						out |= S13B_TX_END_NO_COUNT;
					if (last_t >= 0 && e->t_us >= 0 &&
					    e->t_us - last_t > (int64_t)S13B_TX_END_BOUND_US)
						out |= S13B_TX_END_LATE;
				}
				if (failed && handed && !taken && last_done && err_n >= 0)
					out |= S13B_TX_ERROR_IGNORED;
				taken = true;
				err_n = -1;
			}
			break;
		case S13B_TX_DMA_DONE:
		default:
			break;
		}
	}
	if (taken && last_done && !failed && !stopped)
		out |= S13B_TX_NO_STOP;
	if (handed && !taken) {
		if (failed)
			out |= S13B_TX_ERROR_IGNORED;
		else if (last_done)
			out |= S13B_TX_END_MISSING;
	}
	return out;
}

#endif /* WAFFLE_TESTS_S13B_CHECK_H */
