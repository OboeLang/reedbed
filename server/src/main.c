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
#include "ratelimit.h"
#include "session.h"
#include "store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <grp.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* Fork per connection.

   The session is one request in flight, blocking, request/response. A poll loop
   would need a resumable parser for partial lines, partial bodies and partial
   writes -- the single richest source of bugs in a hand-rolled server -- and
   would buy nothing at a registry's load. Forking also means a crash or a
   runaway allocation costs one session rather than the service. All shared
   state is the filesystem, and every store mutation is atomic. */

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_children;

static void on_term(int sig)
{
	g_stop = 1;
}

static void on_chld(int sig)
{
	int saved = errno;

	/* reap everything available; we count children ourselves for
	   max_children, so SA_NOCLDWAIT is not an option */
	while (waitpid(-1, NULL, WNOHANG) > 0) {
		if (g_children > 0)
			g_children--;
	}
	errno = saved;
}

static void install_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_chld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	/* a peer that vanishes mid-write must surface as EPIPE on the write,
	   not as a signal that kills the process */
	signal(SIGPIPE, SIG_IGN);
}

static int listen_on(int port, int *bound_port)
{
	int fd = socket(AF_INET6, SOCK_STREAM, 0);
	bool v6 = fd >= 0;

	if (!v6)
		fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "reedbed: socket: %s\n", strerror(errno));
		return -1;
	}

	int one = 1;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

	if (v6) {
		/* accept v4-mapped addresses too, so one socket serves both */
		int off = 0;

		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);

		struct sockaddr_in6 a;

		memset(&a, 0, sizeof a);
		a.sin6_family = AF_INET6;
		a.sin6_addr = in6addr_any;
		a.sin6_port = htons((uint16_t)port);
		if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
			fprintf(stderr, "reedbed: bind %d: %s\n", port,
				strerror(errno));
			close(fd);
			return -1;
		}
	} else {
		struct sockaddr_in a;

		memset(&a, 0, sizeof a);
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		a.sin_port = htons((uint16_t)port);
		if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
			fprintf(stderr, "reedbed: bind %d: %s\n", port,
				strerror(errno));
			close(fd);
			return -1;
		}
	}

	if (listen(fd, 64) != 0) {
		fprintf(stderr, "reedbed: listen: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	/* with --port 0 the kernel picks; report it so a test harness never has
	   to guess a port or race another run of itself */
	struct sockaddr_storage ss;
	socklen_t sl = sizeof ss;

	if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0) {
		if (ss.ss_family == AF_INET6)
			*bound_port =
				ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
		else
			*bound_port =
				ntohs(((struct sockaddr_in *)&ss)->sin_port);
	} else {
		*bound_port = port;
	}
	return fd;
}

static bool drop_privileges(const char *user)
{
	struct passwd *pw = getpwnam(user);

	if (!pw) {
		fprintf(stderr, "reedbed: no such user '%s'\n", user);
		return false;
	}
	if (setgroups(0, NULL) != 0 || setgid(pw->pw_gid) != 0 ||
	    setuid(pw->pw_uid) != 0) {
		fprintf(stderr, "reedbed: cannot drop privileges: %s\n",
			strerror(errno));
		return false;
	}
	/* verify rather than assume: a setuid that silently did nothing would
	   leave the whole service running as root */
	if (pw->pw_uid != 0 && setuid(0) == 0) {
		fprintf(stderr, "reedbed: privileges were not dropped\n");
		return false;
	}
	return true;
}

static void describe_peer(int fd, char *out, size_t cap)
{
	struct sockaddr_storage ss;
	socklen_t sl = sizeof ss;

	if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) {
		snprintf(out, cap, "?");
		return;
	}
	char host[64] = "?";

	if (ss.ss_family == AF_INET6)
		inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr,
			  host, sizeof host);
	else
		inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, host,
			  sizeof host);
	snprintf(out, cap, "%s", host);
}

int main(int argc, char **argv)
{
	struct rb_config cfg;

	rb_config_defaults(&cfg);
	if (!rb_config_parse_args(&cfg, argc, argv))
		return 2;

	install_signals();

	struct store store;
	char *serr = NULL;

	if (!store_open(&store, cfg.root, true, &serr)) {
		fprintf(stderr, "reedbed: %s\n",
			serr ? serr : "cannot open store");
		return 1;
	}
	/* clear out any publish that died partway before serving anything */
	store_sweep(&store);

	/* shared before any fork: a counter in the child would die with it */
	struct rl_table *rl = rl_create(4096);

	if (!rl)
		fprintf(stderr, "reedbed: no rate limiting (mmap failed)\n");

	int bound = cfg.port;
	int lfd = listen_on(cfg.port, &bound);

	if (lfd < 0)
		return 1;

	if (cfg.user && !drop_privileges(cfg.user)) {
		close(lfd);
		return 1;
	}

	if (cfg.print_port) {
		/* flushed before the accept loop, so a harness can block on
		   this line and know the socket is already listening */
		printf("listening %d\n", bound);
		fflush(stdout);
	}
	if (cfg.verbose)
		fprintf(stderr, "reedbed: listening on %d, root %s%s\n", bound,
			cfg.root, cfg.mirror ? " (mirror)" : "");

	while (!g_stop) {
		struct sockaddr_storage peer;
		socklen_t plen = sizeof peer;
		int fd = accept(lfd, (struct sockaddr *)&peer, &plen);

		if (fd < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			if (errno == EMFILE || errno == ENFILE) {
				/* out of descriptors: back off rather than
				   spin, and never exit -- this recovers */
				sleep(1);
				continue;
			}
			if (g_stop)
				break;
			fprintf(stderr, "reedbed: accept: %s\n",
				strerror(errno));
			continue;
		}

		if (g_children >= cfg.max_children) {
			struct rb_conn c;

			rb_conn_init(&c, fd, 5);
			rb_write_line(&c, "%s 5", rb_status_word(ST_RAMUZHU));
			close(fd);
			continue;
		}

		pid_t pid = fork();

		if (pid == 0) {
			struct rb_session s;

			close(lfd);
			memset(&s, 0, sizeof s);
			s.cfg = &cfg;
			s.store = &store;
			s.rl = rl;
			s.peer_addr = peer;
			rb_conn_init(&s.conn, fd, cfg.handshake_timeout);
			describe_peer(fd, s.peer, sizeof s.peer);
			rb_session_run(&s);
			shutdown(fd, SHUT_WR);
			close(fd);
			_exit(0);
		}

		if (pid < 0)
			fprintf(stderr, "reedbed: fork: %s\n", strerror(errno));
		else
			g_children++;
		close(fd);
	}

	close(lfd);
	if (cfg.verbose)
		fprintf(stderr, "reedbed: shutting down\n");
	return 0;
}
