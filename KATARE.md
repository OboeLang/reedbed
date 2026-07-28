# katare/1

The wire protocol spoken by reedbed.

This document is normative. MUST, MUST NOT, SHOULD, SHOULD NOT and MAY, those terms carry their RFC 2119 meanings.

The archive format carried by `ghazema` and `kango` is specified separately in [KABUK.md](KABUK.md).

## 1. Transport

katare runs over TCP. The standard port is 440.

The URI form is:

```
katare://host[:port]/[package]
```

`port` defaults to 440. The `/package` component is optional; a URI naming only a host identifies the registry itself.

It is plaintext. There is no transport encryption right now until the `firme` capability is specified (see §9).

A connection carries exactly one session. Sessions are request-response with one request in flight at a time. A client MUST NOT send a second request line before it has read the previous response in full, including any body. There is no pipelining and no multiplexing.

The server speaks first.

## 2. Framing

### 2.1 Lines

The protocol is line-oriented. A line is terminated by exactly `CR LF` (`0x0D 0x0A`).

- A line MUST NOT exceed 1024 octets including its CRLF, that is, at most 1022 octets of content.
- A bare `LF` not preceded by `CR` is a framing error.
- A `CR` not immediately followed by `LF` is a framing error.

A line is a sequence of tokens separated by a single SP (`0x20`). There is no leading SP, no trailing SP, no empty token, and no tab. Two adjacent SPs are a framing error.

The first token of a line is the verb (in a request) or the status (in a response). This means a parser can classify a line before it has interpreted it.

Structural tokens (verbs, statuses, names, versions, digests, numbers, capability words) MUST consist only of ASCII `0x21`–`0x7E`. The final token sequence of the `wuwoji` and `kaldy` lines is free human text and MAY contain UTF-8; see §2.4.

### 2.2 Bodies

A line whose final two tokens are `kyx <n>` is followed immediately by exactly `n` octets of opaque body. The body has no terminator, framing, or escaping; the next line begins at octet `n + 1`.

`<n>` is decimal ASCII. It MUST NOT carry a sign, MUST NOT have leading zeros (`0` itself is the only representation of zero), and MUST be at most 10 digits. `kyx 0` is valid and means an empty body.

Because a body follows its line with no separator, an implementation MUST read lines through a buffer and MUST NOT assume a `read(2)` boundary falls at CRLF.

Every katare number is decimal ASCII. There are no binary integers on the wire outside of body octets.

### 2.3 Body size cap

A server declares a maximum body size it will accept and produce. The default is 67108864 (64 MiB). The value is configurable and is advertised in the capability line (§3) as a bare word of the form `kyx<n>` (for example `kyx67108864`, written with no space so that it remains a single token).

A `kyx <n>` with `n` greater than the cap is a framing error.

### 2.4 Human text

The trailing text of `wuwoji <kata...>` and `kaldy <izim> <waktanimra> <kata...>` is intended for a person to read. It MAY contain any UTF-8 sequence whose octets are not `CR`, `LF` or `NUL`. It is still bounded by the 1024-octet line limit, and a sender that truncates such text to fit MUST NOT split a UTF-8 sequence.

Record bodies (§6) are opaque octets and MAY contain UTF-8 in any value.

### 2.5 Framing errors are fatal

On any framing error the receiver sends `wuwoji <reason>` and closes the connection. See §5.

## 3. Handshake

```
S: dijabon katare/1 <agent>
C: dijabon katare/1 <agent>
S: si <capability>... kyx<n>
```

Concretely:

```
S: dijabon katare/1 reedbed/0.1
C: dijabon katare/1 oboe/0.1
S: si jexa cizujo kalit kango kaldy kyx67108864
```

An `<agent>` token is an implementation name and version:

```
agent = izim "/" 1*( ALPHA / DIGIT / "." / "-" )
```

with a total length of at most 64 octets. The `izim` production is §8.1. Note that `/` is not part of the `izim` charset, which is what makes `oboe/0.1` a legal agent token but not a legal package name.

The client MUST send its `dijabon` before any other line. A server MUST close on a first client line that is not a well-formed `dijabon`, on a protocol name other than `katare`, and on a major version other than `1`.

A client MUST close on a greeting that is not a well-formed `dijabon katare/1 <agent>`.

The capability line lists the optional features the server supports, and always ends with the body cap. Its first token is the status `si`; every remaining token is a capability word. Capabilities are how katare is extended: a client MUST ignore capability words it does not recognise, and MUST NOT send a request whose verb is gated behind a capability the server did not advertise.

