/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10b: the MTP responder, level R (waffle/s10b_mtp.h).
 */
#include "waffle/s10b_mtp.h"

#include <string.h>

#include "waffle/bytes.h"
#include "waffle/s04_records.h"

#define TYPE_COMMAND	1u
#define TYPE_DATA	2u
#define TYPE_RESPONSE	3u

#define FORMAT_FOLDER	0x3001u

#define DT_UINT8	0x0002u
#define DT_UINT16	0x0004u
#define DT_UINT32	0x0006u
#define DT_UINT64	0x0008u
#define DT_UINT128	0x000Au
#define DT_STR		0xFFFFu

#define PROP_BATTERY	0x5001u
#define PROP_DATETIME	0x5011u
#define PROP_DISPLAY_NAME 0xDCE0u

#define DATE_BYTES	33u	/* count byte 16, 15 characters, the NUL */
#define RX_ARM_MAX	512u	/* bytes one OUT transfer is armed with */

/* S-10b.021, level R. */
static const uint16_t ops_r[] = {
	0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008,
	0x1009, 0x1014, 0x1015, 0x1016, 0x101B, 0x9801, 0x9802, 0x9803,
	0x9805, 0x9810,
};

/* S-10b.020 */
static const uint16_t device_props[] = { PROP_BATTERY, PROP_DATETIME };
static const uint16_t capture_formats[] = {
	0x3000, 0x3001, 0x3007, 0x3008, 0xB900, 0x3004,
};
static const uint16_t playback_formats[] = {
	0x3000, 0x3001, 0x3007, 0x3008, 0xB900,
};

/* The supported properties (S-10b.070) and their types (S-10b.071). */
static const struct {
	uint16_t code, type;
} props[] = {
	{ 0xDC01, DT_UINT32 },	/* StorageID */
	{ 0xDC02, DT_UINT16 },	/* ObjectFormat */
	{ 0xDC03, DT_UINT16 },	/* ProtectionStatus */
	{ 0xDC04, DT_UINT64 },	/* ObjectSize */
	{ 0xDC05, DT_UINT16 },	/* AssociationType */
	{ 0xDC07, DT_STR },	/* ObjectFileName */
	{ 0xDC08, DT_STR },	/* DateCreated */
	{ 0xDC09, DT_STR },	/* DateModified */
	{ 0xDC0B, DT_UINT32 },	/* ParentObject */
	{ 0xDC41, DT_UINT128 },	/* PersistentUniqueObjectIdentifier */
	{ 0xDC44, DT_STR },	/* Name */
};

#define N_PROPS (sizeof(props) / sizeof(props[0]))

enum select_mode {
	SEL_NONE,
	SEL_ALL,
	SEL_CHILDREN,	/* index: the folder's index + 1, 0 the root */
	SEL_ONE,	/* index: the object's index + 1 */
};

enum phase {
	PHASE_HEADER,
	PHASE_COUNT,
	PHASE_BODY,
	PHASE_END,
};

/* --- Bytes --------------------------------------------------------------- */

/* Little-endian writes that return the next byte, for building a dataset. */
static uint8_t *put16(uint8_t *p, unsigned v)
{
	wb_put_le16(p, (uint16_t)v);
	return p + 2;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
	wb_put_le32(p, v);
	return p + 4;
}

static uint8_t *put64(uint8_t *p, uint64_t v)
{
	wb_put_le64(p, v);
	return p + 8;
}

/* An MTP String of n units: the count byte counts the NUL; none is 0x00. */
static uint8_t *put_units(uint8_t *p, const uint16_t *u, size_t n)
{
	size_t i;

	if (n == 0) {
		*p = 0;
		return p + 1;
	}
	*p++ = (uint8_t)(n + 1u);
	for (i = 0; i < n; i++)
		p = put16(p, u[i]);
	return put16(p, 0);
}

static size_t units_bytes(size_t n)
{
	return n ? 1u + 2u * (n + 1u) : 1u;
}

static uint8_t *put_text(uint8_t *p, const char *text)
{
	uint16_t u[S10B_NAME_UNITS] = { 0 };
	const size_t len = text ? strlen(text) : 0;

	return put_units(p, u, s10b_name_utf16((const uint8_t *)text, len, u));
}

static uint8_t *put_date(uint8_t *p, uint32_t secs)
{
	char text[16];
	size_t i;

	s10b_datetime_text(secs, text);
	*p++ = 16;
	for (i = 0; i < 16; i++)
		p = put16(p, (uint8_t)text[i]);
	return p;
}

static uint8_t *put_array16(uint8_t *p, const uint16_t *v, size_t n)
{
	size_t i;

	p = put32(p, (uint32_t)n);
	for (i = 0; i < n; i++)
		p = put16(p, v[i]);
	return p;
}

#define ARRAY16(p, a) put_array16(p, a, sizeof(a) / sizeof(a[0]))

