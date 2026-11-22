/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The host link's protocol (waffle/hostlink.h). */
#include "waffle/hostlink.h"

#include "waffle/bytes.h"

/* SPEC-Q: the classification; the doc's table mirrors it. */
const struct hl_entry hl_calls[] = {
	{ HL_HELLO, HL_SAFE },      { HL_SESSION, HL_SAFE },
	{ HL_APPROVE, HL_SAFE },    { HL_END, HL_SAFE },
	{ HL_INFO, HL_SAFE },       { HL_SET, HL_NEEDS },
	{ HL_JOB, HL_SAFE },
	{ HL_STAT, HL_BY_ROOT },    { HL_LIST, HL_BY_ROOT },
	{ HL_GET, HL_NEEDS },       { HL_HASH, HL_NEEDS },
	{ HL_PUT_OPEN, HL_NEEDS },  { HL_PUT_DATA, HL_NEEDS },
	{ HL_PUT_END, HL_NEEDS },   { HL_REMOVE, HL_NEEDS },
	{ HL_MKDIR, HL_NEEDS },     { HL_RENAME, HL_NEEDS },
	{ HL_RUN, HL_NEEDS },
};
const size_t hl_ncalls = sizeof hl_calls / sizeof hl_calls[0];

const struct hl_entry hl_actions[] = {
	{ HL_A_INSTALL, HL_NEEDS },        { HL_A_OS_WRITE, HL_NEEDS },
	{ HL_A_OS_ONCE, HL_NEEDS },        { HL_A_RESTART, HL_NEEDS },
	{ HL_A_CONTINUE_OS, HL_NEEDS },    { HL_A_RESTART_MODE, HL_NEEDS },
	{ HL_A_FACTORY_RESET, HL_NEVER_C }, { HL_A_PROTECTED_WRITES, HL_NEVER_C },
};
const size_t hl_nactions = sizeof hl_actions / sizeof hl_actions[0];

const struct hl_entry hl_settings[] = {
	{ HL_SET_PROTECTED, HL_NEVER_C }, { HL_SET_HOST_LINK, HL_NEEDS },
};
const size_t hl_nsettings = sizeof hl_settings / sizeof hl_settings[0];

/* The drive (yaffs2) is read-write and lists openly; the installed OS's
 * copies and the OS's own FAT volume are read only, behind the approval. */
const struct hl_root_info hl_roots[HL_ROOTS] = {
	[HL_ROOT_DRIVE] = { "yaffs2", HL_RF_READ | HL_RF_WRITE | HL_RF_OPEN },
	[HL_ROOT_OS] = { "os", HL_RF_READ },
	[HL_ROOT_USER] = { "user", HL_RF_READ },
};

uint8_t hl_class_of(const struct hl_entry *t, size_t n, uint8_t id)
{
	for (size_t i = 0; i < n; i++)
		if (t[i].id == id)
			return t[i].cls;
	return 0xFFu;
}

/* ---- the path check ---- */

static bool byte_ok(uint8_t c)
{
	return c >= 0x20u && c != 0x7Fu && c != '\\' && c != ':';
}

int hl_path_parse(const uint8_t *p, size_t n, struct hl_path *out)
{
	size_t i, start, rn;
	unsigned r;

	if (n < 2u || n > HL_PATH_MAX || p[0] != '/')
		return HL_E_PATH;
	for (i = 0; i < n; i++)
		if (!byte_ok(p[i]))
			return HL_E_PATH;
	for (rn = 1; rn < n && p[rn] != '/'; rn++)
		;
	for (r = 0; r < HL_ROOTS; r++) {
		const size_t k = wb_strlen(hl_roots[r].name);

		if (k == rn - 1u && wb_equal(p + 1, (const uint8_t *)hl_roots[r].name, k))
			break;
	}
	if (r == HL_ROOTS)
		return HL_E_PATH;
	/* components after the root: none empty, none "." or "..", <= 255 */
	for (start = rn + 1u; rn < n; start = i + 1u) {
		for (i = start; i < n && p[i] != '/'; i++)
			;
		if (i == start || i - start > 255u ||
		    (i - start == 1u && p[start] == '.') ||
		    (i - start == 2u && p[start] == '.' && p[start + 1u] == '.'))
			return HL_E_PATH;
		if (i >= n)
			break;
	}
	out->root = (uint8_t)r;
	wb_copy((uint8_t *)out->full, p, n);
	out->full[n] = '\0';
	rn = 1u + wb_strlen(hl_roots[r].name);
	out->rest = out->full + (rn < n ? rn + 1u : rn);
	return HL_OK;
}

