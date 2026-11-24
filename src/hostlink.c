/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The host link's protocol (waffle/hostlink.h). */
#include "waffle/hostlink.h"

#include "waffle/bytes.h"

/* S-10c.123, S-10c.127: the classification; the doc's table mirrors it. */
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
	/* S-10c.131: warned, not approved */
	{ HL_PSOC_OPEN, HL_WARNED },  { HL_PSOC_DATA, HL_WARNED },
	{ HL_PSOC_CLOSE, HL_WARNED }, { HL_PSOC_STATE, HL_SAFE },
	{ HL_PSOC_RESET, HL_WARNED },
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

/* The drive (yaffs2) is read-write and lists openly; the OS's own FAT
 * volume is read-write and the installed OS's copies read only, both behind
 * the approval even to list. */
const struct hl_root_info hl_roots[HL_ROOTS] = {
	[HL_ROOT_DRIVE] = { "yaffs2", HL_RF_READ | HL_RF_WRITE | HL_RF_OPEN },
	[HL_ROOT_OS] = { "os", HL_RF_READ },
	[HL_ROOT_USER] = { "user", HL_RF_READ | HL_RF_WRITE },
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

/* ---- the PSoC calls (S-10c.131 - S-10c.134) ---- */

static bool psoc_built(const struct hostlink *h)
{
	return h->ops->psoc_warn != NULL;
}

static bool is_psoc_call(uint8_t op)
{
	return op >= HL_PSOC_OPEN && op <= HL_PSOC_RESET;
}

bool hl_psoc_busy(const struct hostlink *h)
{
	const uint8_t s = h->psoc.state;

	return s == HL_PS_COUNTING || s == HL_PS_OPEN || s == HL_PS_RESETTING;
}

void hl_psoc_record(const struct hostlink *h, uint8_t *out)
{
	uint32_t over = h->psoc.dropped;

	if (h->psoc.state == HL_PS_OPEN)
		over += h->ops->psoc_lost(h->ops->ctx) - h->psoc.lost_at;
	out[0] = h->psoc.state;
	out[1] = h->psoc.what;
	out[2] = h->psoc.seconds;
	out[3] = h->psoc.flags;
	wb_put_le32(out + 4, over);
	wb_put_le16(out + 8, h->psoc.count);
}

static void psoc_event(struct hostlink *h)
{
	uint8_t p[3u + HL_PSOC_REC];

	p[0] = HL_PSOC_EVENT;
	p[1] = 0u;
	p[2] = HL_OK;
	hl_psoc_record(h, p + 3);
	h->ops->psoc_event(h->ops->ctx, p, sizeof p);
}

/* S-10c.132: what the PSoC sent, into the ring; a byte it has no room for
 * is counted, never dropped unsaid. Bounded: at most twice the ring a call. */
static void psoc_drain(struct hostlink *h)
{
	uint8_t b[64];

	if (h->psoc.state != HL_PS_OPEN)
		return;
	for (unsigned k = 0; k < 2u * HL_PSOC_RING / sizeof b; k++) {
		const size_t n = h->ops->psoc_read(h->ops->ctx, b, sizeof b);
		if (n == 0u)
			return;
		for (size_t i = 0; i < n; i++) {
			const unsigned at = (unsigned)h->psoc.head + (unsigned)h->psoc.count;

			if ((unsigned)h->psoc.count == HL_PSOC_RING) {
				h->psoc.dropped++;
				continue;
			}
			h->psoc.ring[at % HL_PSOC_RING] = b[i];
			h->psoc.count = (uint16_t)((unsigned)h->psoc.count + 1u);
		}
	}
}

static size_t psoc_take(struct hostlink *h, uint8_t *d, size_t max)
{
	size_t n = 0;

	while (n < max && h->psoc.count != 0u) {
		d[n++] = h->psoc.ring[h->psoc.head];
		h->psoc.head = (uint16_t)(((unsigned)h->psoc.head + 1u) % HL_PSOC_RING);
		h->psoc.count = (uint16_t)((unsigned)h->psoc.count - 1u);
	}
	return n;
}

/*
 * S-10c.132: a countdown is canceled with its screen taken down; an open
 * bridge ends with the link framed again and the PSoC greeted. What the
 * ring still holds is counted as lost.
 */
static void psoc_end(struct hostlink *h)
{
	const struct hl_ops *o = h->ops;

	if (h->psoc.state == HL_PS_COUNTING) {
		(void)o->psoc_leave(o->ctx, false);
		h->psoc.state = HL_PS_CANCELED;
		h->psoc.seconds = 0;
		psoc_event(h);
		return;
	}
	if (h->psoc.state != HL_PS_OPEN)
		return;
	psoc_drain(h);
	h->psoc.dropped += o->psoc_lost(o->ctx) - h->psoc.lost_at + (uint32_t)h->psoc.count;
	h->psoc.head = h->psoc.count = 0;
	h->psoc.state = HL_PS_ENDED;
	if (o->psoc_leave(o->ctx, true))
		h->psoc.flags |= HL_PF_GREETED;
	psoc_event(h);
}

/* Whole seconds left of a span of total ms of which el have passed. */
static uint8_t seconds_left(uint32_t total, uint32_t el)
{
	return (uint8_t)((total - el + 999u) / 1000u);
}

/*
 * S-10c.133: the reset request sent alone, the unit running on. Whatever
 * else happens meanwhile, the wait ends at its bound; a unit still running
 * then was not reset, and says so.
 */
static void psoc_reset_wait(struct hostlink *h)
{
	const struct hl_ops *o = h->ops;
	const uint32_t el = o->now_ms(o->ctx) - h->psoc.start_ms;

	if (el < HL_PSOC_RESET_WAIT_MS) {
		const uint8_t s = seconds_left(HL_PSOC_RESET_WAIT_MS, el);

		if (s != h->psoc.seconds) {
			h->psoc.seconds = s;
			psoc_event(h);
		}
		return;
	}
	h->psoc.seconds = 0;
	h->psoc.flags |= HL_PF_NOT_RESET;
	h->psoc.state = HL_PS_ENDED;
	(void)o->psoc_leave(o->ctx, false);
	psoc_event(h);
}

/* S-10c.133: a PSoC that has greeted takes the plain restart's request; one
 * that has not gets the request alone, the unit not torn down for it. */
static void psoc_reset_now(struct hostlink *h)
{
	const struct hl_ops *o = h->ops;

	h->psoc.state = HL_PS_RESETTING;
	if (o->psoc_greeted(o->ctx)) {
		psoc_event(h);
		o->psoc_reset(o->ctx);
		return;
	}
	h->psoc.flags |= HL_PF_RESET_ALONE;
	h->psoc.start_ms = o->now_ms(o->ctx);
	h->psoc.seconds = seconds_left(HL_PSOC_RESET_WAIT_MS, 0);
	o->psoc_reset_alone(o->ctx);
	psoc_event(h);
}

/* S-10c.131: the countdown has run out, or runs on. */
static void psoc_step(struct hostlink *h)
{
	const struct hl_ops *o = h->ops;
	uint32_t el;

	if (!psoc_built(h))
		return;
	if (h->psoc.state == HL_PS_RESETTING &&
	    (h->psoc.flags & HL_PF_RESET_ALONE) != 0u) {
		psoc_reset_wait(h);
		return;
	}
	if (h->psoc.state != HL_PS_COUNTING && h->psoc.state != HL_PS_OPEN)
		return;
	if (!o->link_on(o->ctx) || o->psoc_gone(o->ctx)) {
		psoc_end(h);
		return;
	}
	if (h->psoc.state == HL_PS_OPEN) {
		psoc_drain(h);
		return;
	}
	if (o->psoc_canceled(o->ctx)) {
		h->psoc.state = HL_PS_CANCELED;
		h->psoc.seconds = 0;
		psoc_event(h);
		return;
	}
	el = o->now_ms(o->ctx) - h->psoc.start_ms;
	if (el < HL_PSOC_COUNT_MS) {
		const uint8_t s = seconds_left(HL_PSOC_COUNT_MS, el);

		if (s != h->psoc.seconds) {
			h->psoc.seconds = s;
			(void)o->psoc_warn(o->ctx, h->psoc.what, s);
			psoc_event(h);
		}
		return;
	}
	h->psoc.seconds = 0;
	if (h->psoc.what == HL_PW_RESET) {
		psoc_reset_now(h);
		return;
	}
	o->psoc_enter(o->ctx, h->psoc.what == HL_PW_BRIDGE_ENTRY);
	h->psoc.lost_at = o->psoc_lost(o->ctx);
	h->psoc.state = HL_PS_OPEN;
	psoc_event(h);
}

/* OPEN and RESET: the warning up, then the countdown. */
static int psoc_ask(struct hostlink *h, uint8_t what)
{
	const struct hl_ops *o = h->ops;
	const bool greeted = o->psoc_greeted(o->ctx);
	int rc;

	if (hl_psoc_busy(h) || h->job.state == HL_J_QUEUED || h->job.state == HL_J_RUNNING)
		return HL_E_BUSY;
	rc = o->psoc_warn(o->ctx, what, (uint8_t)(HL_PSOC_COUNT_MS / 1000u));
	if (rc != HL_OK)
		return rc;
	h->psoc.state = HL_PS_COUNTING;
	h->psoc.what = what;
	h->psoc.flags = greeted ? HL_PF_GREETED_AT : 0u;
	h->psoc.seconds = seconds_left(HL_PSOC_COUNT_MS, 0);
	h->psoc.start_ms = o->now_ms(o->ctx);
	h->psoc.dropped = 0;
	h->psoc.head = h->psoc.count = 0;
	return HL_OK;
}

static int psoc_call(struct hostlink *h, uint8_t op, const uint8_t *a, size_t n,
		     uint8_t *d, size_t *dn)
{
	const struct hl_ops *o = h->ops;
	uint8_t rx[HL_PSOC_RX_MAX];
	size_t k = 0;
	int rc = HL_OK;

	switch (op) {
	case HL_PSOC_OPEN:
		if (n != 1u || a[0] > 1u)
			return HL_E_BAD;
		rc = psoc_ask(h, a[0] != 0u ? HL_PW_BRIDGE_ENTRY : HL_PW_BRIDGE);
		break;
	case HL_PSOC_RESET:
		if (n != 0u)
			return HL_E_BAD;
		rc = psoc_ask(h, HL_PW_RESET);
		break;
	case HL_PSOC_DATA:
		if (h->psoc.state != HL_PS_OPEN)
			return HL_E_NOT_OPEN;
		/* the computer's bytes first, then what the PSoC has sent */
		psoc_drain(h);
		if (n != 0u && !o->psoc_write(o->ctx, a, n))
			rc = HL_E_BUSY;		/* nothing of them written */
		psoc_drain(h);
		k = psoc_take(h, rx, sizeof rx);
		break;
	case HL_PSOC_CLOSE:
		if (n != 0u)
			return HL_E_BAD;
		psoc_drain(h);
		k = psoc_take(h, rx, sizeof rx);
		psoc_end(h);
		break;
	case HL_PSOC_STATE:
	default:
		if (n != 0u)
			return HL_E_BAD;
		break;
	}
	if (rc != HL_OK && op != HL_PSOC_DATA)
		return rc;
	hl_psoc_record(h, d);
	wb_copy(d + HL_PSOC_REC, rx, k);
	*dn = HL_PSOC_REC + k;
	return rc;
}

void hl_end(struct hostlink *h)
{
	put_drop(h);
	h->session = HL_S_NONE;
	if (psoc_built(h))
		psoc_end(h);
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
	psoc_step(h);
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
	d[n] = 0;
	for (size_t i = 0; i < hl_ncalls; i++)
		if (psoc_built(h) || !is_psoc_call(hl_calls[i].id))
			d[n + 1u + d[n]++] = hl_calls[i].id;
	n += 1u + d[n];
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
	h->put.path.rest = h->put.path.full + (p.rest - p.full);
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
	if (hl_psoc_busy(h))
		return HL_E_BUSY;		/* S-10c.134 */
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
	uint32_t got = 0;
	uint8_t cls = hl_class_of(hl_calls, hl_ncalls, op);
	int rc = HL_OK;

	p.root = HL_ROOT_DRIVE;

	if (cls == 0xFFu || (is_psoc_call(op) && !psoc_built(h)))
		return HL_E_UNKNOWN;
	/* S-10c.131: the warned class asks for no approval */
	if (cls == HL_WARNED || is_psoc_call(op))
		return psoc_call(h, op, a, n, d, dn);
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
	/* S-10c.134: while the PSoC is taken, only the session's own calls */
	if (hl_psoc_busy(h) && op != HL_SESSION && op != HL_END && op != HL_INFO &&
	    op != HL_JOB)
		return HL_E_BUSY;
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

		const uint32_t off = wb_get_le32(a);

		if ((hl_roots[p.root].flags & HL_RF_READ) == 0u)
			return HL_E_REFUSED;
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
