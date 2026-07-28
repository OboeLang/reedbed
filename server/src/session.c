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
#include "session.h"

#include "proto.h"
#include "ratelimit.h"
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The connection state machine -- KATARE.md §3, §4, §5.

   Two rules shape everything here:

     - wuwoji is always fatal. Framing or semantic, the reply is the same shape
       and the connection closes. One rule, one code path.
     - reads are prefixed `ko`. That is checked before the verb is looked up, so
       a read-only mirror can refuse a write without recognising it. */

enum handled { KEEP_OPEN, CLOSE_NOW };

struct rb_verb {
	const char *name;
	bool is_read;
	bool needs_auth;
	const char *capability; /* NULL when mandatory */
	int min_args, max_args; /* excluding the verb itself */
	enum handled (*fn)(struct rb_session *s, char **argv, int argc);
};

static enum handled fatal(struct rb_session *s, const char *reason)
{
	rb_write_line(&s->conn, "%s %s", rb_status_word(ST_WUWOJI), reason);
	return CLOSE_NOW;
}

static enum handled simple(struct rb_session *s, enum rb_status st)
{
	rb_write_line(&s->conn, "%s", rb_status_word(st));
	return KEEP_OPEN;
}

/* ---- verbs ------------------------------------------------------------ */

static enum handled do_koja(struct rb_session *s, char **argv, int argc)
{
	rb_write_line(&s->conn, "%s", rb_status_word(ST_KOJA));
	return CLOSE_NOW;
}

/* Sends `si kyx <n>` and the body. */
static enum handled send_body(struct rb_session *s, const char *body,
			      size_t len)
{
	if (len > s->cfg->body_cap) {
		/* our own response overruns what we told the peer to expect */
		rb_write_line(&s->conn, "%s 5", rb_status_word(ST_RAMUZHU));
		return KEEP_OPEN;
	}
	if (!rb_write_line(&s->conn, "%s kyx %zu", rb_status_word(ST_SI), len))
		return CLOSE_NOW;
	if (len && !rb_write(&s->conn, body, len))
		return CLOSE_NOW;
	return KEEP_OPEN;
}

static enum handled do_besal(struct rb_session *s, char **argv, int argc)
{
	if (!izim_valid(argv[0]))
		return fatal(s, "bad-izim");
	if (argc == 2 && !waktanimra_valid(argv[1]))
		return fatal(s, "bad-waktanimra");

	size_t len = 0;
	char *body = argc == 2 ?
			     store_besal_one(s->store, argv[0], argv[1], &len) :
			     store_besal_all(s->store, argv[0], &len);

	if (!body)
		return simple(s, ST_KERESEBYR);

	enum handled h = send_body(s, body, len);

	free(body);
	return h;
}

static enum handled do_ghazema(struct rb_session *s, char **argv, int argc)
{
	if (!izim_valid(argv[0]))
		return fatal(s, "bad-izim");
	if (!waktanimra_valid(argv[1]))
		return fatal(s, "bad-waktanimra");

	unsigned long long size = 0;
	char sema[SEMA_STR_LEN];
	int fd = store_open_kabuk(s->store, argv[0], argv[1], &size, sema);

	if (fd < 0)
		return simple(s, ST_KERESEBYR);

	if (size > s->cfg->body_cap) {
		close(fd);
		rb_write_line(&s->conn, "%s 60", rb_status_word(ST_RAMUZHU));
		return KEEP_OPEN;
	}

	/* the digest rides on the status line so the client can verify while it
	   streams, rather than buffering the whole archive first */
	if (!rb_write_line(&s->conn, "%s %s kyx %llu", rb_status_word(ST_SI),
			   sema, size)) {
		close(fd);
		return CLOSE_NOW;
	}

	bool ok = rb_write_body_from_fd(&s->conn, fd, size);

	close(fd);
	return ok ? KEEP_OPEN : CLOSE_NOW;
}

static enum handled do_jexa(struct rb_session *s, char **argv, int argc)
{
	for (int i = 0; i < argc; i++)
		if (strlen(argv[i]) > 128)
			return fatal(s, "query-too-long");

	size_t len = 0;
	bool truncated = false;
	char *body = store_jexa(s->store, argv, argc, s->cfg->jexa_cap, &len,
				&truncated);

	if (!body) {
		rb_write_line(&s->conn, "%s 5", rb_status_word(ST_RAMUZHU));
		return KEEP_OPEN;
	}

	/* no matches is an empty body, not an error -- KATARE.md §4.1 */
	enum handled h = send_body(s, body, len);

	free(body);
	return h;
}

static enum handled do_cizujo(struct rb_session *s, char **argv, int argc)
{
	if (!izim_valid(argv[0]))
		return fatal(s, "bad-izim");
	if (!constraint_valid(argv[1]))
		return fatal(s, "bad-constraint");