/* ---- the session ---- */

void hl_init(struct hostlink *h, const struct hl_ops *ops)
{
	wb_fill((uint8_t *)h, 0, sizeof(*h));
	h->ops = ops;
}

bool hl_approved(const struct hostlink *h)
{
	return h->session == HL_S_APPROVED && h->ops->link_on(h->ops->ctx);
}

static void put_drop(struct hostlink *h)
{
	if (!h->put.open)
		return;
	h->put.open = false;
	h->ops->abandon(h->ops->ctx);
	(void)h->ops->remove(h->ops->ctx, &h->put.tmp);
}

void hl_end(struct hostlink *h)
{
	put_drop(h);
	h->session = HL_S_NONE;
}

void hl_answer(struct hostlink *h, uint8_t outcome)
{
	if (h->session == HL_S_PENDING)
		h->session = outcome;
}

void hl_poll(struct hostlink *h)
{
	if (h->session != HL_S_NONE && !h->ops->link_on(h->ops->ctx))
		hl_end(h);
}

/* ---- jobs ---- */

bool hl_job_set(struct hostlink *h, uint8_t state, uint8_t step, uint8_t percent,
		uint8_t error, const char *text)
{
	struct hl_job *j = &h->job;
	size_t n = 0;
	bool same = j->state == state && j->step == step && j->percent == percent &&
		    j->error == error;

	while (text != NULL && text[n] != '\0' && n < HL_TEXT_MAX) {
		same = same && j->text[n] == text[n];
		n++;
	}
	same = same && j->text[n] == '\0';
	if (same)
		return false;
	j->state = state;
	j->step = step;
	j->percent = percent;
	j->error = error;
	wb_copy((uint8_t *)j->text, (const uint8_t *)(text != NULL ? text : ""), n);
	j->text[n] = '\0';
	return true;
}

/* id action state step percent error, then the text and its NUL. */
static size_t job_record(const struct hl_job *j, uint8_t *out)
{
	const size_t n = wb_strlen(j->text);

	out[0] = j->id;
	out[1] = j->action;
	out[2] = j->state;
	out[3] = j->step;
	out[4] = j->percent;
	out[5] = j->error;
	wb_copy(out + 6, (const uint8_t *)j->text, n + 1u);
	return 7u + n;
}

size_t hl_event(const struct hostlink *h, uint8_t *out)
{
	out[0] = HL_EVENT;
	out[1] = 0u;
	out[2] = HL_OK;
	return 3u + job_record(&h->job, out + 3);
}

/* ---- the calls ---- */

/* The entries of a list reply: type:1 size:4 len:1 name, while they fit. */
struct list_fill {
	uint8_t *out;
	size_t n, cap;
	uint32_t taken;
};

static bool list_each(void *arg, const char *name, const struct hl_stat *st)
{
	struct list_fill *l = arg;
	const size_t k = wb_strlen(name);

	if (k > 255u || l->n + 6u + k > l->cap)
		return false;
	l->out[l->n] = st->type;
	wb_put_le32(l->out + l->n + 1u, st->size);
	l->out[l->n + 5u] = (uint8_t)k;
	wb_copy(l->out + l->n + 6u, (const uint8_t *)name, k);
	l->n += 6u + k;
	l->taken++;
	return true;
}

static size_t hello(struct hostlink *h, uint8_t *d)
{
	const struct hl_ops *o = h->ops;
	size_t n;

	wb_put_le32(d, HL_MAGIC);
	d[4] = HL_VERSION;
	d[5] = HL_MIN_CLIENT;
	d[6] = o->link_on(o->ctx) ? 1u : 0u;
	if (d[6] == 0u)
		return 7u;		/* off: nothing beyond saying so */
	d[7] = h->session;
	d[8] = HL_PAYLOAD;
	d[9] = HL_CHUNK;
	o->info(o->ctx, d + 10);
	n = 10u + HL_INFO_LEN;
	d[n++] = (uint8_t)hl_ncalls;
	for (size_t i = 0; i < hl_ncalls; i++)
		d[n++] = hl_calls[i].id;
	d[n] = 0;
	for (size_t i = 0; i < hl_nactions; i++)
		if (hl_actions[i].cls != HL_NEVER_C)
			d[n + 1u + d[n]++] = hl_actions[i].id;
	n += 1u + d[n];
	d[n] = 0;
	for (unsigned r = 0; r < HL_ROOTS; r++)
		if (o->root_ok(o->ctx, r))
			d[n]++;
	n++;
	for (unsigned r = 0; r < HL_ROOTS; r++) {
		const size_t k = wb_strlen(hl_roots[r].name);

		if (!o->root_ok(o->ctx, r))
			continue;
		d[n++] = (uint8_t)r;
		d[n++] = hl_roots[r].flags;
		wb_copy(d + n, (const uint8_t *)hl_roots[r].name, k + 1u);
		n += k + 1u;
	}
	return n;
}

