# Init and service management

Status: current implementation as of 2026-08-25

zedBSD boots through the native `/sbin/init`. It does not use runlevels,
SysV/BSD shell startup scripts, or a shell inside PID 1. Service policy is data
in `/etc/rc.conf`; executable metadata is data in `/etc/service.d/`.

## Boot sequence

PID 1 creates `/run`, `/var`, and `/var/log`, applies `hostname` from
`rc.conf`, and synchronously invokes `/sbin/mount -a`. The mount command—not
init—parses `/etc/fstab`. Init then loads service definitions, creates the
root-only `/run/init.sock`, starts enabled services in dependency order, and
prints `init: system running`.

Network addresses, routes, DNS, and DHCP settings are not `rc.conf` service
options. They live in `/etc/net.conf` and are applied by the `net` oneshot after
`networkd` reports ready. See the [build guide](../howto/build-from-source.md)
for the current amd64 QEMU launch procedure.

## `/etc/rc.conf`

This is a strict assignment file, not shell code. Blank lines and lines whose
first non-space character is `#` are ignored. A setting is `name=value`; a
single matching pair of single or double quotes may surround the entire value.
Duplicate requested keys and malformed lines are errors. There is no variable
expansion, command substitution, or shell evaluation.

`NAME_enable=YES` enables a service for boot. The accepted true spellings are
`YES`, `yes`, `1`, and `true`. The current base file also contains `hostname`
and service-specific options such as `ntpdate_servers`.

## `/etc/service.d/NAME`

Each regular definition uses the same assignment grammar. Service names are at
most 63 alphanumeric, underscore, or hyphen characters. The command must be an
absolute path.

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
pidfiles and double-forking are not the authority.

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
zedBSD-specific protocol, not systemd's notification socket.

## `/sbin/service`

Runtime requests use the root-only `/run/init.sock`:

```sh
service list
service status networkd
service start cron
service stop cron
service restart cron
service reload
```

`service enable NAME` and `service disable NAME` atomically edit only the exact
`NAME_enable` assignment, then ask init to reload policy. Reloading policy does
not start or stop an existing instance; use the runtime commands explicitly.
Runtime `start` also does not persistently enable a service.

When a supervised child exits, `restart=always` restarts it and
`restart=on-failure` restarts only a non-zero or signalled exit. The current
implementation bounds automatic restart attempts at five per service and
waits one second between attempts.

## Shutdown and signals

`halt`, `poweroff`, `reboot`, and `shutdown` send an orderly request to PID 1.
`SIGTERM` sent directly to PID 1 requests halt, `SIGINT` requests reboot, and
`SIGHUP` reloads enable policy. During shutdown init sends `SIGTERM` to each
tracked service, waits up to five seconds, then uses `SIGKILL` if necessary. It
calls `sync()` before the final `/dev/system` ioctl.

Current shutdown order is the reverse of the loaded service-definition order;
it is not yet a computed reverse dependency order. There is no separate
shutdown runlevel or shutdown-script directory.

## Diagnostics and limits

- `service: init is unavailable` means `/run/init.sock` could not be reached.
- `init: dependency cycle: NAME` means no dependency-ready start ordering was
  found for that enabled service.
- `init: NAME readiness timeout` identifies an FD 3 deadline failure.
- At most 32 definitions and 16 argv fields per service are currently loaded.
- Editing or adding a definition file is not re-read by `service reload`; the
  current reload operation updates only enable policy for definitions loaded at
  boot.

Implementation references are
[`init`](../../userland/base/init/main.c),
[`service`](../../userland/base/service/main.c), and the strict
[`rc.conf` parser](../../userland/base/service/service-config.c).
