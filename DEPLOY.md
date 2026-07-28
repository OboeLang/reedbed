# Running a reedbed

Reedbed is one static binary and one directory. It should be pretty easy to run.

## Build

```
cd server
make
```

Produces `bin/reedbed`, `bin/reedbed-admin`, `bin/katarecat` and `bin/sema`. C11 and POSIX only, so any reasonably current gcc or clang will do.

## The store

Everything lives under one directory, conventionally `/var/lib/reedbed`:

```
/var/lib/reedbed/
├── packages/<izim>/releases/<waktanimra>/{meta,kabuk,sema,ok,kaldy}
├── accounts/<izim>/tokens/<sha256hex>
├── index/{jexa,tokens/}
└── tmp/
```

It is a plain filesystem tree. You can manipulate it with standard tools.

`tmp/` must be on the same filesystem as `packages/`.** Publishing writes to a temporary file and renames it into place; a rename across filesystems fails, and the atomicity that makes a half-written release impossible comes from that rename.

A release directory without an `ok` file is invisible. That marker is written last, so a publish interrupted at any point leaves something no reader will serve. `reedbed-admin sweep` removes them, and the daemon runs a sweep at startup.

## First run

```
sudo install -d -o reedbed -g reedbed /var/lib/reedbed
sudo -u reedbed bin/reedbed-admin --root /var/lib/reedbed create-account robin
sudo -u reedbed bin/reedbed-admin --root /var/lib/reedbed mint-token robin
```

`mint-token` prints the token **once**. The store keeps only its SHA-256, so there is no way to recover it afterwards and no way for someone with a copy of your backups to read it. Mint another if it is lost.

Accounts, tokens and ownership are administrative operations with no protocol verb: katare has no registration yet, and a compromised client cannot mint itself an identity. `grant-owner` adds a co-owner to an existing package; the first account to publish a name owns it.

## Running it

Port 440 is privileged. Either start as root and drop with `--user`, which binds first and then drops:

```
bin/reedbed --root /var/lib/reedbed --user reedbed
```

or grant the capability and never be root at all:

```
sudo setcap cap_net_bind_service=+ep bin/reedbed
```

A systemd unit:

```ini
[Unit]
Description=reedbed package registry
After=network.target

[Service]
Type=exec
ExecStart=/usr/local/bin/reedbed --root /var/lib/reedbed --user reedbed --foreground
Restart=on-failure
RestartSec=5

# it needs one directory and a socket, and nothing else
User=root
AmbientCapabilities=CAP_NET_BIND_SERVICE
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ReadWritePaths=/var/lib/reedbed

[Install]
WantedBy=multi-user.target
```

## Options

| | |
|---|---|
| `--root <dir>` | store directory (default `/var/lib/reedbed`) |
| `--port <n>` | listen port; `0` picks an ephemeral one (default 440) |
| `--user <name>` | drop privileges after binding |
| `--print-port` | write `listening <port>` to stdout once bound |
| `--foreground` | do not detach |
| `--idle-timeout <s>` | session idle timeout (default 300) |
| `--max-children <n>` | concurrent connections (default 64) |
| `--body-cap <octets>` | largest body accepted or sent (default 67108864) |
| `--jexa-cap <n>` | maximum search results (default 100) |
| `--rate <n>` | requests per minute per peer; `0` disables |
| `--rate-bytes <n>` | octets per minute per peer; `0` disables |
| `--rate-auth <n>` | `kalit` attempts per minute per peer |
| `--no-cizujo` | do not offer server-side dependency resolution |
| `--mirror` | read-only: refuse every request not prefixed `ko` |
| `-v`, `--verbose` | |

## Security

**katare/1 is plaintext*** until the reserved `firme` capability is specified in the future.

The server forks per connection, so one session cannot take down the service, and `--max-children` bounds how many can exist. Rate limits are per peer prefix (a /64 for IPv6, a /32 for IPv4) rather than per address. `kalit` has its own much tighter bucket, charged whether or not the token was any good.

The daemon needs write access to its store directory and nothing else.

## Mirrors

A mirror is a copy of the store served read-only:

```
rsync -a origin:/var/lib/reedbed/ /var/lib/reedbed/
bin/reedbed --root /var/lib/reedbed --mirror
```

`--mirror` advertises no write capability and refuses anything whose first token is not `ko`, without needing to recognise the verb. Because archives are content-addressed and versions are immutable, a mirror that is behind is missing releases rather than serving wrong ones.

Keep `cizujo` consistent across mirrors, or run them all with `--no-cizujo`: a client falls back to resolving from `ko besal` itself, and both paths follow the same rules, so they reach the same answer either way.

## Backups

`rsync -a` the store directory. To check one:

```
bin/reedbed-admin --root /var/lib/reedbed verify
```

That re-derives every archive's SHA-256 and compares it to the record. Run it after a restore.

`reedbed-admin reindex` rebuilds `index/jexa` from the package tree; the index is derived state and can always be thrown away.

## Upgrades

Stop the daemon, replace the binary, start it. The store format is plain files and this version does not migrate anything. Children serving requests at the moment of a restart are killed with it, and a client sees a closed connection, which it will treat as a failed request.