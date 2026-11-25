/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-14 log: threshold, sinks, the configuration request, relayed lines and
 * the required lines of S-14.060.
 */
#include "waffle/log/log.h"

/* S-14.011, S-14.022, S-14.024, S-14.026 */
void s14_log_init(struct s14_log *log, enum s14_core core,
		  const struct s14_clock_ops *clock,
		  const struct s14_link_ops *psoc,
		  const struct s14_link_ops *cdc,
		  const struct s14_screen_ops *screen)
{
	log->clock = clock;
	log->psoc = psoc;
	log->cdc = cdc;
	log->screen = screen;
	log->threshold = S14_DEFAULT_THRESHOLD;   /* held in RAM only */
	log->psoc_on = true;
	log->cdc_on = false;
	log->screen_started = false;
	log->silent = core == S14_SLAVE;
}

/* S-14.025 */
void s14_log_screen_start(struct s14_log *log)
{
	log->screen_started = true;
}

/* S-14.012, S-14.013, S-14.014 */
void s14_log_config(struct s14_log *log, const uint8_t *payload, size_t len)
{
	if (len == 0u)
		return;
	if (payload[0] >= 0xa0u && payload[0] <= 0xa3u)
		log->threshold = (uint8_t)(payload[0] - 0xa0u + 1u);
	if (len >= 2u) {
		log->psoc_on = (payload[1] & 0x01u) != 0u;
		log->cdc_on = (payload[1] & 0x02u) != 0u;
	}
}

/* S-14.011 */
bool s14_log_passes(const struct s14_log *log, enum s14_level level)
{
	return (unsigned)level <= log->threshold;
}

/* S-14.021, S-14.023: id 0x04 for level 1 only, level 0 included in 0x03. */
static uint8_t frame_id(enum s14_level level)
{
	return (uint8_t)(level == S14_ERROR ? S14_ID_LOG_ERROR : S14_ID_LOG);
}

/*
 * S-14.020 - S-14.029. The line is offered once to each sink and then
 * forgotten: a sink that does not take it never sees it (S-14.027).
 */
void s14_log_emit(struct s14_log *log, enum s14_level level,
		  struct s14_tag tag, const uint8_t *text, size_t len)
{
	uint8_t line[S14_LINE_MAX];
	size_t n;
	uint32_t now;

	if (log->silent)                      /* S-14.026 */
		return;
	if (!s14_log_passes(log, level))      /* S-14.011, S-16.171 */
		return;

	now = log->clock != NULL ? log->clock->now_us(log->clock->ctx) : 0u;
	n = s14_line_build(line, now, tag, text, len);

	/* S-14.029: one line reaches every sink before the next is built. */
	if (log->psoc_on && log->psoc != NULL)          /* S-14.021, S-14.022 */
		(void)log->psoc->send(log->psoc->ctx, S14_ROUTE_PSOC,
				      frame_id(level), line, n);
	if (log->cdc_on && log->cdc != NULL)            /* S-14.023, S-14.024 */
		(void)log->cdc->send(log->cdc->ctx, S14_ROUTE_CDC,
				     frame_id(level), line, n);
	/* S-14.025: level 0 only, the body without the timestamp. */
	if (level == S14_FATAL && log->screen_started && log->screen != NULL)
		log->screen->show(log->screen->ctx, line + 4, n - 4u);
}

void s14_log_str(struct s14_log *log, enum s14_level level,
		 struct s14_tag tag, const char *text)
{
	size_t n = 0;

	while (text[n] != '\0')
		n++;
	s14_log_emit(log, level, tag, (const uint8_t *)text, n);
}

void s14_log_text(struct s14_log *log, enum s14_level level,
		  struct s14_tag tag, const struct s14_text *text)
{
	/* A text that overflowed is longer than any line, so the line is cut
	 * (S-14.006) whatever the dropped bytes were; pass all kept bytes. */
	s14_log_emit(log, level, tag, text->buf, text->len);
}

/* S-14.040, S-14.041, S-14.042, S-14.008 */
bool s14_log_relay(struct s14_log *log, uint8_t src, uint8_t id,
		   const uint8_t *payload, size_t len)
{
	size_t end;

	if (id != S14_ID_LOG && id != S14_ID_LOG_ERROR)
		return false;
	if (src == S14_ADDR_PSOC)             /* S-14.041: no line, no reply */
		return true;
	if (len < 4u)                         /* S-14.042 */
		return true;
	/* Bytes 4 .. the first 0x00, the end, or payload byte 248 (excluded);
	 * s14_line_build stops at the 0x00. */
	/* S-14.040, S-14.005: the line is tagged, so a text that with the
	 * stamp and the tag passes 248 bytes is cut with ".." (S-14.006;
	 * L-RELAY, L-LONG). */
	end = len < S14_LINE_MAX ? len : S14_LINE_MAX;
	s14_log_emit(log,
		     (enum s14_level)(id == S14_ID_LOG ? S14_INFO : S14_ERROR),
		     S14_TAG("relay"), payload + 4, end - 4u);
	return true;
}

/* S-14.060, S-16.020: the frame's own route, sequence and id, as values. */
void s14_log_bad_crc(struct s14_log *log, uint8_t route, uint8_t seq,
		     uint8_t id)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, "frame crc error: route 0x");
	s14_text_hex(&t, route, 2);
	s14_text_str(&t, " seq 0x");
	s14_text_hex(&t, seq, 2);
	s14_text_str(&t, " id 0x");
	s14_text_hex(&t, id, 2);
	s14_log_text(log, S14_ERROR, S14_TAG("s06"), &t);
}

/* S-14.060, S-14.010, S-16.007: level 0, before any other fault handling. */
void s14_log_double_fault(struct s14_log *log)
{
	s14_log_str(log, S14_FATAL, S14_TAG("fault"),
		    "unrecoverable event: double fault");
}

/* S-14.060, S-14.008, S-16.099: the coprocessor's text is data. */
void s14_log_dfu_error(struct s14_log *log, const char *who,
		       const uint8_t *text, size_t len)
{
	struct s14_text t;

	s14_text_init(&t);
	s14_text_str(&t, who);
	s14_text_str(&t, " dfu error: ");
	s14_text_bytes(&t, text, len);
	s14_log_text(log, S14_ERROR, S14_TAG("dfu"), &t);
}
