/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * S-10b: the MTP responder, levels R and W (waffle/mtp/mtp.h).
 */
#include "waffle/mtp/mtp.h"

#include <string.h>

#include "waffle/util/bytes.h"
#include "waffle/boot/records.h"

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
#define PROP_FILE_NAME	0xDC07u
#define PROP_MODIFIED	0xDC09u
#define PROP_DISPLAY_NAME 0xDCE0u

#define OP_DELETE	0x100Bu
#define OP_SEND_INFO	0x100Cu
#define OP_SEND		0x100Du
#define OP_MOVE		0x1019u
#define OP_SET_PROP	0x9804u

#define DATE_BYTES	33u	/* count byte 16, 15 characters, the NUL */
#define RX_ARM_MAX	512u	/* bytes one OUT transfer is armed with */

/* The ObjectInfo dataset's fixed fields, before Filename (S-10b.049). */
#define INFO_FIXED	52u

/* struct s10b_object's flags. */
#define F_READ		1u	/* a folder whose children have been listed */
#define F_MARK		2u	/* in the subtree a change moves */
#define F_DONE		4u	/* placed again after that change */

/* S-10b.021: level R, and what level W adds. */
static const uint16_t ops_r[] = {
	0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008,
	0x1009, 0x1014, 0x1015, 0x1016, 0x101B, 0x9801, 0x9802, 0x9803,
	0x9805, 0x9810,
};
static const uint16_t ops_w[] = {
	OP_DELETE, OP_SEND_INFO, OP_SEND, OP_MOVE, OP_SET_PROP,
};

#define N_OPS_R (sizeof(ops_r) / sizeof(ops_r[0]))
#define N_OPS_W (sizeof(ops_w) / sizeof(ops_w[0]))

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

/*
 * An MTP String at d[*pos] of a host dataset of len bytes: its units, the
 * last one dropped when it is the NUL, into u (255 units). False when it
 * runs past the end.
 */
static bool take_string(const uint8_t *d, size_t len, size_t *pos,
			uint16_t *u, size_t *n)
{
	size_t c, k;

	if (*pos >= len)
		return false;
	c = d[*pos];
	if (*pos + 1u + 2u * c > len)
		return false;
	for (k = 0; k < c; k++)
		u[k] = wb_get_le16(d + *pos + 1u + 2u * k);
	*pos += 1u + 2u * c;
	*n = c && u[c - 1u] == 0 ? c - 1u : c;
	return true;
}

/* --- Objects and handles (S-10b.012) ----------------------------------- */

/* Handles 1 up, skipping the 0x0001xxxx a storage ID has (S-10b.074). */
uint32_t s10b_handle(size_t i)
{
	const uint32_t h = (uint32_t)i + 1u;

	return h >= 0x10000u ? h + 0x10000u : h;
}

static struct s10b_object *obj(const struct s10b *m, size_t i)
{
	return &m->cfg.objects[i];
}

static bool live(const struct s10b_object *o)
{
	return o->kind == S10B_KIND_FILE || o->kind == S10B_KIND_DIR;
}

static bool index_of(const struct s10b *m, uint32_t h, size_t *i)
{
	uint32_t k;

	if (h == 0 || h == S10B_ALL || h >> 16 == 1u)
		return false;
	/* An object of the session's first listing sits at its handle's
	 * index; a later one is looked for. */
	k = h < 0x10000u ? h - 1u : h - 0x10001u;
	if (k < m->n_objects && obj(m, k)->handle == h && live(obj(m, k))) {
		*i = k;
		return true;
	}
	for (k = 0; k < m->n_objects; k++)
		if (obj(m, k)->handle == h && live(obj(m, k))) {
			*i = k;
			return true;
		}
	return false;
}

static bool is_dir(const struct s10b_object *o)
{
	return o->kind == S10B_KIND_DIR;
}

static uint16_t format_of(const struct s10b_object *o)
{
	return is_dir(o) ? FORMAT_FOLDER : s10b_format(o->rows);
}

