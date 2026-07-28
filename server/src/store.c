/* SPDX-License-Identifier: GPL-2.0-only
 * © 2026 Sushii64
 * © 2026 robinpie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include "store.h"

#include "kabuk.h"
#include "projectjson.h"
#include "record.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool fail(char **err, const char *fmt, ...)
{
	if (err) {
		char buf[512];
		va_list ap;

		va_start(ap, fmt);
		vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
		*err = strdup(buf);
	}
	return false;
}

/* Every path is built from validated components: izim and waktanimra are
   checked before they ever reach here, and both grammars exclude '/' and '.'
   as a whole component, so no traversal is representable. */
static void jp(char *out, size_t cap, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(out, cap, fmt, ap);
	va_end(ap);
}

static bool is_dir(const char *p)
{
	struct stat st;

	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p)
{
	struct stat st;

	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long n = ftell(f);

	if (n < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);

	char *buf = malloc((size_t)n + 1);

	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)n, f);

	fclose(f);
	buf[got] = '\0';
	if (len)
		*len = got;
	return buf;
}

/* Writes via a temporary in the same directory, then renames: a reader either
   sees the old content or the new one, never a partial write. */
static bool write_atomic(const char *dir, const char *name, const void *data,
			 size_t len, char **err)
{
	char tmp[STORE_PATH_MAX], final[STORE_PATH_MAX];

	jp(tmp, sizeof tmp, "%s/.%s.tmp", dir, name);
	jp(final, sizeof final, "%s/%s", dir, name);

	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0)
		return fail(err, "%s: %s", tmp, strerror(errno));

	const char *p = data;
	size_t off = 0;

	while (off < len) {
		ssize_t w = write(fd, p + off, len - off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmp);
			return fail(err, "%s: %s", tmp, strerror(errno));
		}
		off += (size_t)w;
	}
	if (fsync(fd) != 0) {
		close(fd);
		unlink(tmp);
		return fail(err, "%s: fsync: %s", tmp, strerror(errno));
	}
	close(fd);

	if (rename(tmp, final) != 0) {
		unlink(tmp);
		return fail(err, "%s: %s", final, strerror(errno));
	}
	return true;
}

/* fsync a directory so a rename into it survives a crash */
static void sync_dir(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);

	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

static bool mkdirs(const char *path)
{
	char buf[STORE_PATH_MAX];

	snprintf(buf, sizeof buf, "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) != 0 && errno != EEXIST)
			return false;
		*p = '/';
	}
	return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

bool store_open(struct store *s, const char *root, bool create, char **err)
{
	snprintf(s->root, sizeof s->root, "%s", root);

	if (!is_dir(s->root)) {
		if (!create)
			return fail(err, "%s: no such store", s->root);
		if (!mkdirs(s->root))
			return fail(err, "%s: %s", s->root, strerror(errno));
	}

	static const char *const subs[] = { "tmp",   "packages",     "accounts",
					    "index", "index/tokens", NULL };

	for (int i = 0; subs[i]; i++) {
		char p[STORE_PATH_MAX];

		jp(p, sizeof p, "%s/%s", s->root, subs[i]);
		if (!is_dir(p)) {
			if (!create)
				return fail(err, "%s: missing", p);
			if (!mkdirs(p))
				return fail(err, "%s: %s", p, strerror(errno));
		}
	}
	return true;
}

/* ---- release enumeration ---------------------------------------------- */

struct verlist {
	char (*v)[WAKTANIMRA_MAX];
	int n, cap;
};

static int cmp_desc(const void *a, const void *b)
{
	/* newest first, so ko besal leads with what a client most likely wants */
	return -waktanimra_cmp_str((const char *)a, (const char *)b);
}

/* Lists the *complete* releases of a package. A directory without an `ok`
   marker is a publish that died partway and is invisible to every reader. */
static bool list_releases(const struct store *s, const char *izim,
			  struct verlist *out)
{
	char dir[STORE_PATH_MAX];

	jp(dir, sizeof dir, "%s/packages/%s/releases", s->root, izim);

	DIR *d = opendir(dir);

	if (!d)
		return false;

	memset(out, 0, sizeof *out);

	struct dirent *e;

	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		if (!waktanimra_valid(e->d_name))
			continue;

		char ok[STORE_PATH_MAX];

		jp(ok, sizeof ok, "%s/%s/ok", dir, e->d_name);
		if (!is_file(ok))
			continue;

		if (out->n == out->cap) {
			int cap = out->cap ? out->cap * 2 : 16;
			void *p = realloc(out->v, (size_t)cap * WAKTANIMRA_MAX);

			if (!p) {
				closedir(d);
				free(out->v);
				return false;
			}
			out->v = p;
			out->cap = cap;
		}
		/* waktanimra_valid already bounded this above, so the copy
		   cannot truncate -- say so with memcpy rather than leaving a
		   silently-truncating snprintf for someone to trust later */
		size_t vn = strlen(e->d_name);

		memcpy(out->v[out->n++], e->d_name, vn + 1);
	}
	closedir(d);

	if (out->n > 1)
		qsort(out->v, (size_t)out->n, WAKTANIMRA_MAX, cmp_desc);
	return true;
}