The server MUST NOT send any unsolicited line other than the greeting of §3 and the `koja` of §7.

## 4. Verbs

Every read verb is prefixed `ko`. Every write and session-control verb is not. The prefix is there because it lets a read-only mirror reject a request by its first two octets, before it has parsed or even recognised the verb.

| Request | Capability | Auth | Meaning | Success |
|---|---|---|---|---|
| `ko jexa <query>...` | `jexa` | no | search | `si kyx <n>` + records |
| `ko besal <izim>` | - | no | all releases of a package | `si kyx <n>` + records |
| `ko besal <izim> <waktanimra>` | - | no | one release | `si kyx <n>` + one record |
| `ko ghazema <izim> <waktanimra>` | - | no | fetch the archive | `si <sema> kyx <n>` + kabuk |
| `ko cizujo <izim> <constraint>` | `cizujo` | no | resolve dependencies | `si kyx <n>` + pin lines |
| `kalit <token>` | `kalit` | no | authenticate | `si <account>` |
| `kango <izim> <waktanimra> <sema> kyx <n>` + body | `kango` | yes | publish | `si` |
| `kaldy <izim> <waktanimra> [<kata>...]` | `kaldy` | yes | yank a release | `si` |
| `koja` | - | no | close the session | `koja`, then close |

`dijabon`, `besal`, `ghazema` and `koja` are mandatory: a server MUST implement them and MUST NOT advertise them as capabilities. `jexa`, `cizujo`, `kalit`, `kango` and `kaldy` are optional and advertised by exactly those words.

A verb that is not recognised, and a verb that is recognised but whose capability was not advertised, both produce `wuwoji` and close.

### 4.1 `ko jexa`

```
C: ko jexa http client
S: si kyx 412
   <records>
```

Matching is case-insensitive substring over two record fields, `izim` and `kakwam`. Multiple query tokens are combined with AND: every token must match somewhere in one of those two fields. Query tokens are structural tokens and are therefore ASCII; a server MAY fold ASCII case only.

Zero matches is `si kyx 0` (an empty body, not `keresebyr`).

A server caps the number of records it will return (default 100). When a query matches more, the server returns the cap and appends one final record consisting of a single line:

```
wakwa: truncated
```

There is no pagination in katare/1. A client that receives the truncation marker SHOULD tell the user to narrow the query.

Only the latest non-yanked release of each matching package is returned.

### 4.2 `ko besal`

With one argument, the body is every release of the package, **including yanked ones**, as records separated by blank lines, ordered newest first by §8.2.

With two arguments, the body is exactly one record.

An unknown `izim`, or a known `izim` with an unknown `waktanimra`, is `keresebyr`.

### 4.3 `ko ghazema`

```
C: ko ghazema http 1.2.0
S: si sha256:9f86d0…f4e5 kyx 20481
   <20481 octets of kabuk>
```

The status line echoes the release's `sema` so a client can verify the digest while it streams the body rather than buffering the whole archive first. The echoed value MUST equal the `sema` of the release's record.

A client MUST verify the digest and MUST discard the archive on mismatch. A client SHOULD compare the echoed digest against the one it already holds from a record, where it has one, and abort before reading any body octets if they differ.

A yanked release is still served (§7.2).

### 4.4 `ko cizujo`

```
C: ko cizujo http ^1.2.0
S: si kyx 143
   http 1.4.2 sha256:9f86d0…f4e5
   tls 0.3.1 sha256:2c26b4…7d1a
   bytes 1.0.0 sha256:fcde2b…9f81
```

The body is not records. It is one line per resolved package:

```
pin-line = izim SP waktanimra SP sema LF
```

terminated by `LF`, not `CR LF`, because the body is opaque octets and its internal structure is its own. The set is flat and complete: it includes the requested package itself and every transitive dependency, each pinned to one version. It contains no duplicate `izim`.

A server MUST NOT include a yanked release in a resolution.

`keresebyr` if the requested `izim` is unknown. `wuwoji` if the constraint is malformed. If the package exists but no version set satisfies the constraints, the response is `keresebyr` (the resolution, not the package, is what is absent).

`cizujo` exists so that a client need not implement a solver. A client MUST be prepared for a server that does not advertise it, and in that case walks `ko besal` recursively itself. A server that advertises `cizujo` MUST resolve by the rules of §8, so that both paths agree.