/* --- Objects and handles (S-10b.012) ----------------------------------- */

/* Handles 1 up, skipping the 0x0001xxxx a storage ID has (S-10b.074). */
uint32_t s10b_handle(size_t i)
{
	const uint32_t h = (uint32_t)i + 1u;

	return h >= 0x10000u ? h + 0x10000u : h;
}

static bool index_of(const struct s10b *m, uint32_t h, size_t *i)
{
	uint32_t k;

	if (h == 0 || h == S10B_ALL || h >> 16 == 1u)
		return false;
	k = h < 0x10000u ? h - 1u : h - 0x10001u;
	if (k >= m->n_objects)
		return false;
	*i = k;
	return true;
}

static const struct s10b_object *obj(const struct s10b *m, size_t i)
{
	return &m->cfg.objects[i];
}

static bool is_dir(const struct s10b_object *o)
{
	return o->kind == S10B_KIND_DIR;
}

static uint16_t format_of(const struct s10b_object *o)
{
	return is_dir(o) ? FORMAT_FOLDER : s10b_format(o->rows);
}

static uint32_t parent_handle(const struct s10b_object *o)
{
	return o->parent ? s10b_handle(o->parent - 1u) : 0;
}

static size_t name_units(const struct s10b *m, const struct s10b_object *o,
			 uint16_t *u)
{
	return s10b_name_utf16(m->cfg.names + o->name, o->name_len, u);
}

/* S-10b.048: a change time of 0 is the modification time. */
static uint32_t created(const struct s10b_object *o)
{
	return o->ctime ? o->ctime : o->mtime;
}

/* S-10b.041, S-10b.042: a folder whose children are listed. */
static bool readable(const struct s10b_object *o)
{
	return is_dir(o) && o->level < S10B_MAX_LEVEL &&
	       o->path_len <= S10B_MAX_READ_PATH;
}

/* P of object i into m->path. */
static const char *path_of(struct s10b *m, size_t i)
{
	size_t pos = obj(m, i)->path_len, idx = i + 1u;

	m->path[pos] = '\0';
	while (idx) {
		const struct s10b_object *o = obj(m, idx - 1u);

		pos -= o->name_len;
		memcpy(m->path + pos, m->cfg.names + o->name, o->name_len);
		m->path[--pos] = '/';
		idx = o->parent;
	}
	return m->path;
}

/* --- The listing (S-10b.013, S-10b.040 to S-10b.043) ---------------------- */

static void add_entry(void *arg, const struct s10b_entry *e)
{
	struct s10b *m = arg;
	const struct s10b_object *parent = m->cur ? obj(m, m->cur - 1u) : NULL;
	struct s10b_object *o;

	if ((e->kind != S10B_KIND_FILE && e->kind != S10B_KIND_DIR) ||
	    e->name_len == 0 || e->name_len > S10B_NAME_MAX ||
	    m->n_objects == m->cfg.max_objects ||
	    m->cfg.names_size - m->names_used < e->name_len)
		return;
	o = &m->cfg.objects[m->n_objects++];
	o->parent = (uint32_t)m->cur;
	o->name = (uint32_t)m->names_used;
	o->name_len = (uint8_t)e->name_len;
	o->kind = (uint8_t)e->kind;
	o->size = e->kind == S10B_KIND_FILE ? e->size : 0;
	o->mtime = e->mtime;
	o->ctime = e->ctime;
	o->level = (uint8_t)(parent ? parent->level + 1u : 1u);
	o->path_len = (uint16_t)((parent ? parent->path_len : 0u) + 1u +
				 e->name_len);
	o->rows = (uint16_t)((parent ? parent->rows : 0u) |
			     s10b_format_rows(e->name, e->name_len));
	memcpy(m->cfg.names + m->names_used, e->name, e->name_len);
	m->names_used += e->name_len;
}

/* Breadth first, the table itself the queue of folders still to read. */
static void make_listing(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	uint64_t total, free_bytes;
	size_t i;

	m->n_objects = 0;
	m->names_used = 0;
	if (!v || !v->space(v->ctx, &total, &free_bytes))
		return;
	m->cur = 0;
	(void)v->list(v->ctx, "/", add_entry, m);
	for (i = 0; i < m->n_objects; i++) {
		if (!readable(obj(m, i)))
			continue;
		m->cur = i + 1u;
		(void)v->list(v->ctx, path_of(m, i), add_entry, m);
	}
}

/* --- Selections ----------------------------------------------------------- */

static bool selected(const struct s10b *m, size_t i)
{
	const struct s10b_object *o = obj(m, i);
	bool in;

	switch ((enum select_mode)m->sel.mode) {
	case SEL_ALL:
		in = true;
		break;
	case SEL_CHILDREN:
		in = o->parent == m->sel.index;
		break;
	case SEL_ONE:
		in = i + 1u == m->sel.index;
		break;
	case SEL_NONE:
	default:
		in = false;
		break;
	}
	return in && (m->sel.format == 0 || format_of(o) == m->sel.format);
}