bool store_package_exists(const struct store *s, const char *izim)
{
	char p[STORE_PATH_MAX];

	jp(p, sizeof p, "%s/packages/%s", s->root, izim);
	return is_dir(p);
}

bool store_release_exists(const struct store *s, const char *izim,
			  const char *ver)
{
	char p[STORE_PATH_MAX];

	jp(p, sizeof p, "%s/packages/%s/releases/%s/ok", s->root, izim, ver);
	return is_file(p);
}

char *store_besal_one(const struct store *s, const char *izim, const char *ver,
		      size_t *len)
{
	char p[STORE_PATH_MAX];

	if (!store_release_exists(s, izim, ver))
		return NULL;
	jp(p, sizeof p, "%s/packages/%s/releases/%s/meta", s->root, izim, ver);
	return slurp(p, len);
}

char *store_besal_all(const struct store *s, const char *izim, size_t *len)
{
	struct verlist vl;

	if (!store_package_exists(s, izim))
		return NULL;
	if (!list_releases(s, izim, &vl))
		return NULL;

	size_t cap = 4096, n = 0;
	char *buf = malloc(cap);

	if (!buf) {
		free(vl.v);
		return NULL;
	}
	buf[0] = '\0';

	for (int i = 0; i < vl.n; i++) {
		size_t rlen = 0;
		char *rec = store_besal_one(s, izim, vl.v[i], &rlen);

		if (!rec)
			continue;

		/* a blank line separates records, and only separates them: no
		   trailing separator after the last */
		size_t need = n + rlen + 2;

		if (need > cap) {
			while (cap < need)
				cap *= 2;
			char *nb = realloc(buf, cap);

			if (!nb) {
				free(rec);
				free(buf);
				free(vl.v);
				return NULL;
			}
			buf = nb;
		}
		if (n > 0)
			buf[n++] = '\n';
		memcpy(buf + n, rec, rlen);
		n += rlen;
		free(rec);
	}
	free(vl.v);

	if (len)
		*len = n;
	return buf;
}

int store_open_kabuk(const struct store *s, const char *izim, const char *ver,
		     unsigned long long *size, char *sema)
{
	char p[STORE_PATH_MAX];

	if (!store_release_exists(s, izim, ver))
		return -1;

	jp(p, sizeof p, "%s/packages/%s/releases/%s/sema", s->root, izim, ver);

	size_t slen = 0;
	char *sv = slurp(p, &slen);

	if (!sv)
		return -1;
	while (slen > 0 && (sv[slen - 1] == '\n' || sv[slen - 1] == '\r'))
		sv[--slen] = '\0';
	if (!sema_valid(sv)) {
		free(sv);
		return -1;
	}
	memcpy(sema, sv, slen + 1);
	free(sv);

	jp(p, sizeof p, "%s/packages/%s/releases/%s/kabuk", s->root, izim, ver);

	int fd = open(p, O_RDONLY);

	if (fd < 0)
		return -1;

	struct stat st;

	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return -1;
	}
	*size = (unsigned long long)st.st_size;
	return fd;
}

/* ---- ownership --------------------------------------------------------- */

bool store_is_owner(const struct store *s, const char *izim,
		    const char *account)
{
	char p[STORE_PATH_MAX];

	if (!account || !*account)
		return false;
	jp(p, sizeof p, "%s/packages/%s/owners", s->root, izim);

	char *body = slurp(p, NULL);

	if (!body)
		return false;

	bool found = false;

	for (char *line = body, *nl; line && *line; line = nl) {
		nl = strchr(line, '\n');
		size_t n = nl ? (size_t)(nl - line) : strlen(line);

		if (nl)
			nl++;
		if (n == strlen(account) && memcmp(line, account, n) == 0) {
			found = true;
			break;
		}
	}
	free(body);
	return found;
}

