/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-04 persistent records: the reboot record, its reasons and writers, the
 * CDC request file, the metadata record, the PSoC status reports, the RTC
 * encoding and the fatal-halt record.
 */
#include "waffle/s04_records.h"

#include "waffle/bytes.h"
#include "waffle/s10c_host.h"

/* --- The reboot record --- */

/* S-04.002 */
bool s04_reboot_present(const uint8_t rec[S04_REBOOT_SIZE])
{
	return wb_get_le32(rec) == S04_REBOOT_MAGIC;
}

/* S-04.003 */
void s04_reboot_write(uint8_t rec[S04_REBOOT_SIZE], uint16_t reason)
{
	wb_put_le32(rec, S04_REBOOT_MAGIC);
	wb_put_le16(rec + 4, reason);
}

/* S-04.004, S-04.005 */
uint16_t s04_reboot_consume(uint8_t rec[S04_REBOOT_SIZE])
{
	if (!s04_reboot_present(rec))
		return S04_REASON_DEFAULT;	/* and nothing is written */
	wb_put_le32(rec, 0u);
	return wb_get_le16(rec + 4);
}

/* S-04.006 to S-04.011; S-04.030: the values the OS writes keep these meanings. */
void s04_reason_effect(uint16_t reason, struct s04_reason_effect *out)
{
	out->boot_os = true;
	out->screen = S04_SCREEN_MAIN;
	out->cdc = false;
	out->transport = S04_TRANSPORT_NONE;
	switch (reason) {
	case S04_REASON_CDC:		/* S-04.007 */
		out->boot_os = false;
		out->cdc = true;
		break;
	case S04_REASON_MSC:		/* S-04.008 */
		out->boot_os = false;
		out->screen = S04_SCREEN_UPGRADE;
		out->transport = S04_TRANSPORT_MSC;
		break;
	case S04_REASON_MIDI:		/* S-04.009 */
		out->boot_os = false;
		out->screen = S04_SCREEN_UPGRADE;
		out->transport = S04_TRANSPORT_MIDI;
		break;
	case S04_REASON_STAY:		/* S-04.010, S-16.105 */
		out->boot_os = false;
		break;
	default:			/* S-04.011: 0, 20, 50 and every other value */
		break;
	}
}

/* S-04.021, S-16.106 */
uint16_t s04_host_mode_reason(const uint8_t *payload, size_t len)
{
	if (len == 0u)
		return S04_REASON_MIDI;
	switch (payload[0]) {
	case 1u:
		return S04_REASON_MSC;
	case 2u:
		return S04_REASON_CDC;
	case 4u:
		return S04_REASON_BOOT50;
	default:
		return S04_REASON_MIDI;
	}
}

/* --- Writers --- */

/* S-04.027 */
enum s04_nordic s04_nordic_step(uint16_t reason)
{
	if (reason == S04_REASON_DEFAULT)
		return S04_NORDIC_EXIT_DFU;
	if (reason == S04_REASON_BOOT)
		return S04_NORDIC_NONE;
	return S04_NORDIC_ENTER_DFU;
}

/* S-04.020 (writers.tsv) */
void s04_writer_rule(enum s04_writer w, uint16_t arg, struct s04_writer_rule *out)
{
	out->reason = S04_REASON_DEFAULT;
	out->reset = S04_RESET_HARD;
	out->nordic = S04_NORDIC_NONE;
	out->shutdown = true;
	out->psoc_reset_request = false;
	out->min_delay_ms = 0;
	switch (w) {
	case S04_W1:		/* host reboot with a mode byte (S-04.021) */
		out->reason = arg;
		out->nordic = s04_nordic_step(arg);
		break;
	case S04_W2:		/* host reboot to the OS (S-04.022) */
		out->reset = S04_RESET_SOFT;
		out->nordic = S04_NORDIC_EXIT_DFU;
		out->psoc_reset_request = true;
		break;
	case S04_W3:
		/*
		 * The MIDI transfer's sub-command 05 (S-04.023): the reply goes
		 * first (S-10c.091), and the reset comes at least 10 ms after it.
		 */
		out->nordic = S04_NORDIC_EXIT_DFU;
		out->min_delay_ms = 10u;
		break;
	case S04_W4:		/* the CDC request file (S-04.024) */
		out->reason = S04_REASON_CDC;
		out->nordic = s04_nordic_step(S04_REASON_CDC);
		break;
	case S04_W6:		/* CPU-link power-off in power state 0x16 (S-16.140) */
		out->nordic = S04_NORDIC_EXIT_DFU;
		break;
	case S04_W7:		/* the factory screen's reset key (S-04.026) */
		out->reason = arg;
		break;
	case S04_W10:		/* a developer's calibrated start (S-04.012) */
		out->reason = S04_REASON_OS_FRAME_CAL;
		break;
	case S04_W11:		/* Continue to the OS (S-04.013, S-05.033) */
		out->reason = S04_REASON_OS_ONCE;
		out->nordic = S04_NORDIC_EXIT_DFU;
		break;
	case S04_W12:		/* Start /os.ldr once (S-04.014, S-15.194) */
		out->reason = S04_REASON_OS_FILE;
		out->nordic = S04_NORDIC_EXIT_DFU;
		break;
	case S04_W9:		/* the slave's power-off (S-04.025) */
		out->reason = S04_REASON_BOOT;
		out->shutdown = false;
		break;
	default:
		break;
	}
}

