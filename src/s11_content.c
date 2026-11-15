/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "waffle/s11_content.h"

#include "waffle/bytes.h"

#define STAGED_CONTENT "upgrade/firmware/blackfin/content"
#define STAGED_DLC STAGED_CONTENT "/dlc"
#define MARKER "partial_upgrade"
#define MANIFEST_MAX 4096u

static bool contains(const char *s, const char *sub)
{
	size_t n = wb_strlen(sub);

	for (; *s != '\0'; s++) {
		size_t k = 0;

		while (k < n && s[k] == sub[k])
			k++;
		if (k == n)
			return true;
	}
	return false;
}

/* A path built up and taken down one component at a time. */
struct path {
	char buf[S11_PATH_MAX];
	size_t len;
};

static bool path_set(struct path *p, const char *s)
{
	p->len = wb_strlen(s);
	if (p->len >= S11_PATH_MAX)
		return false;
	for (size_t i = 0; i <= p->len; i++)
		p->buf[i] = s[i];
	return true;
}

/* Appends "/name"; returns the length to restore, or 0 when too long. */
static size_t path_push(struct path *p, const char *name)
{
	size_t old = p->len, n = wb_strlen(name);

	if (old + 1 + n >= S11_PATH_MAX)
		return 0;
	p->buf[old] = '/';
	for (size_t i = 0; i <= n; i++)
		p->buf[old + 1 + i] = name[i];
	p->len = old + 1 + n;
	return old;
}

static void path_pop(struct path *p, size_t old)
{
	p->len = old;
	p->buf[old] = '\0';
}

static bool dot_or_dotdot(const char *n)
{
	return n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'));
}

static bool is_dir(const struct s11_fs_ops *fs, const char *path)
{
	bool dir = false;
	uint32_t size = 0;

	return fs->stat(fs->ctx, path, &dir, &size) && dir;
}

static bool exists(const struct s11_fs_ops *fs, const char *path)
{
	bool dir = false;
	uint32_t size = 0;

	return fs->stat(fs->ctx, path, &dir, &size);
}

/* ---- the partial marker (S-11.040) ---- */

static bool partial_in(const struct s11_fs_ops *fs, struct path *src,
		       size_t rel)
{
	char name[S11_PATH_MAX];
	bool dir, found = false;

	for (size_t i = 0; !found && fs->list(fs->ctx, src->buf, i, name,
					      sizeof(name), &dir); i++) {
		size_t old;

		if (dot_or_dotdot(name))
			continue;
		old = path_push(src, name);
		if (old == 0)
			continue;
		found = dir ? partial_in(fs, src, rel) :
			      contains(src->buf + rel, MARKER);
		path_pop(src, old);
	}
	return found;
}

bool s11_content_partial(const struct s11_fs_ops *fs)
{
	struct path src;

	(void)path_set(&src, STAGED_CONTENT);
	return partial_in(fs, &src, src.len + 1);
}

/* ---- the content step ---- */

struct walk {
	const struct s11_fs_ops *fs;
	struct path src, dst;
	size_t rel;			/* where the relative path starts in src */
	unsigned moved;
};

/* S-11.079: every file but the markers, moved to /yaffs2/content/R. A file
 * that leaves the staging area leaves the listing, so the same index then
 * names the next entry. */
static bool install_dir(struct walk *w)
{
	char name[S11_PATH_MAX];
	bool dir;
	size_t i = 0;

	while (w->fs->list(w->fs->ctx, w->src.buf, i, name, sizeof(name), &dir)) {
		size_t so, dold;
		bool ok = true, moved = false;

		if (dot_or_dotdot(name)) {	/* every other name, ".a" too */
			i++;
			continue;
		}
		so = path_push(&w->src, name);
		dold = path_push(&w->dst, name);
		if (so == 0 || dold == 0)
			return false;
		if (dir) {
			ok = install_dir(w);
		} else if (!contains(w->src.buf + w->rel, MARKER)) {
			ok = w->fs->move(w->fs->ctx, w->src.buf, w->dst.buf);
			moved = ok;
		}
		path_pop(&w->src, so);
		path_pop(&w->dst, dold);
		if (!ok)
			return false;	/* no further file (S-11.079) */
		if (moved)
			w->moved++;
		else
			i++;
	}
	return true;
}

/* S-11.075: everything under content/ but content/dlc/, removed in place
 * so that the DLC is never moved aside (S-11.079, S-16.157). A removal that
 * fails is passed over: the step goes on (S-11.075). */
