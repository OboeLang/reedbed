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
#include "conn.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

void rb_conn_init(struct rb_conn *c, int fd, int timeout_secs)
{
	memset(c, 0, sizeof *c);
	c->fd = fd;
	rb_conn_set_timeout(c, timeout_secs);
}

void rb_conn_set_timeout(struct rb_conn *c, int secs)
{
	struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };

	c->timeout_secs = secs;
	/* blocking reads with a socket timeout: the session is strictly one
	   request in flight, so a timer here is all the scheduling it needs */
	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

const char *rb_io_reason(enum rb_io r)
{
	switch (r) {
	case RB_OK:
		return "ok";
	case RB_EOF:
		return "connection closed";
	case RB_TIMEOUT:
		return "timed out";
	case RB_TOOLONG:
		return "line-too-long";
	case RB_MALFORMED:
		return "malformed-line";
	case RB_IOERR:
		return "io-error";
	}
	return "unknown";
}

/* Pulls more octets into the buffer, compacting first. */
static enum rb_io refill(struct rb_conn *c)
{
	if (c->off > 0) {
		memmove(c->buf, c->buf + c->off, c->len - c->off);
		c->len -= c->off;
		c->off = 0;
	}
	if (c->len == sizeof c->buf)
		return RB_TOOLONG;

	for (;;) {
		ssize_t n =
			read(c->fd, c->buf + c->len, sizeof c->buf - c->len);

		if (n > 0) {
			c->len += (size_t)n;
			return RB_OK;
		}
		if (n == 0)
			return RB_EOF;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return RB_TIMEOUT;
		return RB_IOERR;
	}
}

enum rb_io rb_read_line(struct rb_conn *c, char *out, size_t cap, size_t *len)
{
	size_t scanned = 0;

	for (;;) {
		/* scan only what we have not already scanned; a hostile peer
		   dribbling one octet per packet must not make this quadratic */
		for (size_t i = c->off + scanned; i < c->len; i++) {
			if (c->buf[i] != '\n') {
				/* a CR must be immediately followed by LF */
				if (c->buf[i] == '\r' && i + 1 < c->len &&
				    c->buf[i + 1] != '\n')
					return RB_MALFORMED;
				continue;
			}

			size_t linelen = i - c->off; /* excludes the LF */

			if (linelen == 0 || c->buf[i - 1] != '\r')
				return RB_MALFORMED; /* bare LF */

			size_t content = linelen - 1; /* excludes the CR */

			if (content + 2 > KATARE_LINE_MAX)
				return RB_TOOLONG;
			if (content + 1 > cap)
				return RB_TOOLONG;

			memcpy(out, c->buf + c->off, content);
			out[content] = '\0';
			c->off = i + 1;
			if (len)
				*len = content;
			return RB_OK;
		}

		scanned = c->len - c->off;
		/* the cap counts the CRLF, so an unterminated prefix of
		   KATARE_CONTENT_MAX+1 can no longer become a legal line */
		if (scanned > KATARE_CONTENT_MAX)
			return RB_TOOLONG;

		enum rb_io r = refill(c);

		if (r != RB_OK)
			return r == RB_EOF && scanned > 0 ? RB_MALFORMED : r;
	}
}

enum rb_io rb_read_body_to_fd(struct rb_conn *c, int sink, unsigned long long n,
			      struct sha256_ctx *digest)
{
	while (n > 0) {
		if (c->off == c->len) {
			enum rb_io r = refill(c);

			if (r != RB_OK)
				return r == RB_EOF ? RB_MALFORMED : r;
		}

		size_t have = c->len - c->off;
		size_t take = have < n ? have : (size_t)n;

		if (digest)
			sha256_update(digest, c->buf + c->off, take);

		if (sink >= 0) {
			size_t done = 0;

			while (done < take) {
				ssize_t w = write(sink, c->buf + c->off + done,
						  take - done);

				if (w < 0) {
					if (errno == EINTR)
						continue;
					return RB_IOERR;
				}
				done += (size_t)w;
			}
		}

		c->off += take;
		n -= take;
	}
	return RB_OK;
}