/* S-04.022, S-04.023, S-04.027, S-04.028 */
void s04_reboot(const struct s04_reboot_ops *ops, enum s04_writer w, uint16_t arg)
{
	struct s04_writer_rule r;

	s04_writer_rule(w, arg, &r);
	if (r.psoc_reset_request)
		ops->psoc_reset_request(ops->ctx);
	if (r.shutdown)
		ops->shutdown(ops->ctx);
	if (r.nordic == S04_NORDIC_ENTER_DFU)
		ops->nordic_enter_dfu(ops->ctx);
	else if (r.nordic == S04_NORDIC_EXIT_DFU)
		ops->nordic_exit_dfu(ops->ctx);	/* S-08.048 may leave it out */
	if (w == S04_W6)	/* S-04.027: nothing more on UART1 after the exit */
		ops->nordic_link_stop(ops->ctx);
	if (r.min_delay_ms != 0u)
		ops->delay_ms(ops->ctx, r.min_delay_ms);
	/*
	 * S-13b.072: the links' last bytes leave before the watchdog is armed;
	 * before the record, since nothing goes out on a link after it.
	 * S-04.028: the record, then the watchdog; nothing in between.
	 */
	if (ops->drain_links != NULL)
		ops->drain_links(ops->ctx);
	ops->write_record(ops->ctx, r.reason);
	if (r.reset == S04_RESET_SOFT)
		ops->wait_for_psoc(ops->ctx);	/* S-04.022 step 5, S-04.029 */
	else
		ops->hard_reset(ops->ctx);
}

/* --- S-04.024 the CDC request file --- */

static bool contains(const char *s, const char *sub)
{
	size_t i, j;

	for (i = 0; s[i] != '\0'; i++) {
		for (j = 0; sub[j] != '\0' && s[i + j] == sub[j]; j++)
			;
		if (sub[j] == '\0')
			return true;
	}
	return false;
}

void s04_cdc_scan_init(struct s04_cdc_scan *s)
{
	s->tfw_seen = false;
	s->request_seen = false;
}

void s04_cdc_scan_entry(struct s04_cdc_scan *s, const char *name, bool is_regular)
{
	/* A candidate: a regular file whose name does not begin with '.'. */
	if (!is_regular || name[0] == '.' || name[0] == '\0')
		return;
	if (contains(name, "tfw"))
		s->tfw_seen = true;
	if (contains(name, "cdc") && contains(name, "please"))
		s->request_seen = true;
}

bool s04_cdc_scan_result(const struct s04_cdc_scan *s)
{
	return s->request_seen && !s->tfw_seen;
}

/* --- The metadata record --- */

static void zero_meta(struct s04_meta *m)
{
	unsigned i;

	for (i = 0; i < sizeof m->serial; i++)
		m->serial[i] = '\0';
	m->sku = 0;
	m->factory = 0;
	for (i = 0; i < 4u; i++) {
		m->os_version[i] = 0;
		m->sw_version[i] = 0;
	}
	m->state = S04_STATE_IDLE;
	m->cv_gain = 0;
	m->cv_zero = 0;
	m->cv_pair2[0] = 0;
	m->cv_pair2[1] = 0;
}

uint32_t s04_meta_size(const struct wb_device *dev)
{
	return dev->mem.metadata_size;
}