Note that `cizujo` names two different things: this verb, and a record key (§6) meaning "one dependency". They are related but not interchangeable.

### 4.5 `kalit`

```
C: kalit q7v2n4x8k1m6p3w9r5t0j8h2c4b7f1d3
S: si robin
```

The token is an opaque structural token of at most 128 octets. The response names the account the token belongs to.

A client MAY send `kalit` more than once; each success replaces the session's account. Read verbs may be interleaved freely with an authenticated session.

Any failure (e.g. unknown token, malformed token, revoked token) is `ezhazebyr`, with no detail. A server MUST NOT distinguish "no such token" from "token belongs to a disabled account" in its response, and MUST reject on length and charset before performing any lookup so that timing does not distinguish them either.

### 4.6 `kango`

```
C: kango http 1.2.0 sha256:9f86d0…f4e5 kyx 20481
   <20481 octets of kabuk>
S: si
```

Requires a prior successful `kalit`; otherwise `ezhazebyr`.

The server MUST compute the SHA-256 of the received body itself and compare it to the `<sema>` on the request line. A mismatch is `wuwoji sema-mismatch`. The client's digest is never trusted.

The server MUST validate the body as a kabuk archive per KABUK.md, MUST reject an archive whose entries are not strictly sorted, and MUST reject one whose root `project.jsonc` (or `project.json`) declares a `name` other than `<izim>` or a `version` other than `<waktanimra>`.

Republishing an `<izim> <waktanimra>` that already exists is `sentyre`. Versions are immutable: a server MUST NOT allow a release's archive to be replaced. This holds for yanked releases too. A yanked version number is spent.

Publishing a package that does not yet exist creates it and makes the publishing account its sole owner. Publishing to an existing package requires that the account be one of its owners; otherwise `ezhazebyr`.

An `izim` on the reserved list of §8.1 is `wuwoji reserved-name`.

### 4.7 `kaldy`

```
C: kaldy http 1.2.0 leaks the auth header into logs
S: si
```

Marks a release yanked. Requires ownership. The trailing text is the reason and is recorded in the release's record; it MAY be empty.

Yanking is not deletion. See §7.2.

Yanking an already-yanked release is `si` and replaces the reason.

### 4.8 `koja`

Ends the session. The server replies `koja` and closes. A client SHOULD send `koja` rather than closing abruptly, but a server MUST tolerate an abrupt close at any point.

## 5. Statuses

| Status | Meaning | Session |
|---|---|---|
| `si` | success | continues |
| `keresebyr` | not found | continues |
| `sentyre` | already exists / conflict | continues |
| `ezhazebyr` | unauthenticated, or not the owner | continues |
| `vazoj <sego>` | rate-limited; wait `<sego>` seconds | continues |
| `ramuzhu <sego>` | server failure; retry after `<sego>` seconds | continues |
| `wuwoji <kata>...` | client error | closes |
| `byr` | plain negative | reserved |
| `koja` | session closing | closes |

`kyx <n>` is not itself a status. It is a trailing pair on a status line indicating that a body follows, and only `si` currently carries one.

**`wuwoji` is always fatal.** Whether the fault was framing (a bad terminator, an oversized line, a `kyx` over the cap), semantic (an unknown verb, a malformed `izim`, a digest mismatch), or a future reason, the server sends `wuwoji <reason>` and closes. There is one rule and one code path. A client that wants to continue reconnects, we don't bother with trying to resuscitate a broken connection.

`vazoj` and `ramuzhu` are the only negative statuses that leave the session usable. `<sego>` is a decimal integer count of seconds, at least 1 and at most 86400. Zero is not a valid `sego`.

`byr` is reserved. No verb in katare/1 returns it. It exists so that a future version has a general-purpose negative available that does not already carry "not found" or "client error" connotations.

A client that receives a status it does not recognise SHOULD treat it as fatal, close the connection, and report the unrecognised token. It MUST NOT attempt to guess whether a body follows.

## 6. Record bodies

The bodies of `jexa` and `besal` are *records*. A record is a sequence of `key: value` lines; records are separated by a single blank line. Lines within a body are terminated by `LF`; the body is opaque octets, and its internal line structure is independent of the CRLF framing of §2.

```
key    = 1*( "a" / … / "z" )
line   = key ": " value LF
```

Exactly one SP follows the colon. A value extends to the end of the line and MAY contain UTF-8, colons, and SP. A value MUST NOT be empty; a key with nothing to say is omitted instead. Unrecognised keys MUST be ignored by a reader, which is how record bodies are extended.