static void remove_non_dlc(const struct s11_fs_ops *fs)
{
	char name[S11_PATH_MAX];
	struct path p;
	bool dir;
	size_t i = 0;

	if (!is_dir(fs, "content/dlc")) {
		if (exists(fs, "content"))
			(void)fs->remove(fs->ctx, "content");
		return;
	}
	(void)path_set(&p, "content");
	while (fs->list(fs->ctx, "content", i, name, sizeof(name), &dir)) {
		size_t old;

		if (dir && wb_streq(name, "dlc")) {
			i++;
			continue;
		}
		old = path_push(&p, name);
		/* a removed entry leaves the listing; one left in it is
		 * stepped over */
		if (old == 0 || !fs->remove(fs->ctx, p.buf))
			i++;
		if (old != 0)
			path_pop(&p, old);
	}
}

/* S-11.077: each pack the package carries replaces the installed one. A
 * removal that fails is passed over (S-11.075). */
static void replace_packs(const struct s11_fs_ops *fs)
{
	char name[S11_PATH_MAX];
	struct path p;
	bool dir;

	(void)path_set(&p, "content/dlc");
	for (size_t i = 0; fs->list(fs->ctx, STAGED_DLC, i, name, sizeof(name),
				    &dir); i++) {
		size_t old;

		if (name[0] == '.')
			continue;
		old = path_push(&p, name);
		if (old == 0)
			continue;
		if (exists(fs, p.buf))
			(void)fs->remove(fs->ctx, p.buf);
		path_pop(&p, old);
	}
}

/*
 * S-11.066, S-11.075-S-11.079. A removal that fails does not stop the step:
 * the files are installed and the result is theirs (S-11.075).
 */
enum s11_result s11_install_content(const struct s11_fs_ops *fs,
				    bool content_only)
{
	struct walk w;
	bool partial;

	if (!is_dir(fs, STAGED_CONTENT))
		return S11_NOT_FOUND;
	partial = s11_content_partial(fs);
	if (!partial)			/* S-11.075 */
		remove_non_dlc(fs);
	else if (content_only)		/* S-11.077 */
		replace_packs(fs);
	w.fs = fs;
	w.moved = 0;
	(void)path_set(&w.src, STAGED_CONTENT);
	(void)path_set(&w.dst, "content");
	w.rel = w.src.len + 1;
	if (!install_dir(&w))
		return S11_FAILURE;
	/* S-11.078 */
	if (!content_only && exists(fs, "upgrade/" S11_FW_VERSION))
		(void)fs->copy(fs->ctx, "upgrade/" S11_FW_VERSION, "firmware.version");
	return w.moved != 0 ? S11_OK : S11_NOT_FOUND;
}

/* ---- the pack name (S-11.039) ---- */

static size_t skip_ws(const uint8_t *p, size_t n, size_t i)
{
	while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' ||
			 p[i] == '\r'))
		i++;
	return i;
}