static void select_objects(struct s10b *m, enum select_mode mode,
			   size_t index, uint32_t format)
{
	m->sel.mode = (uint8_t)mode;
	m->sel.index = (uint32_t)index;
	m->sel.format = (uint16_t)format;
}

static uint32_t count_selected(const struct s10b *m)
{
	uint32_t n = 0;
	size_t i;

	for (i = 0; i < m->n_objects; i++)
		if (selected(m, i))
			n++;
	return n;
}

/* --- Object properties (S-10b.071, S-10b.072) ---------------------------- */

static int prop_slot(uint16_t code)
{
	size_t k;

	for (k = 0; k < N_PROPS; k++)
		if (props[k].code == code)
			return (int)k;
	return -1;
}

static bool name_prop(uint32_t code)
{
	return code == 0xDC07u || code == 0xDC44u || code == PROP_DISPLAY_NAME;
}

/* The value of a property of object i into p; NULL when it is not one. */
static uint8_t *prop_value(const struct s10b *m, size_t i, uint32_t code,
			   uint8_t *p)
{
	const struct s10b_object *o = obj(m, i);
	uint16_t u[S10B_NAME_UNITS] = { 0 };

	switch (code) {
	case 0xDC01:
		return put32(p, S10B_STORAGE_ID);
	case 0xDC02:
		return put16(p, format_of(o));
	case 0xDC03:
		return put16(p, 0);
	case 0xDC04:
		return put64(p, o->size);
	case 0xDC05:
		return put16(p, is_dir(o) ? 1u : 0u);
	case 0xDC07:
	case 0xDC44:
	case PROP_DISPLAY_NAME:
		return put_units(p, u, name_units(m, o, u));
	case 0xDC08:
		return put_date(p, created(o));
	case 0xDC09:
		return put_date(p, o->mtime);
	case 0xDC0B:
		return put32(p, parent_handle(o));
	case 0xDC41:
		p = put32(p, s10b_handle(i));
		p = put32(p, S10B_STORAGE_ID);
		return put64(p, 0);
	default:
		break;
	}
	return NULL;
}

static size_t prop_bytes(const struct s10b *m, size_t i, uint32_t code)
{
	uint16_t u[S10B_NAME_UNITS] = { 0 };

	switch (props[prop_slot((uint16_t)code)].type) {
	case DT_UINT16:
		return 2;
	case DT_UINT32:
		return 4;
	case DT_UINT64:
		return 8;
	case DT_UINT128:
		return 16;
	default:
		break;
	}
	return name_prop(code) ? units_bytes(name_units(m, obj(m, i), u)) :
				 DATE_BYTES;
}

/* Properties of a list: one code, or every supported one when 0. */
static size_t list_props(const struct s10b *m)
{
	return m->prop ? 1u : N_PROPS;
}

static uint16_t list_prop(const struct s10b *m, size_t k)
{
	return m->prop ? m->prop : props[k].code;
}

/* --- Replies -------------------------------------------------------------- */

static void respond(struct s10b *m, uint16_t code)
{
	m->reply = code;
	m->n_reply = 0;
	m->tx = S10B_TX_RESPONSE;
}

static void respond1(struct s10b *m, uint16_t code, uint32_t param)
{
	respond(m, code);
	m->reply_params[0] = param;
	m->n_reply = 1;
}

/* A data phase of `total` dataset bytes from src, then OK. */
static void send_data(struct s10b *m, enum s10b_source src, uint32_t total)
{
	const uint32_t len = total > UINT32_MAX - S10B_CONTAINER_HEADER ?
			     UINT32_MAX : total + S10B_CONTAINER_HEADER;
	uint8_t *p = m->hdr;

	m->src = src;
	m->data_total = total;
	m->data_sent = 0;
	m->phase = PHASE_HEADER;
	m->cur = 0;
	m->cur_prop = 0;
	p = put32(p, len);
	p = put16(p, TYPE_DATA);
	p = put16(p, m->code);
	(void)put32(p, m->tid);
	m->st = m->hdr;
	m->st_len = 0;
	m->st_pos = 0;
	respond(m, S10B_OK);
	m->tx = S10B_TX_DATA;
}

static void send_buf(struct s10b *m, const uint8_t *end)
{
	send_data(m, S10B_SRC_BUF, (uint32_t)(end - m->buf));
}

/* --- Operations ----------------------------------------------------------- */

static bool supported(unsigned code)
{
	size_t i;

	for (i = 0; i < sizeof(ops_r) / sizeof(ops_r[0]); i++)
		if (ops_r[i] == code)
			return true;
	return false;
}

/* S-10b.091 */
static bool capture(unsigned code)
{
	return code == 0x100Eu || code == 0x1018u || code == 0x101Cu;
}

