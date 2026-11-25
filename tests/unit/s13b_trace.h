/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WAFFLE_TESTS_S13B_TRACE_H
#define WAFFLE_TESTS_S13B_TRACE_H

/*
 * The driver's bridge sessions as the checkers of s13b_check.h take them
 * the register accesses a fake MMR hook sees, by check.py's
 * names, with the descriptor array each DMA_DSCPTR_CUR write gives (the
 * driver's wb_spi_tx_array or wb_spi_rx_array as it is then). With the
 * environment's WB_S13B_TRACES naming a directory, s13b_dump() also writes
 * each trace there as check.py's JSON, for
 * tests/oracle/S-13b-buses-and-pins/check.py's own check_bridge_dma.
 */

#include <stdio.h>
#include <stdlib.h>

#include "s13b_check.h"
#include "support/fake_mmr.h"
#include "waffle/drivers/bf70x/dma.h"
#include "waffle/drivers/bf70x/port.h"
#include "waffle/drivers/bf70x/spi.h"

#define S13B_TRACE_MAX 65536u

struct s13b_trace {
	struct s13b_ev e[S13B_TRACE_MAX];
	size_t n;
	bool on;
};

/* A register's name as check.py has it, or NULL for one the checkers
 * do not name. */
static const char *s13b_reg_name(wb_mmr_addr a)
{
	static char buf[8][24];
	static unsigned next;
	static const unsigned spis[] = { 0u, 2u };
	static const unsigned chans[] = { 4u, 5u, 8u, 9u };
	char *b = buf[next++ % 8u];
	unsigned i, k;

	if (a == PORT_FER_SET(WB_PORT_A))
		return "PORTA_FER_SET";
	if (a == PORT_FER_CLR(WB_PORT_A))
		return "PORTA_FER_CLR";
	for (i = 0; i < 2u; i++) {
		const unsigned n = spis[i];
		const struct {
			wb_mmr_addr at;
			const char *name;
		} r[] = {
			{ SPI_CTL(n), "CTL" }, { SPI_RXCTL(n), "RXCTL" }, { SPI_TXCTL(n), "TXCTL" },
			{ SPI_CLK(n), "CLK" }, { SPI_RWC(n), "RWC" }, { SPI_TWC(n), "TWC" },
			{ SPI_IMSK_CLR(n), "IMSK_CLR" }, { SPI_IMSK_SET(n), "IMSK_SET" },
			{ SPI_STAT(n), "STAT" }, { SPI_ILAT(n), "ILAT" }, { SPI_ILAT_CLR(n), "ILAT_CLR" },
			{ SPI_RFIFO(n), "RFIFO" }, { SPI_TFIFO(n), "TFIFO" },
		};

		for (k = 0; k < sizeof r / sizeof r[0]; k++)
			if (r[k].at == a) {
				snprintf(b, 24, "SPI%u_%s", n, r[k].name);
				return b;
			}
	}
	for (i = 0; i < 4u; i++) {
		const unsigned ch = chans[i];
		const struct {
			wb_mmr_addr at;
			const char *name;
		} r[] = {
			{ DMA_CFG(ch), "CFG" }, { DMA_STAT(ch), "STAT" },
			{ DMA_DSCPTR_CUR(ch), "DSCPTR_CUR" },
		};

		for (k = 0; k < sizeof r / sizeof r[0]; k++)
			if (r[k].at == a) {
				snprintf(b, 24, "DMA%u_%s", ch, r[k].name);
				return b;
			}
	}
	return NULL;
}

/* The bytes of the array a channel is given now. */
static inline uint64_t s13b_array_bytes(unsigned ch)
{
	const struct wb_spi_array *a = ch == 4u || ch == 8u ? &wb_spi_tx_array : &wb_spi_rx_array;
	uint64_t sum = 0;
	unsigned i;

	for (i = 0; i < a->n; i++)
		sum += a->bytes[i];
	return sum;
}

static inline void s13b_push(struct s13b_trace *t, struct s13b_ev e)
{
	if (t->on && t->n < S13B_TRACE_MAX)
		t->e[t->n++] = e;
}

/* From a fake MMR hook, once the access's value is known. Names are kept
 * as string literals the trace can hold. */