bool store_add_owner(struct store *s, const char *izim, const char *account,
		     char **err)
{
	char dir[STORE_PATH_MAX], p[STORE_PATH_MAX];

	if (!store_package_exists(s, izim))
		return fail(err, "no such package '%s'", izim);
	if (store_is_owner(s, izim, account))
		return true;

	jp(dir, sizeof dir, "%s/packages/%s", s->root, izim);
	jp(p, sizeof p, "%s/owners", dir);

	size_t n = 0;
	char *body = slurp(p, &n);
	size_t alen = strlen(account);
	char *nb = malloc(n + alen + 2);

	if (!nb) {
		free(body);
		return fail(err, "out of memory");
	}
	if (body)
		memcpy(nb, body, n);
	free(body);
	memcpy(nb + n, account, alen);
	nb[n + alen] = '\n';

	bool ok = write_atomic(dir, "owners", nb, n + alen + 1, err);

	free(nb);
	return ok;
}

/* ---- accounts and tokens ----------------------------------------------- */

bool store_account_exists(const struct store *s, const char *account)
{
	char p[STORE_PATH_MAX];

	jp(p, sizeof p, "%s/accounts/%s", s->root, account);
	return is_dir(p);
}

bool store_account_create(struct store *s, const char *account, char **err)
{
	char p[STORE_PATH_MAX];

	if (!izim_valid(account))
		return fail(err, "'%s' is not a valid account name", account);
	if (store_account_exists(s, account))
		return fail(err, "account '%s' already exists", account);

	jp(p, sizeof p, "%s/accounts/%s/tokens", s->root, account);
	if (!mkdirs(p))
		return fail(err, "%s: %s", p, strerror(errno));
	return true;
}

/* Tokens are 32 random octets in lowercase base32, so they survive being
   pasted into a shell or a config file without quoting. */
static bool random_token(char *out, size_t cap)
{
	static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
	unsigned char raw[32];

	if (cap < sizeof raw * 8 / 5 + 1)
		return false;

	FILE *f = fopen("/dev/urandom", "rb");

	if (!f)
		return false;
	size_t got = fread(raw, 1, sizeof raw, f);

	fclose(f);
	if (got != sizeof raw)
		return false;

	size_t o = 0;
	unsigned bits = 0;
	unsigned long acc = 0;

	for (size_t i = 0; i < sizeof raw; i++) {
		acc = (acc << 8) | raw[i];
		bits += 8;
		while (bits >= 5) {
			bits -= 5;
			out[o++] = alphabet[(acc >> bits) & 31];
		}
	}
	if (bits)
		out[o++] = alphabet[(acc << (5 - bits)) & 31];
	out[o] = '\0';
	return true;
}

/* Tokens must look like tokens before we hash them, so that a malformed one is
   rejected on shape rather than by a lookup whose timing could distinguish it
   from a well-formed miss. */
static bool token_shape_ok(const char *t)
{
	size_t n = t ? strlen(t) : 0;

	if (n < 16 || n > 128)
		return false;
	for (const char *p = t; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '2' && *p <= '7')))
			return false;
	return true;
}

static void token_digest(const char *token, char *hex)
{
	unsigned char d[SHA256_DIGEST_LEN];
	char sema[SEMA_STR_LEN];

	sha256(token, strlen(token), d);
	sema_format(d, sema);
	memcpy(hex, sema + 7, 65); /* the hex, without the "sha256:" prefix */
}

bool store_token_mint(struct store *s, const char *account, char *out,
		      size_t outcap, char **err)
{
	if (!store_account_exists(s, account))
		return fail(err, "no such account '%s'", account);
	if (!random_token(out, outcap))
		return fail(err, "cannot read /dev/urandom");

	char hex[65];

	token_digest(out, hex);

	char dir[STORE_PATH_MAX];
	char body[128];
	int n = snprintf(body, sizeof body, "%s\n", account);

	jp(dir, sizeof dir, "%s/accounts/%s/tokens", s->root, account);
	if (!write_atomic(dir, hex, body, (size_t)n, err))
		return false;

	/* the index is what kalit consults, so the lookup is one stat rather
	   than a walk over every account */
	jp(dir, sizeof dir, "%s/index/tokens", s->root);
	return write_atomic(dir, hex, body, (size_t)n, err);
}

bool store_token_lookup(const struct store *s, const char *token, char *out)
{
	if (!token_shape_ok(token))
		return false;

	char hex[65], p[STORE_PATH_MAX];

	token_digest(token, hex);
	jp(p, sizeof p, "%s/index/tokens/%s", s->root, hex);

	size_t n = 0;
	char *body = slurp(p, &n);

	if (!body)
		return false;
	while (n > 0 && (body[n - 1] == '\n' || body[n - 1] == '\r'))
		body[--n] = '\0';

	bool ok = izim_valid(body) && n <= IZIM_MAX;

	if (ok)
		memcpy(out, body, n + 1);
	free(body);
	return ok;
}