static uint32_t parent_handle(const struct s10b *m, const struct s10b_object *o)
{
	return o->parent ? obj(m, o->parent - 1u)->handle : 0;
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

/* P of object i into out (S10B_PATH_MAX bytes). */
static const char *path_into(const struct s10b *m, size_t i, char *out)
{
	size_t pos = obj(m, i)->path_len, idx = i + 1u;

	out[pos] = '\0';
	while (idx) {
		const struct s10b_object *o = obj(m, idx - 1u);

		pos -= o->name_len;
		memcpy(out + pos, m->cfg.names + o->name, o->name_len);
		out[--pos] = '/';
		idx = o->parent;
	}
	return out;
}

static const char *path_of(struct s10b *m, size_t i)
{
	return path_into(m, i, m->path);
}

/* P of a name in folder `parent` (index + 1, 0: the root) into out. */
static const char *child_path(const struct s10b *m, uint32_t parent,
			      const uint8_t *name, size_t len, char *out)
{
	size_t n = 0;

	if (parent)
		n = strlen(path_into(m, parent - 1u, out));
	out[n++] = '/';
	memcpy(out + n, name, len);
	out[n + len] = '\0';
	return out;
}

/* --- The name pool ------------------------------------------------------- */

/*
 * Each name is a record: its owner's index + 1 (0 once no object uses it),
 * its length, its bytes. A record no longer used stays until a name does
 * not fit; the pool is then compacted, records kept in order.
 */
static bool name_room(const struct s10b *m, size_t len)
{
	return m->cfg.names_size - m->names_used >= S10B_NAME_REC + len;
}

static void compact(struct s10b *m)
{
	uint8_t *pool = m->cfg.names;
	size_t r = 0, w = 0;

	while (r < m->names_used) {
		const uint32_t owner = wb_get_le32(pool + r);
		const size_t rec = S10B_NAME_REC + pool[r + 4u];

		if (owner) {
			if (w != r)
				memmove(pool + w, pool + r, rec);
			obj(m, owner - 1u)->name = (uint32_t)(w + S10B_NAME_REC);
			w += rec;
		}
		r += rec;
	}
	m->names_used = w;
	m->names_dead = 0;
}

static bool fit_name(struct s10b *m, size_t len)
{
	if (!name_room(m, len) && m->names_dead)
		compact(m);
	return name_room(m, len);
}

/* Object i's name becomes `name`, in a record fit_name() made room for. */
static void set_name(struct s10b *m, size_t i, const uint8_t *name, size_t len)
{
	struct s10b_object *o = obj(m, i);
	uint8_t *rec = m->cfg.names + m->names_used;

	wb_put_le32(rec, (uint32_t)i + 1u);
	rec[4] = (uint8_t)len;
	memcpy(rec + S10B_NAME_REC, name, len);
	o->name = (uint32_t)(m->names_used + S10B_NAME_REC);
	o->name_len = (uint8_t)len;
	m->names_used += S10B_NAME_REC + len;
}

static void drop_name(struct s10b *m, size_t i)
{
	const struct s10b_object *o = obj(m, i);

	wb_put_le32(m->cfg.names + o->name - S10B_NAME_REC, 0);
	m->names_dead += S10B_NAME_REC + o->name_len;
}

/* --- The listing (S-10b.013, S-10b.040 to S-10b.043) ---------------------- */

/* Level, P's length and format rows from the parent and the name. */
static void place(const struct s10b *m, size_t i)
{
	struct s10b_object *o = obj(m, i);
	const struct s10b_object *p = o->parent ? obj(m, o->parent - 1u) : NULL;

	o->level = (uint8_t)(p ? p->level + 1u : 1u);
	o->path_len = (uint16_t)((p ? p->path_len : 0u) + 1u + o->name_len);
	o->rows = (uint16_t)((p ? p->rows : 0u) |
			     s10b_format_rows(m->cfg.names + o->name,
					      o->name_len));
}

static bool free_slot(const struct s10b *m, size_t *i)
{
	size_t k;

	if (m->n_objects < m->cfg.max_objects) {
		*i = m->n_objects;
		return true;
	}
	for (k = 0; k < m->n_objects; k++)
		if (obj(m, k)->kind == S10B_KIND_FREE) {
			*i = k;
			return true;
		}
	return false;
}

/* Room in the table and the pool for one more object (S-10b.014). */
static bool room_for_object(struct s10b *m, size_t name_len)
{
	size_t i;

	return fit_name(m, name_len) && free_slot(m, &i);
}

/* A new listed object in folder `parent` (index + 1, 0: the root);
 * room_for_object() has said there is room. */
static size_t add_object(struct s10b *m, uint32_t parent, const uint8_t *name,
			 size_t len, const struct s10b_entry *e, uint32_t handle)
{
	struct s10b_object *o;
	size_t i = 0;

	(void)free_slot(m, &i);
	if (i == m->n_objects)
		m->n_objects++;
	o = obj(m, i);
	memset(o, 0, sizeof(*o));
	o->handle = handle;
	o->parent = parent;
	o->kind = (uint8_t)e->kind;
	o->size = e->kind == S10B_KIND_FILE ? e->size : 0;
	o->mtime = e->mtime;
	o->ctime = e->ctime;
	set_name(m, i, name, len);
	place(m, i);
	return i;
}

static void free_object(struct s10b *m, size_t i)
{
	struct s10b_object *o = obj(m, i);

	drop_name(m, i);
	o->kind = S10B_KIND_FREE;
	o->handle = 0;
	o->flags = 0;
}

static uint32_t new_handle(struct s10b *m)
{
	return s10b_handle(m->next_handle++);
}

static void add_entry(void *arg, const struct s10b_entry *e)
{
	struct s10b *m = arg;

	if ((e->kind != S10B_KIND_FILE && e->kind != S10B_KIND_DIR) ||
	    e->name_len == 0 || e->name_len > S10B_NAME_MAX ||
	    !room_for_object(m, e->name_len))
		return;
	(void)add_object(m, (uint32_t)m->cur, e->name, e->name_len, e,
			 new_handle(m));
}

/*
 * Lists the children of every listed folder whose children are listed
 * (S-10b.041, S-10b.042) and that has not been read: breadth first, the
 * table itself the queue. Run again after a move or a rename that lets a
 * folder's children be listed.
 */
static void fill(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	bool more = true;
	size_t i;

	while (more) {
		more = false;
		for (i = 0; i < m->n_objects; i++) {
			struct s10b_object *o = obj(m, i);

			if (!live(o) || !readable(o) || (o->flags & F_READ))
				continue;
			o->flags |= F_READ;
			m->cur = i + 1u;
			(void)v->list(v->ctx, path_of(m, i), add_entry, m);
			more = true;
		}
	}
}

static void make_listing(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	uint64_t total, free_bytes;

	m->n_objects = 0;
	m->names_used = 0;
	m->names_dead = 0;
	m->next_handle = 0;
	if (!v || !v->space(v->ctx, &total, &free_bytes))
		return;
	m->cur = 0;
	(void)v->list(v->ctx, "/", add_entry, m);
	fill(m);
}

/* --- Subtrees (level W) ---------------------------------------------------- */

/* Marks object i and every listed object beneath it. */
static void mark_subtree(const struct s10b *m, size_t i)
{
	size_t j;

	for (j = 0; j < m->n_objects; j++) {
		struct s10b_object *o = obj(m, j);
		uint32_t k = (uint32_t)j + 1u;
		unsigned steps = 0;

		o->flags &= (uint8_t)~(F_MARK | F_DONE);
		if (!live(o))
			continue;
		while (k && steps++ <= S10B_MAX_LEVEL) {
			if (k == i + 1u) {
				o->flags |= F_MARK;
				break;
			}
			k = obj(m, k - 1u)->parent;
		}
	}
}

static void unmark(const struct s10b *m)
{
	size_t j;

	for (j = 0; j < m->n_objects; j++)
		obj(m, j)->flags &= (uint8_t)~(F_MARK | F_DONE);
}

/* After object i moved or was renamed: it and everything beneath it placed
 * again, parents first. */
static void replace(const struct s10b *m, size_t i)
{
	unsigned pass;
	size_t j;

	mark_subtree(m, i);
	place(m, i);
	obj(m, i)->flags |= F_DONE;
	for (pass = 0; pass < S10B_MAX_LEVEL; pass++)
		for (j = 0; j < m->n_objects; j++) {
			struct s10b_object *o = obj(m, j);

			if ((o->flags & (F_MARK | F_DONE)) != F_MARK ||
			    !(obj(m, o->parent - 1u)->flags & F_DONE))
				continue;
			place(m, j);
			o->flags |= F_DONE;
		}
	unmark(m);
}

/*
 * S-10b.087: object i placed at `level` with a path of path_len bytes keeps
 * every listed object beneath it listed: each one's folder lists its
 * children (S-10b.041, S-10b.042).
 */
static bool keeps_listed(const struct s10b *m, size_t i, unsigned level,
			 size_t path_len)
{
	const struct s10b_object *top = obj(m, i);
	bool ok = true;
	size_t j;

	mark_subtree(m, i);
	for (j = 0; j < m->n_objects; j++) {
		const struct s10b_object *o = obj(m, j);
		size_t parent_path;
		unsigned lv;

		if (j == i || !(o->flags & F_MARK))
			continue;
		lv = level + (unsigned)(o->level - top->level);
		parent_path = path_len + (size_t)(o->path_len - top->path_len) -
			      1u - o->name_len;
		if (lv > S10B_MAX_LEVEL || parent_path > S10B_MAX_READ_PATH)
			ok = false;
	}
	unmark(m);
	return ok;
}

/* Whether `name` is taken in folder `parent` (index + 1, 0: the root). */
static bool name_taken(const struct s10b *m, uint32_t parent,
		       const uint8_t *name, size_t len)
{
	size_t j;

	for (j = 0; j < m->n_objects; j++) {
		const struct s10b_object *o = obj(m, j);

		if (live(o) && o->parent == parent && o->name_len == len &&
		    memcmp(m->cfg.names + o->name, name, len) == 0)
			return true;
	}
	return false;
}

/* `lost+found` cannot be deleted, moved or renamed (S-10b §9). */
static bool lost_found(const struct s10b *m, const struct s10b_object *o)
{
	return o->level == 1u && is_dir(o) && o->name_len == 10u &&
	       memcmp(m->cfg.names + o->name, "lost+found", 10) == 0;
}

/* The clock's time in seconds since 1970 (S-04.070). */
static uint32_t clock_secs(const struct s10b *m)
{
	const struct s10b_device_ops *d = m->cfg.dev;
	struct s04_datetime t;
	struct s10b_datetime dt;
	uint32_t secs = 0;

	s04_rtc_decode(d->clock_read(d->ctx), &t);
	dt.year = t.year;
	dt.month = t.month;
	dt.day = t.day;
	dt.hour = t.hour;
	dt.minute = t.minute;
	dt.second = t.second;
	(void)s10b_datetime_secs(&dt, &secs);
	return secs;
}

/* Object i's size and times as the volume now has them. */
static void restat(struct s10b *m, size_t i)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	struct s10b_object *o = obj(m, i);
	struct s10b_entry e;

	memset(&e, 0, sizeof(e));
	if (!v->stat(v->ctx, path_of(m, i), &e))
		return;
	if (o->kind == S10B_KIND_FILE)
		o->size = e.size;
	o->mtime = e.mtime;
	o->ctime = e.ctime;
}