/* S-10b.006 */
static bool host_data(unsigned code)
{
	return code == 0x100Cu || code == 0x100Du || code == 0x1016u ||
	       code == 0x9804u || code == 0x9811u || code == 0x95C2u;
}

/* S-10b.020 */
static void device_info(struct s10b *m)
{
	uint8_t *p = m->buf;

	p = put16(p, 100);
	p = put32(p, 6);
	p = put16(p, 100);
	p = put_text(p, "microsoft.com: 1.0;");
	p = put16(p, 0);
	p = ARRAY16(p, ops_r);
	p = put32(p, 0);		/* EventsSupported (S-10b.008) */
	p = ARRAY16(p, device_props);
	p = ARRAY16(p, capture_formats);
	p = ARRAY16(p, playback_formats);
	p = put_text(p, m->cfg.manufacturer);
	p = put_text(p, m->cfg.model);
	p = put_text(p, m->cfg.version);
	p = put_text(p, m->cfg.serial);
	send_buf(m, p);
}

/* S-10b.031 */
static void storage_info(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	uint64_t total = 0, free_bytes = 0;
	uint8_t *p = m->buf;

	if (m->params[0] != S10B_STORAGE_ID) {
		respond(m, S10B_INVALID_STORAGE);
		return;
	}
	if (!v || !v->space(v->ctx, &total, &free_bytes))
		total = free_bytes = 0;
	p = put16(p, 3);		/* Fixed RAM */
	p = put16(p, 2);		/* Generic Hierarchical */
	p = put16(p, 1);		/* read-only without deletion */
	p = put64(p, total & ~(uint64_t)1023u);
	p = put64(p, free_bytes & ~(uint64_t)1023u);
	p = put32(p, S10B_ALL);
	p = put_text(p, m->cfg.model);
	p = put_text(p, "");
	send_buf(m, p);
}

/* S-10b.044's selection; false (and the response) when it fails. */
static bool select_handles(struct s10b *m)
{
	const uint32_t parent = m->params[2];
	size_t i;

	if (m->params[0] != S10B_STORAGE_ID && m->params[0] != S10B_ALL) {
		respond(m, S10B_INVALID_STORAGE);
		return false;
	}
	if (parent == S10B_ALL) {
		select_objects(m, SEL_CHILDREN, 0, m->params[1]);
	} else if (parent == 0) {
		select_objects(m, SEL_ALL, 0, m->params[1]);
	} else if (!index_of(m, parent, &i)) {
		respond(m, S10B_INVALID_HANDLE);
		return false;
	} else if (!is_dir(obj(m, i))) {
		respond(m, S10B_INVALID_PARENT);
		return false;
	} else {
		select_objects(m, SEL_CHILDREN, i + 1u, m->params[1]);
	}
	return true;
}

static void send_handles(struct s10b *m)
{
	m->count = count_selected(m);
	send_data(m, S10B_SRC_HANDLES, 4u + 4u * m->count);
}

/* S-10b.049 */
static void object_info(struct s10b *m, size_t i)
{
	const struct s10b_object *o = obj(m, i);
	uint16_t u[S10B_NAME_UNITS] = { 0 };
	uint8_t *p = m->buf;

	p = put32(p, S10B_STORAGE_ID);
	p = put16(p, format_of(o));
	p = put16(p, 0);
	p = put32(p, o->size);
	p = put16(p, 0);
	memset(p, 0, 24);		/* thumbnail and image fields */
	p += 24;
	p = put32(p, parent_handle(o));
	p = put16(p, is_dir(o) ? 1u : 0u);
	p = put32(p, 0);
	p = put32(p, 0);
	p = put_units(p, u, name_units(m, o, u));
	p = put_date(p, created(o));
	p = put_date(p, o->mtime);
	p = put_units(p, u, 0);
	send_buf(m, p);
}

/* S-10b.050, S-10b.051: a file from offset, at most max bytes. */
static void get_object(struct s10b *m, bool partial)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	const struct s10b_object *o;
	uint32_t off = 0, n;
	size_t i;
	long got;

	if (!index_of(m, m->params[0], &i) || is_dir(obj(m, i))) {
		respond(m, S10B_INVALID_HANDLE);
		return;
	}
	o = obj(m, i);
	n = o->size;
	if (partial) {
		off = m->params[1];
		if (off > o->size) {
			respond(m, S10B_INVALID_PARAMETER);
			return;
		}
		n = o->size - off;
		if (m->params[2] < n)
			n = m->params[2];
	}
	/* A file that cannot be opened has no data phase. */
	got = v->read(v->ctx, path_of(m, i), off, m->stage, 0);
	if (got < 0) {
		respond(m, S10B_GENERAL_ERROR);
		return;
	}
	m->file_off = off;
	m->file_end = off + n;
	send_data(m, S10B_SRC_FILE, n);
	if (partial) {
		m->reply_params[0] = n;
		m->n_reply = 1;
	}
}

