# WS002: system services

Last updated: 2026-08-25

WSID: `ws002`

Status: complete through `ws002-p020`; follow-ups transferred to WS005

Parent: [master plan](../master.md)

Last verified Phase: `ws002-p020`

Resume point: this WS is closed as a baseline. Continue DHCP lifecycle, physical
networking, WLAN, and WPA in [WS005](../ws005-networking/ws.md).

Shared tests: [WS002 test index](tests/README.md)

## Goals

- Provide a shell-independent native init and service environment.
- Supply logging, console login, networking, scheduling, initial time setup,
  and a usable shell as one bootable base-system service set.
- Make service startup, supervision, control, and shutdown observable and
  recoverable under QEMU.

## WS completion conditions

WS002 is complete when the installed system boots through native PID 1, reaches
login, operates the declared services, and shuts down safely; Phases 11–20 have
recorded evidence or explicit handoffs; and remaining network expansion is
transferred to WS005 without an unowned service-baseline defect.

## 1. Objective

This roadmap defines the post-Phase-10 implementation sequence for a usable,
shell-independent zedBSD boot and service environment.  It introduces a
single native PID 1, declarative service definitions, console login, system
logging, network management, job scheduling, optional boot-time clock setting,
and a POSIX.1-2024 shell remediation phase.  The final phase boots and operates
the complete system under `qemu-system-x86_64` and repairs integration failures
until the documented minimum system works.

The roadmap is iterative.  A Phase may leave a standards gap only when the
implemented subset is safe and useful, the limitation is recorded in
[WS001](../ws001-posix/ws.md), and the Phase acceptance result states
whether it is complete or partially successful.  Integration defects that
prevent the documented minimum system from booting, logging in, operating, or
shutting down are not hand-off items: Phase 19 must repair them.

No external implementation is imported into `userland/base`.  No commit is
created by this work.

## 2. Fixed architecture

### 2.1 Boot and service management

- zedBSD has one native `/sbin/init`; the earlier `init.sysv`/`init.bsd`
  hard-link proposal is superseded.
- Runlevels are not implemented.
- PID 1 uses explicit lifecycle states: booting, running, stopping,
  finalizing, and halt/poweroff/reboot.
- `/etc/service.d/NAME` contains declarative service metadata, never shell
  code.
- `/etc/rc.conf` is parsed as data, never sourced or evaluated by a shell.
- Enabled services are started in dependency order and active services are
  stopped in reverse dependency/start order.
- Daemons run in the foreground.  PID 1 owns child supervision; pidfiles are
  not the authority.
- `/sbin/service` edits only exact `NAME_enable` assignments and performs
  runtime operations through `/run/init.sock`.
- `enable` and `disable` change persistent policy but do not implicitly start
  or stop the current instance.
- `halt`, `poweroff`, and `reboot` request an orderly transition through PID 1.

### 2.2 Configuration boundaries

`/etc/rc.conf` is the source for host settings, service enablement, and service
options. Structured databases remain separate where their format is the
interface. Network interface/address/route/DNS data moved to `/etc/net.conf`
in `ws011-p003`; the Phase 20 `net_*` format below is retained as history.

| Data | Path |
|---|---|
| filesystems | `/etc/fstab` |
| service definitions | `/etc/service.d/*` |
| account databases | `/etc/passwd`, `/etc/group`, `/etc/shadow` |
| user periodic jobs | cron spool/crontabs |
| one-shot jobs | at spool |
| resolver output | `/etc/resolv.conf` |
| persistent network configuration | `/etc/net.conf` |

PID 1 does not parse fstab.  A required internal oneshot invokes
`/sbin/mount -a`; the mount utility owns fstab parsing.

### 2.3 Logging

- `/run/log` is the initial local AF_UNIX datagram endpoint.
- `/var/log/messages` is the only initial persistent general log.
- `/run/dmesg.boot` is the boot-time kernel-message snapshot.
- `/var/log/syslog` and `/var/log/dmesg` are not created.
- `logger` implements POSIX.1-2024 behavior.  The libc syslog family is an XSI
  interface and is tracked separately from the non-standard `syslogd` daemon.

### 2.4 Network management

- `networkd` is authoritative for interfaces named in `/etc/net.conf`.
- `net` is the control front end and does not mutate managed interfaces behind
  the daemon's back.
- The first implementation supports loopback, interface up/down, static IPv4,
  a default route, initial DHCP acquisition, DNS output, and status.