static void commit(const struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;

	if (v->commit)
		v->commit(v->ctx);
}

static uint16_t failure(enum s10b_result r)
{
	return r == S10B_V_FULL ? S10B_STORE_FULL : S10B_GENERAL_ERROR;
}

/* --- Selections ----------------------------------------------------------- */

static bool selected(const struct s10b *m, size_t i)
{
	const struct s10b_object *o = obj(m, i);
	bool in;

	if (!live(o))
		return false;
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
	return code == PROP_FILE_NAME || code == 0xDC44u ||
	       code == PROP_DISPLAY_NAME;
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
		return put32(p, parent_handle(m, o));
	case 0xDC41:
		p = put32(p, o->handle);
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

static void respond3(struct s10b *m, uint16_t code, uint32_t a, uint32_t b,
		     uint32_t c)
{
	respond(m, code);
	m->reply_params[0] = a;
	m->reply_params[1] = b;
	m->reply_params[2] = c;
	m->n_reply = 3;
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

static bool level_w(const struct s10b *m)
{
	return m->cfg.level == S10B_LEVEL_W;
}

static bool supported(const struct s10b *m, unsigned code)
{
	size_t i;

	for (i = 0; i < N_OPS_R; i++)
		if (ops_r[i] == code)
			return true;
	for (i = 0; level_w(m) && i < N_OPS_W; i++)
		if (ops_w[i] == code)
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
	return code == OP_SEND_INFO || code == OP_SEND || code == 0x1016u ||
	       code == OP_SET_PROP || code == 0x9811u || code == 0x95C2u;
}

/* S-10b.020 */
static void device_info(struct s10b *m)
{
	uint8_t *p = m->buf;
	size_t i;

	p = put16(p, 100);
	p = put32(p, 6);
	p = put16(p, 100);
	p = put_text(p, "microsoft.com: 1.0;");
	p = put16(p, 0);
	p = put32(p, (uint32_t)(N_OPS_R + (level_w(m) ? N_OPS_W : 0u)));
	for (i = 0; i < N_OPS_R; i++)
		p = put16(p, ops_r[i]);
	for (i = 0; level_w(m) && i < N_OPS_W; i++)
		p = put16(p, ops_w[i]);
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
	/* read-write, or read-only without deletion */
	p = put16(p, level_w(m) ? 0u : 1u);
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
	p = put32(p, parent_handle(m, o));
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
	uint16_t u[S10B_NAME_MAX];
	struct s10b_datetime t;
	size_t n, pos = 0;
	uint32_t count;

	if (m->params[0] == PROP_BATTERY) {
		respond(m, S10B_ACCESS_DENIED);
		return;
	}
	if (m->params[0] != PROP_DATETIME) {
		respond(m, S10B_DEVPROP_NOT_SUPPORTED);
		return;
	}
	if (!take_string(m->data, m->data_len, &pos, u, &n) ||
	    !s10b_datetime_parse(u, n, true, &t) ||
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
	/* get; ObjectFileName get/set at level W */
	*p++ = (uint8_t)(level_w(m) && code == PROP_FILE_NAME ? 1u : 0u);
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

/* --- Changing the volume (level W, S-10b.080 to S-10b.087) ----------------- */

/* A name from an MTP String at d[*pos] (S-10b.080); 0 when it is none. */
static size_t host_name(const uint8_t *d, size_t len, size_t *pos,
			uint8_t *out)
{
	uint16_t u[S10B_NAME_MAX];
	size_t n;

	if (!take_string(d, len, pos, u, &n))
		return 0;
	return s10b_name_utf8(u, n, out);
}

/* S-10b.081, S-10b.085: DateModified's time, 0 when it gives none. */
static uint32_t host_time(const uint16_t *u, size_t n)
{
	struct s10b_datetime t;
	uint32_t secs;

	if (!s10b_datetime_parse(u, n, false, &t) ||
	    (t.year == 1970u && t.month == 1u && t.day == 1u) ||
	    !s10b_datetime_secs(&t, &secs))
		return 0;
	return secs;
}

/* S-10b.080 */
static void send_object_info(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	const uint32_t h = m->params[1];
	uint16_t u[S10B_NAME_MAX];
	uint32_t parent = 0;
	uint64_t total = 0, free_bytes = 0;
	uint8_t name[S10B_NAME_MAX];
	size_t i, len, n, pos = INFO_FIXED;
	enum s10b_result r;
	struct s10b_entry e;

	m->send_ready = false;
	if (m->params[0] != S10B_STORAGE_ID && m->params[0] != 0) {
		respond(m, S10B_INVALID_STORAGE);
		return;
	}
	if (h != 0 && h != S10B_ALL) {
		/* S-10b.087: a folder that lists no children takes none */
		if (!index_of(m, h, &i) || !readable(obj(m, i))) {
			respond(m, S10B_INVALID_PARENT);
			return;
		}
		parent = (uint32_t)i + 1u;
	}
	len = host_name(m->data, m->data_len, &pos, name);
	if (m->data_len < INFO_FIXED || len == 0 ||
	    !take_string(m->data, m->data_len, &pos, u, &n) ||
	    !take_string(m->data, m->data_len, &pos, u, &n)) {
		respond(m, S10B_INVALID_DATASET);
		return;
	}
	if (name_taken(m, parent, name, len)) {
		respond(m, S10B_GENERAL_ERROR);		/* S-10b Q-05 */
		return;
	}
	if (!room_for_object(m, len)) {
		respond(m, S10B_GENERAL_ERROR);		/* S-10b.014 */
		return;
	}
	if (wb_get_le16(m->data + 4) != FORMAT_FOLDER) {
		m->send_size = wb_get_le32(m->data + 8);
		if (!v->space(v->ctx, &total, &free_bytes) ||
		    m->send_size > free_bytes) {
			respond(m, S10B_STORE_FULL);
			return;
		}
		m->send_parent = parent;
		m->send_len = (uint8_t)len;
		memcpy(m->send_name, name, len);
		m->send_mtime = host_time(u, n);
		m->send_handle = new_handle(m);
		m->send_ready = true;
		respond3(m, S10B_OK, S10B_STORAGE_ID,
			 parent ? obj(m, parent - 1u)->handle : S10B_ALL,
			 m->send_handle);
		return;
	}
	r = v->mkdir(v->ctx, child_path(m, parent, name, len, m->path));
	if (r != S10B_V_OK) {
		respond(m, failure(r));
		return;
	}
	memset(&e, 0, sizeof(e));
	if (!v->stat(v->ctx, m->path, &e))
		e.mtime = e.ctime = clock_secs(m);
	e.kind = S10B_KIND_DIR;
	i = add_object(m, parent, name, len, &e, new_handle(m));
	obj(m, i)->flags |= F_READ;		/* it is empty */
	commit(m);
	respond3(m, S10B_OK, S10B_STORAGE_ID,
		 parent ? obj(m, parent - 1u)->handle : S10B_ALL,
		 obj(m, i)->handle);
}

/* The data phase's header is in: the file is created when the length is
 * the one announced (S-10b.081). */
static void send_begin(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;

	if (m->rx_total - S10B_CONTAINER_HEADER != m->send_size) {
		m->w_short = true;
		return;
	}
	(void)child_path(m, m->send_parent, m->send_name, m->send_len,
			 m->w_path);
	m->w_res = v->create(v->ctx, m->w_path, &m->w_fd);
	m->w_open = m->w_res == S10B_V_OK;
}

static void send_flush(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;

	if (m->w_len && m->w_open && m->w_res == S10B_V_OK)
		m->w_res = v->write(v->ctx, m->w_fd, m->w_buf, m->w_len);
	m->w_len = 0;
}

/* File bytes of the data phase, written in chunks as they come. */
static void send_take(struct s10b *m, const uint8_t *p, size_t n)
{
	while (n && m->w_open && m->w_res == S10B_V_OK) {
		size_t k = sizeof(m->w_buf) - m->w_len;

		if (k > n)
			k = n;
		memcpy(m->w_buf + m->w_len, p, k);
		m->w_len += k;
		p += k;
		n -= k;
		if (m->w_len == sizeof(m->w_buf))
			send_flush(m);
	}
}

/* S-10b.081: the whole data phase is in. */
static void send_object(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	struct s10b_entry e;

	if (!m->sending) {
		respond(m, S10B_NO_VALID_OBJECT_INFO);
		return;
	}
	m->sending = false;
	m->send_ready = false;
	if (m->w_short) {
		respond(m, S10B_INCOMPLETE_TRANSFER);	/* nothing created */
		return;
	}
	if (m->w_open) {
		enum s10b_result r;

		send_flush(m);
		r = v->close(v->ctx, m->w_fd);
		m->w_open = false;
		if (m->w_res == S10B_V_OK)
			m->w_res = r;
		if (m->w_res == S10B_V_OK) {
			const uint32_t now = clock_secs(m);

			m->w_res = v->set_times(v->ctx, m->w_path, now,
						m->send_mtime ? m->send_mtime :
								now);
		}
		if (m->w_res != S10B_V_OK) {
			/* no file is left */
			(void)v->remove(v->ctx, m->w_path);
			commit(m);
		}
	}
	if (m->w_res != S10B_V_OK) {
		respond(m, failure(m->w_res));
		return;
	}
	memset(&e, 0, sizeof(e));
	if (!v->stat(v->ctx, m->w_path, &e)) {
		e.size = m->send_size;
		e.mtime = m->send_mtime ? m->send_mtime : clock_secs(m);
		e.ctime = e.mtime;
	}
	e.kind = S10B_KIND_FILE;
	(void)add_object(m, m->send_parent, m->send_name, m->send_len, &e,
			 m->send_handle);
	commit(m);
	respond(m, S10B_OK);
}

/* After a failed delete, the objects beneath it that are gone. */
static void drop_gone(struct s10b *m, size_t i)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	struct s10b_entry e;
	size_t j;

	mark_subtree(m, i);
	for (j = 0; j < m->n_objects; j++) {
		struct s10b_object *o = obj(m, j);

		if ((o->flags & F_MARK) &&
		    !v->stat(v->ctx, path_of(m, j), &e))
			o->flags |= F_DONE;
	}
	/* parents first is not needed: a gone folder's objects are gone */
	for (j = 0; j < m->n_objects; j++)
		if (obj(m, j)->flags & F_DONE)
			free_object(m, j);
	unmark(m);
}

/* S-10b.082 */
static void delete_object(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	enum s10b_result r;
	size_t i, j;

	if (m->params[0] == S10B_ALL) {
		respond(m, S10B_PARAM_NOT_SUPPORTED);
		return;
	}
	if (!index_of(m, m->params[0], &i)) {
		respond(m, S10B_INVALID_HANDLE);
		return;
	}
	if (lost_found(m, obj(m, i))) {
		respond(m, S10B_WRITE_PROTECTED);
		return;
	}
	r = v->remove(v->ctx, path_of(m, i));
	if (r != S10B_V_OK) {
		drop_gone(m, i);
		commit(m);
		respond(m, failure(r));
		return;
	}
	mark_subtree(m, i);
	for (j = 0; j < m->n_objects; j++)
		if (obj(m, j)->flags & F_MARK)
			free_object(m, j);
	unmark(m);
	commit(m);
	respond(m, S10B_OK);
}

/* S-10b.083 */
static void move_object(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	const uint32_t to = m->params[2];
	const struct s10b_object *p = NULL;
	uint32_t parent = 0;
	enum s10b_result r;
	size_t i, k;

	if (m->params[1] != S10B_STORAGE_ID) {
		respond(m, S10B_INVALID_STORAGE);
		return;
	}
	if (!index_of(m, m->params[0], &i)) {
		respond(m, S10B_INVALID_HANDLE);
		return;
	}
	if (to != 0 && to != S10B_ALL) {
		if (!index_of(m, to, &k)) {
			respond(m, S10B_INVALID_HANDLE);
			return;
		}
		mark_subtree(m, i);
		p = obj(m, k);
		if (!is_dir(p) || (p->flags & F_MARK)) {
			unmark(m);
			respond(m, S10B_INVALID_PARENT);	/* S-16.065 */
			return;
		}
		unmark(m);
		parent = (uint32_t)k + 1u;
	}
	if (lost_found(m, obj(m, i))) {
		respond(m, S10B_WRITE_PROTECTED);
		return;
	}
	/* S-10b.087 */
	if ((p && !readable(p)) ||
	    !keeps_listed(m, i, p ? p->level + 1u : 1u,
			  (p ? p->path_len : 0u) + 1u + obj(m, i)->name_len)) {
		respond(m, S10B_INVALID_PARENT);
		return;
	}
	if (name_taken(m, parent, m->cfg.names + obj(m, i)->name,
		       obj(m, i)->name_len)) {
		respond(m, S10B_GENERAL_ERROR);
		return;
	}
	(void)path_of(m, i);
	r = v->rename(v->ctx, m->path,
		      child_path(m, parent, m->cfg.names + obj(m, i)->name,
				 obj(m, i)->name_len, m->w_path));
	if (r != S10B_V_OK) {
		respond(m, failure(r));
		return;
	}
	obj(m, i)->parent = parent;
	replace(m, i);
	restat(m, i);
	fill(m);
	commit(m);
	respond(m, S10B_OK);
}

/* S-10b.084: a rename within the object's folder. */
static void rename_object(struct s10b *m, size_t i)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	struct s10b_object *o = obj(m, i);
	const struct s10b_object *p = o->parent ? obj(m, o->parent - 1u) : NULL;
	uint8_t name[S10B_NAME_MAX];
	enum s10b_result r;
	size_t len, pos = 0;

	if (lost_found(m, o)) {
		respond(m, S10B_WRITE_PROTECTED);
		return;
	}
	len = host_name(m->data, m->data_len, &pos, name);
	/* S-10b.087 */
	if (len == 0 ||
	    !keeps_listed(m, i, o->level,
			  (p ? p->path_len : 0u) + 1u + len)) {
		respond(m, S10B_INVALID_PROP_VALUE);
		return;
	}
	if (name_taken(m, o->parent, name, len)) {
		respond(m, S10B_GENERAL_ERROR);		/* S-10b Q-05 */
		return;
	}
	if (!fit_name(m, len)) {
		respond(m, S10B_GENERAL_ERROR);		/* S-10b.014 */
		return;
	}
	(void)path_of(m, i);
	r = v->rename(v->ctx, m->path,
		      child_path(m, o->parent, name, len, m->w_path));
	if (r != S10B_V_OK) {
		respond(m, failure(r));
		return;
	}
	drop_name(m, i);
	set_name(m, i, name, len);
	replace(m, i);
	restat(m, i);
	fill(m);
	commit(m);
	respond(m, S10B_OK);
}

/* S-10b.085 */
static void set_modified(struct s10b *m, size_t i)
{
	const struct s10b_volume_ops *v = m->cfg.vol;
	struct s10b_object *o = obj(m, i);
	uint16_t u[S10B_NAME_MAX];
	struct s10b_datetime t;
	uint32_t secs;
	size_t n, pos = 0;
	enum s10b_result r;

	if (!take_string(m->data, m->data_len, &pos, u, &n) ||
	    !s10b_datetime_parse(u, n, false, &t) ||
	    !s10b_datetime_secs(&t, &secs)) {
		respond(m, S10B_INVALID_PROP_VALUE);
		return;
	}
	if (secs == o->mtime) {
		respond(m, S10B_OK);		/* S-16.067: nothing written */
		return;
	}
	r = v->set_times(v->ctx, path_of(m, i), clock_secs(m), secs);
	if (r != S10B_V_OK) {
		respond(m, failure(r));
		return;
	}
	o->mtime = secs;
	restat(m, i);
	commit(m);
	respond(m, S10B_OK);
}

/* S-10b.084, S-10b.085 */
static void set_object_prop(struct s10b *m)
{
	size_t i;

	if (!index_of(m, m->params[0], &i)) {
		respond(m, S10B_INVALID_HANDLE);
		return;
	}
	if (name_prop(m->params[1]))
		rename_object(m, i);
	else if (m->params[1] == PROP_MODIFIED)
		set_modified(m, i);
	else
		respond(m, S10B_ACCESS_DENIED);
}

/* --- Sessions ------------------------------------------------------------- */

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
	m->names_dead = 0;
	m->send_ready = false;
}

static void execute(struct s10b *m)
{
	const unsigned code = m->code;
	uint8_t *p;
	size_t i;

	m->rx = S10B_RX_COMMAND;
	if (!supported(m, code) && !capture(code)) {
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
	case OP_DELETE:
		delete_object(m);
		break;
	case OP_SEND_INFO:
		send_object_info(m);
		break;
	case OP_SEND:
		send_object(m);
		break;
	case OP_MOVE:
		move_object(m);
		break;
	case OP_SET_PROP:
		set_object_prop(m);
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
		p = put32(p, obj(m, m->cur)->handle);
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

	s10b_service(m);
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

	s10b_service(m);
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
		/* S-10b.080: only the next command may send the file
		 * announced. */
		if (m->code != OP_SEND)
			m->send_ready = false;
		if (host_data(m->code)) {
			m->rx = S10B_RX_DATA;
			m->rx_header = false;
			m->rx_got = 0;
			m->rx_total = 0;
			m->data_len = 0;
			m->sending = m->code == OP_SEND && level_w(m) &&
				     m->session && m->send_ready;
			m->w_res = S10B_V_OK;
			m->w_short = false;
			m->w_open = false;
			m->w_len = 0;
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
			if (m->sending)
				send_begin(m);
		}
		if (m->sending) {
			const size_t left = m->rx_total > m->rx_got ?
					    m->rx_total - m->rx_got : 0u;

			send_take(m, p + k, n - k < left ? n - k : left);
		} else {
			keep(m, p + k, n - k);
		}
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

/*
 * S-10b.005: a SendObject whose data phase was cut short has no effect.
 * The file it had begun is removed by s10b_service(), not here: this may
 * run where the volume cannot be used.
 */
void s10b_cancel(struct s10b *m)
{
	if (m->rx == S10B_RX_DATA && m->code == OP_SEND)
		m->send_ready = false;
	if (m->w_open)
		m->w_abandon = true;
	m->sending = false;
	m->rx = S10B_RX_COMMAND;
	m->tx = S10B_TX_IDLE;
}

void s10b_service(struct s10b *m)
{
	const struct s10b_volume_ops *v = m->cfg.vol;

	if (!m->w_abandon)
		return;
	m->w_abandon = false;
	if (m->w_open) {
		m->w_open = false;
		(void)v->close(v->ctx, m->w_fd);
		(void)v->remove(v->ctx, m->w_path);
		commit(m);
	}
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