/* S-04.040, S-04.041, S-04.042 */
void s04_meta_read(const struct wb_device *dev, const uint8_t *raw, struct s04_meta *m)
{
	unsigned i;

	zero_meta(m);
	if (wb_get_le32(raw) != S04_META_MAGIC)
		return;
	for (i = 0; i < 8u && raw[4u + i] != 0u; i++)
		m->serial[i] = (char)raw[4u + i];
	m->sku = wb_get_le16(raw + 0x0E);
	if (wb_has(dev, WB_F_FACTORY_WORD))
		m->factory = wb_get_le16(raw + 0x10);
	for (i = 0; i < 4u; i++) {
		m->os_version[i] = wb_get_le16(raw + 0x12 + 2u * i);
		m->sw_version[i] = wb_get_le16(raw + 0x1A + 2u * i);
	}
	m->state = wb_get_le32(raw + 0x24) == S04_STATE_UPGRADE ? S04_STATE_UPGRADE : S04_STATE_IDLE;
	/* +0x28 on: part of the record only with cv-calibration */
	if (wb_has(dev, WB_F_CV_CALIBRATION) && s04_meta_size(dev) >= S04_META_SIZE) {
		m->cv_gain = wb_get_le32(raw + 0x28);
		m->cv_zero = wb_get_le16(raw + 0x2C);
		m->cv_pair2[0] = wb_get_le16(raw + 0x2E);
		m->cv_pair2[1] = wb_get_le16(raw + 0x30);
	}
}

/* S-04.043, S-16.107 */
void s04_meta_build(const struct wb_device *dev, const uint8_t *before,
		    const struct s04_meta *cur, bool factory_mode, uint8_t *out)
{
	const uint32_t size = s04_meta_size(dev);
	unsigned i;

	/* Every byte kept as raw bytes unless a rule below changes it. */
	for (i = 0; i < size; i++)
		out[i] = before[i];
	wb_put_le32(out, S04_META_MAGIC);
	for (i = 0; i < 4u; i++) {
		wb_put_le16(out + 0x12 + 2u * i, cur->os_version[i]);
		wb_put_le16(out + 0x1A + 2u * i, cur->sw_version[i]);
	}
	wb_put_le32(out + 0x24, cur->state == S04_STATE_UPGRADE ? S04_STATE_UPGRADE : S04_STATE_IDLE);
	if (factory_mode) {
		/* The 8 characters and a NUL at +0x04..+0x0C; +0x0D kept. */
		bool end = false;

		for (i = 0; i < 8u; i++) {
			if (cur->serial[i] == '\0')
				end = true;
			out[4u + i] = end ? 0u : (uint8_t)cur->serial[i];
		}
		out[0x0C] = 0u;
		wb_put_le16(out + 0x0E, cur->sku);
		/* without factory-word +0x10..+0x11 are kept bytes */
		if (wb_has(dev, WB_F_FACTORY_WORD))
			wb_put_le16(out + 0x10, cur->factory);
	}
}

/* S-04.045 M6: any nonzero field counts. */
bool s04_m6_needed(const uint16_t sw_version[4])
{
	return (sw_version[0] | sw_version[1] | sw_version[2] | sw_version[3]) != 0u;
}

/* S-04.046: the number S-10c.056 reads from a NUL-terminated value; every text reads as one. */
static uint32_t read_number(const char *s)
{
	size_t len = 0;

	while (s[len] != '\0')
		len++;
	return s10c_parse_u32(s, len);
}

/* S-04.046, S-16.110 */
bool s04_set_metadata(const struct wb_device *dev, struct s04_meta *cur, bool factory_mode,
		      bool option_on, const struct s04_setmeta *req)
{
	uint16_t sku = cur->sku, factory = cur->factory;
	size_t n = 0;

	if (!factory_mode || !option_on)
		return false;
	if (req->serial != NULL) {
		while (n <= 8u && req->serial[n] != '\0')
			n++;
		if (n != 8u)
			return false;
	}
	if (req->sku_variant != NULL) {
		uint32_t v = read_number(req->sku_variant);

		sku = (uint16_t)(v > 999u ? 0u : v);	/* S-16.110 */
	}
	/* without factory-word the key is passed over (S-04.046) */
	if (req->factory != NULL && wb_has(dev, WB_F_FACTORY_WORD))
		factory = (uint16_t)read_number(req->factory);	/* modulo 2^16 */
	if (req->serial != NULL) {
		for (n = 0; n < 8u; n++)
			cur->serial[n] = req->serial[n];
		cur->serial[8] = '\0';
	}
	cur->sku = sku;
	cur->factory = factory;
	return true;
}

/* --- Status reports to the PSoC --- */