enum rb_io rb_read_body_alloc(struct rb_conn *c, unsigned long long n,
			      struct sha256_ctx *digest, unsigned char **out)
{
	unsigned char *buf = malloc(n ? (size_t)n : 1);

	if (!buf)
		return RB_IOERR;

	unsigned long long got = 0;

	while (got < n) {
		if (c->off == c->len) {
			enum rb_io r = refill(c);

			if (r != RB_OK) {
				free(buf);
				/* a body that stops short is a framing error,
				   not a clean close: the peer promised octets */
				return r == RB_EOF ? RB_MALFORMED : r;
			}
		}

		size_t have = c->len - c->off;
		size_t take = have < n - got ? have : (size_t)(n - got);

		memcpy(buf + got, c->buf + c->off, take);
		if (digest)
			sha256_update(digest, c->buf + c->off, take);
		c->off += take;
		got += take;
	}

	*out = buf;
	return RB_OK;
}

bool rb_write(struct rb_conn *c, const void *p, size_t n)
{
	const unsigned char *b = p;
	size_t off = 0;

	if (c->broken)
		return false;

	while (off < n) {
		ssize_t w = write(c->fd, b + off, n - off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			/* EPIPE included: SIGPIPE is ignored process-wide, so a
			   vanished peer surfaces here rather than as a signal */
			c->broken = true;
			return false;
		}
		off += (size_t)w;
	}
	return true;
}

bool rb_write_line(struct rb_conn *c, const char *fmt, ...)
{
	char buf[KATARE_LINE_MAX];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, KATARE_CONTENT_MAX + 1, fmt, ap);
	va_end(ap);

	if (n < 0)
		return false;
	/* refuse to emit a line we would have to truncate: a truncated status
	   line is worse than a closed connection, since the peer would parse it
	   as something else entirely */
	if (n > KATARE_CONTENT_MAX)
		n = KATARE_CONTENT_MAX;

	buf[n] = '\r';
	buf[n + 1] = '\n';
	return rb_write(c, buf, (size_t)n + 2);
}

bool rb_write_body_from_fd(struct rb_conn *c, int fd, unsigned long long n)
{
	char buf[65536];

	while (n > 0) {
		size_t want = n < sizeof buf ? (size_t)n : sizeof buf;
		ssize_t got = read(fd, buf, want);

		if (got < 0) {
			if (errno == EINTR)
				continue;
			c->broken = true;
			return false;
		}
		if (got == 0) {
			/* the file shrank under us; we already committed to a
			   length on the wire, so the only honest move is to drop
			   the connection rather than send a short body */
			c->broken = true;
			return false;
		}
		if (!rb_write(c, buf, (size_t)got))
			return false;
		n -= (unsigned long long)got;
	}
	return true;
}

int rb_tokenize(char *line, char **argv, int max)
{
	int argc = 0;
	char *p = line;

	if (*p == '\0')
		return 0;
	if (*p == ' ')
		return -1; /* leading space */

	for (;;) {
		if (argc >= max)
			return -1;
		argv[argc++] = p;

		while (*p && *p != ' ') {
			if (*p == '\t')
				return -1;
			p++;
		}
		if (*p == '\0')
			return argc;

		*p++ = '\0';
		if (*p == ' ' || *p == '\0')
			return -1; /* double space, or trailing space */
	}
}

bool rb_parse_count(const char *s, unsigned long long *out)
{
	if (!s || !*s)
		return false;
	if (s[0] == '0' && s[1] != '\0')
		return false; /* leading zeros */

	unsigned long long v = 0;
	int digits = 0;

	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9')
			return false;
		if (++digits > 10)
			return false;
		v = v * 10 + (unsigned long long)(*p - '0');
	}
	*out = v;
	return true;
}

bool rb_line_has_body(char **argv, int argc, unsigned long long *n, int *at)
{
	if (argc < 2 || strcmp(argv[argc - 2], "kyx") != 0)
		return false;
	if (!rb_parse_count(argv[argc - 1], n))
		return false;
	if (at)
		*at = argc - 2;
	return true;
}
