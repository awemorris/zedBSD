# Phases 11--19 execution plan

Last updated: 2026-08-25

This document is the executable detail for
[`system-services-roadmap.md`](system-services-roadmap.md).  A Phase is complete
only when its implementation, focused tests, amd64 QEMU evidence, build gate,
and master-document transition all pass.  A useful partial result is reported
as partial, never silently promoted.

## Phase 11: service foundation and administrative layout

### Scope

1. Define a strict line-oriented `rc.conf` grammar supporting comments,
   quoted strings, exact keys, duplicate-key rejection, bounded values, and no
   expansion or command execution.
2. Add a reusable reader plus an atomic editor that preserves comments,
   ordering, blank lines, and unknown keys while changing one exact
   `NAME_enable` value under a lock.
3. Define and parse `/etc/service.d/NAME` metadata: `type`, `command`,
   `arguments`, `after`, `requires`, `restart`, `restart_delay`,
   `stop_signal`, `stop_timeout`, and `required`.
4. Validate service names, absolute executable paths, duplicate fields,
   dependency cycles, missing requirements, numeric bounds, and unsafe
   arguments.
5. Establish `/run/init.sock`, `/run/log`, and
   `/run/networkd.sock` as initial endpoints protected by filesystem
   permissions.
6. Reclassify administrative programs.  Daemons and privileged system
   controls install below `/sbin`; ordinary-user front ends such as `logger`,
   `crontab`, `at`, and `batch` remain in the normal command path.
7. Add sample/default `rc.conf`, fstab, and service definitions through the
   normal data-package installation mechanism.

### Gates

- strict parser/editor host tests including malformed input, duplicates,
  interrupted replacement, and preservation;
- service-graph host tests including cycles and stable ordering;
- direct package/staged installation checks for every affected package;
- `make -j16`; and
- QEMU parsing of the installed configuration without invoking a shell.

### Master transitions

Add configuration/service-definition component rows.  Do not promote
`KERN-BOOT-01` or `SVC-INIT-01` until Phase 12 runtime evidence exists.

## Phase 12: native init, service control, and orderly shutdown

### Scope

1. Change the kernel's initial user process to `/sbin/init`, with a bounded
   emergency fallback only when init cannot be executed.
2. Define PID 1 behavior for orphan reaping, fatal/default signals, explicit
   HUP reload, TERM poweroff, INT reboot, and emergency death.
3. Implement boot states and mandatory internal tasks: hostname,
   `mount -a`, and runtime-directory creation.
4. Extend `mount` with checked `/etc/fstab` parsing and `-a`.  Root and already
   mounted entries are handled idempotently; required and `nofail` failures are
   distinguished.
5. Load, validate, order, and start enabled services without `/bin/sh`.
6. Supervise daemon/respawn children, retain exit/signal status, and suppress
   crash loops with bounded restart policy.
7. Implement the versioned root-only init control protocol and `/sbin/service`
   commands: list, status, start, stop, restart, enable, disable, and reload.
8. Implement `/sbin/halt`, `/sbin/poweroff`, `/sbin/reboot`, and shutdown
   requests through init.  Remove direct user-command power transitions.
9. On shutdown, freeze starts, stop every active service in reverse order,
   run shutdown-only hooks, terminate remaining processes, sync, unmount, and
   invoke the selected kernel action.

### Gates

- host tests for graphs, state transitions, protocol framing, and rc editing;
- QEMU boot reaches the running state with no shell in the boot path;
- service start/stop/restart and persistent enable/disable work;
- crash/restart, cycle, missing required service, and stop-timeout cases work;
- halt, poweroff, and reboot pass through PID 1 and perform finalization;
- `make -j16` and formatting/diff checks pass.

### Master transitions

Update `KERN-BOOT-01` and `SVC-INIT-01` to the evidence-supported state.  Add
separate hand-offs for any missing credential, readiness-notification, mount,
or shutdown semantics.

## Phase 13: POSIX logger and zedBSD syslogd

### Scope

1. Replace the `logger` deferred stub with the POSIX.1-2024 command, including
   stdin/file/message input, required priority/tag/PID options, checked output,
   and diagnostics.
2. Add the XSI `<syslog.h>` interfaces `openlog()`, `syslog()`,
   `setlogmask()`, and `closelog()` with thread-safe process state.
3. Implement foreground `/sbin/syslogd` receiving bounded AF_UNIX datagrams
   on `/run/log` and writing checked records to `/var/log/messages`.
4. Define timestamp, hostname, tag, PID, facility, severity, invalid-message,
   permission, queue-pressure, and disk/write failure behavior.
5. Add a kernel log stream/cursor that does not destroy the existing
   `kern.msgbuf`/`dmesg` view, import early messages, and persist new kernel
   messages through syslogd.
6. Save `/run/dmesg.boot` once after local filesystems are available.
7. Reopen output on HUP and flush/close on TERM.  Log rotation and arbitrary
   syslog routing remain later work.

### Gates

- POSIX logger host tests plus libc formatting/mask tests;
- QEMU user and daemon messages appear once with correct metadata;
- early and live kernel messages are visible through both `dmesg` and the
  documented persistent/snapshot paths;