- DHCP renewal, rebinding, expiry, and release are explicit later hand-offs.
- The existing `dhcpcd` remains available temporarily but cannot manage an
  interface concurrently with `networkd`; eventual integration/removal is a
  later project.
- Wi-Fi is a future `networkd` responsibility but is outside Phases 11--19.

### 2.5 Scheduling and time

- A single OpenBSD-style `/sbin/cron` owns periodic crontabs and the `at` and
  `batch` queues.
- Cron job command text is executed by `/bin/sh`; boot and PID 1 remain
  independent of the shell.
- Job output is durably spooled while no mail provider exists.
- `ntpd`, `adjtime()`, and `adjfreq()` are outside this roadmap and are not
  POSIX requirements.
- An optional `/sbin/ntpdate` oneshot may set the clock once after networking
  and before cron.  It is disabled by default.

### 2.6 Shell compatibility

- Phase 18 brings `/bin/sh` to the POSIX.1-2024 shell contract.
- zedBSD-only administration builtins and direct power ioctls are removed.
- Interactive libedit/readline-compatible editing remains an extension and
  must not affect non-interactive execution.
- A documented, tested whitelist of widespread bash/ksh-style extensions may
  remain.  Strict POSIX mode disables non-standard syntax and behavior.
- POSIX.1-2024 features such as dollar-single-quote syntax and `pipefail` are
  core requirements, not extension credits.

## 3. Phase registry

| Combined ID | Phase | Status | Required outcome |
| --- | --- | --- | --- |
| `ws002-p011` | [service foundation](phase011-service-foundation/phase.md) | Complete | Parsers, formats, path policy, tests, and `/sbin` classification are stable |
| `ws002-p012` | [init and service lifecycle](phase012-init/phase.md) | Complete | Native PID 1 boots, controls services, mounts filesystems, and shuts down orderly |
| `ws002-p013` | [system logging](phase013-logging/phase.md) | Complete | `logger`, `/run/log`, `syslogd`, boot log persistence, and messages work |
| `ws002-p014` | [console sessions](phase014-sessions/phase.md) | Complete | Supervised `getty` reaches `login` and respawns safely |
| `ws002-p015` | [network management](phase015-network/phase.md) | Complete baseline | `networkd` and `net` configure static IPv4 and initial DHCP |
| `ws002-p016` | [scheduled work](phase016-scheduling/phase.md) | Complete baseline | `cron`, `crontab`, `at`, and `batch` execute jobs with durable state |
| `ws002-p017` | [initial time](phase017-time/phase.md) | Complete optional feature | Bounded `ntpdate` works without making boot depend on network time |
| `ws002-p018` | [POSIX shell](phase018-shell/phase.md) | Partial with handoffs | Shell is usable; remaining incompatibilities are recorded in WS001 |
| `ws002-p019` | [integrated QEMU acceptance](phase019-integration/phase.md) | Complete minimum system | Boot, login, services, jobs, network, and shutdown were exercised and repaired |
| `ws002-p020` | [synchronous network service](phase020-network-service/phase.md) | Complete milestone | fd 3 readiness and synchronous `net` orchestration pass host/build/QEMU gates |

The original Phase 11–19 detail is retained in the
[legacy aggregate plan](history/phase011-019-legacy-plan.md).

## 4. Evidence policy

Every Phase must:

1. format new and modified userland C/header files with clang-format;
2. build through `make -j16`;
3. use focused host tests where they test parsers or pure logic without
   pretending to prove kernel behavior;
4. use bounded `qemu-system-x86_64` tests for runtime behavior;
5. avoid the aggregate `make check` target;
6. run `git diff --check` and the applicable provenance/matrix checks;
7. update [WS001](../ws001-posix/ws.md) with actual evidence and remaining
   gaps; and
8. leave the working tree uncommitted.

Phase 19 may mark the roadmap partially successful when a newly discovered
POSIX incompatibility is outside the planned implementation and the minimum
system still operates safely.  It must not use that rule for ordinary
integration bugs, boot failures, data corruption, or shutdown failures.

## 5. Reconsideration boundary

Stop and request direction instead of expanding the design if work requires:

- importing an external base implementation;
- changing the single-init, no-runlevel model;
- introducing shell execution into PID 1 or service definitions;
- replacing the single `rc.conf` service-policy model;
- adding a material public kernel ABI not identified by the applicable Phase;
  or
- weakening a correct POSIX expectation or QEMU acceptance test.
