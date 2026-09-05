# Init and service management

Status: current; implementation reconciled on 2026-08-28

zedBSD boots through the native `/sbin/init`. It does not use runlevels,
SysV/BSD shell startup scripts, or a shell inside PID 1. Persistent service
policy is strict YAML data in `/etc/rc.conf`; executable metadata remains a
separate assignment file in `/etc/service.d/NAME`.

## Boot sequence

PID 1 creates `/run`, `/var`, and `/var/log`, loads one complete `rc.conf`
snapshot, applies its `hostname`, and synchronously invokes `/sbin/mount -a`.
The mount command—not init—parses `/etc/fstab`. Init then loads service
definitions, creates the root-only `/run/init.sock`, starts enabled services in
dependency order, and prints `init: system running`.

An invalid startup `rc.conf` does not expose a partially parsed policy. Init
continues without enabled policy from that file and reports the load failure.

Network addresses, routes, DNS, and DHCP settings are not `rc.conf` service
options. They live in `/etc/net.conf` and are applied by the `net` oneshot after
`networkd` reports ready. See the [build guide](../howto/build-from-source.md)
for the current amd64 QEMU launch procedure.

## `/etc/rc.conf`: fixed YAML v1

`/etc/rc.conf` accepts a deliberately small YAML-shaped contract, not general
YAML and not shell assignments. The installed form is:

```yaml
version: 1
hostname: zedbsd

services:
  cron:
    enabled: true
  ntpdate:
    enabled: false
    settings:
      servers: "pool.example.net time.example.net"
```

The top-level keys are exactly `version`, `hostname`, and `services`;
`version` must be `1`. Each service must contain one Boolean `enabled` value.
The only service-specific setting currently accepted is
`ntpdate.settings.servers`. Service and setting names contain at most 63
alphanumeric, underscore, or hyphen characters.

Indentation is exactly two spaces per level. Tabs, duplicate keys, unknown
keys, sequences, flow collections, anchors, aliases, tags, document markers,
multiline scalars, inline comments, escape processing, and shell evaluation are
not supported.
Blank lines and full-line comments are accepted. Plain strings use only
alphanumeric characters plus `.`, `_`, `-`, `/`, and `:`; strings containing
spaces must use one matching pair of single or double quotes. A quoted string
cannot contain its own delimiter.

The writer emits a canonical model: top-level fields first, then services and
settings in bytewise name order. Unsafe strings are quoted. A failed load or
reload is all-or-nothing; reload failure preserves the last valid in-memory
policy and does not change running service instances.

### Locked persistent updates

`service enable NAME` and `service disable NAME` serialize writers with an
exclusive `fcntl()` lock on the stable `/etc/rc.conf.lock` companion file. A
writer acquires the lock before re-reading the latest policy, writes an
exclusive temporary file in `/etc`, calls `fsync()`, and atomically renames it
over `/etc/rc.conf` while still holding the lock. Pre-rename failure removes
the temporary file and leaves the old bytes visible. Root writes a
root-owned, mode `0644` `rc.conf`; the lock file is mode `0600`.

Readers rely on the atomic rename and do not take the writer lock. The lock is
per update, not per interactive console session.

## `/etc/service.d/NAME`

Each regular definition uses the older strict assignment grammar. Blank lines
and full-line comments are ignored; a setting is `name=value`, with one
matching pair of single or double quotes optionally surrounding the complete
value. It is data, not shell code. Service names are at most 63 alphanumeric,
underscore, or hyphen characters, and `command` must be an absolute path.

| Key | Current meaning |
| --- | --- |
| `type` | `daemon`, `oneshot`, or `respawn`; default is `daemon` |
| `command` | Absolute executable path; required |
| `arguments` | Whitespace-separated argv fields; shell quoting is not interpreted |
| `after` | Comma-separated ordering dependencies |
| `requires` | Comma-separated hard dependencies which must be enabled and successful |
| `restart` | `no`, `always`, or `on-failure` |
| `notify-fd3` | `on` enables the readiness protocol for non-oneshot services |
| `notify-timeout` | Readiness deadline in seconds, from 1 through 300; default 10 |
| `required` | Parsed as current metadata; it does not yet make all boot failures fatal |

Daemons must remain in the foreground. Init tracks their child PID directly;
pidfiles and double-forking are not the authority. `service reload` reloads the
YAML policy only; it does not re-read definition files added or edited after
boot.

## FD 3 readiness

For `notify-fd3=on`, init gives the child an open descriptor 3 and sets
`ZEDBSD_NOTIFY_FD=3`. The daemon must write exactly one newline-terminated
record:

```text
READY
```

or:

```text
FAIL 5 reason without control characters
```

The failure code must be a positive decimal integer. Extra records, malformed
text, an oversized record, early EOF, or timeout fail startup; init terminates
and reaps that child. A dependent `requires` service is then skipped. This is a
zedBSD-specific child-readiness protocol, separate from the ZSV1 control
protocol.

## ZSV1 init control protocol

Privileged clients control PID 1 through `/run/init.sock`, a mode `0600` Unix
stream socket. Version 1 accepts only these exact newline-terminated requests:

```text
ZSV1 LIST
ZSV1 SHOW NAME
ZSV1 START NAME
ZSV1 STOP NAME
ZSV1 RESTART NAME
ZSV1 RELOAD
ZSV1 HALT
ZSV1 POWEROFF
ZSV1 REBOOT
```

A client sends one request, performs `shutdown(SHUT_WR)`, and reads the complete
typed response. PID 1 waits for EOF before dispatch and applies a five-second
receive deadline. Requests are bounded at 256 bytes. Response records are
bounded at 512 bytes per line and end exactly once with `ZSV1 END`, for example:

