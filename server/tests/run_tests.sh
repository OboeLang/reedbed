#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# © 2026 Sushii64
# © 2026 robinpie
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.

# Run from server/, like oboe's suite runs from compiler/.
cd "$(dirname "$0")/.." || exit 1

pass=0
fail=0
skip=0

note_pass() { echo "PASS $1"; pass=$((pass + 1)); }
note_fail() { echo "FAIL $1"; fail=$((fail + 1)); }
note_skip() { echo "SKIP $1 ($2)"; skip=$((skip + 1)); }

# BSD wc right-aligns its count in a field, so `wc -c < f` is "      59" on
# macOS where GNU gives "59". Interpolated into a request line that padding
# becomes a run of spaces, and katare allows exactly one SP between tokens --
# so it turns every kyx line into a framing error, on that host only.
filesize() { wc -c < "$1" | tr -d '[:space:]'; }

# ---- offline unit tests ------------------------------------------------

if ./bin/t_common; then
	note_pass common
else
	note_fail common
fi

# ---- spec fixtures -----------------------------------------------------
#
# KABUK.md §8 and KATARE.md §10 walk through one archive octet by octet. These
# check that the committed fixtures still are what the specs claim, so an edit
# to either the format or the prose that drifts from the other gets caught.

F=tests/fixtures

want="$(cat "$F/simple.sema")"
got="$(./bin/sema "$F/simple.kabuk" | cut -d' ' -f1)"
if [ "$want" = "$got" ]; then
	note_pass fixture_sema
else
	note_fail "fixture_sema (want $want, got $got)"
fi

if [ "$(wc -c < "$F/simple.kabuk")" -eq 59 ]; then
	note_pass fixture_kabuk_size
else
	note_fail "fixture_kabuk_size (KABUK.md §8 says 59)"
fi

if [ "$(wc -c < "$F/simple.record")" -eq 165 ]; then
	note_pass fixture_record_size
else
	note_fail "fixture_record_size (KATARE.md §10 says 165)"
fi

# The archive must be reproducible from the tree it was packed from. Built here
# with shell rather than the packer, so this stays an independent statement of
# the format rather than a tautology against our own writer.
tmp="$(mktemp -d)"
{
	printf 'kabuk1\n'
	for f in main.oboe project.jsonc; do
		printf '%s\n%d\n' "$f" "$(wc -c < "$F/simple_tree/$f")"
		cat "$F/simple_tree/$f"
	done
	printf 'sampura\n'
} > "$tmp/rebuilt.kabuk"
if cmp -s "$tmp/rebuilt.kabuk" "$F/simple.kabuk"; then
	note_pass fixture_reproducible
else
	note_fail fixture_reproducible
fi
rm -rf "$tmp"

# ---- store ---------------------------------------------------------------
#
# Driven through reedbed-admin, which calls store.c directly. No socket is
# involved, so a failure here is unambiguously about the store.

check() {
	if [ "$2" = "$3" ]; then
		note_pass "$1"
	else
		note_fail "$1 (want '$3', got '$2')"
	fi
}

st="$(mktemp -d)"
pk="$(mktemp -d)"
A="./bin/reedbed-admin --root $st"

mkpkg() { # <dir> <name> <version> [extra-json]
	mkdir -p "$1"
	cat > "$1/project.jsonc" <<EOF
{
    "project": {
        "name": "$2",
        "version": "$3",
        "entry": "main.oboe",
        "description": "a test package",
        "author": "robin",
        "license": "GPL-2.0-only"
    }
}
EOF
	printf 'func f() { return 1 }\n' > "$1/main.oboe"
}

pack() { # <dir> <out>
	{
		printf 'kabuk1\n'
		for f in main.oboe project.jsonc; do
			printf '%s\n%d\n' "$f" "$(wc -c < "$1/$f")"
			cat "$1/$f"
		done
		printf 'sampura\n'
	} > "$2"
}

mkpkg "$pk/bytes" bytes 1.0.0 && pack "$pk/bytes" "$pk/bytes-1.0.0.kabuk"
mkpkg "$pk/bytes2" bytes 1.1.0 && pack "$pk/bytes2" "$pk/bytes-1.1.0.kabuk"
mkpkg "$pk/wrong" notbytes 1.0.0 && pack "$pk/wrong" "$pk/wrong.kabuk"