/* A path call's path, at a and to the request's end. */
static int path_of(struct hostlink *h, const uint8_t *a, size_t n, struct hl_path *p)
{
	int rc = hl_path_parse(a, n, p);

	if (rc == HL_OK && !h->ops->root_ok(h->ops->ctx, p->root))
		rc = HL_E_PATH;
	return rc;
}

static bool writable(const struct hl_path *p)
{
	return (hl_roots[p->root].flags & HL_RF_WRITE) != 0u;
}

static int put_open(struct hostlink *h, const uint8_t *a, size_t n, uint8_t *d, size_t *dn)
{
	const struct hl_ops *o = h->ops;
	struct hl_path p;
	uint32_t size;
	size_t k;
	int rc;

	if (n < 37u)
		return HL_E_BAD;
	rc = path_of(h, a + 36, n - 36u, &p);
	if (rc != HL_OK)
		return rc;
	if (!writable(&p))
		return HL_E_READ_ONLY;
	if (*p.rest == '\0')
		return HL_E_PATH;
	size = wb_get_le32(a);
	/* the same put again: resumed where it stands */
	if (h->put.open && h->put.size == size && wb_equal(h->put.sha, a + 4, 32) &&
	    wb_strlen(h->put.path.full) == n - 36u &&
	    wb_equal((const uint8_t *)h->put.path.full, a + 36, n - 36u)) {
		wb_put_le32(d, h->put.have);
		*dn = 4u;
		return HL_OK;
	}
	put_drop(h);
	k = n - 36u;
	if (k + sizeof(HL_PUT_SUFFIX) - 1u > HL_PATH_MAX)
		return HL_E_PATH;
	h->put.path = p;
	h->put.tmp = p;
	wb_copy((uint8_t *)h->put.tmp.full + k, (const uint8_t *)HL_PUT_SUFFIX,
		sizeof(HL_PUT_SUFFIX));
	h->put.tmp.rest = h->put.tmp.full + (p.rest - p.full);
	rc = o->create(o->ctx, &h->put.tmp);
	if (rc != HL_OK)
		return rc;
	h->put.open = true;
	h->put.size = size;
	h->put.have = 0;
	wb_copy(h->put.sha, a + 4, 32);
	wb_put_le32(d, 0u);
	*dn = 4u;
	return HL_OK;
}

static int put_data(struct hostlink *h, const uint8_t *a, size_t n, uint8_t *d, size_t *dn)
{
	const uint32_t k = n >= 4u ? (uint32_t)(n - 4u) : 0u;
	int rc;

	if (!h->put.open || n < 4u || k > HL_CHUNK)
		return HL_E_BAD;
	wb_put_le32(d, h->put.have);
	*dn = 4u;
	if (wb_get_le32(a) != h->put.have)
		return HL_E_OFFSET;
	if (k > h->put.size - h->put.have)
		return HL_E_BAD;
	rc = h->ops->write(h->ops->ctx, a + 4, k);
	if (rc != HL_OK) {
		put_drop(h);
		*dn = 0;
		return rc;
	}
	h->put.have += k;
	wb_put_le32(d, h->put.have);
	return HL_OK;
}

/* Finished, then read back whole and checked; only then renamed into
 * place. A file that does not check is removed. */
static int put_end(struct hostlink *h, uint8_t *d, size_t *dn)
{
	const struct hl_ops *o = h->ops;
	uint8_t sha[32];
	uint32_t size = 0;
	int rc;

	if (!h->put.open)
		return HL_E_BAD;
	if (h->put.have != h->put.size) {
		wb_put_le32(d, h->put.have);
		*dn = 4u;
		return HL_E_OFFSET;
	}
	h->put.open = false;
	rc = o->finish(o->ctx);
	if (rc == HL_OK)
		rc = o->hash(o->ctx, &h->put.tmp, sha, &size);
	if (rc == HL_OK && (size != h->put.size || !wb_equal(sha, h->put.sha, 32)))
		rc = HL_E_CHECKSUM;
	if (rc == HL_OK)
		rc = o->rename(o->ctx, &h->put.tmp, &h->put.path);
	if (rc != HL_OK)
		(void)o->remove(o->ctx, &h->put.tmp);
	return rc;
}

