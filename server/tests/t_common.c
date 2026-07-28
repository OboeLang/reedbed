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

/* Offline tests for common/: everything that is a pure function of its input,
   tested without a socket. Prints one line per case and exits non-zero if any
   failed. */

#include "izim.h"
#include "kabuk.h"
#include "record.h"
#include "sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

static void ok(bool cond, const char *fmt, ...)
{
	va_list ap;
	char what[256];

	va_start(ap, fmt);
	vsnprintf(what, sizeof what, fmt, ap);
	va_end(ap);

	checks++;
	if (!cond) {
		failures++;
		printf("FAIL %s\n", what);
	}
}

/* ---- sha256 ---------------------------------------------------------- */

static const char *hexdigest(const void *p, size_t n)
{
	static char out[SEMA_STR_LEN];
	unsigned char d[SHA256_DIGEST_LEN];

	sha256(p, n, d);
	sema_format(d, out);
	return out + 7; /* skip "sha256:" */
}

static void test_sha256(void)
{
	/* FIPS 180-4 / NIST examples */
	ok(strcmp(hexdigest("", 0),
		  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") ==
		   0,
	   "sha256 of empty string");
	ok(strcmp(hexdigest("abc", 3),
		  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ==
		   0,
	   "sha256 of \"abc\"");
	ok(strcmp(hexdigest(
			  "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
			  56),
		  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") ==
		   0,
	   "sha256 of the 56-octet message");

	/* the padding boundaries: 55 fits with the length, 56 does not and
	   spills into a second block, 64 is exactly one block */
	static const struct {
		size_t n;
		const char *want;
	} lens[] = {
		{ 55,
		  "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
		{ 56,
		  "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
		{ 63,
		  "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
		{ 64,
		  "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
		{ 65,
		  "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0" },
	};
	for (size_t i = 0; i < sizeof lens / sizeof *lens; i++) {
		char *buf = malloc(lens[i].n);

		memset(buf, 'a', lens[i].n);
		ok(strcmp(hexdigest(buf, lens[i].n), lens[i].want) == 0,
		   "sha256 of %zu 'a's", lens[i].n);
		free(buf);
	}

	/* a million 'a', fed in awkward chunks so the buffering path is
	   exercised rather than the aligned fast path */
	struct sha256_ctx c;
	unsigned char d[SHA256_DIGEST_LEN];
	char chunk[1000];
	char out[SEMA_STR_LEN];

	memset(chunk, 'a', sizeof chunk);
	sha256_init(&c);
	for (int i = 0; i < 1000; i++)
		sha256_update(&c, chunk, sizeof chunk);
	sha256_final(&c, d);
	sema_format(d, out);
	ok(strcmp(out + 7,
		  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") ==
		   0,
	   "sha256 of a million 'a's");

	sha256_init(&c);
	for (int i = 0; i < 1000000; i++)
		sha256_update(&c, "a", 1);
	sha256_final(&c, d);
	sema_format(d, out);
	ok(strcmp(out + 7,
		  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") ==
		   0,
	   "sha256 of a million 'a's, one octet at a time");

	ok(sema_valid(
		   "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
	   "sema_valid accepts a good digest");
	ok(!sema_valid(
		   "sha256:E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"),
	   "sema_valid rejects uppercase hex");
	ok(!sema_valid("sha256:e3b0c4"), "sema_valid rejects a short digest");
	ok(!sema_valid(
		   "md5:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
	   "sema_valid rejects another algorithm");
	ok(!sema_valid(
		   "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855x"),
	   "sema_valid rejects trailing junk");
}

/* ---- izim, versions, constraints -------------------------------------- */

static void test_izim(void)
{
	ok(izim_valid("http"), "izim accepts 'http'");
	ok(izim_valid("my_lib2"), "izim accepts 'my_lib2'");
	ok(!izim_valid("Http"), "izim rejects uppercase");
	ok(!izim_valid("2fast"), "izim rejects a leading digit");
	ok(!izim_valid("_x"), "izim rejects a leading underscore");
	ok(!izim_valid("my-lib"), "izim rejects a hyphen");
	ok(!izim_valid(""), "izim rejects empty");
	ok(izim_reserved("os") && izim_reserved("math") &&
		   izim_reserved("random") && izim_reserved("oboe"),
	   "reserved names are reserved");
	ok(!izim_reserved("http"), "'http' is not reserved");

	ok(waktanimra_valid("1.0.0"), "version accepts 1.0.0");
	ok(waktanimra_valid("0.0.1-rc.1"), "version accepts a dotted tag");
	ok(!waktanimra_valid("1.0"), "version rejects two components");
	ok(!waktanimra_valid("1.0.0."), "version rejects a trailing dot");
	ok(!waktanimra_valid("01.0.0"), "version rejects leading zeros");
	ok(!waktanimra_valid("1.0.0-"), "version rejects an empty tag");
	ok(!waktanimra_valid("1.0.0+meta"), "version rejects build metadata");

	ok(waktanimra_cmp_str("1.0.0", "1.0.1") < 0, "1.0.0 < 1.0.1");
	ok(waktanimra_cmp_str("1.9.0", "1.10.0") < 0,
	   "1.9.0 < 1.10.0 (numeric, not lexical)");
	ok(waktanimra_cmp_str("1.0.0-rc1", "1.0.0") < 0,
	   "a prerelease precedes its release");
	ok(waktanimra_cmp_str("1.0.0-rc.1", "1.0.0-rc.2") < 0,
	   "numeric tag fields compare numerically");
	ok(waktanimra_cmp_str("1.0.0-rc.9", "1.0.0-rc.10") < 0, "rc.9 < rc.10");
	ok(waktanimra_cmp_str("1.0.0-alpha", "1.0.0-beta") < 0, "alpha < beta");
	ok(waktanimra_cmp_str("1.0.0-rc", "1.0.0-rc.1") < 0,
	   "a prefix tag sorts first");
	ok(waktanimra_cmp_str("1.0.0", "1.0.0") == 0, "equal versions");

	struct constraint c;
	struct waktanimra v;

#define MATCH(cs, vs)                                            \
	(constraint_parse(cs, &c) && waktanimra_parse(vs, &v) && \
	 constraint_match(&c, &v))

	ok(MATCH("*", "1.2.3"), "* matches any release");
	ok(!MATCH("*", "1.2.3-rc1"), "* does not match a prerelease");
	ok(MATCH("=1.2.3", "1.2.3"), "=1.2.3 matches 1.2.3");
	ok(!MATCH("=1.2.3", "1.2.4"), "=1.2.3 does not match 1.2.4");
	ok(MATCH(">=1.2.3", "1.9.0"), ">=1.2.3 matches 1.9.0");
	ok(!MATCH(">=1.2.3", "1.2.2"), ">=1.2.3 does not match 1.2.2");

	ok(MATCH("^1.2.3", "1.2.3"), "^1.2.3 matches 1.2.3");
	ok(MATCH("^1.2.3", "1.9.9"), "^1.2.3 matches 1.9.9");
	ok(!MATCH("^1.2.3", "2.0.0"), "^1.2.3 stops at 2.0.0");
	ok(!MATCH("^1.2.3", "1.2.2"), "^1.2.3 excludes earlier");
	ok(MATCH("^0.2.3", "0.2.9"), "^0.2.3 matches 0.2.9");
	ok(!MATCH("^0.2.3", "0.3.0"), "^0.2.3 stops at 0.3.0 (Cargo rule)");
	ok(MATCH("^0.0.3", "0.0.3"), "^0.0.3 matches 0.0.3");
	ok(!MATCH("^0.0.3", "0.0.4"), "^0.0.3 stops at 0.0.4");

	/* prereleases are opt-in, and only for the exact release led up to */
	ok(!MATCH(">=1.0.0", "1.1.0-rc1"), ">=1.0.0 does not admit 1.1.0-rc1");
	ok(!MATCH(">=1.0.0-rc1", "1.1.0-rc1"),
	   ">=1.0.0-rc1 does not admit a prerelease of a different release");
	ok(MATCH(">=1.1.0-rc1", "1.1.0-rc1"), ">=1.1.0-rc1 admits 1.1.0-rc1");
	ok(MATCH(">=1.1.0-rc1", "1.1.0-rc2"),
	   ">=1.1.0-rc1 admits the later rc2");
	ok(MATCH("^1.1.0-rc1", "1.1.0-rc1"), "^ admits its own prerelease");

	ok(!constraint_valid(">1.0.0"), "bare > is not a constraint form");
	ok(!constraint_valid("~1.0.0"), "~ is not a constraint form");
	ok(!constraint_valid("1.0.0"), "a bare version is not a constraint");
	ok(!constraint_valid(""), "empty is not a constraint");
#undef MATCH
}

/* ---- records ---------------------------------------------------------- */

static void test_record(void)
{
	static const char body[] = "izim: http\n"
				   "waktanimra: 1.4.2\n"
				   "cizujo: tls >=0.3.0\n"
				   "cizujo: bytes ^1.0.0\n"
				   "\n"
				   "izim: tls\n"
				   "waktanimra: 0.3.1\n";
	struct record_set rs;

	ok(record_set_parse(body, sizeof body - 1, &rs), "record body parses");
	ok(rs.n == 2, "two records, got %d", rs.n);
	if (rs.n == 2) {
		ok(strcmp(record_get(&rs.records[0], "izim"), "http") == 0,
		   "first record's izim");
		ok(record_get(&rs.records[0], "nosuch") == NULL,
		   "absent key is NULL");

		int it = 0;
		const char *a = record_next(&rs.records[0], "cizujo", &it);
		const char *b = record_next(&rs.records[0], "cizujo", &it);
		const char *c = record_next(&rs.records[0], "cizujo", &it);

		ok(a && strcmp(a, "tls >=0.3.0") == 0, "first cizujo");
		ok(b && strcmp(b, "bytes ^1.0.0") == 0, "second cizujo");
		ok(c == NULL, "cizujo iteration ends");
		ok(strcmp(record_get(&rs.records[1], "izim"), "tls") == 0,
		   "second record's izim");
	}
	record_set_free(&rs);

	/* a value may hold colons and spaces; only the first ": " splits */
	static const char url[] = "punjur: https://example.org/a b\n";

	ok(record_set_parse(url, sizeof url - 1, &rs), "url value parses");
	ok(rs.n == 1 && strcmp(record_get(&rs.records[0], "punjur"),
			       "https://example.org/a b") == 0,
	   "value keeps colons and spaces");
	record_set_free(&rs);

	ok(record_set_parse("", 0, &rs) && rs.n == 0,
	   "an empty body is zero records, not an error");
	record_set_free(&rs);

	static const struct {
		const char *body;
		const char *why;
	} bad[] = {
		{ "izim http\n", "no colon" },
		{ "izim:http\n", "no space after colon" },
		{ "izim: \n", "empty value" },
		{ ": x\n", "empty key" },
		{ "Izim: x\n", "uppercase key" },
		{ "izim: http", "no trailing LF" },
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		ok(!record_set_parse(bad[i].body, strlen(bad[i].body), &rs),
		   "record body rejected: %s", bad[i].why);
		record_set_free(&rs);
	}

	struct record r;

	memset(&r, 0, sizeof r);
	record_put(&r, "izim", "http");
	record_put(&r, "waktanimra", "1.0.0");

	size_t len;
	char *out = record_format(&r, &len);

	ok(out && strcmp(out, "izim: http\nwaktanimra: 1.0.0\n") == 0,
	   "record_format round-trips");
	ok(len == 29, "formatted length is %zu", len);
	ok(!record_complete(&r), "an incomplete record is detected");
	free(out);
	record_free(&r);
}

/* ---- kabuk ------------------------------------------------------------ */

static void test_kabuk_paths(void)
{
	const char *why;

	ok(kabuk_path_ok("main.oboe", 9, &why), "plain filename");
	ok(kabuk_path_ok("src/a/b.oboe", 12, &why), "nested path");

	static const struct {
		const char *p;
		const char *why;
	} bad[] = {
		{ "", "empty" },
		{ "/etc/passwd", "absolute" },
		{ "../evil", "leading .." },
		{ "a/../../evil", "interior .." },
		{ "./a", "leading ./" },
		{ "a/./b", "interior ." },
		{ "a//b", "empty component" },
		{ "a/", "trailing slash" },
		{ "a\\b", "backslash" },
		{ "a\nb", "newline" },
		{ "a\tb", "tab" },
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
		ok(!kabuk_path_ok(bad[i].p, strlen(bad[i].p), &why),
		   "path rejected: %s", bad[i].why);

	char toolong[600];

	memset(toolong, 'a', sizeof toolong);
	ok(!kabuk_path_ok(toolong, 513, &why),
	   "path rejected: over 512 octets");
	ok(kabuk_path_ok(toolong, 512, &why), "path of exactly 512 octets");
}

/* Builds the KABUK.md §8 example in memory so the archive under test is the one
   the spec walks through octet by octet. */
static const unsigned char k_simple[] = "kabuk1\n"
					"main.oboe\n12\nprint(\"hi\")\n"
					"project.jsonc\n3\n{}\n"
					"sampura\n";
#define K_SIMPLE_LEN (sizeof k_simple - 1)

static bool count_entry(const char *path, const unsigned char *data, size_t len,
			void *ctx, char **err)
{
	int *n = ctx;

	(*n)++;
	return true;
}

static void test_kabuk_read(void)
{
	char *err = NULL;
	int n = 0;

	ok(K_SIMPLE_LEN == 59, "the spec example is 59 octets, got %zu",
	   K_SIMPLE_LEN);
	ok(strcmp(hexdigest(k_simple, K_SIMPLE_LEN),
		  "26f21312d1f381d3ff37b4a14ab4dc4756bab9e3a76c0923027502c6c0bfbc24") ==
		   0,
	   "the spec example hashes to the documented sema");

	ok(kabuk_read(k_simple, K_SIMPLE_LEN, NULL, count_entry, &n, &err),
	   "the spec example validates: %s", err ? err : "");
	ok(n == 2, "the spec example has two entries, got %d", n);
	free(err);

	/* Each of these is a way an archive can be wrong. The length-vs-newline
	   cases matter most: a reader that scans for LF instead of counting
	   octets passes the first two and corrupts every text file. */
	static const struct {
		const char *data;
		size_t len;
		const char *why;
	} bad[] = {
		{ "kabuk2\nsampura\n", 15, "wrong magic" },
		{ "kabuk1\n", 7, "no terminator" },
		{ "kabuk1\nsampura\nx", 16, "octets after sampura" },
		{ "kabuk1\na\n5\nabc\nsampura\n", 23,
		  "length runs past the end" },
		{ "kabuk1\na\n01\nx\nsampura\n", 22, "leading zero in length" },
		{ "kabuk1\na\n-1\nx\nsampura\n", 22, "negative length" },
		{ "kabuk1\na\nx\nsampura\n", 19, "non-numeric length" },
		{ "kabuk1\nb\n0\na\n0\nsampura\n", 23, "entries out of order" },
		{ "kabuk1\na\n0\na\n0\nsampura\n", 23, "duplicate entry" },
		{ "kabuk1\n../evil\n0\nsampura\n", 25, "traversal path" },
		{ "kabuk1\n/abs\n0\nsampura\n", 22, "absolute path" },
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		err = NULL;
		ok(!kabuk_read((const unsigned char *)bad[i].data, bad[i].len,
			       NULL, NULL, NULL, &err),
		   "archive rejected: %s", bad[i].why);
		free(err);
	}

	/* limits are enforced, not merely documented */
	struct kabuk_limits tight = { .max_entries = 1,
				      .max_file = 1,
				      .max_total = 1 };

	err = NULL;
	ok(!kabuk_read(k_simple, K_SIMPLE_LEN, &tight, NULL, NULL, &err),
	   "limits are enforced");
	free(err);
}

int main(void)
{
	test_sha256();
	test_izim();
	test_record();
	test_kabuk_paths();
	test_kabuk_read();

	printf("%s common: %d checks, %d failed\n", failures ? "FAIL" : "ok",
	       checks, failures);
	return failures ? 1 : 0;
}