	size_t len = 0;
	enum store_result why = STORE_OK;
	char *body = store_cizujo(s->store, argv[0], argv[1], &len, &why);

	if (!body) {
		if (why == STORE_INVALID)
			return fatal(s, "bad-constraint");
		if (why == STORE_ERROR) {
			rb_write_line(&s->conn, "%s 30",
				      rb_status_word(ST_RAMUZHU));
			return KEEP_OPEN;
		}
		/* an unknown package and an unsatisfiable set are both a
		   resolution that does not exist */
		return simple(s, ST_KERESEBYR);
	}

	enum handled h = send_body(s, body, len);

	free(body);
	return h;
}

static enum handled do_kalit(struct rb_session *s, char **argv, int argc)
{
	char account[IZIM_MAX + 1];
	int wait = 0;

	/* guessing a bearer token is throttled far below the ordinary request
	   rate, and this bucket is charged whether or not the token is good */
	if (!rl_allow_auth(s->rl, (const struct sockaddr *)&s->peer_addr,
			   s->cfg->rate_auth, &wait)) {
		rb_write_line(&s->conn, "%s %d", rb_status_word(ST_VAZOJ),
			      wait);
		return KEEP_OPEN;
	}

	/* every failure is the same answer with no detail, so a caller cannot
	   learn whether a token merely does not exist */
	if (!store_token_lookup(s->store, argv[0], account))
		return simple(s, ST_EZHAZEBYR);

	snprintf(s->account, sizeof s->account, "%s", account);
	rb_write_line(&s->conn, "%s %s", rb_status_word(ST_SI), s->account);
	return KEEP_OPEN;
}

/* Maps a store outcome onto a status. STORE_INVALID is the only one that
   closes, because it means the request itself was wrong. */
static enum handled store_reply(struct rb_session *s, enum store_result r,
				char *err)
{
	enum handled h;

	switch (r) {
	case STORE_OK:
		h = simple(s, ST_SI);
		break;
	case STORE_EXISTS:
		h = simple(s, ST_SENTYRE);
		break;
	case STORE_MISSING:
		h = simple(s, ST_KERESEBYR);
		break;
	case STORE_DENIED:
		h = simple(s, ST_EZHAZEBYR);
		break;
	case STORE_INVALID:
		h = fatal(s, err ? err : "invalid");
		break;
	default:
		rb_write_line(&s->conn, "%s 30", rb_status_word(ST_RAMUZHU));
		h = KEEP_OPEN;
		break;
	}
	free(err);
	return h;
}

static enum handled do_kango(struct rb_session *s, char **argv, int argc)
{
	/* argv is <izim> <waktanimra> <sema> kyx <n>; the loop already parsed
	   and cap-checked the count before dispatching here */
	unsigned long long n = 0;

	if (!rb_line_has_body(argv, argc, &n, NULL))
		return fatal(s, "kango-needs-body");
	if (!izim_valid(argv[0]) || izim_reserved(argv[0]))
		return fatal(s, "bad-izim");
	if (!waktanimra_valid(argv[1]))
		return fatal(s, "bad-waktanimra");
	if (!sema_valid(argv[2]))
		return fatal(s, "bad-sema");

	/* The body is consumed before anything is decided. Every reply below
	   except a fatal one leaves the session open, and a session with an
	   unread body in the pipe is a session out of sync. */
	struct sha256_ctx ctx;
	unsigned char *body = NULL;

	sha256_init(&ctx);

	enum rb_io r = rb_read_body_alloc(&s->conn, n, &ctx, &body);

	s->has_body = false;
	s->body_pending = 0;

	if (r != RB_OK) {
		free(body);
		return fatal(s, rb_io_reason(r));
	}

	unsigned char digest[SHA256_DIGEST_LEN];
	char actual[SEMA_STR_LEN];

	sha256_final(&ctx, digest);
	sema_format(digest, actual);

	/* the publisher's digest is never trusted -- we compare against what we
	   actually received */
	if (!sema_equal(actual, argv[2])) {
		free(body);
		return fatal(s, "sema-mismatch");
	}

	char *err = NULL;
	enum store_result sr = store_publish(s->store, argv[0], argv[1],
					     s->account, body, (size_t)n, &err);

	free(body);
	return store_reply(s, sr, err);
}

static enum handled do_kaldy(struct rb_session *s, char **argv, int argc)
{
	if (!izim_valid(argv[0]))
		return fatal(s, "bad-izim");
	if (!waktanimra_valid(argv[1]))
		return fatal(s, "bad-waktanimra");

	/* the reason is the rest of the line, rejoined; tokenising split it on
	   the single spaces the framing guarantees, so this is lossless */
	char reason[KATARE_LINE_MAX];
	size_t o = 0;