/* ---- publish ----------------------------------------------------------- */

struct find_ctx {
	const char *want;
	char *data;
	size_t len;
};

static bool find_entry(const char *path, const unsigned char *data, size_t len,
		       void *ctxp, char **err)
{
	struct find_ctx *c = ctxp;

	if (strcmp(path, c->want) != 0)
		return true;
	c->data = malloc(len + 1);
	if (!c->data)
		return false;
	memcpy(c->data, data, len);
	c->data[len] = '\0';
	c->len = len;
	return true;
}

/* Pulls the package's own project.jsonc out of the archive. A package is
   imported by the name in *that* file (the Oboe compiler matches folder modules
   on it), so if it disagrees with the izim being published, the package would
   install and then fail to import -- which is why this is enforced here rather
   than left to be discovered later. */
static char *archive_project_json(const unsigned char *kabuk, size_t len)
{
	struct find_ctx c = { .want = "project.jsonc" };
	char *err = NULL;

	if (!kabuk_read(kabuk, len, NULL, find_entry, &c, &err)) {
		free(err);
		return NULL;
	}
	if (!c.data) {
		c.want = "project.json";
		if (!kabuk_read(kabuk, len, NULL, find_entry, &c, &err)) {
			free(err);
			return NULL;
		}
	}
	return c.data;
}

