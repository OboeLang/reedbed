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
#include "config.h"

#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rb_config_defaults(struct rb_config *c)
{
	memset(c, 0, sizeof *c);
	c->root = "/var/lib/reedbed";
	c->port = KATARE_DEFAULT_PORT;
	c->idle_timeout = KATARE_DEFAULT_IDLE;
	c->handshake_timeout = KATARE_HANDSHAKE_TIMEOUT;
	c->max_children = 64;
	c->body_cap = KATARE_DEFAULT_BODY_CAP;
	c->jexa_cap = 100;
	c->rate_requests = 600;
	c->rate_bytes = 512ULL * 1024 * 1024;
	c->rate_auth = 10;
	c->cizujo = true;
}

void rb_config_usage(void)
{
	fprintf(stderr,
		"usage: reedbed [options]\n"
		"  --root <dir>           store directory (default /var/lib/reedbed)\n"
		"  --port <n>             listen port, 0 for an ephemeral one (default 440)\n"
		"  --print-port           write 'listening <port>' to stdout once bound\n"
		"  --foreground           do not detach\n"
		"  --user <name>          drop privileges after binding\n"
		"  --idle-timeout <secs>  session idle timeout (default 300)\n"
		"  --max-children <n>     concurrent connections (default 64)\n"
		"  --body-cap <octets>    largest body accepted or sent (default 67108864)\n"
		"  --jexa-cap <n>         max search results (default 100)\n"
		"  --rate <n>             requests per minute per peer, 0 to disable\n"
		"  --rate-bytes <n>       octets per minute per peer, 0 to disable\n"
		"  --rate-auth <n>        kalit attempts per minute per peer\n"
		"  --no-cizujo            do not offer server-side resolution\n"
		"  --mirror               read-only: reject every request not prefixed 'ko'\n"
		"  -v, --verbose\n");
}

static bool want_int(int argc, char **argv, int *i, long *out, long lo, long hi)
{
	if (*i + 1 >= argc) {
		fprintf(stderr, "reedbed: %s needs a value\n", argv[*i]);
		return false;
	}
	char *end;
	long v = strtol(argv[++*i], &end, 10);

	if (*end != '\0' || v < lo || v > hi) {
		fprintf(stderr, "reedbed: %s: bad value '%s'\n", argv[*i - 1],
			argv[*i]);
		return false;
	}
	*out = v;
	return true;
}

static bool want_str(int argc, char **argv, int *i, const char **out)
{
	if (*i + 1 >= argc) {
		fprintf(stderr, "reedbed: %s needs a value\n", argv[*i]);
		return false;
	}
	*out = argv[++*i];
	return true;
}

bool rb_config_parse_args(struct rb_config *c, int argc, char **argv)
{
	long v;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "--root") == 0) {
			if (!want_str(argc, argv, &i, &c->root))
				return false;
		} else if (strcmp(a, "--user") == 0) {
			if (!want_str(argc, argv, &i, &c->user))
				return false;
		} else if (strcmp(a, "--port") == 0) {
			if (!want_int(argc, argv, &i, &v, 0, 65535))
				return false;
			c->port = (int)v;
		} else if (strcmp(a, "--idle-timeout") == 0) {
			if (!want_int(argc, argv, &i, &v, 1, 86400))
				return false;
			c->idle_timeout = (int)v;
		} else if (strcmp(a, "--max-children") == 0) {
			if (!want_int(argc, argv, &i, &v, 1, 4096))
				return false;
			c->max_children = (int)v;
		} else if (strcmp(a, "--body-cap") == 0) {
			if (!want_int(argc, argv, &i, &v, 1, 1073741824))
				return false;
			c->body_cap = (unsigned long long)v;
		} else if (strcmp(a, "--jexa-cap") == 0) {
			if (!want_int(argc, argv, &i, &v, 1, 100000))
				return false;
			c->jexa_cap = (int)v;
		} else if (strcmp(a, "--rate") == 0) {
			if (!want_int(argc, argv, &i, &v, 0, 1000000))
				return false;
			c->rate_requests = (int)v;
		} else if (strcmp(a, "--rate-auth") == 0) {
			if (!want_int(argc, argv, &i, &v, 0, 1000000))
				return false;
			c->rate_auth = (int)v;
		} else if (strcmp(a, "--rate-bytes") == 0) {
			if (!want_int(argc, argv, &i, &v, 0, 1073741824))
				return false;
			c->rate_bytes = (unsigned long long)v;
		} else if (strcmp(a, "--no-cizujo") == 0) {
			c->cizujo = false;
		} else if (strcmp(a, "--print-port") == 0) {
			c->print_port = true;
		} else if (strcmp(a, "--foreground") == 0) {
			c->foreground = true;
		} else if (strcmp(a, "--mirror") == 0) {
			c->mirror = true;
		} else if (strcmp(a, "-v") == 0 ||
			   strcmp(a, "--verbose") == 0) {
			c->verbose = true;
		} else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
			rb_config_usage();
			exit(0);
		} else {
			fprintf(stderr, "reedbed: unknown option '%s'\n", a);
			rb_config_usage();
			return false;
		}
	}
	return true;
}

bool rb_cap_enabled(const struct rb_config *c, const char *cap)
{
	/* jexa is always on; the write verbs are exactly what a mirror drops */
	if (strcmp(cap, CAP_JEXA) == 0)
		return true;
	if (strcmp(cap, CAP_CIZUJO) == 0)
		return c->cizujo;
	if (strcmp(cap, CAP_KALIT) == 0 || strcmp(cap, CAP_KANGO) == 0 ||
	    strcmp(cap, CAP_KALDY) == 0)
		return !c->mirror;
	/* firme is reserved for a future TLS upgrade that katare/1 must never
	   advertise */
	return false;
}

void rb_config_caps(const struct rb_config *c, char *out, size_t cap)
{
	int n = snprintf(out, cap, "%s", CAP_JEXA);

	if (rb_cap_enabled(c, CAP_CIZUJO))
		n += snprintf(out + n, cap - (size_t)n, " %s", CAP_CIZUJO);

	if (rb_cap_enabled(c, CAP_KALIT))
		n += snprintf(out + n, cap - (size_t)n, " %s %s %s", CAP_KALIT,
			      CAP_KANGO, CAP_KALDY);
	snprintf(out + n, cap - (size_t)n, " kyx%llu", c->body_cap);
}