- malformed datagrams, unavailable socket, full/broken output, HUP, and TERM
  have bounded behavior;
- `make -j16` passes.

### Master transitions

Update `SVC-LOG-01`, add the XSI syslog API row, and update the `logger` matrix
row only to the level supported by the complete Issue 8 utility tests.

## Phase 14: getty, login sessions, and respawn

### Scope

1. Implement foreground `/sbin/getty DEVICE`, opening and validating the tty,
   creating a session, acquiring the controlling tty, setting foreground
   process groups and termios, and executing `/bin/login`.
2. Register `getty-console` as a respawn service with rate limiting.
3. Audit `login` credential changes, environment, shell argv, PATH,
   controlling-tty behavior, exit propagation, and sensitive-buffer clearing.
4. Establish BOOT_TIME/LOGIN_PROCESS/USER_PROCESS/DEAD_PROCESS ownership and
   lock-safe utmpx updates; clean stale console records at boot.
5. Keep production autologin disabled.  A build/test-only QEMU fixture may
   enable console autologin without changing installed production defaults.

### Gates

- QEMU presents a login path on `/dev/console`;
- successful test login starts the account shell with correct ids, cwd,
  environment, session, and foreground tty;
- logout records DEAD_PROCESS and getty respawns;
- failed authentication, tty hangup, repeated failure, and shutdown are
  bounded;
- `who`, `write`, and utmpx readers observe consistent state;
- `make -j16` passes.

### Master transitions

Update `LIBC-UTMPX-01`, `API-UTMPX-01`, and tty/session hand-offs without
claiming complete tty conformance from the console scenario alone.

## Phase 15: networkd and net

### Scope

1. Implement foreground `/sbin/networkd` with one state object per configured
   interface and states starting, configuring, online, degraded, offline, and
   stopping.
2. Read `network_interfaces` and structured per-interface mode/address/
   gateway/nameserver keys from `rc.conf`.
3. Configure loopback unconditionally and managed interfaces through shared
   checked networking helpers, not shell command strings.
4. Support static IPv4, prefix/netmask/broadcast, interface flags, one initial
   default route, and atomic resolver-file output.
5. Reuse the local DHCP packet codec and implement bounded initial discover,
   offer, request, and acknowledgement acquisition with rollback on failure.
6. Implement the versioned root-only `/run/networkd.sock` protocol and
   `/sbin/net` commands: show, show-interface, up, down, dhcp, static, reload.
7. Ensure every mutation requested by `net` goes through networkd.  Retain
   `ifconfig` and `route` for diagnostics/manual unmanaged use.
8. Refuse concurrent `dhcpcd` ownership.  Record renewal/rebind/expiry/release,
   multi-route policy, IPv6, link events, and Wi-Fi as later work.

### Gates

- parser/state/DHCP codec host tests;
- QEMU static IPv4 configuration and route/resolver inspection;
- QEMU DHCP acquisition against the controlled test network;
- `net` status and mutations agree with kernel state;
- timeout, malformed reply, route failure, resolver failure, reload, and
  shutdown remain bounded and correctly reported;
- `make -j16` passes.

### Master transitions

Add network service and control rows.  Explicitly retain the first-lease-only
DHCP limitation; do not describe it as a complete DHCP daemon.

## Phase 16: cron, crontab, at, and batch

### Scope

1. Replace the three deferred stubs with local queue/spool front ends and add
   foreground `/sbin/cron`.
2. Parse periodic crontabs with bounded lines/fields, documented timezone/DST
   semantics, atomic replacement, owner validation, and permission checks.
3. Parse POSIX `at` time operands and durably create one-shot or batch queue
   records under a root-owned spool.
4. Have cron monitor periodic and one-shot queues with race-safe claiming,
   stable job ids, restart recovery, and corrupt-entry quarantine.
5. Restore recorded uid/gid, supplementary groups, cwd, umask, environment,
   and separate process group before executing command text with `/bin/sh`.
6. Capture stdout/stderr durably.  Until a mail provider exists, retain output
   in the documented output spool and record the delivery gap in the master.
7. Implement cancellation/list/edit operations required by `at`, `batch`, and
   `crontab`; handle shutdown and running-job policy explicitly.

### Gates

- parser/time/spool host tests including malformed and hostile records;
- QEMU periodic, at, and batch jobs execute under the intended credentials;
- restart recovery neither duplicates nor loses a claimed job;
- clock changes, locked accounts, output failures, full spool, cancellation,
  and shutdown have documented behavior;
- `make -j16` passes.

### Master transitions

Update `SVC-SCHED-01` and the three utility rows.  Mail delivery and any shell
semantic dependency that remains after Phase 18 stay explicit hand-offs.

## Phase 17: optional boot-time ntpdate

### Scope

1. Implement local `/sbin/ntpdate` as an optional, disabled-by-default
   oneshot after networkd and before cron.
2. Send bounded NTP client requests to configured IPv4 servers and validate
   source, packet length/version/mode, stratum, timestamps, and arithmetic.