static int run(struct hostlink *h, const uint8_t *a, size_t n, uint8_t *d, size_t *dn)
{
	struct hl_path p;
	const struct hl_path *pp = NULL;
	uint8_t arg = 0, cls;
	size_t at = 1;
	int rc;

	if (n < 1u)
		return HL_E_BAD;
	cls = hl_class_of(hl_actions, hl_nactions, a[0]);
	if (cls == 0xFFu)
		return HL_E_UNKNOWN;
	if (cls == HL_NEVER_C)
		return HL_E_NEVER;
	if (!hl_approved(h))
		return HL_E_APPROVAL;
	if (a[0] == HL_A_INSTALL || a[0] == HL_A_RESTART_MODE) {
		if (n < 2u)
			return HL_E_BAD;
		arg = a[1];
		at = 2;
	}
	if (a[0] == HL_A_INSTALL || a[0] == HL_A_OS_WRITE) {
		rc = path_of(h, a + at, n - at, &p);
		if (rc != HL_OK)
			return rc;
		if (p.root != HL_ROOT_DRIVE)
			return HL_E_PATH;
		pp = &p;
	} else if (n != at) {
		return HL_E_BAD;
	}
	if (h->job.state == HL_J_QUEUED || h->job.state == HL_J_RUNNING)
		return HL_E_BUSY;
	rc = h->ops->run(h->ops->ctx, a[0], arg, pp);
	if (rc != HL_OK)
		return rc;
	h->job.id = ++h->next_job == 0u ? ++h->next_job : h->next_job;
	h->job.action = a[0];
	(void)hl_job_set(h, HL_J_QUEUED, 0, HL_PERCENT_NONE, HL_OK, "");
	*dn = job_record(&h->job, d);
	return HL_OK;
}