const struct s04_psoc_msg s04_msg_start = {0x51, 3, {0x00, 0x00, 0x00, 0x00}};
const struct s04_psoc_msg s04_msg_held = {0x51, 3, {0x3C, 0xFF, 0x02, 0x00}};
const struct s04_psoc_msg s04_msg_held_screen = {0x40, 1, {0x01, 0x00, 0x00, 0x00}};
const struct s04_psoc_msg s04_msg_held_bar = {0x40, 2, {0x02, 0x00, 0x00, 0x00}};
const struct s04_psoc_msg s04_msg_handover = {0x51, 3, {0x3C, 0xFF, 0x32, 0x00}};
const struct s04_psoc_msg s04_msg_shutdown = {0x51, 3, {0x00, 0x00, 0x02, 0x00}};
const struct s04_psoc_msg s04_msg_shutdown_40 = {0x40, 1, {0x01, 0x00, 0x00, 0x00}};
const struct s04_psoc_msg s04_msg_reset_request = {0x97, 4, {0xDE, 0xAD, 0xBE, 0xEF}};

/* S-04.066 */
void s04_shutdown_one_cpu(const struct s04_shutdown_ops *ops)
{
	if (ops->mounted(ops->ctx))
		(void)ops->emmc_shutdown(ops->ctx);	/* a failure does not stop it */
	ops->reports(ops->ctx);
}

/* --- S-04.070 the RTC, S-16.102 --- */

#define DAY_SKIP_FROM 11323u	/* 2001-01-01, true days since 1970-01-01 */
#define DAY_SKIP_CODE 11321u	/* its encoded value */
#define DAY_FIELD_MAX 32767u

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian), y >= 1970. */
static uint32_t days_from_civil(unsigned y, unsigned m, unsigned d)
{
	uint32_t yy = (uint32_t)y - (m <= 2u ? 1u : 0u);
	uint32_t era = yy / 400u;
	uint32_t yoe = yy - era * 400u;
	uint32_t mp = m > 2u ? m - 3u : m + 9u;
	uint32_t doy = (153u * mp + 2u) / 5u + d - 1u;
	uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;

	return era * 146097u + doe - 719468u;
}

static void civil_from_days(uint32_t z, struct s04_datetime *out)
{
	uint32_t zz = z + 719468u;
	uint32_t era = zz / 146097u;
	uint32_t doe = zz - era * 146097u;
	uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
	uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
	uint32_t mp = (5u * doy + 2u) / 153u;
	uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
	uint32_t m = mp < 10u ? mp + 3u : mp - 9u;
	uint32_t y = yoe + era * 400u + (m <= 2u ? 1u : 0u);

	out->year = y;
	out->month = m;
	out->day = d;
}

static bool valid_date(unsigned y, unsigned m, unsigned d)
{
	static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	unsigned lim;

	if (y < 1970u || m < 1u || m > 12u || d < 1u)
		return false;
	lim = mdays[m - 1u];
	if (m == 2u && (y % 4u == 0u) && (y % 100u != 0u || y % 400u == 0u))
		lim = 29u;
	return d <= lim;
}

bool s04_rtc_encode(unsigned year, unsigned month, unsigned day, unsigned hour,
		    unsigned minute, unsigned second, uint32_t *clk)
{
	uint32_t days;

	if (!valid_date(year, month, day) || hour > 23u || minute > 59u || second > 59u ||
	    year > 2100u)
		return false;
	days = days_from_civil(year, month, day);
	if (days >= DAY_SKIP_FROM)
		days -= 2u;
	if (days > DAY_FIELD_MAX)
		return false;
	*clk = (uint32_t)second | ((uint32_t)minute << 6) | ((uint32_t)hour << 12) | (days << 17);
	return true;
}

void s04_rtc_decode(uint32_t clk, struct s04_datetime *out)
{
	uint32_t n = clk >> 17;

	if (n >= DAY_SKIP_CODE)
		n += 2u;
	civil_from_days(n, out);
	out->hour = (clk >> 12) & 0x1Fu;
	out->minute = (clk >> 6) & 0x3Fu;
	out->second = clk & 0x3Fu;
}

/* --- S-04.071 the fatal-halt record, S-16.104 --- */

void s04_fatal_record(enum s04_fatal_cause cause, uint32_t pc,
		      uint8_t out[S04_FATAL_SIZE])
{
	uint32_t general, specific, val;

	switch (cause) {
	case S04_FATAL_DDR_CHECK:
		general = 0xBu;
		specific = 1u;
		val = 0x84000000u;
		break;
	default:
		general = 0u;
		specific = 0u;
		val = 0u;
		break;
	}
	wb_put_le32(out, general);
	wb_put_le32(out + 4, specific);
	wb_put_le32(out + 8, val);
	wb_put_le32(out + 12, pc);
}