3. Select a sane result from multiple samples and set `CLOCK_REALTIME` once
   with existing privilege checks.
4. Bound name resolution and network timeout; failure does not make ordinary
   boot depend on external network time.
5. Do not implement periodic stepping, `ntpd`, `adjtime()`, or `adjfreq()` in
   this roadmap.

### Gates

- packet/math host tests;
- QEMU success against a controlled test endpoint;
- malformed, spoofed, unreachable, timeout, and disabled cases;
- cron starts only after the enabled ntpdate oneshot reaches a terminal state;
- `make -j16` passes.

### Master transitions

Record ntpdate as a zedBSD extension, not a POSIX completion requirement.

## Phase 18: POSIX.1-2024 `/bin/sh`

### Scope

1. Build a clause/matrix-driven shell test inventory before promotion.
2. Remove zedBSD administration builtins and direct system-power ioctls:
   `halt`, `reboot`, `run`, `noct`, `emacs`, and shell-only `vmstat` behavior.
3. Remove the implicit `/etc/zinit.rc` startup path.  Implement the applicable
   POSIX interactive `ENV` rules.
4. Remove or fully regularize duplicated utility builtins (`cat`, `ls`, `cp`,
   `stat`, `touch`, `clear`) so command search, PATH, functions, hashing, and
   external replacements obey the standard.
5. Implement the Issue 8 lexical grammar, reserved words, compound commands,
   functions, lists, pipelines, asynchronous execution, subshells, grouping,
   all redirections, and here-documents.
6. Implement expansion ordering and semantics: tilde, parameter, command,
   arithmetic, field splitting, pathname expansion, quote removal, positional
   and special parameters, assignments, and environment export.
7. Implement special/regular/intrinsic builtin classification, command lookup,
   error/exit rules, traps, signals, waits, jobs, and interactive job control.
8. Treat Issue 8 dollar-single-quote syntax and `pipefail` as standard work.
9. Preserve libedit only for interactive input and prove it does not alter
   scripts, EOF, SIGINT, or diagnostics.
10. Provide strict POSIX mode plus a documented extension whitelist.  Initial
    candidates are `[[ ]]`, arithmetic commands/for, brace expansion, `local`,
    `source`, `function`, `&>`, and global parameter replacement.  Arrays,
    process substitution, coprocesses, programmable completion, and `shopt`
    are outside the required scope unless separately approved.

### Gates

- independent local host tests mapped to the Issue 8 shell sections;
- strict-mode and extension-mode separation tests;
- malformed grammar, expansion/resource bounds, redirection failures,
  short/broken I/O, signals, and job-control tests;
- QEMU non-interactive scripts, interactive libedit, tty/job control, cron
  command execution, and login-shell scenarios;
- no boot or shutdown dependency on shell internals;
- `make -j16` passes and `SHELL-CORE-01` is updated conservatively.

### Partial-success rule

A newly discovered POSIX requirement omitted from this scope may be handed to
the master when the shell remains safe and the claimed status is reduced
accordingly.  A parser/executor defect in a behavior explicitly listed above
must be repaired before this Phase is complete.

## Phase 19: integrated QEMU acceptance and repair

### Scope

Run the installed system, not host substitutes, through an end-to-end bounded
`qemu-system-x86_64` acceptance suite:

1. kernel to `/sbin/init`, hostname, fstab, runtime directories, and service
   ordering;
2. syslogd, early/live kernel logging, logger, persistence, and failure paths;
3. getty, authentication/test login, interactive shell, logout, and respawn;
4. static networking, DHCP acquisition, `net`, degraded networking, and
   shutdown;
5. cron, at, batch, shell execution, credentials, output spool, and recovery;
6. optional ntpdate success and timeout;
7. service list/status/start/stop/restart/enable/disable/reload;
8. daemon crash/restart/crash-loop, malformed configuration, dependency cycle,
   required/optional failures, and stop timeout;
9. orderly halt, poweroff, reboot, reverse stopping, sync, and unmount; and
10. persistence of policy and durable state across reboot.

Repair every defect that prevents the documented minimum system from booting,
reaching a usable console, performing the implemented service operations, or
shutting down safely.  Repeat the affected focused and QEMU tests after each
repair and finish with `make -j16`, formatting, provenance/matrix checks, and
`git diff --check`.

### Acceptance result

Phase 19 is complete when the minimum system operates through the scenarios
above.  If integration reveals an unplanned POSIX incompatibility, choose the
safest of:

- implement it when bounded and within the existing architecture;
- retain an honest, non-success stub while preserving minimum operation; or
- declare the affected Phase/row partially successful.

In the latter two cases, record the exact behavior, impact, evidence, and next
implementation unit in `docs/posix-compliance-master.md`.  Never weaken the
test, hide the limitation, or call a stub conforming.

## Plan-wide command policy

- Build: `make -j16`.
- Runtime evidence: `qemu-system-x86_64` with headless, bounded, explicit
  completion/failure markers.
- Do not use aggregate `make check`.
- Format every new/changed userland C/header file with clang-format and verify
  formatting.
- Do not import external source into `userland/base`.
- Do not commit.