	reason[0] = '\0';
	for (int i = 2; i < argc; i++) {
		size_t len = strlen(argv[i]);

		if (o + len + 2 > sizeof reason)
			break;
		if (o)
			reason[o++] = ' ';
		memcpy(reason + o, argv[i], len);
		o += len;
		reason[o] = '\0';
	}

	char *err = NULL;
	enum store_result sr = store_yank(s->store, argv[0], argv[1],
					  s->account, o ? reason : NULL, &err);

	return store_reply(s, sr, err);
}

static const struct rb_verb k_verbs[] = {
	{ "koja", false, false, NULL, 0, 0, do_koja },
	{ "kalit", false, false, CAP_KALIT, 1, 1, do_kalit },
	{ "kango", false, true, CAP_KANGO, 5, 5, do_kango },
	{ "kaldy", false, true, CAP_KALDY, 2, 32, do_kaldy },
	{ "ko besal", true, false, NULL, 1, 2, do_besal },
	{ "ko ghazema", true, false, NULL, 2, 2, do_ghazema },
	{ "ko jexa", true, false, CAP_JEXA, 1, 16, do_jexa },
	{ "ko cizujo", true, false, CAP_CIZUJO, 2, 2, do_cizujo },
};

/* The `ko` prefix is what a mirror filters on, so an entry that disagrees with
   its own is_read flag would silently open a hole. Checked once at startup
   rather than trusted. */
static bool verbs_consistent(void)
{
	for (size_t i = 0; i < sizeof k_verbs / sizeof *k_verbs; i++) {
		bool prefixed = strncmp(k_verbs[i].name, "ko ", 3) == 0;

		if (prefixed != k_verbs[i].is_read)
			return false;
	}
	return true;
}

/* ---- dispatch --------------------------------------------------------- */

/* Requests are `<verb>` or `ko <verb>`, so the lookup key is one or two
   tokens. Returns the matched entry and how many tokens it consumed. */
static const struct rb_verb *lookup(char **argv, int argc, int *consumed)
{
	for (size_t i = 0; i < sizeof k_verbs / sizeof *k_verbs; i++) {
		const struct rb_verb *v = &k_verbs[i];
		const char *sp = strchr(v->name, ' ');

		if (sp) {
			if (argc >= 2 &&
			    strncmp(argv[0], v->name, (size_t)(sp - v->name)) ==
				    0 &&
			    strlen(argv[0]) == (size_t)(sp - v->name) &&
			    strcmp(argv[1], sp + 1) == 0) {
				*consumed = 2;
				return v;
			}
		} else if (strcmp(argv[0], v->name) == 0) {
			*consumed = 1;
			return v;
		}
	}
	return NULL;
}

static enum handled dispatch(struct rb_session *s, char **argv, int argc)
{
	/* classify before recognising: a mirror refuses every write without
	   needing to know what the verb would have done */
	bool is_read = strcmp(argv[0], "ko") == 0;

	if (s->cfg->mirror && !is_read)
		return simple(s, ST_EZHAZEBYR);

	int consumed = 0;
	const struct rb_verb *v = lookup(argv, argc, &consumed);

	if (!v)
		return fatal(s, "unknown-verb");
	if (v->is_read != is_read)
		return fatal(s, "unknown-verb");

	int nargs = argc - consumed;

	if (nargs < v->min_args || nargs > v->max_args)
		return fatal(s, "bad-argument-count");

	/* a verb gated behind a capability we did not advertise is, from the
	   client's side, indistinguishable from one we never had */
	if (v->capability && !rb_cap_enabled(s->cfg, v->capability))
		return fatal(s, "unknown-verb");

	if (v->needs_auth && s->account[0] == '\0')
		return simple(s, ST_EZHAZEBYR);

	return v->fn(s, argv + consumed, nargs);
}

/* ---- handshake -------------------------------------------------------- */

