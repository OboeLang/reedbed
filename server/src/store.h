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
#ifndef REEDBED_STORE_H
#define REEDBED_STORE_H

#include "izim.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>

/* The package store: a plain filesystem tree.
 *
 *   $ROOT/
 *     tmp/                                scratch, same filesystem as the rest
 *     packages/<izim>/
 *       owners                            one account izim per line
 *       releases/<waktanimra>/
 *         meta                            the record, verbatim wire octets
 *         kabuk                           the archive
 *         sema                            the digest, so ghazema needn't parse
 *         ok                              written last; readers ignore a
 *                                         release directory without it
 *         kaldy                           present only when yanked
 *     accounts/<izim>/tokens/<sha256hex>
 *     index/
 *       jexa                              one TAB-separated line per package
 *       tokens/<sha256hex>                one line naming the account
 *
 * Everything is inspectable with ls and backed up with rsync, which is the
 * point. `meta` holding the exact octets ko besal sends means that request is
 * open, fstat, read, ship -- no serialisation at request time. */

#define STORE_PATH_MAX 4096
/* The root is bounded well below STORE_PATH_MAX so that every path built from
   it -- root plus a fixed suffix plus a bounded izim and waktanimra -- provably
   fits, and the compiler can see that it does. */
#define STORE_ROOT_MAX 3072

struct store {
	char root[STORE_ROOT_MAX];
};

/* `create` makes the skeleton directories when missing. */
bool store_open(struct store *s, const char *root, bool create, char **err);

/* ---- reads ---- */

bool store_package_exists(const struct store *s, const char *izim);
bool store_release_exists(const struct store *s, const char *izim,
			  const char *ver);

/* Every release of a package, newest first, as records separated by blank
   lines. Yanked releases are included and carry a `kaldy` key -- KATARE.md
   §7.2. NULL when the package is unknown. */
char *store_besal_all(const struct store *s, const char *izim, size_t *len);

/* One release's record. NULL when absent. */
char *store_besal_one(const struct store *s, const char *izim, const char *ver,
		      size_t *len);

/* Opens the archive for streaming. Returns a fd, or -1. `sema` must hold
   SEMA_STR_LEN. */
int store_open_kabuk(const struct store *s, const char *izim, const char *ver,
		     unsigned long long *size, char *sema);

/* Search -- KATARE.md §4.1. Case-insensitive substring, tokens ANDed, over
   izim and kakwam. Returns a record body; `*truncated` is set when the cap was
   hit. */
char *store_jexa(const struct store *s, char **terms, int nterms, int cap,
		 size_t *len, bool *truncated);

/* ---- writes ---- */

enum store_result {
	STORE_OK = 0,
	STORE_EXISTS, /* -> sentyre */
	STORE_MISSING, /* -> keresebyr */
	STORE_DENIED, /* -> ezhazebyr */
	STORE_INVALID, /* -> wuwoji; *err says why */
	STORE_ERROR /* -> ramuzhu */
};

/* How many packages one resolution may pin, and how many constraints may
   accumulate on any one of them. Both are bounds on a hostile request as much
   as on a legitimate one. */
#define STORE_RESOLVE_MAX 256
#define STORE_CONSTRAINTS_MAX 32

/* Server-side dependency resolution -- KATARE.md §4.4.
 *
 * Returns the body of a cizujo response: one `<izim> <waktanimra> <sema>` line
 * per package, LF-terminated, flat and complete, no duplicates. NULL on
 * failure, with *why set to STORE_MISSING when the package or a satisfying
 * version set does not exist.
 *
 * This must agree exactly with the resolver a client runs when the server does
 * not advertise `cizujo`, or a mirror would hand out different answers than the
 * origin. Both follow §8.4: highest satisfying version, yanked never chosen. */
char *store_cizujo(const struct store *s, const char *izim, const char *cons,
		   size_t *len, enum store_result *why);

/* Publishes one release. Validates the archive, requires its root
   project.jsonc to declare exactly this izim and waktanimra, derives the
   metadata record from it, and writes everything atomically.
 *
 * The caller must have already verified that the body's digest matches what the
 * publisher claimed; this recomputes it regardless, because the version slot is
 * spent either way and a wrong digest must never become the record. */
enum store_result store_publish(struct store *s, const char *izim,
				const char *ver, const char *account,
				const unsigned char *kabuk, size_t len,
				char **err);

/* Marks a release yanked. `reason` may be NULL. */
enum store_result store_yank(struct store *s, const char *izim, const char *ver,
			     const char *account, const char *reason,
			     char **err);

bool store_is_owner(const struct store *s, const char *izim,
		    const char *account);
bool store_add_owner(struct store *s, const char *izim, const char *account,
		     char **err);

/* Rebuilds index/jexa from the package tree. Called after every publish and
   yank, and by `reedbed-admin reindex`. */
bool store_reindex(struct store *s, char **err);

/* Removes release directories that were claimed but never completed -- a
   publish that died between mkdir and the ok marker. Run at startup. */
void store_sweep(struct store *s);

/* ---- accounts ---- */

bool store_account_create(struct store *s, const char *account, char **err);
bool store_account_exists(const struct store *s, const char *account);

/* Records a token for an account and returns it. `out` must hold at least 64
   bytes. The token itself is never stored -- only its digest -- so a stolen
   backup yields no credentials. */
bool store_token_mint(struct store *s, const char *account, char *out,
		      size_t outcap, char **err);

/* Resolves a bearer token to an account. `out` must hold IZIM_MAX+1. */
bool store_token_lookup(const struct store *s, const char *token, char *out);

#endif