$A create-account robin > /dev/null 2>&1
$A create-account mallory > /dev/null 2>&1

$A import bytes 1.0.0 robin "$pk/bytes-1.0.0.kabuk" > /dev/null 2>&1
check store_import "$?" 0

# Versions are immutable: the slot is spent whether or not the publish stuck.
$A import bytes 1.0.0 robin "$pk/bytes-1.0.0.kabuk" > /dev/null 2>&1
check store_republish_conflicts "$?" 1

# A package's project.jsonc name is what the compiler matches folder modules on,
# so publishing under a different izim would install and then fail to import.
$A import bytes 2.0.0 robin "$pk/wrong.kabuk" > /dev/null 2>&1
check store_name_mismatch_rejected "$?" 1

# First publisher owns it; nobody else may add to it.
$A import bytes 1.1.0 mallory "$pk/bytes-1.1.0.kabuk" > /dev/null 2>&1
check store_non_owner_denied "$?" 1

$A import bytes 1.1.0 robin "$pk/bytes-1.1.0.kabuk" > /dev/null 2>&1
check store_owner_may_publish "$?" 0

$A verify > /dev/null 2>&1
check store_verify_clean "$?" 0

# A release directory without its ok marker was a publish that died partway; no
# reader may see it, and the sweep must clear it.
mkdir -p "$st/packages/bytes/releases/9.9.9"
echo junk > "$st/packages/bytes/releases/9.9.9/kabuk"
got="$($A list 2>/dev/null | grep -c 9.9.9)"
check store_incomplete_invisible "$got" 0
$A sweep > /dev/null 2>&1
if [ -d "$st/packages/bytes/releases/9.9.9" ]; then
	note_fail store_sweep_removes_incomplete
else
	note_pass store_sweep_removes_incomplete
fi

# Yanking marks and hides from search, but never deletes -- an existing pin has
# to keep working (KATARE.md §7.2).
$A yank bytes 1.0.0 robin "leaks a token" > /dev/null 2>&1
check store_yank "$?" 0

if grep -q '^kaldy: leaks a token$' "$st/packages/bytes/releases/1.0.0/meta"; then
	note_pass store_yank_marks_record
else
	note_fail store_yank_marks_record
fi
if [ -f "$st/packages/bytes/releases/1.0.0/kabuk" ]; then
	note_pass store_yank_keeps_archive
else
	note_fail store_yank_keeps_archive
fi
got="$($A list 2>/dev/null | grep -c '	1\.0\.0	')"
check store_yank_drops_from_index "$got" 0

$A yank bytes 1.0.0 mallory > /dev/null 2>&1
check store_yank_needs_ownership "$?" 1
$A yank bytes 4.5.6 robin > /dev/null 2>&1
check store_yank_missing_release "$?" 1

# The plaintext token is never written down; only its digest names the file.
tok="$($A mint-token robin 2>/dev/null)"
if [ -n "$tok" ] && ! grep -rq "$tok" "$st" 2>/dev/null; then
	note_pass store_token_not_stored
else
	note_fail store_token_not_stored
fi

rm -rf "$st" "$pk"

# ---- protocol conformance ----------------------------------------------
#
# One server on an ephemeral port, one katarecat script per case, diffed
# against a golden transcript. Port 0 and --print-port mean the harness never
# guesses a port, so two runs of the suite cannot collide.
#
# Only framing and dispatch are tested here. Everything that is a pure function
# of its input -- digests, versions, archives -- is tested above, without a
# socket, because a network test that fails tells you much less about why.

start_server() {
	srv_dir="$(mktemp -d)"
	./bin/reedbed --port 0 --root "$srv_dir" --print-port --foreground \
		"$@" > "$srv_dir/out" 2>&1 &
	srv_pid=$!
	# the port line is flushed before the accept loop starts, so once it
	# appears the socket is already listening -- no sleep-and-hope
	for _ in $(seq 1 100); do
		srv_port="$(sed -n 's/^listening //p' "$srv_dir/out" 2>/dev/null)"
		[ -n "$srv_port" ] && return 0
		kill -0 "$srv_pid" 2>/dev/null || break
		sleep 0.05
	done
	return 1
}