static void rfc3339_now(char *out, size_t cap)
{
	time_t t = time(NULL);
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* Values reach the wire as a `key: value` line, so anything that could forge a
   line break or an empty value is refused rather than sanitised. */
static void put_clean(struct record *r, const char *key, char *value)
{
	if (!value)
		return;
	if (*value && !strchr(value, '\n') && !strchr(value, '\r'))
		record_put(r, key, value);
	free(value);
}

enum store_result store_publish(struct store *s, const char *izim,
				const char *ver, const char *account,
				const unsigned char *kabuk, size_t len,
				char **err)
{
	if (!izim_valid(izim)) {
		fail(err, "bad-izim");
		return STORE_INVALID;
	}
	if (izim_reserved(izim)) {
		fail(err, "reserved-name");
		return STORE_INVALID;
	}
	if (!waktanimra_valid(ver)) {
		fail(err, "bad-waktanimra");
		return STORE_INVALID;
	}

	char *verr = NULL;

	if (!kabuk_validate(kabuk, len, NULL, &verr)) {
		fail(err, "bad-kabuk");
		free(verr);
		return STORE_INVALID;
	}

	char *pj = archive_project_json(kabuk, len);

	if (!pj) {
		fail(err, "no-project-json");
		return STORE_INVALID;
	}

	char *proj = json_extract_object(pj, "project");
	const char *scope = proj ? proj : pj;
	char *pname = json_get_string(scope, "name");
	char *pver = json_get_string(scope, "version");

	if (!pname || strcmp(pname, izim) != 0) {
		fail(err, "name-mismatch");
		goto invalid;
	}
	if (!pver || strcmp(pver, ver) != 0) {
		fail(err, "version-mismatch");
		goto invalid;
	}

	/* ownership before anything is created: an unknown package is claimed
	   by its first publisher, an existing one requires membership */
	bool fresh = !store_package_exists(s, izim);

	if (!fresh && !store_is_owner(s, izim, account)) {
		free(pname);
		free(pver);
		free(proj);
		free(pj);
		return STORE_DENIED;
	}

	char pkgdir[STORE_PATH_MAX], reldir[STORE_PATH_MAX];

	jp(pkgdir, sizeof pkgdir, "%s/packages/%s", s->root, izim);
	jp(reldir, sizeof reldir, "%s/releases/%s", pkgdir, ver);

	{
		char rels[STORE_PATH_MAX];

		jp(rels, sizeof rels, "%s/releases", pkgdir);
		if (!mkdirs(rels)) {
			fail(err, "cannot create %s", rels);
			goto error;
		}
	}

	/* mkdir is the claim: it is atomic, and its EEXIST is exactly the
	   `sentyre` case. Renaming a staged directory into place would not be,
	   because rename(2) silently replaces an empty target -- a crashed
	   earlier publish would let a second publisher overwrite it. */
	if (mkdir(reldir, 0755) != 0) {
		if (errno == EEXIST) {
			free(pname);
			free(pver);
			free(proj);
			free(pj);
			return STORE_EXISTS;
		}
		fail(err, "%s: %s", reldir, strerror(errno));
		goto error;
	}

	unsigned char digest[SHA256_DIGEST_LEN];
	char sema[SEMA_STR_LEN];

	sha256(kabuk, len, digest);
	sema_format(digest, sema);

	struct record rec;

	memset(&rec, 0, sizeof rec);
	record_put(&rec, "izim", izim);
	record_put(&rec, "waktanimra", ver);

	char *kind = json_get_string(scope, "kind");

	record_put(&rec, "warna",
		   kind && strcmp(kind, "pawi") == 0 ? "pawi" : "vivlijotiki");
	free(kind);

	char num[64];

	snprintf(num, sizeof num, "%zu", len);
	record_put(&rec, "ozhon", num);
	record_put(&rec, "sema", sema);

	char when[64];

	rfc3339_now(when, sizeof when);
	record_put(&rec, "wakta", when);

	put_clean(&rec, "kakwam", json_get_string(scope, "description"));
	put_clean(&rec, "tojar", json_get_string(scope, "author"));
	put_clean(&rec, "terezh", json_get_string(scope, "license"));
	put_clean(&rec, "punjur", json_get_string(scope, "repository"));
	put_clean(&rec, "asulna", json_get_string(scope, "homepage"));

	char *deps = json_extract_object(pj, "dependencies");

	if (deps) {
		int ndeps = 0;
		char **keys = json_object_keys(deps, &ndeps);

		for (int i = 0; i < ndeps; i++) {
			char *cons = json_get_string(deps, keys[i]);

			/* `oboe` in dependencies names the toolchain, not a
			   package, so it is not a cizujo */
			if (cons && izim_valid(keys[i]) &&
			    strcmp(keys[i], "oboe") != 0 &&
			    constraint_valid(cons)) {
				char line[256];

				snprintf(line, sizeof line, "%s %s", keys[i],
					 cons);
				record_put(&rec, "cizujo", line);
			}
			free(cons);
			free(keys[i]);
		}
		free(keys);
		free(deps);
	}

	size_t mlen = 0;
	char *meta = record_format(&rec, &mlen);

	record_free(&rec);

	if (!meta) {
		fail(err, "out of memory");
		goto error;
	}

	char semaline[SEMA_STR_LEN + 2];
	int sl = snprintf(semaline, sizeof semaline, "%s\n", sema);

	/* order matters: content first, then the ok marker. A crash anywhere
	   before the marker leaves a directory that every reader ignores and
	   the startup sweep removes. */
	if (!write_atomic(reldir, "kabuk", kabuk, len, err) ||
	    !write_atomic(reldir, "meta", meta, mlen, err) ||
	    !write_atomic(reldir, "sema", semaline, (size_t)sl, err)) {
		free(meta);
		goto error;
	}
	free(meta);
	sync_dir(reldir);

	if (!write_atomic(reldir, "ok", "", 0, err))
		goto error;
	sync_dir(reldir);

	if (fresh && !store_add_owner(s, izim, account, err))
		goto error;

	free(pname);
	free(pver);
	free(proj);
	free(pj);

	char *ierr = NULL;

	if (!store_reindex(s, &ierr))
		free(ierr); /* a stale search index is not a failed publish */
	return STORE_OK;

invalid:
	free(pname);
	free(pver);
	free(proj);
	free(pj);
	return STORE_INVALID;
error:
	free(pname);
	free(pver);
	free(proj);
	free(pj);
	return STORE_ERROR;
}

enum store_result store_yank(struct store *s, const char *izim, const char *ver,
			     const char *account, const char *reason,
			     char **err)
{
	if (!izim_valid(izim) || !waktanimra_valid(ver)) {
		fail(err, "bad-argument");
		return STORE_INVALID;
	}
	if (!store_release_exists(s, izim, ver))
		return STORE_MISSING;
	if (!store_is_owner(s, izim, account))
		return STORE_DENIED;

	char reldir[STORE_PATH_MAX];

	jp(reldir, sizeof reldir, "%s/packages/%s/releases/%s", s->root, izim,
	   ver);

	const char *why = (reason && *reason) ? reason : "-";
	char body[1024];
	int n = snprintf(body, sizeof body, "%s\n", why);

	if (!write_atomic(reldir, "kaldy", body, (size_t)n, err))
		return STORE_ERROR;

	/* the record carries the yank so a single ko besal shows it; the blob
	   stays, because an existing pin must keep working */
	size_t mlen = 0;
	char path[STORE_PATH_MAX];

	jp(path, sizeof path, "%s/meta", reldir);

	char *meta = slurp(path, &mlen);

	if (meta) {
		struct record_set rs;

		if (record_set_parse(meta, mlen, &rs) && rs.n == 1) {
			struct record *r = &rs.records[0];
			bool had = record_get(r, "kaldy") != NULL;

			if (!had)
				record_put(r, "kaldy", why);
			else
				for (int i = 0; i < r->n; i++)
					if (strcmp(r->fields[i].key, "kaldy") ==
					    0) {
						free(r->fields[i].value);
						r->fields[i].value =
							strdup(why);
					}

			size_t nlen = 0;
			char *nm = record_format(r, &nlen);

			if (nm) {
				write_atomic(reldir, "meta", nm, nlen, err);
				free(nm);
			}
			record_set_free(&rs);
		}
		free(meta);
	}

	char *ierr = NULL;

	if (!store_reindex(s, &ierr))
		free(ierr);
	return STORE_OK;
}

/* ---- search index ------------------------------------------------------ */

/* Rebuilt in full on every publish and yank. A full sweep of ten thousand
   packages is a readdir and a few milliseconds, and publishes are rare, so an
   incremental index would be complexity bought with nothing. */
bool store_reindex(struct store *s, char **err)
{
	char pdir[STORE_PATH_MAX];

	jp(pdir, sizeof pdir, "%s/packages", s->root);

	DIR *d = opendir(pdir);

	if (!d)
		return fail(err, "%s: %s", pdir, strerror(errno));

	size_t cap = 8192, n = 0;
	char *buf = malloc(cap);

	if (!buf) {
		closedir(d);
		return fail(err, "out of memory");
	}

	struct dirent *e;

	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || !izim_valid(e->d_name))
			continue;

		struct verlist vl;

		if (!list_releases(s, e->d_name, &vl))
			continue;

		/* the index names the latest release a resolver would pick, so
		   yanked ones are skipped here exactly as they are there */
		char *rec = NULL;
		size_t rlen = 0;

		for (int i = 0; i < vl.n; i++) {
			rec = store_besal_one(s, e->d_name, vl.v[i], &rlen);
			if (!rec)
				continue;

			struct record_set rs;

			if (record_set_parse(rec, rlen, &rs) && rs.n == 1 &&
			    !record_get(&rs.records[0], "kaldy")) {
				const char *warna =
					record_get(&rs.records[0], "warna");
				const char *kakwam =
					record_get(&rs.records[0], "kakwam");
				char line[1024];
				int ln = snprintf(line, sizeof line,
						  "%s\t%s\t%s\t%s\n", e->d_name,
						  warna ? warna : "vivlijotiki",
						  vl.v[i],
						  kakwam ? kakwam : "");

				if (n + (size_t)ln + 1 > cap) {
					while (n + (size_t)ln + 1 > cap)
						cap *= 2;
					char *nb = realloc(buf, cap);

					if (nb)
						buf = nb;
				}
				memcpy(buf + n, line, (size_t)ln);
				n += (size_t)ln;
				record_set_free(&rs);
				free(rec);
				break;
			}
			record_set_free(&rs);
			free(rec);
			rec = NULL;
		}
		free(vl.v);
	}
	closedir(d);

	char idir[STORE_PATH_MAX];

	jp(idir, sizeof idir, "%s/index", s->root);

	bool ok = write_atomic(idir, "jexa", buf, n, err);

	free(buf);
	return ok;
}

