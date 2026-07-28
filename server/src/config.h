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
#ifndef REEDBED_CONFIG_H
#define REEDBED_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* Server settings.

   Timeouts and limits are settings rather than constants specifically so the
   test suite can exercise them: a 300-second idle timeout is untestable, a
   1-second one is a fixture. */

struct rb_config {
	const char *root; /* store directory */
	int port;
	int idle_timeout; /* seconds; KATARE.md §7.1 */
	int handshake_timeout;
	int max_children;
	unsigned long long body_cap;
	int jexa_cap; /* max records per search */
	int rate_requests; /* per minute per peer; 0 disables */
	unsigned long long rate_bytes; /* per minute per peer; 0 disables */
	int rate_auth; /* kalit attempts per minute per peer */
	bool cizujo; /* advertise server-side resolution */
	bool mirror; /* read-only: reject anything not prefixed `ko` */
	bool print_port; /* announce the bound port on stdout */
	bool foreground;
	bool verbose;
	const char *user; /* drop privileges to this after binding */
};

void rb_config_defaults(struct rb_config *c);

/* Is an optional verb's capability advertised by this configuration? A mirror
   advertises no write verb, so the same predicate that builds the capability
   line also gates dispatch -- the two can never disagree. */
bool rb_cap_enabled(const struct rb_config *c, const char *cap);

/* Builds the capability line's word list (without the leading status). */
void rb_config_caps(const struct rb_config *c, char *out, size_t cap);

/* Parses argv. Returns false and prints usage on a bad option. */
bool rb_config_parse_args(struct rb_config *c, int argc, char **argv);

void rb_config_usage(void);

#endif
