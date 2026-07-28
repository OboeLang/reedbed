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

/* katarecat -- replays a scripted exchange against a katare server and echoes
   everything received, with CR shown as \r and other non-printables as \xNN.
   The transcript is therefore plain text and diffs against a golden file.

   This deliberately speaks the protocol badly on request: the framing rules are
   only worth writing down if something tests that they are enforced, and that
   something has to be able to send a bare LF, a doubled space and a truncated
   body.

   Script directives, one per line:

     > <text>          send <text> followed by CRLF
     >lf <text>        send <text> followed by a bare LF   (a framing error)
     >raw <hex>...     send exact octets, e.g. `>raw 0d 0a`
     >body <file>      send a file's octets with no framing
     >rep <n> <char>   send <char> repeated <n> times, no terminator
     < <n>             read and echo <n> lines
     <all              read and echo until the peer closes
     #  ...            comment
*/

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int dial(const char *host, const char *port)
{
	struct addrinfo hints, *res, *p;

	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;

	int rc = getaddrinfo(host, port, &hints, &res);

	if (rc != 0) {
		fprintf(stderr, "katarecat: %s:%s: %s\n", host, port,
			gai_strerror(rc));
		return -1;
	}

	for (p = res; p; p = p->ai_next) {
		int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);

		if (fd < 0)
			continue;
		if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
			struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };

			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
			setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
			freeaddrinfo(res);
			return fd;
		}
		close(fd);
	}
	freeaddrinfo(res);
	fprintf(stderr, "katarecat: cannot connect to %s:%s\n", host, port);
	return -1;
}

static bool send_all(int fd, const void *p, size_t n)
{
	const char *b = p;
	size_t off = 0;

	while (off < n) {
		ssize_t w = write(fd, b + off, n - off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		off += (size_t)w;
	}
	return true;
}

/* Echoes one octet in a form that survives a diff. */
static void echo_octet(unsigned char c)
{
	if (c == '\r')
		fputs("\\r", stdout);
	else if (c == '\n')
		fputc('\n', stdout);
	else if (isprint(c))
		fputc(c, stdout);
	else
		printf("\\x%02x", c);
}

/* Reads until `lines` LFs have been echoed, or until close when lines < 0. */
static void echo_lines(int fd, int lines)
{
	unsigned char c;
	int seen = 0;

	for (;;) {
		ssize_t n = read(fd, &c, 1);

		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			if (lines < 0)
				puts("[closed]");
			else if (n == 0)
				puts("[closed early]");
			else
				puts("[read error]");
			return;
		}
		echo_octet(c);
		if (c == '\n' && lines >= 0 && ++seen >= lines)
			return;
	}
}

static bool send_hex(int fd, const char *spec)
{
	unsigned char buf[512];
	size_t n = 0;

	while (*spec && n < sizeof buf) {
		while (*spec == ' ')
			spec++;
		if (!*spec)
			break;

		char *end;
		long v = strtol(spec, &end, 16);

		if (end == spec || v < 0 || v > 255) {
			fprintf(stderr, "katarecat: bad hex '%s'\n", spec);
			return false;
		}
		buf[n++] = (unsigned char)v;
		spec = end;
	}
	return send_all(fd, buf, n);
}

static bool send_file(int fd, const char *path)
{
	FILE *f = fopen(path, "rb");

	if (!f) {
		fprintf(stderr, "katarecat: cannot open %s\n", path);
		return false;
	}

	char buf[65536];
	size_t n;
	bool ok = true;

	while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0)
		ok = send_all(fd, buf, n);
	fclose(f);
	return ok;
}

static bool send_repeat(int fd, const char *spec)
{
	long n = strtol(spec, (char **)&spec, 10);

	while (*spec == ' ')
		spec++;
	if (n <= 0 || n > 1000000 || !*spec) {
		fprintf(stderr, "katarecat: bad >rep\n");
		return false;
	}

	char chunk[4096];
	memset(chunk, *spec, sizeof chunk);

	while (n > 0) {
		size_t take = n < (long)sizeof chunk ? (size_t)n : sizeof chunk;

		if (!send_all(fd, chunk, take))
			return false;
		n -= (long)take;
	}
	return true;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr,
			"usage: katarecat <host> <port> [script]\n"
			"       reads the script from stdin when not given\n");
		return 2;
	}

	FILE *script = stdin;

	if (argc >= 4) {
		script = fopen(argv[3], "r");
		if (!script) {
			fprintf(stderr, "katarecat: cannot open %s\n", argv[3]);
			return 2;
		}
	}

	int fd = dial(argv[1], argv[2]);

	if (fd < 0)
		return 1;

	char line[4096];
	int rc = 0;

	while (fgets(line, sizeof line, script)) {
		size_t n = strlen(line);

		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';

		if (n == 0 || line[0] == '#')
			continue;

		bool ok = true;

		if (strncmp(line, ">raw ", 5) == 0) {
			ok = send_hex(fd, line + 5);
		} else if (strncmp(line, ">body ", 6) == 0) {
			ok = send_file(fd, line + 6);
		} else if (strncmp(line, ">rep ", 5) == 0) {
			ok = send_repeat(fd, line + 5);
		} else if (strncmp(line, ">lf ", 4) == 0) {
			ok = send_all(fd, line + 4, n - 4) &&
			     send_all(fd, "\n", 1);
		} else if (strncmp(line, "> ", 2) == 0) {
			ok = send_all(fd, line + 2, n - 2) &&
			     send_all(fd, "\r\n", 2);
		} else if (strcmp(line, ">") == 0) {
			ok = send_all(fd, "\r\n", 2);
		} else if (strcmp(line, "<all") == 0) {
			echo_lines(fd, -1);
		} else if (strncmp(line, "< ", 2) == 0) {
			echo_lines(fd, atoi(line + 2));
		} else {
			fprintf(stderr, "katarecat: bad directive: %s\n", line);
			rc = 2;
			break;
		}

		if (!ok) {
			puts("[write failed]");
			break;
		}
	}

	close(fd);
	if (script != stdin)
		fclose(script);
	return rc;
}