static bool ci_contains(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);

	if (nl == 0)
		return true;
	for (const char *p = hay; *p; p++) {
		size_t i = 0;

		while (i < nl && p[i] &&
		       tolower((unsigned char)p[i]) ==
			       tolower((unsigned char)needle[i]))
			i++;
		if (i == nl)
			return true;
	}
	return false;
}

char *store_jexa(const struct store *s, char **terms, int nterms, int cap,
		 size_t *len, bool *truncated)
{
	char p[STORE_PATH_MAX];

	*truncated = false;
	jp(p, sizeof p, "%s/index/jexa", s->root);

	size_t ilen = 0;
	char *index = slurp(p, &ilen);

	size_t obcap = 4096, n = 0;
	char *out = malloc(obcap);

	if (!out) {
		free(index);
		return NULL;
	}
	out[0] = '\0';
	if (!index) {
		if (len)
			*len = 0;
		return out; /* an empty registry searches to zero matches */
	}

	int hits = 0;

	for (char *line = index, *nl; line && *line; line = nl) {
		nl = strchr(line, '\n');
		if (nl)
			*nl++ = '\0';
		if (!*line)
			continue;

		/* <izim>\t<warna>\t<version>\t<kakwam> */
		char *f[4] = { line, NULL, NULL, NULL };
		char *q = line;

		for (int i = 1; i < 4; i++) {
			q = strchr(q, '\t');
			if (!q)
				break;
			*q++ = '\0';
			f[i] = q;
		}
		if (!f[2])
			continue;

		/* every term must match, in the name or the description */
		bool all = true;

		for (int t = 0; t < nterms && all; t++)
			all = ci_contains(f[0], terms[t]) ||
			      (f[3] && ci_contains(f[3], terms[t]));
		if (!all)
			continue;

		if (hits >= cap) {
			*truncated = true;
			break;
		}

		size_t rlen = 0;
		char *rec = store_besal_one(s, f[0], f[2], &rlen);

		if (!rec)
			continue;

		size_t need = n + rlen + 2;

		if (need > obcap) {
			while (obcap < need)
				obcap *= 2;
			char *nb = realloc(out, obcap);

			if (!nb) {
				free(rec);
				break;
			}
			out = nb;
		}
		if (n > 0)
			out[n++] = '\n';
		memcpy(out + n, rec, rlen);
		n += rlen;
		free(rec);
		hits++;
	}
	free(index);

	/* the marker is its own record, so a client that ignores unknown keys
	   simply sees one extra record and a reader that knows it can say so */
	if (*truncated) {
		static const char mark[] = "wakwa: truncated\n";
		size_t need = n + sizeof mark + 1;

		if (need > obcap) {
			char *nb = realloc(out, need);

			if (nb)
				out = nb;
		}
		if (n > 0)
			out[n++] = '\n';
		memcpy(out + n, mark, sizeof mark - 1);
		n += sizeof mark - 1;
	}

	if (len)
		*len = n;
	return out;
}