| Key | Cardinality | Value |
|---|---|---|
| `izim` | required | package name (§8.1) |
| `waktanimra` | required | version (§8.2) |
| `warna` | required | `vivlijotiki` (library, `oboe get`) or `pawi` (tool, `oboe install`) |
| `ozhon` | required | archive size in octets, decimal |
| `sema` | required | `sha256:` + 64 lowercase hex digits, over the kabuk |
| `wakta` | required | publish time, RFC 3339, UTC, e.g. `2026-07-27T14:03:11Z` |
| `kakwam` | optional | one-line description |
| `tojar` | optional | author |
| `terezh` | optional | licence, an SPDX identifier |
| `punjur` | optional | upstream repository URL |
| `asulna` | optional | homepage URL |
| `cizujo` | repeatable | one dependency: `<izim> SP <constraint>` |
| `kaldy` | optional | present only when yanked; the reason, or `-` when none was given |
| `wakwa` | optional | protocol marker; see §4.1 |

A record's required keys MUST all be present. Key order is not significant, but a server SHOULD emit them in the order of the table above for the sake of humans reading a socket dump.

Example (NOT A REAL PACKAGE):

```
izim: http
waktanimra: 1.4.2
warna: vivlijotiki
ozhon: 20481
sema: sha256:9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
wakta: 2026-07-14T09:21:00Z
kakwam: an HTTP client
tojar: robin
terezh: GPL-2.0-only
punjur: https://github.com/OboeLang/http
cizujo: tls >=0.3.0
cizujo: bytes ^1.0.0
```

## 7. Session rules

### 7.1 Timeouts

A server closes an idle session after, by default, 300 seconds by sending `koja` and closing. The timeout is measured from the end of the last complete request-response exchange. The value is configurable; 300 is the default and the value a client should assume.

A server MAY apply a shorter timeout to the handshake specifically.

A server that is over capacity MAY answer with `ramuzhu <sego>` and close without processing a request.

### 7.2 Yanked releases

A yanked release is visible, still fetchable, and marked*

- `ko besal <izim>` includes it in the listing.
- `ko besal <izim> <waktanimra>` returns its record, carrying a `kaldy` key.
- `ko ghazema <izim> <waktanimra>` still serves the archive.
- `ko cizujo` never selects it.

A resolver, whether server-side or client-side, MUST skip yanked releases when choosing a version, and MUST still be able to fetch one that is already pinned. This is the whole point: a yank stops new adoption without breaking anything that already builds.

The archive of a yanked release MUST NOT be deleted, and its `ozhon` and `sema` remain valid. katare/1 has no verb that deletes a release; a registry that must remove content (a leaked credential, a legal demand, etc) does so out of band, admittedly breaking stuff.

### 7.3 Rate limiting

A server MAY answer any request with `vazoj <sego>`. A client MUST wait at least `<sego>` seconds before retrying, MUST NOT retry more than a small number of times, and SHOULD apply its own backoff on top.

## 8. Names, versions and constraints

### 8.1 `izim`

```
izim = LOWER *( LOWER / DIGIT / "_" )
LOWER = %x61-7A
```

At most 64 octets. A package name must be a valid Oboe identifier, because `import foo` uses the bare name.

Reserved, and rejected by `kango`: `math`, `random`, `os` (the Oboe runtime built-in modules) and `oboe`, which names the toolchain itself in a project's `dependencies`.

The `izim` production is also used for account names.

### 8.2 `waktanimra`

```
waktanimra = major "." minor "." patch [ "-" tag ]
major = minor = patch = "0" / ( %x31-39 *DIGIT )
tag   = 1*( ALPHA / DIGIT / "." / "-" )
```

Each numeric component is decimal with no leading zeros. At most 64 octets total.

Ordering is semantic-versioning ordering:

1. Compare `major`, then `minor`, then `patch`, numerically.
2. A version **with** a tag sorts **before** the otherwise-identical version without one: `1.0.0-rc1` < `1.0.0`.
3. Two tags are compared by splitting on `.` and comparing each field: a field of only digits compares numerically and sorts before a field containing anything else; other fields compare by ASCII. A shorter tag sorts first when it is a prefix of the longer.

### 8.3 Constraints

These are the values of `dependencies` in a project's `project.jsonc`, and the second argument of `ko cizujo`.