stop_server() {
	[ -n "$srv_pid" ] && kill "$srv_pid" 2>/dev/null
	# never `wait` on it: a server that ignores the signal would hang CI
	[ -n "$srv_dir" ] && rm -rf "$srv_dir"
	srv_pid=; srv_dir=; srv_port=
}

# a stray server outliving the suite would hold the port and hang the next run
trap 'stop_server' EXIT

if start_server; then
	for script in tests/proto/*.script; do
		name="$(basename "$script" .script)"
		expected="tests/proto/$name.expected"

		if [ ! -f "$expected" ]; then
			note_skip "proto_$name" "no .expected"
			continue
		fi
		got="$(./bin/katarecat 127.0.0.1 "$srv_port" "$script" 2>&1)"
		if [ "$got" = "$(cat "$expected")" ]; then
			note_pass "proto_$name"
		else
			note_fail "proto_$name"
			diff -u "$expected" - <<< "$got" | sed 's/^/    /'
		fi
	done

	# The idle timeout is a config knob precisely so it can be tested: the
	# 300-second default is not a fixture anyone can wait for.
	stop_server
	if start_server --idle-timeout 1; then
		got="$(printf '< 1\n> dijabon katare/1 oboe/0.1\n< 1\n<all\n' |
			./bin/katarecat 127.0.0.1 "$srv_port" 2>&1)"
		if printf '%s' "$got" | grep -q '^koja'; then
			note_pass proto_idle_timeout
		else
			note_fail "proto_idle_timeout (no koja before close)"
		fi
	else
		note_fail "proto_idle_timeout (server did not start)"
	fi

	# A mirror advertises no write capability and refuses anything not
	# prefixed ko, without having to recognise the verb.
	stop_server
	if start_server --mirror; then
		got="$(printf '< 1\n> dijabon katare/1 oboe/0.1\n< 1\n> kango x 1.0.0 y kyx 0\n<all\n' |
			./bin/katarecat 127.0.0.1 "$srv_port" 2>&1)"
		if printf '%s' "$got" | grep -q '^ezhazebyr' &&
		   ! printf '%s' "$got" | grep -q 'kango'; then
			note_pass proto_mirror_rejects_write
		else
			note_fail proto_mirror_rejects_write
		fi
	else
		note_fail "proto_mirror_rejects_write (server did not start)"
	fi
	stop_server
else
	note_fail "protocol conformance (server did not start)"
	stop_server
fi

# ---- reads against a populated store ------------------------------------
#
# The conformance cases above run against an empty store, so they only ever see
# keresebyr. These check what the read verbs actually return, and in particular
# the yank rule: hidden from search, still listed, still fetchable.

pk="$(mktemp -d)"
mkpkg "$pk/a" bytes 1.0.0 && pack "$pk/a" "$pk/a.kabuk"
mkpkg "$pk/b" bytes 1.1.0 && pack "$pk/b" "$pk/b.kabuk"

if start_server; then
	A="./bin/reedbed-admin --root $srv_dir"
	$A create-account robin > /dev/null 2>&1
	$A import bytes 1.0.0 robin "$pk/a.kabuk" > /dev/null 2>&1
	$A import bytes 1.1.0 robin "$pk/b.kabuk" > /dev/null 2>&1
	$A yank bytes 1.0.0 robin "superseded" > /dev/null 2>&1

	# katarecat renders CR as a literal \r so a transcript diffs as text.
	# These checks match single lines rather than whole transcripts, so the
	# marker is stripped here and patterns can anchor with $.
	ask() { printf '< 1\n> dijabon katare/1 oboe/0.1\n< 1\n%s\n<all\n' "$1" |
		./bin/katarecat 127.0.0.1 "$srv_port" 2>&1 | sed 's/\\r$//'; }

	got="$(ask '> ko besal bytes')"
	if printf '%s' "$got" | grep -q '^waktanimra: 1.1.0$' &&
	   printf '%s' "$got" | grep -q '^waktanimra: 1.0.0$'; then
		note_pass read_besal_lists_all_releases
	else
		note_fail read_besal_lists_all_releases
	fi

	# newest first, so a client reading the first record gets the current one
	if [ "$(printf '%s' "$got" | grep -m1 '^waktanimra: ')" = "waktanimra: 1.1.0" ]; then
		note_pass read_besal_newest_first
	else
		note_fail read_besal_newest_first
	fi

	got="$(ask '> ko besal bytes 1.0.0')"
	if printf '%s' "$got" | grep -q '^kaldy: superseded$'; then
		note_pass read_yanked_record_is_marked
	else
		note_fail read_yanked_record_is_marked
	fi

	# a pin on a yanked version must keep resolving, or a yank breaks builds
	got="$(ask '> ko ghazema bytes 1.0.0')"
	if printf '%s' "$got" | grep -q '^si sha256:.* kyx '; then
		note_pass read_yanked_still_fetchable
	else
		note_fail read_yanked_still_fetchable
	fi

	# ... but nothing new should adopt it
	got="$(ask '> ko jexa bytes')"
	if printf '%s' "$got" | grep -q '^waktanimra: 1.1.0$' &&
	   ! printf '%s' "$got" | grep -q '^waktanimra: 1.0.0$'; then
		note_pass read_jexa_skips_yanked
	else
		note_fail read_jexa_skips_yanked
	fi

	got="$(ask '> ko jexa bytes nonsense')"
	if printf '%s' "$got" | grep -q '^si kyx 0$'; then
		note_pass read_jexa_and_is_conjunctive
	else
		note_fail read_jexa_and_is_conjunctive
	fi

	got="$(ask '> ko jexa BYTES')"
	if printf '%s' "$got" | grep -q '^izim: bytes$'; then
		note_pass read_jexa_case_insensitive
	else
		note_fail read_jexa_case_insensitive
	fi

	# the digest on the ghazema line must be the digest of what follows
	want="$(./bin/sema "$pk/b.kabuk" | cut -d' ' -f1)"
	got="$(ask '> ko ghazema bytes 1.1.0' | grep -m1 '^si sha256:' |
		cut -d' ' -f2)"
	if [ "$want" = "$got" ]; then
		note_pass read_ghazema_echoes_true_digest
	else
		note_fail "read_ghazema_echoes_true_digest (want $want, got $got)"
	fi

	got="$(ask '> ko besal nosuch')"
	if printf '%s' "$got" | grep -q '^keresebyr'; then
		note_pass read_missing_is_keresebyr
	else
		note_fail read_missing_is_keresebyr
	fi

	# writes still need a kalit, even though the verbs exist
	got="$(ask '> kango bytes 2.0.0 sha256:0 kyx 0')"
	if printf '%s' "$got" | grep -q '^ezhazebyr\|^wuwoji'; then
		note_pass read_write_needs_auth
	else
		note_fail read_write_needs_auth
	fi

	stop_server
else
	note_fail "populated reads (server did not start)"
	stop_server
fi
rm -rf "$pk"

# ---- writes over the wire ------------------------------------------------

pk="$(mktemp -d)"
mkpkg "$pk/a" bytes 1.0.0 && pack "$pk/a" "$pk/a.kabuk"
mkpkg "$pk/b" bytes 1.1.0 && pack "$pk/b" "$pk/b.kabuk"
mkpkg "$pk/c" other 1.0.0 && pack "$pk/c" "$pk/c.kabuk"

if start_server; then
	A="./bin/reedbed-admin --root $srv_dir"
	$A create-account robin > /dev/null 2>&1
	$A create-account mallory > /dev/null 2>&1
	tok="$($A mint-token robin 2>/dev/null)"
	mtok="$($A mint-token mallory 2>/dev/null)"

	n="$(filesize "$pk/a.kabuk")"
	sema="$(./bin/sema "$pk/a.kabuk" | cut -d' ' -f1)"
	n2="$(filesize "$pk/b.kabuk")"
	sema2="$(./bin/sema "$pk/b.kabuk" | cut -d' ' -f1)"
	nc="$(filesize "$pk/c.kabuk")"
	semac="$(./bin/sema "$pk/c.kabuk" | cut -d' ' -f1)"

	# Ends with koja rather than reading to close: the server holds an idle
	# session open for its timeout, so waiting for a close would cost
	# katarecat's full socket timeout on every case. A session the server
	# already closed makes the koja write fail, which returns immediately.
	speak() { { printf '< 1\n> dijabon katare/1 oboe/0.1\n< 1\n'
		    printf '%b' "$1"; printf '> koja\n< 1\n'; } |
		  ./bin/katarecat 127.0.0.1 "$srv_port" 2>&1 | sed 's/\\r$//'; }

	auth="> kalit $tok\n< 1\n"
	pub="> kango bytes 1.0.0 $sema kyx $n\n>body $pk/a.kabuk\n< 1\n"

	got="$(speak "$auth$pub")"
	if printf '%s' "$got" | grep -q '^si$'; then
		note_pass write_kango_publishes
	else
		note_fail write_kango_publishes
	fi

	# versions are immutable, so the same one again is a conflict
	got="$(speak "$auth$pub")"
	if printf '%s' "$got" | grep -q '^sentyre'; then
		note_pass write_kango_republish_is_sentyre
	else
		note_fail write_kango_republish_is_sentyre
	fi

	# the publisher's digest is never trusted
	bad="> kango bytes 5.0.0 sha256:$(printf '0%.0s' $(seq 64)) kyx $n\n>body $pk/a.kabuk\n"
	got="$(speak "$auth$bad")"
	if printf '%s' "$got" | grep -q '^wuwoji sema-mismatch'; then
		note_pass write_kango_rejects_wrong_digest
	else
		note_fail write_kango_rejects_wrong_digest
	fi

	# an archive whose project.jsonc names a different package would install
	# and then fail to import, so it is refused at publish time
	mism="> kango bytes 6.0.0 $semac kyx $nc\n>body $pk/c.kabuk\n"
	got="$(speak "$auth$mism")"
	if printf '%s' "$got" | grep -q '^wuwoji name-mismatch'; then
		note_pass write_kango_rejects_name_mismatch
	else
		note_fail write_kango_rejects_name_mismatch
	fi

	# first publisher owns it
	got="$(speak "> kalit $mtok\n< 1\n> kango bytes 1.1.0 $sema2 kyx $n2\n>body $pk/b.kabuk\n< 1\n")"
	if printf '%s' "$got" | grep -q '^ezhazebyr'; then
		note_pass write_kango_non_owner_denied
	else
		note_fail write_kango_non_owner_denied
	fi

	# An unauthenticated kango is refused before its body is read. The body
	# must still be drained, or every later request on the session parses
	# archive octets as request lines.
	got="$(speak "> kango bytes 7.0.0 $sema kyx $n\n>body $pk/a.kabuk\n< 1\n> ko besal bytes 1.0.0\n< 1\n")"
	if printf '%s' "$got" | grep -q '^ezhazebyr' &&
	   printf '%s' "$got" | grep -q '^si kyx '; then
		note_pass write_unauth_body_is_drained
	else
		note_fail write_unauth_body_is_drained
	fi

	got="$(speak "$auth> kaldy bytes 1.0.0 leaks a token\n< 1\n")"
	if printf '%s' "$got" | grep -q '^si$'; then
		note_pass write_kaldy
	else
		note_fail write_kaldy
	fi

	# the reason survives being split into tokens and rejoined
	got="$(speak "> ko besal bytes 1.0.0\n< 12\n")"
	if printf '%s' "$got" | grep -q '^kaldy: leaks a token$'; then
		note_pass write_kaldy_reason_round_trips
	else
		note_fail write_kaldy_reason_round_trips
	fi

	got="$(speak "$auth> kaldy bytes 9.9.9\n< 1\n")"
	if printf '%s' "$got" | grep -q '^keresebyr'; then
		note_pass write_kaldy_missing_is_keresebyr
	else
		note_fail write_kaldy_missing_is_keresebyr
	fi

	got="$(speak "> kaldy bytes 1.0.0\n< 1\n")"
	if printf '%s' "$got" | grep -q '^ezhazebyr'; then
		note_pass write_kaldy_needs_auth
	else
		note_fail write_kaldy_needs_auth
	fi

	stop_server
else
	note_fail "writes (server did not start)"
	stop_server
fi

# A mirror must refuse a kango and stay in sync, which means draining a body
# for a verb it will not even look up.
if start_server --mirror; then
	got="$(printf '< 1\n> dijabon katare/1 oboe/0.1\n< 1\n> kango bytes 1.0.0 %s kyx %s\n>body %s\n< 1\n> ko besal bytes\n< 1\n> koja\n< 1\n' \
		"$sema" "$n" "$pk/a.kabuk" |
		./bin/katarecat 127.0.0.1 "$srv_port" 2>&1 | sed 's/\\r$//')"
	if printf '%s' "$got" | grep -q '^ezhazebyr$' &&
	   printf '%s' "$got" | grep -q '^keresebyr$'; then
		note_pass write_mirror_drains_refused_body
	else
		note_fail write_mirror_drains_refused_body
	fi
	stop_server
else
	note_fail "mirror drain (server did not start)"
	stop_server
fi
rm -rf "$pk"

# ---- resolution ----------------------------------------------------------
#
# A diamond: http wants tls ^0.3.0 and bytes ^1.0.0, tls wants bytes >=1.1.0.
# The two constraints on bytes have to merge, and the caret has to cap the
# result below 2.0.0.

pk="$(mktemp -d)"
mkdep() { # <dir> <name> <version> <deps-json>
	mkdir -p "$pk/$1"
	cat > "$pk/$1/project.jsonc" <<EOF
{"project":{"name":"$2","version":"$3","entry":"main.oboe","description":"pkg $2"},"dependencies":{$4}}
EOF
	printf 'x\n' > "$pk/$1/main.oboe"
	pack "$pk/$1" "$pk/$1.kabuk"
}

mkdep h http 1.0.0 '"tls":"^0.3.0","bytes":"^1.0.0"'
mkdep t1 tls 0.3.0 '"bytes":">=1.1.0"'
mkdep t2 tls 0.3.5 '"bytes":">=1.1.0"'
mkdep t3 tls 0.4.0 '"bytes":">=1.1.0"'
mkdep b1 bytes 1.0.0 ''
mkdep b2 bytes 1.1.0 ''
mkdep b3 bytes 1.2.0 ''
mkdep b4 bytes 2.0.0 ''

if start_server; then
	A="./bin/reedbed-admin --root $srv_dir"
	$A create-account robin > /dev/null 2>&1
	for spec in "h http 1.0.0" "t1 tls 0.3.0" "t2 tls 0.3.5" \
		    "t3 tls 0.4.0" "b1 bytes 1.0.0" "b2 bytes 1.1.0" \
		    "b3 bytes 1.2.0" "b4 bytes 2.0.0"; do
		set -- $spec
		$A import "$2" "$3" robin "$pk/$1.kabuk" > /dev/null 2>&1
	done

	ask2() { printf '< 1\n> dijabon katare/1 oboe/0.1\n< 1\n%s\n> koja\n< 1\n' "$1" |
		 ./bin/katarecat 127.0.0.1 "$srv_port" 2>&1 | sed 's/\\r$//'; }

	got="$(ask2 '> ko cizujo http ^1.0.0
< 4')"
	if printf '%s' "$got" | grep -q '^si .*\bcizujo\b'; then
		note_pass resolve_advertised
	else
		note_fail resolve_advertised
	fi

	# ^0.3.0 is Cargo-style below 1.0, so 0.4.0 is out of range
	if printf '%s' "$got" | grep -q '^tls 0\.3\.5 sha256:'; then
		note_pass resolve_caret_below_one
	else
		note_fail resolve_caret_below_one
	fi

	# ^1.0.0 and >=1.1.0 merge; 2.0.0 is excluded by the caret
	if printf '%s' "$got" | grep -q '^bytes 1\.2\.0 sha256:'; then
		note_pass resolve_diamond_merges_constraints
	else
		note_fail resolve_diamond_merges_constraints
	fi

	# the request package itself is part of the resolved set
	if printf '%s' "$got" | grep -q '^http 1\.0\.0 sha256:'; then
		note_pass resolve_includes_root
	else
		note_fail resolve_includes_root
	fi

	got="$(ask2 '> ko cizujo bytes >=9.0.0
< 1')"
	if printf '%s' "$got" | grep -q '^keresebyr$'; then
		note_pass resolve_unsatisfiable_is_keresebyr
	else
		note_fail resolve_unsatisfiable_is_keresebyr
	fi

	got="$(ask2 '> ko cizujo nosuch *
< 1')"
	if printf '%s' "$got" | grep -q '^keresebyr$'; then
		note_pass resolve_unknown_is_keresebyr
	else
		note_fail resolve_unknown_is_keresebyr
	fi

	# a yanked release is never chosen, so the pick falls back
	$A yank bytes 1.2.0 robin superseded > /dev/null 2>&1
	got="$(ask2 '> ko cizujo http ^1.0.0
< 4')"
	if printf '%s' "$got" | grep -q '^bytes 1\.1\.0 sha256:'; then
		note_pass resolve_skips_yanked
	else
		note_fail resolve_skips_yanked
	fi
	stop_server
else
	note_fail "resolution (server did not start)"
	stop_server
fi

# --no-cizujo is what a client's fallback path exists for, so it has to be a
# configuration the server can actually be in.
if start_server --no-cizujo; then
	got="$(printf '< 1\n> dijabon katare/1 oboe/0.1\n< 1\n> ko cizujo http *\n<all\n' |
		./bin/katarecat 127.0.0.1 "$srv_port" 2>&1 | sed 's/\\r$//')"
	if ! printf '%s' "$got" | grep -q 'cizujo kalit' &&
	   printf '%s' "$got" | grep -q '^wuwoji unknown-verb$'; then
		note_pass resolve_optional_when_unadvertised
	else
		note_fail resolve_optional_when_unadvertised
	fi
	stop_server
else
	note_fail "no-cizujo (server did not start)"
	stop_server
fi
rm -rf "$pk"

# ---- rate limiting -------------------------------------------------------
#
# Testable only because the rates are configuration rather than constants.

if start_server --rate 3; then
	got="$({ printf '< 1\n> dijabon katare/1 oboe/0.1\n< 1\n'
		 for _ in 1 2 3 4 5; do printf '> ko besal nosuch\n< 1\n'; done
		 printf '> koja\n< 1\n'; } |
		./bin/katarecat 127.0.0.1 "$srv_port" 2>&1 | sed 's/\\r$//')"

	if [ "$(printf '%s' "$got" | grep -c '^keresebyr$')" -eq 3 ] &&
	   printf '%s' "$got" | grep -q '^vazoj [1-9]'; then
		note_pass rate_limits_requests
	else
		note_fail rate_limits_requests
	fi

	# vazoj must leave the session usable, and koja is never throttled --
	# refusing a close just strands the connection slot
	if printf '%s' "$got" | grep -q '^koja$'; then
		note_pass rate_koja_exempt
	else
		note_fail rate_koja_exempt
	fi
	stop_server
else
	note_fail "rate limiting (server did not start)"
	stop_server
fi

if start_server --rate-auth 2; then
	got="$({ printf '< 1\n> dijabon katare/1 oboe/0.1\n< 1\n'
		 for _ in 1 2 3 4; do printf '> kalit aaaaaaaaaaaaaaaaaaaaaa\n< 1\n'; done
		 printf '> koja\n< 1\n'; } |
		./bin/katarecat 127.0.0.1 "$srv_port" 2>&1 | sed 's/\\r$//')"

	# charged whether or not the token was any good, so guessing is throttled
	if [ "$(printf '%s' "$got" | grep -c '^ezhazebyr$')" -eq 2 ] &&
	   printf '%s' "$got" | grep -q '^vazoj [1-9]'; then
		note_pass rate_limits_auth_attempts
	else
		note_fail rate_limits_auth_attempts
	fi
	stop_server
else
	note_fail "auth rate limiting (server did not start)"
	stop_server
fi

# ---- vendor manifest ---------------------------------------------------
#
# common/ is the canonical copy; the Oboe compiler carries a vendored one. This
# catches the usual failure -- someone edits a copy in place -- locally, without
# needing the other repo checked out.

if [ -f ../common/MANIFEST.sha256 ]; then
	if (cd ../common && "$OLDPWD/bin/sema" sha256.c sha256.h izim.c izim.h \
		record.c record.h kabuk.c kabuk.h projectjson.c projectjson.h |
		diff -u MANIFEST.sha256 - \
		> /dev/null); then
		note_pass vendor_manifest
	else
		note_fail "vendor_manifest (run 'make manifest' if the change was intended)"
	fi
else
	note_skip vendor_manifest "no MANIFEST.sha256"
fi

# ---- tally -------------------------------------------------------------

echo
echo "$pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