```text
ZSV1 SERVICE networkd running 1 184
ZSV1 AFTER syslogd
ZSV1 OK reloaded
ZSV1 END
```

An operation can instead return `ZSV1 ERROR ERRNO REASON` followed by the end
record. Non-ZSV1 input is rejected as an unsupported protocol version; there
is no unversioned fallback and clients do not parse human display text. The
six runtime states are `stopped`, `starting`, `running`, `completed`, `failed`,
and `skipped`. LIST and SHOW each use one coherent PID 1 snapshot.

The shared client uses one 310-second monotonic deadline for connection,
request transmission, and response reception. Control clients validate record
types, expected success tokens, names, counts, and bounds before accepting a
result.

## `/sbin/service` argument mode

Every well-formed command requires effective UID 0 before contacting init or
changing policy:

```sh
service list
service show
service show networkd
service status networkd
service start cron
service stop cron
service restart cron
service enable cron
service disable cron
service reload
```

`service list` and argument-free `service show` print the same name-sorted
table. All six runtime states are represented, and a service without a live PID
uses `-`:

```text
NAME        STATUS    ENABLED   PID
networkd    running   yes       184
ntpdate     stopped   no        -
```

`show NAME` and `status NAME` are aliases. Their detailed output adds validated
`type`, `command` and arguments, `restart`, and sorted direct `AFTER` and
`REQUIRES` relationships. It does not claim transitive dependency or container
state.

Runtime `start`, `stop`, and `restart` do not modify `/etc/rc.conf`.
`enable` and `disable` do not start or stop a service. They first verify the
definition and current ZSV1 SHOW record, atomically change only that service's
`enabled` Boolean, and then synchronously request ZSV1 RELOAD.

If persistence succeeds but reload transport, protocol, typed-error, or token
validation fails, the command exits with an explicit “persistent policy
changed” and “runtime policy may remain stale” diagnostic. It deliberately
does not restore old bytes, because doing so could overwrite a later concurrent
writer. A later successful reload or reboot reconciles runtime policy.

Argument-mode exit status is `0` for success, `1` for authorization,
configuration, transport, protocol, timeout, or operation failure, and `2` for
invalid command grammar.

## `/sbin/service` interactive mode

Invoking `service` without arguments enters the console after checking
effective UID 0:

```text
zedBSD Service Console
Type '?' for help.
service>
```

The console provides all argument-mode commands plus `help`, `?`, `exit`, and
`quit`. EOF, `exit`, and `quit` return success. Help intentionally has no
`save`, `commit`, or candidate configuration. Command errors are reported and
the prompt continues; each command opens a fresh ZSV1 exchange and uses the
same dispatcher and policy-update path as argument mode.

Input is a small command grammar, not a shell: only space and tab split fields,
there is no quoting, expansion, redirection, or pipeline syntax, and at most 16
fields are accepted. A line of at most 511 bytes is accepted. A 512-byte or
longer line is completely consumed, rejected once, and cannot leak a suffix
into the next command. Control characters are rejected. Completion, history,
and line editing are not currently provided.

## Supervision and restart

When a supervised child exits, `restart=always` restarts it and
`restart=on-failure` restarts only a non-zero or signalled exit. The current
implementation bounds automatic restart attempts at five per service and waits
one second between attempts.

Reloading persistent policy does not reconcile process state: it neither
starts a newly enabled service nor stops a newly disabled one. Use the runtime
commands explicitly.

## Shutdown and signals

`halt`, `poweroff`, `reboot`, and `shutdown` use the shared typed ZSV1 client.
PID 1 schedules the action only after the full `ZSV1 OK scheduled` plus
`ZSV1 END` response has been sent successfully. `SIGTERM` sent directly to PID
1 remains a halt request, `SIGINT` requests reboot, and `SIGHUP` requests policy
reload.

During shutdown init sends `SIGTERM` to each tracked service, waits up to five
seconds, then uses `SIGKILL` if necessary. It calls `sync()` before the final
`/dev/system` ioctl. The present `/dev/system` backend maps both HALT and
POWEROFF to the halt operation, so physical power removal is not yet
guaranteed.

Current shutdown order is the reverse of the loaded service-definition order;
it is not yet a computed reverse dependency order. There is no separate
shutdown runlevel or shutdown-script directory.

## Diagnostics and limits

- `service: effective UID 0 is required` means authorization failed before any
  backend operation.
- `service: init request failed: ...` identifies a socket, timeout, or other
  transport failure.
- `service: init rejected request: REASON (errno N)` is a typed PID 1 failure.
- `service: invalid ZSV1 ... response` means framing, bounds, record shape, or
  the command-specific success token did not match the contract.
- `init: dependency cycle: NAME` means no dependency-ready start ordering was
  found for that enabled service.
- `init: NAME readiness timeout` identifies an FD 3 deadline failure.
- At most 32 definitions and 16 argv fields per definition are loaded. The
  policy model also holds at most 32 services and four settings per service.

Implementation references are
[`init`](../../userland/base/init/main.c),
[`service`](../../userland/base/service/main.c),
the shared [`command dispatcher`](../../userland/base/service/service-command.c),
the [`interactive console`](../../userland/base/service/service-console.c),
the strict [`YAML policy model`](../../userland/base/service/rcconf.c),
the definition [`assignment parser`](../../userland/base/service/service-config.c),
and the shared
[`ZSV1 protocol`](../../userland/base/service/zsv1-protocol.c),
[`client`](../../userland/base/service/zsv1-client.c), and
[`server transport`](../../userland/base/service/zsv1-server.c).

The executable evidence and current acceptance matrix are indexed by the
[WS012 shared tests](../../plan/ws012-service-console/tests/README.md).
