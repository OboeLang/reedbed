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

/* reedbed-admin -- everything katare deliberately has no verb for.
 *
 * Accounts, tokens and owner grants are created here, on the server host, by
 * someone with filesystem access. There is no registration verb and no
 * self-service, which keeps the protocol small and means a compromised client
 * cannot mint itself an identity.
 *
 * It also drives store.c directly, so publishing, yanking and ownership can be
 * tested without a socket in the way -- a failing test then tells you about the
 * store rather than about the network. */

#include "store.h"

#include "record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct store g_store;

static void usage(void)
{
	fprintf(stderr,
		"usage: reedbed-admin --root <dir> <command> [args]\n"
		"\n"
		"  create-account <name>            make an account\n"
		"  mint-token <name>                issue a token, printed once\n"
		"  grant-owner <package> <account>  add a package owner\n"
		"  import <package> <version> <account> <kabuk>\n"
		"                                   publish an archive from disk\n"
		"  yank <package> <version> <account> [reason]\n"
		"  reindex                          rebuild the search index\n"
		"  sweep                            drop incomplete publishes\n"
		"  verify                           check every release's digest\n"
		"  list                             list packages and releases\n");
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
	*len = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[*len] = '\0';
	return buf;
}

static int die(const char *what, char *err)
{
	fprintf(stderr, "reedbed-admin: %s: %s\n", what, err ? err : "failed");
	free(err);
	return 1;
}

static const char *result_word(enum store_result r)
{
	switch (r) {
	case STORE_OK:
		return "ok";
	case STORE_EXISTS:
		return "that version already exists";
	case STORE_MISSING:
		return "no such release";
	case STORE_DENIED:
		return "not an owner of that package";
	case STORE_INVALID:
		return "invalid";
	case STORE_ERROR:
		return "store error";
	}
	return "?";
}

static int cmd_import(int argc, char **argv)
{
	if (argc < 4) {
		usage();
		return 2;
	}
	size_t len = 0;
	char *body = slurp(argv[3], &len);

	if (!body) {
		fprintf(stderr, "reedbed-admin: cannot read %s\n", argv[3]);
		return 1;
	}

	char *err = NULL;
	enum store_result r = store_publish(&g_store, argv[0], argv[1], argv[2],
					    (const unsigned char *)body, len,
					    &err);

	free(body);
	if (r != STORE_OK) {
		fprintf(stderr, "reedbed-admin: import: %s%s%s\n",
			result_word(r), err ? ": " : "", err ? err : "");
		free(err);
		return 1;
	}
	free(err);
	printf("published %s %s\n", argv[0], argv[1]);
	return 0;
}

static int cmd_verify(void)
{
	/* Re-derives every archive's digest and compares it to the record. This
	   is what a backup restore should be checked with; bit rot and a
	   half-copied rsync both surface here rather than at a client. */
	char path[STORE_PATH_MAX];
	int bad = 0, seen = 0;

	snprintf(path, sizeof path, "%s/index/jexa", g_store.root);

	char *err = NULL;

	if (!store_reindex(&g_store, &err))
		return die("reindex", err);

	size_t ilen = 0;
	char *index = slurp(path, &ilen);

	if (!index) {
		printf("store is empty\n");
		return 0;
	}

	for (char *line = index, *nl; line && *line; line = nl) {
		nl = strchr(line, '\n');
		if (nl)
			*nl++ = '\0';
		char *tab = strchr(line, '\t');

		if (!tab)
			continue;
		*tab = '\0';

		size_t blen = 0;
		char *all = store_besal_all(&g_store, line, &blen);

		if (!all)
			continue;

		struct record_set rs;

		if (record_set_parse(all, blen, &rs)) {
			for (int i = 0; i < rs.n; i++) {
				const char *ver = record_get(&rs.records[i],
							     "waktanimra");
				const char *want =
					record_get(&rs.records[i], "sema");

				if (!ver || !want)
					continue;
				seen++;

				unsigned long long size = 0;
				char got[SEMA_STR_LEN];
				int fd = store_open_kabuk(&g_store, line, ver,
							  &size, got);

				if (fd < 0) {
					printf("MISSING %s %s\n", line, ver);
					bad++;
					continue;
				}
				close(fd);

				snprintf(path, sizeof path,
					 "%s/packages/%s/releases/%s/kabuk",
					 g_store.root, line, ver);

				size_t klen = 0;
				char *kb = slurp(path, &klen);
				unsigned char d[SHA256_DIGEST_LEN];
				char actual[SEMA_STR_LEN];

				if (!kb) {
					printf("UNREADABLE %s %s\n", line, ver);
					bad++;
					continue;
				}
				sha256(kb, klen, d);
				sema_format(d, actual);
				free(kb);

				if (!sema_equal(actual, want)) {
					printf("CORRUPT %s %s\n", line, ver);
					bad++;
				}
			}
			record_set_free(&rs);
		}
		free(all);
	}
	free(index);

	printf("%d releases checked, %d bad\n", seen, bad);
	return bad ? 1 : 0;
}

