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
#ifndef REEDBED_CONN_H
#define REEDBED_CONN_H

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>

/* Framed line/body I/O over a socket -- KATARE.md §2.

   The connection must be buffered. A body follows its status line with no
   separator, so a read() sized to "one line" will swallow the first octets of
   the body; the only correct shape is a buffer that a line is scanned out of
   and that a body read then continues from. */

/* 1024 octets including CRLF */
#define KATARE_LINE_MAX 1024
#define KATARE_CONTENT_MAX (KATARE_LINE_MAX - 2)
#define KATARE_TOKENS_MAX 64

enum rb_io {
	RB_OK = 0,
	RB_EOF, /* peer closed cleanly */
	RB_TIMEOUT, /* idle too long */
	RB_TOOLONG, /* line exceeded the cap before terminating */
	RB_MALFORMED, /* bad terminator, bad spacing */
	RB_IOERR
};

struct rb_conn {
	int fd;
	unsigned char buf[8192];
	size_t off, len; /* buffered but unconsumed: buf[off..len) */
	int timeout_secs;
	bool broken; /* a write failed; stop trying */
};

void rb_conn_init(struct rb_conn *c, int fd, int timeout_secs);

/* Applies `secs` to subsequent reads and writes. The handshake runs on a
   shorter timeout than an established session. */
void rb_conn_set_timeout(struct rb_conn *c, int secs);

/* Reads one CRLF-terminated line into `out` (NUL-terminated, CRLF stripped).
   `cap` must be at least KATARE_LINE_MAX.

   RB_TOOLONG is returned as soon as the unterminated prefix reaches the cap,
   without consuming further -- there is no resynchronising after it, so the
   caller's only move is wuwoji + close. */
enum rb_io rb_read_line(struct rb_conn *c, char *out, size_t cap, size_t *len);

/* Reads exactly `n` octets. `sink` may be -1 to discard. When `digest` is
   non-NULL every octet is fed to it, so a body can be hashed while it streams
   rather than after it lands. */
enum rb_io rb_read_body_to_fd(struct rb_conn *c, int sink, unsigned long long n,
			      struct sha256_ctx *digest);

/* Reads exactly `n` octets into a fresh buffer, hashing as it goes. The caller
   frees it. `n` must already have been checked against the body cap -- there is
   no way to skip a body we refuse, so that check belongs before this call. */
enum rb_io rb_read_body_alloc(struct rb_conn *c, unsigned long long n,
			      struct sha256_ctx *digest, unsigned char **out);

/* Writes; returns false once the connection is broken. */
bool rb_write(struct rb_conn *c, const void *p, size_t n);
bool rb_write_line(struct rb_conn *c, const char *fmt, ...);
bool rb_write_body_from_fd(struct rb_conn *c, int fd, unsigned long long n);

/* Splits a line in place on single SPs, per KATARE.md §2.1: no leading SP, no
   trailing SP, no empty token, no tab. Returns the token count, or -1 if the
   spacing is malformed. */
int rb_tokenize(char *line, char **argv, int max);

/* Parses a decimal count: no sign, no leading zeros, at most 10 digits. */
bool rb_parse_count(const char *s, unsigned long long *out);

/* True when the last two tokens are `kyx <n>`; writes the count to *n and the
   index of `kyx` to *at. */
bool rb_line_has_body(char **argv, int argc, unsigned long long *n, int *at);

const char *rb_io_reason(enum rb_io r);

#endif