/* S-10b.060 */
static void device_prop_desc(struct s10b *m)
{
	const struct s10b_device_ops *d = m->cfg.dev;
	uint8_t *p = m->buf;

	if (m->params[0] == PROP_BATTERY) {
		const unsigned charge = d->battery(d->ctx);

		p = put16(p, PROP_BATTERY);
		p = put16(p, DT_UINT8);
		*p++ = 0;			/* get */
		*p++ = 100;			/* FactoryDefaultValue */
		*p++ = (uint8_t)(charge > 100u ? 100u : charge);
		*p++ = 1;			/* Range */
		*p++ = 0;
		*p++ = 100;
		*p++ = 1;
	} else if (m->params[0] == PROP_DATETIME) {
		p = put16(p, PROP_DATETIME);
		p = put16(p, DT_STR);
		*p++ = 1;			/* get/set */
		*p++ = 0;			/* "" (S-16.058) */
		*p++ = 0;			/* "" (S-10b Q-03) */
		*p++ = 0;			/* None */
	} else {
		respond(m, S10B_DEVPROP_NOT_SUPPORTED);
		return;
	}
	send_buf(m, p);
}

/* S-10b.061 */
static void device_prop_value(struct s10b *m)
{
	const struct s10b_device_ops *d = m->cfg.dev;
	uint8_t *p = m->buf;

	if (m->params[0] == PROP_BATTERY) {
		const unsigned charge = d->battery(d->ctx);

		*p++ = (uint8_t)(charge > 100u ? 100u : charge);
	} else if (m->params[0] == PROP_DATETIME) {
		struct s04_datetime t;

		s04_rtc_decode(d->clock_read(d->ctx), &t);
		*p++ = 16;
		{
			const unsigned f[6] = { t.year, t.month, t.day,
						t.hour, t.minute, t.second };
			const unsigned w[6] = { 4, 2, 2, 2, 2, 2 };
			char text[16];
			size_t pos = 0, j, k;

			for (k = 0; k < 6; k++) {
				unsigned v = f[k];

				for (j = w[k]; j-- > 0;) {
					text[pos + j] = (char)('0' + v % 10u);
					v /= 10u;
				}
				pos += w[k];
				if (k == 2)
					text[pos++] = 'T';
			}
			text[15] = '\0';
			for (k = 0; k < 16; k++)
				p = put16(p, (uint8_t)text[k]);
		}
	} else {
		respond(m, S10B_DEVPROP_NOT_SUPPORTED);
		return;
	}
	send_buf(m, p);
}

/* S-10b.062, S-10b.064: the value is the host data phase. */
static void set_device_prop(struct s10b *m)
{
	const struct s10b_device_ops *d = m->cfg.dev;
	uint16_t u[S10B_DATA_MAX / 2u];
	struct s10b_datetime t;
	size_t n, k;
	uint32_t count;

	if (m->params[0] == PROP_BATTERY) {
		respond(m, S10B_ACCESS_DENIED);
		return;
	}
	if (m->params[0] != PROP_DATETIME) {
		respond(m, S10B_DEVPROP_NOT_SUPPORTED);
		return;
	}
	n = m->data_len ? m->data[0] : 0;
	if (n == 0 || m->data_len < 1u + 2u * n) {
		respond(m, S10B_INVALID_DEVPROP_VALUE);
		return;
	}
	for (k = 0; k < n; k++)
		u[k] = wb_get_le16(m->data + 1 + 2 * k);
	if (u[n - 1u] == 0)
		n--;
	if (!s10b_datetime_parse(u, n, true, &t) ||
	    !s04_rtc_encode(t.year, t.month, t.day, t.hour, t.minute,
			    t.second, &count)) {
		respond(m, S10B_INVALID_DEVPROP_VALUE);
		return;
	}
	d->clock_write(d->ctx, count);
	respond(m, S10B_OK);
}

/* S-10b.071 */
static void object_prop_desc(struct s10b *m)
{
	const uint32_t code = m->params[0], format = m->params[1];
	const int k = prop_slot((uint16_t)code);
	uint8_t *p = m->buf;
	uint16_t type;

	if (name_prop(code)) {
		if (format != FORMAT_FOLDER &&
		    (format > 0xFFFFu ||
		     !s10b_format_known((uint16_t)format))) {
			respond(m, S10B_INVALID_PROP_FORMAT);
			return;
		}
		type = DT_STR;
	} else if (code > 0xFFFFu || k < 0) {
		respond(m, S10B_INVALID_PROP_CODE);
		return;
	} else {
		type = props[k].type;
	}
	p = put16(p, code);
	p = put16(p, type);
	*p++ = 0;			/* get (level R) */
	switch (type) {
	case DT_UINT16:
		p = put16(p, code == 0xDC02u ? format & 0xFFFFu :
			     code == 0xDC05u && format == FORMAT_FOLDER ? 1u :
			     0u);
		break;
	case DT_UINT32:
		p = put32(p, 0);
		break;
	case DT_UINT64:
		p = put64(p, 0);
		break;
	case DT_UINT128:
		p = put64(put64(p, 0), 0);
		break;
	default:
		*p++ = 0;		/* "" */
		break;
	}
	p = put32(p, 0);		/* GroupCode */
	*p++ = 0;			/* FormFlag: None */
	send_buf(m, p);
}