| Form | Matches |
|---|---|
| `=X.Y.Z` | exactly that version |
| `>=X.Y.Z` | that version or any later one |
| `^X.Y.Z` | that version or later, up to the next change in the leftmost non-zero component |
| `*` | any version |

`^` is Cargo-style, which means it behaves differently below 1.0:

```
^1.2.3   >=1.2.3, <2.0.0
^0.2.3   >=0.2.3, <0.3.0
^0.0.3   >=0.0.3, <0.0.4
```

Prereleases are excluded unless asked for. A constraint matches a tagged version only when both of these hold:

1. the constraint is itself tagged, and
2. the constraint's `MAJOR.MINOR.PATCH` equals the version's.

So `>=1.0.0` does not match `1.1.0-rc1`; neither does `>=1.0.0-rc1`. Only a constraint naming `1.1.0` itself (e.g. `>=1.1.0-rc1`, `^1.1.0-rc1`, `=1.1.0-rc1`) can select it. 

`*` never selects a tagged version, so a package with only tagged releases is not selectable by `*`.

A constraint token contains no spaces. A dependency with several constraints is
expressed as several `cizujo` record lines naming the same `izim`.

### 8.4 Resolution

When several versions satisfy every accumulated constraint on a package, a
resolver MUST choose the **highest** by §8.2. Yanked releases are never chosen
(§7.2).

A resolved set contains at most one version of each `izim`. This is not a
simplification: Oboe installs packages into a flat `.oboe/libraries/<izim>/`
namespace, so two versions of one package cannot coexist. Incompatible constraints
on a shared dependency are an error, not a nested install.

## 9. Capabilities

A capability word is a bare structural token. Words defined by katare/1:

| Word | Means |
|---|---|
| `jexa` | `ko jexa` is available |
| `cizujo` | `ko cizujo` is available |
| `kalit` | `kalit` is available |
| `kango` | `kango` is available |
| `kaldy` | `kaldy` is available |
| `kyx<n>` | the body cap is `<n>` octets |
| `firme` | **reserved.** Names a future in-session TLS upgrade. No katare/1 server may advertise it, and no katare/1 client may request it. |

Capabilities are named without the `ko` prefix even where the verb carries it: the capability `jexa` gates the verb `ko jexa`.

A read-only mirror advertises neither `kalit`, `kango` nor `kaldy`, and answers `ezhazebyr` to any request whose first token is not `ko`.

## 10. Worked example

A complete session, with `→` for octets the client sends and `←` for octets the server sends. `<CR><LF>` is written explicitly, but nothing else is escaped.

```
← dijabon katare/1 reedbed/0.1<CR><LF>
→ dijabon katare/1 oboe/0.1<CR><LF>
← si jexa cizujo kalit kango kaldy kyx67108864<CR><LF>

→ ko besal bytes 1.0.0<CR><LF>
← si kyx 165<CR><LF>
← izim: bytes<LF>
  waktanimra: 1.0.0<LF>
  warna: vivlijotiki<LF>
  ozhon: 59<LF>
  sema: sha256:26f21312d1f381d3ff37b4a14ab4dc4756bab9e3a76c0923027502c6c0bfbc24<LF>
  wakta: 2026-07-14T09:21:00Z<LF>

→ ko ghazema bytes 1.0.0<CR><LF>
← si sha256:26f21312d1f381d3ff37b4a14ab4dc4756bab9e3a76c0923027502c6c0bfbc24 kyx 59<CR><LF>
← <59 octets of kabuk>

→ ko besal nosuch<CR><LF>
← keresebyr<CR><LF>

→ koja<CR><LF>
← koja<CR><LF>
[server closes]
```

The `kyx 165` above counts every octet of the record body including each `LF`, and excludes the blank line that would separate it from a following record. A single record body has no trailing separator.

## 11. Deliberate limits of katare/1

Stated so they are understood as choices rather than discovered as gaps.

- No transport security. §1.
- No pagination. `ko jexa` truncates and says so. §4.1.
- No deletion. Yanking is the only retraction, and it does not remove octets. §7.2.
- No account creation, token minting or owner management over the wire. Those are administrative operations performed on the server host.
- No pipelining. One request in flight.
- Search scales to roughly 10⁵ packages. The reference server scans a flat index linearly. Beyond that, `ko jexa` needs an inverted index, which is a server implementation change and not a protocol change.
- One version of a package per project. §8.4.
- No mirroring or replication protocol. A mirror is made by copying the store and running read-only.
