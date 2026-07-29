# kabuk1

The archive format carried by katare's `ghazema` and `kango` verbs (see [KATARE.md](KATARE.md)).

This document is normative. MUST, MUST NOT, SHOULD, SHOULD NOT and MAY carry their RFC 2119 meanings.

## 1. What it is for

A kabuk holds one package's source tree. The driving philosophy is that the same tree always produces the same octets, and therefore the same digest. That makes this format deliberately simple and hard to footgun.

A publisher can rebuild an archive from a checkout and confirm it matches what the registry serves.

## 2. Grammar

```
kabuk   = magic *entry terminator
magic   = "kabuk1" LF
entry   = path LF length LF octets
path    = 1*512OCTET
length  = "0" / ( %x31-39 *DIGIT )
octets  = <exactly `length` octets>
terminator = "sampura" LF
```

`LF` is `0x0A`. There is no `CR` anywhere in the framing.

`length` is decimal ASCII, no sign, no leading zeros, at most 10 digits. A zero-length file is `0` followed by no octets.

The file octets are opaque. They are copied verbatim and are never interpreted, transformed, or newline-converted.

There is nothing after `sampura` LF. A reader MUST reject trailing octets, and MUST reject a stream that ends without the terminator.

## 3. Paths

A `path` is the file's location relative to the package root, as a POSIX path.

It MUST:

- be valid UTF-8;
- be at most 512 octets;
- use `/` as its only separator.

It MUST NOT:

- be empty;
- begin with `/`;
- end with `/`;
- contain a `\` (`0x5C`);
- contain any octet below `0x20`, or `0x7F`;
- contain a `NUL`;
- contain an empty component (`a//b`);
- contain a component equal to `.` or `..`, including a leading `./`.

Directories are implicit. There are no directory entries, directories only exist when files inside them do. An empty directory cannot be represented and is not preserved.

### 3.1 Ordering

Entries MUST be sorted by path, ascending, comparing octets, meaning a `memcmp` ordering, not a locale collation and not a Unicode collation. A packer that sorts with `strcoll` produces different archives in different locales, which defeats the entire point of the format.

Each path MUST be strictly greater than the one before it. Equal paths are therefore forbidden, which is what makes duplicate entries impossible.

A reader MUST verify the ordering as it goes. This is not merely an integrity check: it is what proves the archive was produced by a conforming packer, and it makes "last entry wins" attacks unrepresentable.

## 4. What is not in a kabuk

A kabuk MUST NOT have symlinks, permission bits, timestamps, uid, gid, special files, or compression. The format has no way to represent these.

Every extracted file is a regular file with the extractor's default mode. A package that needs an executable bit sets it at build time; Oboe packages are source, and source does not need one.

## 5. Digest

`sema` is the SHA-256 of the **entire** octet stream, including the `kabuk1` magic, every entry, and the `sampura` terminator included. It is written as

```
sha256:<64 lowercase hex digits>
```

Uppercase hex is not valid. There is no other digest algorithm in this version; the `sha256:` prefix exists so that adding one later does not require a new field.

## 6. Extraction

An extractor is reading input from a stranger and MUST behave accordingly.

It MUST:

- validate every path against §3 before creating anything, and reject the entire archive on the first violation (reject, never normalisze, because normalizsation is a big "HERE BE DRAGONS")
- verify the strict ordering of §3.1;
- create parent directories itself, one component at a time, and tolerate an existing one only after confirming it is a real directory and not a symlink;
- create each file with `O_CREAT | O_EXCL | O_NOFOLLOW`, so that neither a pre-existing file nor a planted symlink can be written through;
- verify the `sema` before, or while, extracting, and discard everything on mismatch;
- enforce limits on the entry count, on any single file's length, and on the total extracted size, refusing an archive that exceeds them.

It MUST NOT extract into a live directory in place. The safe sequence is: extract to a fresh temporary directory on the same filesystem, verify, remove the old directory, then rename the new one into position. A partially extracted package must never be visible to the compiler.

## 7. Packing

A packer walks the tree, collects relative paths, sorts them by §3.1, and writes the entries.

It MUST refuse, rather than silently skip or follow, anything it cannot represent: a symlink, a FIFO, a device, a socket, a path that violates §3.

Exclusions are applied while walking. The reference packer (`oboe publish`) always excludes, with no way to turn it off:

| | |
|---|---|
| `.git/`, `.oboe/` | at any depth: tool state, never source |
| `dist/` | **top level only** |
| `*.o`, `*.kate-swp`, `.DS_Store` | at any depth |
| any other name beginning with `.` | top level only, except `.oboeignore` itself |

`dist` is deliberately not excluded at depth. A package may legitimately have a
`src/dist/` of its own, and dropping it would produce an archive that is
well-formed, reproducible, and quietly missing source.

Further exclusions come from `.oboeignore` in the package root: one glob per line, `#` for comments, `!` to negate an earlier pattern, and a trailing `/` to match directories only. A pattern is tested against both the full relative path and the entry's own name, so `notes.md` and `docs/notes.md` both work.

Because exclusions are part of what determines the octets, two packers agree only if they agree on exclusions. A publisher who cares about reproducibility should publish from a clean checkout and record the printed `sema`.

## 8. Worked example

A package with two files:

```
main.oboe        contains "print(\"hi\")\n"   (12 octets)
project.jsonc    contains "{}\n"              (3 octets)
```

`main.oboe` < `project.jsonc` in octet order (`m` is `0x6D`, `p` is `0x70`), so it comes first. The archive is:

```
6b 61 62 75 6b 31 0a                             "kabuk1" LF          7
6d 61 69 6e 2e 6f 62 6f 65 0a                    "main.oboe" LF      10
31 32 0a                                         "12" LF              3
70 72 69 6e 74 28 22 68 69 22 29 0a              "print(\"hi\")" LF  12
70 72 6f 6a 65 63 74 2e 6a 73 6f 6e 63 0a        "project.jsonc" LF  14
33 0a                                            "3" LF               2
7b 7d 0a                                         "{}" LF              3
73 61 6d 70 75 72 61 0a                          "sampura" LF         8
```

Total: 59 octets, with digest

```
sha256:26f21312d1f381d3ff37b4a14ab4dc4756bab9e3a76c0923027502c6c0bfbc24
```

As text, with `¶` marking each LF:

```
kabuk1¶main.oboe¶12¶print("hi")
¶project.jsonc¶3¶{}
¶sampura¶
```

Note that an entry's octets run straight into the next entry's path with no separator of their own. The `¶` after `print("hi")` is part of the file's contents, not framing. A reader that incorrectly scans for newlines instead of counting `length` octets will corrupt every archive containing a text file.

This example is committed as `server/tests/fixtures/simple.kabuk`, alongside the tree it was packed from (`simple_tree/`) and its expected digest (`simple.sema`). It is a good first test vector both implementations must pass, in both directions: packing `simple_tree/` must reproduce `simple.kabuk` octet for octet, and hashing `simple.kabuk` must reproduce `simple.sema`.