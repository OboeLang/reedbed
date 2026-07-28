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

/* sema -- prints `sha256:<hex>  <path>` for each argument, or for stdin when
   given none.

   This exists so the test suite and the vendor-drift check never have to reach
   for sha256sum, which is spelled `shasum -a 256` on macOS, and so the digest
   under test is the one the server and the compiler actually use. */

#include "sha256.h"

#include <stdio.h>
#include <string.h>

static int hash_stream(FILE *f, char *out)
{
	struct sha256_ctx c;
	unsigned char digest[SHA256_DIGEST_LEN];
	unsigned char buf[65536];
	size_t n;

	sha256_init(&c);
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		sha256_update(&c, buf, n);
	if (ferror(f))
		return -1;
	sha256_final(&c, digest);
	sema_format(digest, out);
	return 0;
}

int main(int argc, char **argv)
{
	char out[SEMA_STR_LEN];

	if (argc < 2) {
		if (hash_stream(stdin, out) != 0) {
			fprintf(stderr, "sema: read error on stdin\n");
			return 1;
		}
		printf("%s  -\n", out);
		return 0;
	}

	int rc = 0;

	for (int i = 1; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");

		if (!f) {
			fprintf(stderr, "sema: cannot open %s\n", argv[i]);
			rc = 1;
			continue;
		}
		if (hash_stream(f, out) != 0) {
			fprintf(stderr, "sema: read error on %s\n", argv[i]);
			rc = 1;
		} else {
			printf("%s  %s\n", out, argv[i]);
		}
		fclose(f);
	}
	return rc;
}