static int cmd_list(void)
{
	char path[STORE_PATH_MAX];

	snprintf(path, sizeof path, "%s/index/jexa", g_store.root);

	size_t ilen = 0;
	char *index = slurp(path, &ilen);

	if (!index || ilen == 0) {
		free(index);
		printf("store is empty\n");
		return 0;
	}
	fwrite(index, 1, ilen, stdout);
	free(index);
	return 0;
}

int main(int argc, char **argv)
{
	const char *root = NULL;
	int i = 1;

	for (; i < argc; i++) {
		if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
			root = argv[++i];
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			usage();
			return 0;
		} else {
			break;
		}
	}

	if (!root || i >= argc) {
		usage();
		return 2;
	}

	char *err = NULL;

	if (!store_open(&g_store, root, true, &err))
		return die("store", err);

	const char *cmd = argv[i++];
	int rest = argc - i;
	char **args = argv + i;

	if (strcmp(cmd, "create-account") == 0) {
		if (rest < 1) {
			usage();
			return 2;
		}
		if (!store_account_create(&g_store, args[0], &err))
			return die("create-account", err);
		printf("created account %s\n", args[0]);
		return 0;
	}

	if (strcmp(cmd, "mint-token") == 0) {
		if (rest < 1) {
			usage();
			return 2;
		}
		char token[128];

		if (!store_token_mint(&g_store, args[0], token, sizeof token,
				      &err))
			return die("mint-token", err);
		/* printed once and never stored: the store keeps only its
		   digest, so this is the only time anyone can read it */
		printf("%s\n", token);
		return 0;
	}

	if (strcmp(cmd, "grant-owner") == 0) {
		if (rest < 2) {
			usage();
			return 2;
		}
		if (!store_add_owner(&g_store, args[0], args[1], &err))
			return die("grant-owner", err);
		printf("%s now owns %s\n", args[1], args[0]);
		return 0;
	}

	if (strcmp(cmd, "import") == 0)
		return cmd_import(rest, args);

	if (strcmp(cmd, "yank") == 0) {
		if (rest < 3) {
			usage();
			return 2;
		}
		enum store_result r =
			store_yank(&g_store, args[0], args[1], args[2],
				   rest > 3 ? args[3] : NULL, &err);

		if (r != STORE_OK) {
			fprintf(stderr, "reedbed-admin: yank: %s\n",
				result_word(r));
			free(err);
			return 1;
		}
		free(err);
		printf("yanked %s %s\n", args[0], args[1]);
		return 0;
	}

	if (strcmp(cmd, "reindex") == 0) {
		if (!store_reindex(&g_store, &err))
			return die("reindex", err);
		printf("reindexed\n");
		return 0;
	}

	if (strcmp(cmd, "sweep") == 0) {
		store_sweep(&g_store);
		printf("swept\n");
		return 0;
	}

	if (strcmp(cmd, "verify") == 0)
		return cmd_verify();

	if (strcmp(cmd, "list") == 0)
		return cmd_list();

	fprintf(stderr, "reedbed-admin: unknown command '%s'\n", cmd);
	usage();
	return 2;
}