/* ---- resolution -------------------------------------------------------- */

struct res_pkg {
	char izim[IZIM_MAX + 1];
	char chosen[WAKTANIMRA_MAX];
	char sema[SEMA_STR_LEN];
	char cons[STORE_CONSTRAINTS_MAX][WAKTANIMRA_MAX + 4];
	int ncons;
	bool needs_pick;
};

struct resolver {
	const struct store *s;
	struct res_pkg p[STORE_RESOLVE_MAX];
	int n;
};

static struct res_pkg *res_find(struct resolver *r, const char *izim)
{
	for (int i = 0; i < r->n; i++)
		if (strcmp(r->p[i].izim, izim) == 0)
			return &r->p[i];
	return NULL;
}

/* Adds a constraint, creating the package slot if new. A constraint already
   present changes nothing, which is what makes the fixpoint below terminate. */
static bool res_constrain(struct resolver *r, const char *izim,
			  const char *cons)
{
	struct res_pkg *p = res_find(r, izim);

	if (!p) {
		if (r->n >= STORE_RESOLVE_MAX)
			return false;
		p = &r->p[r->n++];
		memset(p, 0, sizeof *p);
		snprintf(p->izim, sizeof p->izim, "%s", izim);
		p->needs_pick = true;
	}

	for (int i = 0; i < p->ncons; i++)
		if (strcmp(p->cons[i], cons) == 0)
			return true;

	if (p->ncons >= STORE_CONSTRAINTS_MAX)
		return false;
	snprintf(p->cons[p->ncons++], sizeof p->cons[0], "%s", cons);
	p->needs_pick = true;
	return true;
}

/* Highest version satisfying every accumulated constraint, skipping yanked
   releases -- §8.4. Also hands back that release's own dependencies, so the
   caller can widen the problem without re-reading the record. */
static bool res_pick(struct resolver *r, struct res_pkg *p, struct record *deps,
		     bool *found)
{
	struct verlist vl;

	*found = false;
	if (!list_releases(r->s, p->izim, &vl))
		return false;

	bool ok = true;

	for (int i = 0; i < vl.n && !*found; i++) {
		struct waktanimra v;

		if (!waktanimra_parse(vl.v[i], &v))
			continue;

		bool all = true;

		for (int c = 0; c < p->ncons && all; c++) {
			struct constraint cc;

			if (!constraint_parse(p->cons[c], &cc)) {
				ok = false;
				all = false;
				break;
			}
			all = constraint_match(&cc, &v);
		}
		if (!all)
			continue;

		size_t rlen = 0;
		char *rec = store_besal_one(r->s, p->izim, vl.v[i], &rlen);

		if (!rec)
			continue;

		struct record_set rs;

		if (record_set_parse(rec, rlen, &rs) && rs.n == 1) {
			/* a yanked release is never chosen, though one already
			   pinned stays fetchable */
			if (!record_get(&rs.records[0], "kaldy")) {
				const char *sema =
					record_get(&rs.records[0], "sema");

				if (sema && sema_valid(sema)) {
					snprintf(p->chosen, sizeof p->chosen,
						 "%s", vl.v[i]);
					snprintf(p->sema, sizeof p->sema, "%s",
						 sema);

					int it = 0;
					const char *d;

					while ((d = record_next(&rs.records[0],
								"cizujo", &it)))
						record_put(deps, "cizujo", d);
					*found = true;
				}
			}
			record_set_free(&rs);
		}
		free(rec);
	}
	free(vl.v);
	return ok;
}