static int call(struct hostlink *h, uint8_t op, const uint8_t *a, size_t n,
		uint8_t *d, size_t *dn)
{
	const struct hl_ops *o = h->ops;
	struct hl_path p, q;
	struct hl_stat st;
	uint32_t off, got = 0;
	uint8_t cls = hl_class_of(hl_calls, hl_ncalls, op);
	int rc = HL_OK;

	p.root = HL_ROOT_DRIVE;

	if (cls == 0xFFu)
		return HL_E_UNKNOWN;
	/* the path, where the call names one, and the class it gives */
	switch (op) {
	case HL_STAT: case HL_HASH: case HL_REMOVE: case HL_MKDIR:
		rc = path_of(h, a, n, &p);
		break;
	case HL_LIST:
		rc = n < 2u ? HL_E_BAD : path_of(h, a + 2, n - 2u, &p);
		break;
	case HL_GET:
		rc = n < 5u ? HL_E_BAD : path_of(h, a + 5, n - 5u, &p);
		break;
	default:
		break;
	}
	if (rc != HL_OK)
		return rc;
	if (cls == HL_BY_ROOT)
		cls = (hl_roots[p.root].flags & HL_RF_OPEN) != 0u ? HL_SAFE : HL_NEEDS;
	if (op == HL_RUN)
		return run(h, a, n, d, dn);	/* its action's class */
	if (op == HL_SET && n == 2u &&
	    hl_class_of(hl_settings, hl_nsettings, a[0]) == HL_NEVER_C)
		return HL_E_NEVER;
	if (cls == HL_NEEDS && !hl_approved(h))
		return HL_E_APPROVAL;

	switch (op) {
	case HL_SESSION:
		d[0] = h->session;
		*dn = 1;
		break;
	case HL_APPROVE:
		if (h->session != HL_S_APPROVED && h->session != HL_S_PENDING) {
			if (!o->approve_open(o->ctx))
				return HL_E_BUSY;
			h->session = HL_S_PENDING;
		}
		d[0] = h->session;
		*dn = 1;
		break;
	case HL_END:
		hl_end(h);
		break;
	case HL_INFO:
		o->info(o->ctx, d);
		*dn = HL_INFO_LEN;
		break;
	case HL_SET:
		if (n != 2u)
			return HL_E_BAD;
		if (hl_class_of(hl_settings, hl_nsettings, a[0]) == 0xFFu)
			return HL_E_UNKNOWN;
		rc = o->set(o->ctx, a[0], a[1]);
		break;
	case HL_JOB:
		if (n != 1u || h->job.id == 0u || a[0] != h->job.id)
			return n != 1u ? HL_E_BAD : HL_E_NOT_FOUND;
		*dn = job_record(&h->job, d);
		break;
	case HL_STAT:
		rc = o->stat(o->ctx, &p, &st);
		if (rc == HL_OK) {
			d[0] = st.type;
			wb_put_le32(d + 1, st.size);
			wb_put_le32(d + 5, st.mtime);
			*dn = 9;
		}
		break;
	case HL_LIST: {
		struct list_fill l = { d + 2, 0, HL_PAYLOAD - 5u, 0 };
		const uint32_t start = (uint32_t)a[0] | (uint32_t)a[1] << 8;
		bool end = false;

		rc = o->list(o->ctx, &p, start, list_each, &l, &end);
		if (rc == HL_OK) {
			const uint32_t next = start + l.taken;

			if (!end && l.taken == 0u)
				return HL_E_BAD;	/* an entry too long for a frame */
			wb_put_le16(d, end || next >= 0xFFFFu ? 0xFFFFu : (uint16_t)next);
			*dn = 2u + l.n;
		}
		break;
	}
	case HL_GET: {
		uint32_t k = a[4] == 0u || a[4] > HL_CHUNK ? HL_CHUNK : a[4];

		if ((hl_roots[p.root].flags & HL_RF_READ) == 0u)
			return HL_E_REFUSED;
		off = wb_get_le32(a);
		rc = o->read(o->ctx, &p, off, d + 4, k, &got);
		if (rc == HL_OK) {
			wb_put_le32(d, off);
			*dn = 4u + got;
		}
		break;
	}
	case HL_HASH:
		rc = o->hash(o->ctx, &p, d + 4, &got);
		if (rc == HL_OK) {
			wb_put_le32(d, got);
			*dn = 36;
		}
		break;
	case HL_PUT_OPEN:
		rc = put_open(h, a, n, d, dn);
		break;
	case HL_PUT_DATA:
		rc = put_data(h, a, n, d, dn);
		break;
	case HL_PUT_END:
		rc = put_end(h, d, dn);
		break;
	case HL_REMOVE:
	case HL_MKDIR:
		if (!writable(&p))
			return HL_E_READ_ONLY;
		if (*p.rest == '\0')
			return HL_E_PATH;	/* never the root itself */
		rc = op == HL_REMOVE ? o->remove(o->ctx, &p) : o->mkdir(o->ctx, &p);
		break;
	case HL_RENAME: {
		/* from, NUL, to; both in one root, never across */
		size_t z = 0;

		while (z < n && a[z] != 0u)
			z++;
		if (z >= n)
			return HL_E_BAD;
		rc = path_of(h, a, z, &p);
		if (rc == HL_OK)
			rc = path_of(h, a + z + 1u, n - z - 1u, &q);
		if (rc != HL_OK)
			return rc;
		if (p.root != q.root || *p.rest == '\0' || *q.rest == '\0')
			return HL_E_PATH;
		if (!writable(&p))
			return HL_E_READ_ONLY;
		if (o->stat(o->ctx, &q, &st) == HL_OK)
			return HL_E_EXISTS;	/* no name in the way is replaced */
		rc = o->rename(o->ctx, &p, &q);
		break;
	}
	default:
		return HL_E_UNKNOWN;
	}
	return rc;
}

size_t hl_request(struct hostlink *h, const uint8_t *req, size_t n, uint8_t *out)
{
	size_t dn = 0;
	int rc;

	out[0] = n >= 1u ? req[0] : 0u;
	out[1] = n >= 2u ? req[1] : 0u;
	if (n < 2u) {
		out[2] = HL_E_BAD;
		return 3u;
	}
	if (req[0] == HL_HELLO) {
		out[2] = HL_OK;
		return 3u + hello(h, out + 3);
	}
	if (!h->ops->link_on(h->ops->ctx)) {
		out[2] = HL_E_OFF;
		return 3u;
	}
	rc = call(h, req[0], req + 2, n - 2u, out + 3, &dn);
	out[2] = (uint8_t)rc;
	return 3u + dn;
}