/* S-10b.072 */
static void object_prop_value(struct s10b *m)
{
	const uint8_t *p;
	size_t i;

	if (!index_of(m, m->params[0], &i)) {
		respond(m, S10B_INVALID_HANDLE);
		return;
	}
	p = prop_value(m, i, m->params[1], m->buf);
	if (!p) {
		respond(m, S10B_INVALID_PROP_CODE);
		return;
	}
	send_buf(m, p);
}

/* S-10b.073, S-10b.074 */
static void object_prop_list(struct s10b *m)
{
	const uint32_t h = m->params[0], format = m->params[1],
		       code = m->params[2], depth = m->params[4];
	size_t i, j;
	uint32_t bytes = 4;

	if (depth != 0 && depth != 1u && depth != S10B_ALL) {
		respond(m, S10B_DEPTH_UNSUPPORTED);
		return;
	}
	if (h == S10B_ALL || h == S10B_STORAGE_ID || (h == 0 && depth == S10B_ALL)) {
		select_objects(m, SEL_ALL, 0, format);
	} else if (h == 0) {
		/* The root is not an object. */
		select_objects(m, depth == 0 ? SEL_NONE : SEL_CHILDREN, 0,
			       format);
	} else if (h >> 16 == 1u) {
		respond(m, S10B_INVALID_STORAGE);
		return;
	} else if (!index_of(m, h, &i)) {
		respond(m, S10B_INVALID_HANDLE);
		return;
	} else if (depth == 0) {
		select_objects(m, SEL_ONE, i + 1u, format);
	} else if (depth == S10B_ALL) {
		respond(m, S10B_DEPTH_UNSUPPORTED);
		return;
	} else {
		select_objects(m, is_dir(obj(m, i)) ? SEL_CHILDREN : SEL_NONE,
			       i + 1u, format);
	}
	if (code == 0 || code == S10B_ALL) {
		m->prop = 0;
	} else if (code <= 0xFFFFu && prop_slot((uint16_t)code) >= 0) {
		m->prop = (uint16_t)code;
	} else {
		respond(m, S10B_INVALID_PROP_CODE);
		return;
	}
	m->count = 0;
	for (i = 0; i < m->n_objects; i++) {
		if (!selected(m, i))
			continue;
		for (j = 0; j < list_props(m); j++) {
			m->count++;
			bytes += 8u + (uint32_t)prop_bytes(m, i, list_prop(m, j));
		}
	}
	send_data(m, S10B_SRC_PROPS, bytes);
}

/* S-10b.075 */
static void object_references(struct s10b *m)
{
	size_t i;

	if (!index_of(m, m->params[0], &i)) {
		respond(m, S10B_INVALID_HANDLE);
		return;
	}
	select_objects(m, is_dir(obj(m, i)) ? SEL_CHILDREN : SEL_NONE,
		       i + 1u, 0);
	send_handles(m);
}

/* S-10b.010 */
static void open_session(struct s10b *m)
{
	if (m->session) {
		respond1(m, S10B_SESSION_ALREADY_OPEN, m->session);
		return;
	}
	if (m->params[0] == 0) {
		respond(m, S10B_INVALID_PARAMETER);
		return;
	}
	m->session = m->params[0];
	make_listing(m);
	respond(m, S10B_OK);
}

static void close_session(struct s10b *m)
{
	m->session = 0;
	m->n_objects = 0;
	m->names_used = 0;
}

static void execute(struct s10b *m)
{
	const unsigned code = m->code;
	uint8_t *p;
	size_t i;

	m->rx = S10B_RX_COMMAND;
	if (!supported(code) && !capture(code)) {
		respond(m, S10B_OP_NOT_SUPPORTED);	/* S-10b.090 */
		return;
	}
	if (!m->session && code != 0x1001u && code != 0x1002u) {
		respond(m, S10B_SESSION_NOT_OPEN);	/* S-10b.011 */
		return;
	}
	switch (code) {
	case 0x1001:
		device_info(m);
		break;
	case 0x1002:
		open_session(m);
		break;
	case 0x1003:
		close_session(m);
		respond(m, S10B_OK);
		break;
	case 0x1004:		/* S-10b.030 */
		p = put32(m->buf, 1);
		send_buf(m, put32(p, S10B_STORAGE_ID));
		break;
	case 0x1005:
		storage_info(m);
		break;
	case 0x1006:		/* S-10b.045 */
		if (select_handles(m))
			respond1(m, S10B_OK, count_selected(m));
		break;
	case 0x1007:
		if (select_handles(m))
			send_handles(m);
		break;
	case 0x1008:
		if (index_of(m, m->params[0], &i))
			object_info(m, i);
		else
			respond(m, S10B_INVALID_HANDLE);
		break;
	case 0x1009:
		get_object(m, false);
		break;
	case 0x101B:
		get_object(m, true);
		break;
	case 0x1014:
		device_prop_desc(m);
		break;
	case 0x1015:
		device_prop_value(m);
		break;
	case 0x1016:
		set_device_prop(m);
		break;
	case 0x9801:		/* S-10b.070, for any format */
		p = put32(m->buf, N_PROPS);
		for (i = 0; i < N_PROPS; i++)
			p = put16(p, props[i].code);
		send_buf(m, p);
		break;
	case 0x9802:
		object_prop_desc(m);
		break;
	case 0x9803:
		object_prop_value(m);
		break;
	case 0x9805:
		object_prop_list(m);
		break;
	case 0x9810:
		object_references(m);
		break;
	default:		/* the capture operations: nothing */
		respond(m, S10B_OK);
		break;
	}
}