static inline void s13b_access(struct s13b_trace *t, enum fake_mmr_op op, wb_mmr_addr a, uint32_t v)
{
	static const char *const keep[] = {
		"SPI0_CTL", "SPI0_RXCTL", "SPI0_TXCTL", "SPI0_CLK", "SPI0_RWC", "SPI0_TWC",
		"SPI0_IMSK_CLR", "SPI0_IMSK_SET", "SPI0_STAT", "SPI0_ILAT", "SPI0_ILAT_CLR",
		"SPI0_RFIFO", "SPI0_TFIFO",
		"SPI2_CTL", "SPI2_RXCTL", "SPI2_TXCTL", "SPI2_CLK", "SPI2_RWC", "SPI2_TWC",
		"SPI2_IMSK_CLR", "SPI2_IMSK_SET", "SPI2_STAT", "SPI2_ILAT", "SPI2_ILAT_CLR",
		"SPI2_RFIFO", "SPI2_TFIFO",
		"DMA4_CFG", "DMA4_STAT", "DMA4_DSCPTR_CUR", "DMA5_CFG", "DMA5_STAT", "DMA5_DSCPTR_CUR",
		"DMA8_CFG", "DMA8_STAT", "DMA8_DSCPTR_CUR", "DMA9_CFG", "DMA9_STAT", "DMA9_DSCPTR_CUR",
		"PORTA_FER_SET", "PORTA_FER_CLR",
	};
	const char *name;
	unsigned i;

	if (!t->on || (op != FAKE_MMR_READ && op != FAKE_MMR_WRITE))
		return;
	name = s13b_reg_name(a);
	if (name == NULL)
		return;
	for (i = 0; i < sizeof keep / sizeof keep[0]; i++)
		if (strcmp(keep[i], name) == 0)
			break;
	if (i == sizeof keep / sizeof keep[0])
		return;
	if (op == FAKE_MMR_WRITE && strstr(name, "DSCPTR_CUR") != NULL) {
		const unsigned ch = (unsigned)strtoul(name + 3, NULL, 10);

		s13b_push(t, (struct s13b_ev){ S13B_DESC, NULL, 0u, ch, s13b_array_bytes(ch), false });
	}
	/* a read the same as the event before it adds nothing the checkers
	 * use: a status polled */
	if (op == FAKE_MMR_READ && t->n != 0u && t->e[t->n - 1u].op == S13B_R &&
	    t->e[t->n - 1u].reg == keep[i] && t->e[t->n - 1u].val == v)
		return;
	if (op == FAKE_MMR_READ && strstr(name, "DSCPTR_CUR") != NULL)
		return;
	s13b_push(t, (struct s13b_ev){ op == FAKE_MMR_WRITE ? S13B_W : S13B_R, keep[i], v, 0u, 0u,
				       false });
}

static inline void s13b_select(struct s13b_trace *t, bool on)
{
	s13b_push(t, (struct s13b_ev){ S13B_SELECT, NULL, 0u, 0u, 0u, on });
}

static inline void s13b_start(struct s13b_trace *t)
{
	t->n = 0;
	t->on = true;
}

/* The trace as check.py's JSON row, into $WB_S13B_TRACES/<name>.json. */
static inline void s13b_dump(const struct s13b_trace *t, const char *name, const char *kind,
		      uint32_t words, const uint32_t *parts, unsigned nparts)
{
	const char *dir = getenv("WB_S13B_TRACES");
	char path[512];
	FILE *f;
	size_t i;

	if (dir == NULL || *dir == '\0')
		return;
	snprintf(path, sizeof path, "%s/%s.json", dir, name);
	f = fopen(path, "w");
	if (f == NULL)
		return;
	fprintf(f, "{\"name\": \"%s\", \"kind\": \"%s\", \"words\": %lu, \"parts\": [", name, kind,
		(unsigned long)words);
	for (i = 0; i < nparts; i++)
		fprintf(f, "%s%lu", i ? ", " : "", (unsigned long)parts[i]);
	fprintf(f, "], \"trace\": [\n");
	for (i = 0; i < t->n; i++) {
		const struct s13b_ev *e = &t->e[i];

		if (e->op == S13B_DESC)
			fprintf(f, " {\"op\": \"ev\", \"ev\": \"desc\", \"ch\": %u, \"bytes\": [%llu]}", e->ch,
				(unsigned long long)e->bytes);
		else if (e->op == S13B_SELECT)
			fprintf(f, " {\"op\": \"ev\", \"ev\": \"select\", \"on\": %s}", e->on ? "true" : "false");
		else
			fprintf(f, " {\"op\": \"%s\", \"reg\": \"%s\", \"val\": %lu}", e->op == S13B_W ? "w" : "r",
				e->reg, (unsigned long)e->val);
		fprintf(f, "%s\n", i + 1u < t->n ? "," : "");
	}
	fprintf(f, "]}\n");
	fclose(f);
}