static int hex(uint8_t c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool hex4(const uint8_t *p, size_t n, size_t i, uint32_t *v)
{
	*v = 0;
	if (i + 4 > n)
		return false;
	for (size_t k = 0; k < 4; k++) {
		int h = hex(p[i + k]);

		if (h < 0)
			return false;
		*v = *v << 4 | (uint32_t)h;
	}
	return true;
}

struct out {
	char *buf;
	size_t len, cap;	/* cap includes the NUL */
};

static void out_byte(struct out *o, uint32_t b)
{
	if (o != NULL && o->len + 1 < o->cap)
		o->buf[o->len++] = (char)(uint8_t)b;
}

static void out_utf8(struct out *o, uint32_t c)
{
	if (c < 0x80) {
		out_byte(o, c);
	} else if (c < 0x800) {
		out_byte(o, 0xC0 | c >> 6);
		out_byte(o, 0x80 | (c & 0x3F));
	} else if (c < 0x10000) {
		out_byte(o, 0xE0 | c >> 12);
		out_byte(o, 0x80 | (c >> 6 & 0x3F));
		out_byte(o, 0x80 | (c & 0x3F));
	} else {
		out_byte(o, 0xF0 | c >> 18);
		out_byte(o, 0x80 | (c >> 12 & 0x3F));
		out_byte(o, 0x80 | (c >> 6 & 0x3F));
		out_byte(o, 0x80 | (c & 0x3F));
	}
}

/* A JSON string at p[*i] (the opening quote); decoded into `o` when not
 * NULL. */
static bool json_string(const uint8_t *p, size_t n, size_t *i, struct out *o)
{
	size_t k = *i + 1;
	uint32_t c, lo;

	if (*i >= n || p[*i] != '"')
		return false;
	while (k < n && p[k] != '"') {
		if (p[k] != '\\') {
			out_byte(o, p[k++]);
			continue;
		}
		if (++k >= n)
			return false;
		switch (p[k]) {
		case 'b': out_byte(o, '\b'); break;
		case 'f': out_byte(o, '\f'); break;
		case 'n': out_byte(o, '\n'); break;
		case 'r': out_byte(o, '\r'); break;
		case 't': out_byte(o, '\t'); break;
		case 'u':
			if (!hex4(p, n, k + 1, &c))
				return false;
			k += 4;
			if (c >= 0xD800 && c < 0xDC00 && k + 6 < n &&
			    p[k + 1] == '\\' && p[k + 2] == 'u' &&
			    hex4(p, n, k + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
				c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
				k += 6;
			} else if (c >= 0xD800 && c < 0xE000) {
				c = 0xFFFD;
			}
			out_utf8(o, c);
			break;
		default:	/* '"', '\\', '/' */
			out_byte(o, p[k]);
			break;
		}
		k++;
	}
	if (k >= n)
		return false;
	*i = k + 1;
	return true;
}

/* Skips one value: a string, or anything else up to the next ',', '}' or
 * ']' at its own nesting level. */
static bool json_skip(const uint8_t *p, size_t n, size_t *i)
{
	unsigned depth = 0;

	if (*i < n && p[*i] == '"')
		return json_string(p, n, i, NULL);
	while (*i < n) {
		uint8_t c = p[*i];

		if (c == '"') {
			if (!json_string(p, n, i, NULL))
				return false;
			continue;
		}
		if (c == '{' || c == '[') {
			depth++;
		} else if (c == '}' || c == ']') {
			if (depth == 0)
				return true;
			depth--;
		} else if (c == ',' && depth == 0) {
			return true;
		}
		(*i)++;
	}
	return depth == 0;
}

/* The top-level member "name" of a JSON object, when it is a string. */
static void json_name(const uint8_t *p, size_t n, struct out *o)
{
	size_t i = skip_ws(p, n, 0);

	if (i >= n || p[i] != '{')
		return;
	i++;
	for (;;) {
		char key[8];
		struct out k = { key, 0, sizeof(key) };
		bool is_name;

		i = skip_ws(p, n, i);
		if (i >= n || p[i] == '}')
			return;
		if (!json_string(p, n, &i, &k))
			return;
		key[k.len] = '\0';
		is_name = wb_streq(key, "name");	/* longer keys are cut to 7 */
		i = skip_ws(p, n, i);
		if (i >= n || p[i] != ':')
			return;
		i = skip_ws(p, n, i + 1);
		if (is_name && i < n && p[i] == '"') {
			struct out save = *o;

			if (!json_string(p, n, &i, o))
				*o = save;
			return;
		}
		if (!json_skip(p, n, &i))
			return;
		i = skip_ws(p, n, i);
		if (i >= n || p[i] != ',')
			return;
		i++;
	}
}

/*
 * S-11.039: the first pack directory, in listing order, whose manifest.json
 * exists and is not empty. A manifest.json over 4096 bytes is read in its
 * first 4096 bytes only, which S-11.039 allows: its name is then empty
 * unless the name member's string ends within them.
 */
void s11_pack_name(const struct s11_fs_ops *fs, char *name, size_t cap)
{
	static uint8_t buf[MANIFEST_MAX];
	char entry[S11_PATH_MAX];
	struct path p;
	struct out o = { name, 0, cap };
	bool dir;

	if (cap == 0)
		return;
	name[0] = '\0';
	(void)path_set(&p, STAGED_DLC);
	for (size_t i = 0; fs->list(fs->ctx, STAGED_DLC, i, entry, sizeof(entry),
				    &dir); i++) {
		uint32_t size = 0;
		bool fdir = false;
		size_t old;

		if (!dir || dot_or_dotdot(entry))
			continue;
		old = path_push(&p, entry);
		if (old == 0 || path_push(&p, "manifest.json") == 0)
			continue;
		if (fs->stat(fs->ctx, p.buf, &fdir, &size) && !fdir && size != 0) {
			size_t n = fs->read(fs->ctx, p.buf, buf, sizeof(buf));
			json_name(buf, n, &o);
			name[o.len] = '\0';
			return;
		}
		path_pop(&p, old);
	}
}

/* ---- the RAM disk (S-11.046) ---- */

bool s11_disk_pick(s11_root_fn root, void *ctx, char *name)
{
	bool dir;

	for (size_t i = 0; root(ctx, i, name, S11_PATH_MAX, &dir); i++) {
		/* regular files whose names do not begin with '.'; the first
		 * whose name holds "tfw", case-sensitive */
		if (!dir && name[0] != '.' && contains(name, "tfw"))
			return true;
	}
	return false;
}