/* --- The data phase ------------------------------------------------------- */

/* The next piece of the data container into m->st; false at its end or
 * when the file cannot be read. */
static bool produce(struct s10b *m)
{
	uint8_t *p = m->stage;

	m->st = m->stage;
	m->st_pos = 0;
	m->st_len = 0;
	if (m->phase == PHASE_HEADER) {
		m->st = m->hdr;
		m->st_len = S10B_CONTAINER_HEADER;
		m->phase = m->src == S10B_SRC_HANDLES ||
			   m->src == S10B_SRC_PROPS ? PHASE_COUNT : PHASE_BODY;
		return true;
	}
	if (m->phase == PHASE_COUNT) {
		(void)put32(p, m->count);
		m->st_len = 4;
		m->phase = PHASE_BODY;
		return true;
	}
	if (m->phase != PHASE_BODY)
		return false;
	switch (m->src) {
	case S10B_SRC_BUF:
		m->st = m->buf;
		m->st_len = m->data_total;
		m->phase = PHASE_END;
		return true;
	case S10B_SRC_HANDLES:
	case S10B_SRC_PROPS:
		while (m->cur < m->n_objects && !selected(m, m->cur))
			m->cur++;
		if (m->cur == m->n_objects)
			return false;
		p = put32(p, s10b_handle(m->cur));
		if (m->src == S10B_SRC_HANDLES) {
			m->cur++;
		} else {
			const uint16_t code = list_prop(m, m->cur_prop);

			p = put16(p, code);
			p = put16(p, props[prop_slot(code)].type);
			p = prop_value(m, m->cur, code, p);
			if (++m->cur_prop == list_props(m)) {
				m->cur_prop = 0;
				m->cur++;
			}
		}
		m->st_len = (size_t)(p - m->stage);
		return true;
	case S10B_SRC_FILE: {
		const struct s10b_volume_ops *v = m->cfg.vol;
		uint32_t n = m->file_end - m->file_off;
		long got;

		if (n == 0)
			return false;
		if (n > S10B_STAGE_MAX)
			n = S10B_STAGE_MAX;
		/* m->path still holds the file's P. */
		got = v->read(v->ctx, m->path, m->file_off, m->stage, n);
		if (got <= 0 || (unsigned long)got > n)
			return false;
		m->file_off += (uint32_t)got;
		m->st_len = (size_t)got;
		return true;
	}
	default:
		break;
	}
	return false;
}

bool s10b_tx(struct s10b *m, uint8_t *buf, size_t cap, size_t *n)
{
	uint32_t len, left, want, got = 0;
	uint8_t *p;
	unsigned k;

	switch (m->tx) {
	case S10B_TX_DATA:
		len = m->data_total > UINT32_MAX - S10B_CONTAINER_HEADER ?
		      UINT32_MAX : m->data_total + S10B_CONTAINER_HEADER;
		left = len - m->data_sent;
		want = (uint32_t)(cap - cap % m->mps);
		if (want > left)
			want = left;
		while (got < want) {
			size_t k2;

			if (m->st_pos == m->st_len && !produce(m))
				break;
			k2 = m->st_len - m->st_pos;
			if (k2 > want - got)
				k2 = want - got;
			memcpy(buf + got, m->st + m->st_pos, k2);
			m->st_pos += k2;
			got += (uint32_t)k2;
		}
		m->data_sent += got;
		*n = got;
		if (got < want) {
			/* Ended short of its length (S-10b.050). */
			respond(m, S10B_INCOMPLETE_TRANSFER);
			if (got && got % m->mps == 0)
				m->tx = S10B_TX_ZLP;
		} else if (m->data_sent == len) {
			m->tx = len % m->mps == 0 ? S10B_TX_ZLP :
						    S10B_TX_RESPONSE;
		}
		return true;
	case S10B_TX_ZLP:
		*n = 0;
		m->tx = S10B_TX_RESPONSE;
		return true;
	case S10B_TX_RESPONSE:
		p = put32(buf, S10B_CONTAINER_HEADER + 4u * m->n_reply);
		p = put16(p, TYPE_RESPONSE);
		p = put16(p, m->reply);
		p = put32(p, m->tid);
		for (k = 0; k < m->n_reply; k++)
			p = put32(p, m->reply_params[k]);
		*n = (size_t)(p - buf);
		m->tx = S10B_TX_IDLE;
		return true;
	case S10B_TX_IDLE:
	default:
		break;
	}
	return false;
}