/* $WB_S13B_TRACES/<name>.<what>.json, or NULL. */
static inline FILE *s13b_dump_open(const char *name, const char *what)
{
	const char *dir = getenv("WB_S13B_TRACES");
	char path[512];

	if (dir == NULL || *dir == '\0')
		return NULL;
	snprintf(path, sizeof path, "%s/%s.%s.json", dir, name, what);
	return fopen(path, "w");
}

/* A reply's set-up as check_reply_setup's link and `s`. */
static inline void s13b_dump_setup(const char *name, const struct s13b_reply_setup *s)
{
	static const char *const links[] = { "pre", "post", "quad" };
	static const char *const steps[] = { "stat", "feed", "twc", "txctl", "rxctl", "en",
					     "dma", "dma_done", "tf_unmask" };
	FILE *f = s13b_dump_open(name, "setup");
	size_t i;

	if (f == NULL)
		return;
	fprintf(f, "{\"name\": \"%s\", \"link\": \"%s\", \"setup\": {\"ctl\": %lu, \"txctl\": %lu, "
		"\"rxctl\": %lu, \"twc\": %lu, \"words\": %lu, \"queued\": %lu, \"end\": %s, \"order\": [",
		name, links[s->link], (unsigned long)s->ctl, (unsigned long)s->txctl,
		(unsigned long)s->rxctl, (unsigned long)s->twc, (unsigned long)s->words,
		(unsigned long)s->queued, s->tf_end ? "\"tf\"" : "null");
	for (i = 0; i < s->norder; i++)
		fprintf(f, "%s\"%s\"", i ? ", " : "", steps[s->order[i]]);
	fprintf(f, "]}}\n");
	fclose(f);
}

/* A reply's events as check_slave_tx's words, trace, io and end. */
static inline void s13b_dump_tx(const char *name, uint32_t words, const struct s13b_txe *t, size_t n,
				bool quad, bool tf)
{
	static const char *const evs[] = { "hand", "take", "stop", "pb15_high", "start", "end",
					   "status", "dma_done" };
	static const char *const flags[] = { "tf", "tur", "tc", "ror", "twc_zero", "tfifo_empty",
					     "spif" };
	FILE *f = s13b_dump_open(name, "tx");
	size_t i, k;

	if (f == NULL)
		return;
	fprintf(f, "{\"name\": \"%s\", \"words\": %lu, \"io\": \"%s\", \"end\": %s, \"trace\": [\n",
		name, (unsigned long)words, quad ? "quad" : "single", tf ? "\"tf\"" : "null");
	for (i = 0; i < n; i++) {
		const struct s13b_txe *e = &t[i];

		fprintf(f, " {\"ev\": \"%s\"", evs[e->ev]);
		if (e->ev == S13B_TX_START || e->ev == S13B_TX_END)
			fprintf(f, ", \"word\": %lu", (unsigned long)e->word);
		if (e->ev == S13B_TX_STATUS) {
			for (k = 0; k < sizeof flags / sizeof flags[0]; k++)
				if (e->flags & (1u << k))
					fprintf(f, ", \"%s\": true", flags[k]);
			if (e->rx_words >= 0)
				fprintf(f, ", \"rx_words\": %lld", (long long)e->rx_words);
			fprintf(f, ", \"quiet_us\": %lu", (unsigned long)e->quiet_us);
		}
		if (e->t_us >= 0)
			fprintf(f, ", \"t_us\": %lld", (long long)e->t_us);
		fprintf(f, "}%s\n", i + 1u < n ? "," : "");
	}
	fprintf(f, "]}\n");
	fclose(f);
}

/* check_bridge_dma on the trace, printing what it finds; the trace dumped. */
static inline bool s13b_ok(const struct s13b_trace *t, const char *name, enum s13b_kind kind,
		    uint32_t words, const uint32_t *parts, unsigned nparts)
{
	static const char *const kinds[] = { "master_send", "master_receive", "slave_receive",
					     "slave_reply" };
	const unsigned got = s13b_bridge_dma(kind, t->e, t->n, words, parts, nparts);

	s13b_dump(t, name, kinds[kind], words, parts, nparts);
	if (got == 0u)
		return true;
	printf("  %s: check_bridge_dma rules 0x%x over %zu events\n", name, got, t->n);
	return false;
}

#endif /* WAFFLE_TESTS_S13B_TRACE_H */
