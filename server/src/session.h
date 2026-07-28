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
#ifndef REEDBED_SESSION_H
#define REEDBED_SESSION_H

#include "conn.h"
#include "config.h"
#include "izim.h"
#include "ratelimit.h"
#include "store.h"

struct rb_session {
	struct rb_conn conn;
	const struct rb_config *cfg;
	struct store *store;
	struct rl_table *rl;
	struct sockaddr_storage peer_addr;
	char account[IZIM_MAX + 1]; /* empty when unauthenticated */

	/* Octets the current request said would follow. A handler that consumes
	   them clears this; anything still pending when the handler returns is
	   drained by the loop. Without that, every early rejection -- not
	   authenticated, capability off, wrong arity, read-only mirror -- would
	   leave a body in the pipe and desynchronise the session. */
	unsigned long long body_pending;
	bool has_body;
	char peer[64]; /* for logging */
};

/* Runs one connection start to finish: greeting, handshake, capabilities, then
   requests until koja, timeout, EOF or a fatal wuwoji. Closes nothing -- the
   caller owns the fd. */
void rb_session_run(struct rb_session *s);

#endif
