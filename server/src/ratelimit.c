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
#include "ratelimit.h"

#include <netinet/in.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/* Tokens are fixed point so a fractional refill is not lost to truncation. */
#define RL_ONE (1u << 16)
#define RL_KINDS 3 /* requests, octets, auth attempts */

struct rl_slot {
	_Atomic uint64_t key;
	_Atomic uint64_t tokens;
	_Atomic uint64_t stamp;
};

struct rl_table {
	int slots;
	struct rl_slot s[];
};

struct rl_table *rl_create(int slots)
{
	size_t n = sizeof(struct rl_table) +
		   (size_t)slots * RL_KINDS * sizeof(struct rl_slot);
	void *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		return NULL;

	struct rl_table *t = p;

	t->slots = slots;
	return t;
}

/* Peers are keyed by prefix, not by exact address: a v6 client has a /64 to
   itself and could otherwise walk to a fresh address for every request. */
static uint64_t peer_key(const struct sockaddr *sa)
{
	uint64_t h = 1469598103934665603ULL; /* FNV-1a */
	const unsigned char *p;
	size_t n;

	if (sa->sa_family == AF_INET6) {
		p = (const unsigned char *)&((const struct sockaddr_in6 *)sa)
			    ->sin6_addr;
		n = 8;
	} else if (sa->sa_family == AF_INET) {
		p = (const unsigned char *)&((const struct sockaddr_in *)sa)
			    ->sin_addr;
		n = 4;
	} else {
		return 1;
	}

	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h ? h : 1; /* 0 marks an empty slot */
}

static uint64_t now_secs(void)
{
	struct timespec ts;

	/* monotonic, so a clock step cannot hand out a windfall of tokens */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec;
}

/* Refills by elapsed time, then tries to take `cost`. Burst is one minute's
   worth, so an idle client can spend a full minute's budget at once. */
static bool take(struct rl_table *t, const struct sockaddr *peer, int kind,
		 unsigned long long cost, unsigned long long per_minute,
		 int *retry_after)
{
	if (per_minute == 0)
		return true; /* unlimited */

	uint64_t key = peer_key(peer);
	size_t idx =
		(size_t)(key % (uint64_t)t->slots) * RL_KINDS + (size_t)kind;
	struct rl_slot *s = &t->s[idx];
	uint64_t now = now_secs();
	uint64_t cap = per_minute * RL_ONE;

	uint64_t prev = atomic_load(&s->key);

	if (prev != key) {
		/* a different peer had this slot: claim it with a full bucket.
		   Collisions are benign by design -- the cost is a limit that
		   is occasionally too generous, never a lock. */
		atomic_store(&s->key, key);
		atomic_store(&s->tokens, cap);
		atomic_store(&s->stamp, now);
	} else {
		uint64_t last = atomic_load(&s->stamp);

		if (now > last) {
			uint64_t add = (now - last) * cap / 60;
			uint64_t have = atomic_load(&s->tokens);

			have = have + add > cap ? cap : have + add;
			atomic_store(&s->tokens, have);
			atomic_store(&s->stamp, now);
		}
	}

	uint64_t want = cost * RL_ONE;
	uint64_t have = atomic_load(&s->tokens);

	if (have < want) {
		/* seconds until enough tokens exist, rounded up and clamped to
		   the sego range the protocol allows */
		uint64_t missing = want - have;
		uint64_t per_sec = cap / 60 ? cap / 60 : 1;
		uint64_t wait = (missing + per_sec - 1) / per_sec;

		if (wait < 1)
			wait = 1;
		if (wait > 86400)
			wait = 86400;
		*retry_after = (int)wait;
		return false;
	}

	atomic_store(&s->tokens, have - want);
	return true;
}

bool rl_allow_request(struct rl_table *t, const struct sockaddr *peer, int rate,
		      int *retry_after)
{
	if (!t)
		return true;
	return take(t, peer, 0, 1, (unsigned long long)rate, retry_after);
}

bool rl_allow_bytes(struct rl_table *t, const struct sockaddr *peer,
		    unsigned long long bytes, unsigned long long rate,
		    int *retry_after)
{
	if (!t)
		return true;
	return take(t, peer, 1, bytes, rate, retry_after);
}

bool rl_allow_auth(struct rl_table *t, const struct sockaddr *peer, int rate,
		   int *retry_after)
{
	if (!t)
		return true;
	return take(t, peer, 2, 1, (unsigned long long)rate, retry_after);
}
