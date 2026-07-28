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
#ifndef REEDBED_PROTO_H
#define REEDBED_PROTO_H

#include <stdbool.h>

/* Protocol vocabulary -- KATARE.md §4, §5, §9. */

#define KATARE_VERSION "katare/1"
#define REEDBED_AGENT "reedbed/0.1"
#define KATARE_DEFAULT_PORT 440
#define KATARE_DEFAULT_BODY_CAP 67108864ULL
#define KATARE_DEFAULT_IDLE 300
#define KATARE_HANDSHAKE_TIMEOUT 30

enum rb_status {
	ST_SI, /* success */
	ST_KERESEBYR, /* not found */
	ST_SENTYRE, /* already exists */
	ST_EZHAZEBYR, /* unauthenticated, or not the owner */
	ST_VAZOJ, /* rate-limited */
	ST_RAMUZHU, /* server failure */
	ST_WUWOJI, /* client error -- always closes */
	ST_BYR, /* reserved */
	ST_KOJA /* session closing */
};

const char *rb_status_word(enum rb_status s);

/* Capability words. Mandatory verbs are never advertised, so they are absent
   here: dijabon, besal, ghazema and koja are always available. */
#define CAP_JEXA "jexa"
#define CAP_CIZUJO "cizujo"
#define CAP_KALIT "kalit"
#define CAP_KANGO "kango"
#define CAP_KALDY "kaldy"
/* reserved for a future in-session TLS upgrade; never advertised by katare/1 */
#define CAP_FIRME "firme"

/* An agent token: <izim>/<version>, at most 64 octets. This is looser than an
   izim because it has to admit the '/', and looser than a waktanimra because an
   agent version is whatever its author calls it. */
bool rb_agent_valid(const char *s);

#endif