static bool handshake(struct rb_session *s)
{
	char line[KATARE_LINE_MAX];
	char *argv[KATARE_TOKENS_MAX];

	rb_conn_set_timeout(&s->conn, s->cfg->handshake_timeout);

	if (!rb_write_line(&s->conn, "dijabon %s %s", KATARE_VERSION,
			   REEDBED_AGENT))
		return false;

	enum rb_io r = rb_read_line(&s->conn, line, sizeof line, NULL);

	if (r != RB_OK) {
		if (r != RB_EOF)
			rb_write_line(&s->conn, "%s %s",
				      rb_status_word(ST_WUWOJI),
				      rb_io_reason(r));
		return false;
	}

	int argc = rb_tokenize(line, argv, KATARE_TOKENS_MAX);

	if (argc != 3 || strcmp(argv[0], "dijabon") != 0) {
		rb_write_line(&s->conn, "%s expected-dijabon",
			      rb_status_word(ST_WUWOJI));
		return false;
	}
	if (strcmp(argv[1], KATARE_VERSION) != 0) {
		rb_write_line(&s->conn, "%s unsupported-protocol",
			      rb_status_word(ST_WUWOJI));
		return false;
	}
	if (!rb_agent_valid(argv[2])) {
		rb_write_line(&s->conn, "%s bad-agent",
			      rb_status_word(ST_WUWOJI));
		return false;
	}

	/* Capabilities. Built by the same predicate that gates dispatch, so a
	   verb can never be advertised and then refused, or refused and then
	   quietly served. */
	char caps[KATARE_LINE_MAX];

	rb_config_caps(s->cfg, caps, sizeof caps);

	if (!rb_write_line(&s->conn, "%s %s", rb_status_word(ST_SI), caps))
		return false;

	rb_conn_set_timeout(&s->conn, s->cfg->idle_timeout);
	return true;
}

/* ---- the loop --------------------------------------------------------- */

void rb_session_run(struct rb_session *s)
{
	char line[KATARE_LINE_MAX];
	char *argv[KATARE_TOKENS_MAX];

	if (!verbs_consistent()) {
		/* a build-time invariant that failed; refuse to serve rather
		   than serve with a mirror that leaks writes */
		rb_write_line(&s->conn, "%s internal",
			      rb_status_word(ST_RAMUZHU));
		return;
	}

	if (!handshake(s))
		return;

	for (;;) {
		enum rb_io r = rb_read_line(&s->conn, line, sizeof line, NULL);

		if (r == RB_EOF)
			return;
		if (r == RB_TIMEOUT) {
			/* the one unsolicited line a server may send */
			rb_write_line(&s->conn, "%s", rb_status_word(ST_KOJA));
			return;
		}
		if (r != RB_OK) {
			rb_write_line(&s->conn, "%s %s",
				      rb_status_word(ST_WUWOJI),
				      rb_io_reason(r));
			return;
		}

		int argc = rb_tokenize(line, argv, KATARE_TOKENS_MAX);

		if (argc <= 0) {
			rb_write_line(&s->conn, "%s malformed-line",
				      rb_status_word(ST_WUWOJI));
			return;
		}

		/* A body is announced by the request line, and the stream is
		   only parseable if we agree with the peer about where it
		   ends. So both of these are settled before any handler runs:

		     - a `kyx` whose count does not parse leaves us unable to
		       find the next line at all;
		     - a count over the cap is one we have refused, and we
		       cannot drain what we refused.

		   Neither is recoverable, so both close. */
		unsigned long long bodylen;

		s->has_body = false;
		s->body_pending = 0;

		if (argc >= 2 && strcmp(argv[argc - 2], "kyx") == 0) {
			if (!rb_parse_count(argv[argc - 1], &bodylen)) {
				rb_write_line(&s->conn, "%s bad-length",
					      rb_status_word(ST_WUWOJI));
				return;
			}
			if (bodylen > s->cfg->body_cap) {
				rb_write_line(&s->conn, "%s body-too-large",
					      rb_status_word(ST_WUWOJI));
				return;
			}
			s->has_body = true;
			s->body_pending = bodylen;
		}

		int wait = 0;

		/* koja is never throttled: refusing a close achieves nothing
		   and leaves the caller holding a connection slot it is trying
		   to give back */
		if (argc == 1 && strcmp(argv[0], "koja") == 0) {
			do_koja(s, argv, argc);
			return;
		}

		if (!rl_allow_request(s->rl,
				      (const struct sockaddr *)&s->peer_addr,
				      s->cfg->rate_requests, &wait) ||
		    (s->has_body &&
		     !rl_allow_bytes(
			     s->rl, (const struct sockaddr *)&s->peer_addr,
			     s->body_pending, s->cfg->rate_bytes, &wait))) {
			/* vazoj is one of the two statuses that leave the
			   session usable, so the body still has to go */
			rb_write_line(&s->conn, "%s %d",
				      rb_status_word(ST_VAZOJ), wait);
			if (s->has_body) {
				enum rb_io d = rb_read_body_to_fd(
					&s->conn, -1, s->body_pending, NULL);

				s->has_body = false;
				if (d != RB_OK)
					return;
			}
			continue;
		}

		if (dispatch(s, argv, argc) == CLOSE_NOW)
			return;

		/* the handler declined without reading its body; drop the
		   octets on the floor so the next line starts where the peer
		   thinks it does */
		if (s->has_body) {
			enum rb_io d = rb_read_body_to_fd(
				&s->conn, -1, s->body_pending, NULL);

			s->has_body = false;
			if (d != RB_OK)
				return;
		}
	}
}
