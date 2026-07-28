# Reedbed

The package repository for [Oboe](https://github.com/OboeLang/oboe).

Reedbed serves packages over *katare*, a line protocol on TCP port 440. Packages travel as *kabuk* archives, which is an uncompressed, deterministic format.

- [KATARE.md](KATARE.md): the protocol. Normative.
- [KABUK.md](KABUK.md): the archive format. Normative.
- [DEPLOY.md](DEPLOY.md): running a server.

```
katare://host[:440]/<package>
```

## Build

Everything lives under `server/`.

```
cd server
make          # builds bin/reedbed, bin/reedbed-admin, bin/katarecat, bin/sema
make test
```

C11, POSIX, no weird dependencies. Storage is a plain filesystem tree.

## Binaries

| | |
|---|---|
| `reedbed` | the daemon |
| `reedbed-admin` | accounts, tokens, ownership, store verification |
| `katarecat` | scripted raw-protocol client, for debugging and tests |
| `sema` | SHA-256 over this tree's own implementation |

Accounts and tokens are created only by `reedbed-admin` on the server host, katare has no registration verb yet.

## Shared code

`common/` is the canonical copy of the five wire-format modules: `sha256`, `kabuk`, `izim`, `record`, and `projectjson`. The Oboe compiler will vendor the same files under `compiler/src/`, checked against `common/MANIFEST.sha256`. I recommend editing them here and then re-vendoring.