/* --- Receiving ------------------------------------------------------------ */

size_t s10b_rx_size(const struct s10b *m)
{
	uint32_t left;
	size_t n;

	if (m->tx != S10B_TX_IDLE || m->mps == 0)
		return 0;
	if (m->rx == S10B_RX_COMMAND || !m->rx_header ||
	    m->rx_got >= m->rx_total)
		return m->mps;
	/* No further than the container's end: the next one is not part of
	 * this transfer even when this one ends on a full packet. */
	left = m->rx_total - m->rx_got;
	n = (left + m->mps - 1u) / m->mps * m->mps;
	return n < RX_ARM_MAX ? n : RX_ARM_MAX;
}

static void keep(struct s10b *m, const uint8_t *p, size_t n)
{
	const size_t room = sizeof(m->data) - m->data_len;

	memcpy(m->data + m->data_len, p, n < room ? n : room);
	m->data_len += n < room ? n : room;
}

void s10b_rx(struct s10b *m, const uint8_t *p, size_t n)
{
	const bool ends = n == 0 || n % m->mps != 0;
	uint32_t len;
	size_t k;

	if (m->tx != S10B_TX_IDLE)
		return;
	switch (m->rx) {
	case S10B_RX_COMMAND:
		if (n < S10B_CONTAINER_HEADER)
			return;		/* a zero-length packet, or noise */
		len = wb_get_le32(p);
		if (wb_get_le16(p + 4) != TYPE_COMMAND) {
			/* Out of place (S-10b.004): its bytes go nowhere. */
			if (!ends && len > n) {
				m->rx_total = len;
				m->rx_got = (uint32_t)n;
				m->rx = S10B_RX_SKIP;
			}
			return;
		}
		m->code = wb_get_le16(p + 6);
		m->tid = wb_get_le32(p + 8);
		if (len > n)
			len = (uint32_t)n;
		for (k = 0; k < 5; k++)
			m->params[k] = S10B_CONTAINER_HEADER + 4u * (k + 1u) <=
				       len ? wb_get_le32(p + 12 + 4 * k) : 0;
		if (host_data(m->code)) {
			m->rx = S10B_RX_DATA;
			m->rx_header = false;
			m->rx_got = 0;
			m->rx_total = 0;
			m->data_len = 0;
			return;
		}
		execute(m);
		return;
	case S10B_RX_DATA:
		/* Its end is its length, however the host splits it into
		 * packets (S-10b.003). */
		for (k = 0; k < n && m->rx_got < S10B_CONTAINER_HEADER; k++)
			m->hdr[m->rx_got++] = p[k];
		if (!m->rx_header && m->rx_got == S10B_CONTAINER_HEADER) {
			m->rx_header = true;
			m->rx_total = wb_get_le32(m->hdr);
			if (m->rx_total < S10B_CONTAINER_HEADER)
				m->rx_total = S10B_CONTAINER_HEADER;
		}
		keep(m, p + k, n - k);
		m->rx_got += (uint32_t)(n - k);
		if (m->rx_header && m->rx_got >= m->rx_total)
			execute(m);	/* S-10b.002: after the whole phase */
		return;
	case S10B_RX_SKIP:
		m->rx_got += (uint32_t)n;
		if (ends || m->rx_got >= m->rx_total)
			m->rx = S10B_RX_COMMAND;
		return;
	default:
		break;
	}
}

/* --- State ---------------------------------------------------------------- */

void s10b_init(struct s10b *m, const struct s10b_config *cfg)
{
	memset(m, 0, sizeof(*m));
	m->cfg = *cfg;
	m->mps = 512;
}

void s10b_identity(struct s10b *m, const char *manufacturer,
		   const char *model, const char *serial)
{
	m->cfg.manufacturer = manufacturer;
	m->cfg.model = model;
	m->cfg.serial = serial;
}

void s10b_cancel(struct s10b *m)
{
	m->rx = S10B_RX_COMMAND;
	m->tx = S10B_TX_IDLE;
}

void s10b_reset(struct s10b *m, size_t mps)
{
	s10b_cancel(m);
	close_session(m);
	m->mps = mps ? mps : 512u;
}

bool s10b_busy(const struct s10b *m)
{
	return m->rx != S10B_RX_COMMAND || m->tx != S10B_TX_IDLE;
}
