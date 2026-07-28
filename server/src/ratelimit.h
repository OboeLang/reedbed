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
#ifndef REEDBED_RATELIMIT_H
#define REEDBED_RATELIMIT_H

#include <stdbool.h>
#include <sys/socket.h>

/* Token buckets in shared memory -- KATARE.md §7.3.
 *
 * Fork-per-connection means a counter in the child dies with the child, so the
 * table is mmap'd MAP_SHARED before any fork and every child updates the same
 * memory. Slots are direct-mapped by a hash of the peer prefix and updated with
 * C11 atomics rather than a lock: two peers colliding on a slot, or an update
 * lost to a race, cost at worst a slightly wrong limit, and that is a much
 * better trade than a lock every child contends on.
 *
 * Two buckets per peer. Requests alone would let one client stream 64 MiB
 * archives all day inside its quota, so bytes are limited separately. */

struct rl_table;

/* Allocates the shared table. Call before forking anything. */
struct rl_table *rl_create(int slots);

/* Consumes one request token. Returns true when allowed; otherwise writes the
   seconds to wait to *retry_after. `rate` is requests per minute. */
bool rl_allow_request(struct rl_table *t, const struct sockaddr *peer, int rate,
		      int *retry_after);

/* Consumes `bytes` from the peer's byte bucket. `rate` is octets per minute. */
bool rl_allow_bytes(struct rl_table *t, const struct sockaddr *peer,
		    unsigned long long bytes, unsigned long long rate,
		    int *retry_after);

/* A much tighter bucket for authentication attempts, so a stolen-token guessing
   run is throttled far below the ordinary request rate. */
bool rl_allow_auth(struct rl_table *t, const struct sockaddr *peer, int rate,
		   int *retry_after);

#endif