char *store_cizujo(const struct store *s, const char *izim, const char *cons,
		   size_t *len, enum store_result *why)
{
	*why = STORE_MISSING;

	if (!izim_valid(izim) || !constraint_valid(cons)) {
		*why = STORE_INVALID;
		return NULL;
	}
	if (!store_package_exists(s, izim))
		return NULL;

	struct resolver *r = calloc(1, sizeof *r);

	if (!r) {
		*why = STORE_ERROR;
		return NULL;
	}
	r->s = s;

	if (!res_constrain(r, izim, cons)) {
		free(r);
		*why = STORE_ERROR;
		return NULL;
	}

	/* Re-picking a package can widen a constraint on one already chosen, so
	   this runs to a fixpoint rather than in one pass. The iteration cap
	   turns a pathological graph into an error instead of a hang. */
	int budget = 4 * STORE_RESOLVE_MAX;
	bool changed = true;

	while (changed && budget-- > 0) {
		changed = false;

		for (int i = 0; i < r->n; i++) {
			struct res_pkg *p = &r->p[i];

			if (!p->needs_pick)
				continue;

			struct record deps;

			memset(&deps, 0, sizeof deps);

			bool found = false;

			if (!res_pick(r, p, &deps, &found) || !found) {
				record_free(&deps);
				free(r);
				return NULL; /* unsatisfiable */
			}
			p->needs_pick = false;
			changed = true;

			int it = 0;
			const char *d;

			while ((d = record_next(&deps, "cizujo", &it))) {
				char dn[IZIM_MAX + 1], dc[WAKTANIMRA_MAX + 4];
				const char *sp = strchr(d, ' ');

				if (!sp)
					continue;

				size_t nlen = (size_t)(sp - d);

				if (nlen > IZIM_MAX)
					continue;
				memcpy(dn, d, nlen);
				dn[nlen] = '\0';
				snprintf(dc, sizeof dc, "%s", sp + 1);

				if (!izim_valid(dn) || !constraint_valid(dc))
					continue;
				if (!res_constrain(r, dn, dc)) {
					record_free(&deps);
					free(r);
					*why = STORE_ERROR;
					return NULL;
				}
			}
			record_free(&deps);
		}
	}

	if (budget <= 0) {
		free(r);
		return NULL;
	}

	size_t cap = 1024, n = 0;
	char *out = malloc(cap);

	if (!out) {
		free(r);
		*why = STORE_ERROR;
		return NULL;
	}

	for (int i = 0; i < r->n; i++) {
		char line[512];
		int ln = snprintf(line, sizeof line, "%s %s %s\n", r->p[i].izim,
				  r->p[i].chosen, r->p[i].sema);

		if (n + (size_t)ln + 1 > cap) {
			while (n + (size_t)ln + 1 > cap)
				cap *= 2;
			char *nb = realloc(out, cap);

			if (!nb) {
				free(out);
				free(r);
				*why = STORE_ERROR;
				return NULL;
			}
			out = nb;
		}
		memcpy(out + n, line, (size_t)ln);
		n += (size_t)ln;
	}
	free(r);

	if (len)
		*len = n;
	*why = STORE_OK;
	return out;
}

void store_sweep(struct store *s)
{
	char pdir[STORE_PATH_MAX];

	jp(pdir, sizeof pdir, "%s/packages", s->root);

	DIR *d = opendir(pdir);

	if (!d)
		return;

	struct dirent *e;

	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;

		char rels[STORE_PATH_MAX];

		jp(rels, sizeof rels, "%s/%s/releases", pdir, e->d_name);

		DIR *rd = opendir(rels);

		if (!rd)
			continue;

		struct dirent *re;

		while ((re = readdir(rd))) {
			if (re->d_name[0] == '.')
				continue;

			char ok[STORE_PATH_MAX], rel[STORE_PATH_MAX];

			jp(rel, sizeof rel, "%s/%s", rels, re->d_name);
			jp(ok, sizeof ok, "%s/ok", rel);
			if (is_file(ok))
				continue;

			/* an incomplete publish: no reader ever saw it, so
			   removing it is invisible and frees the version slot */
			static const char *const parts[] = { "kabuk", "meta",
							     "sema", "kaldy",
							     NULL };

			for (int i = 0; parts[i]; i++) {
				char f[STORE_PATH_MAX];

				jp(f, sizeof f, "%s/%s", rel, parts[i]);
				unlink(f);
			}
			rmdir(rel);
		}
		closedir(rd);
	}
	closedir(d);
}
