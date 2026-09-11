<!-- awesome-plan project=zedbsd record=ws001 -->

## 登録済みの子Issue

- [ws001-p000](https://github.com/awemorris/zedBSD/issues/28)
- [ws001-p001](https://github.com/awemorris/zedBSD/issues/29)
- [ws001-p002](https://github.com/awemorris/zedBSD/issues/30)
- [ws001-p003](https://github.com/awemorris/zedBSD/issues/31)
- [ws001-p004](https://github.com/awemorris/zedBSD/issues/32)
- [ws001-p005](https://github.com/awemorris/zedBSD/issues/33)
- [ws001-p006](https://github.com/awemorris/zedBSD/issues/34)
- [ws001-p007](https://github.com/awemorris/zedBSD/issues/35)
- [ws001-p008](https://github.com/awemorris/zedBSD/issues/36)
- [ws001-p009](https://github.com/awemorris/zedBSD/issues/37)
- [ws001-p010](https://github.com/awemorris/zedBSD/issues/38)
- [ws001-p011](https://github.com/awemorris/zedBSD/issues/39)
- [ws001-p012](https://github.com/awemorris/zedBSD/issues/40)
- [ws001-p013](https://github.com/awemorris/zedBSD/issues/41)
- [ws001-p014](https://github.com/awemorris/zedBSD/issues/42)
- [ws001-p015](https://github.com/awemorris/zedBSD/issues/43)
- [ws001-p016](https://github.com/awemorris/zedBSD/issues/44)
- [ws001-p017](https://github.com/awemorris/zedBSD/issues/45)
- [ws001-p018](https://github.com/awemorris/zedBSD/issues/46)
- [ws001-p019](https://github.com/awemorris/zedBSD/issues/47)
- [ws001-p020](https://github.com/awemorris/zedBSD/issues/48)
- [ws001-p021](https://github.com/awemorris/zedBSD/issues/49)
- [ws001-p022](https://github.com/awemorris/zedBSD/issues/50)
- [ws001-p023](https://github.com/awemorris/zedBSD/issues/51)
- [ws001-p085](https://github.com/awemorris/zedBSD/issues/52)


既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws001/ws.md`

親: [master](https://github.com/awemorris/zedBSD/issues/1)

# WS001: POSIX.1-2024 compliance

Last updated: 2026-09-01

WSID: `ws001`

Status: in progress; compliance ledger remains active

Parent: [master plan](https://github.com/awemorris/zedBSD/issues/1)

Last verified Phase: `ws001-p023`

Resume point: q050 completed canonical p022/p023; completed WS005 consumed
their released VFS dependencies. The POSIX compliance ledger remains active;
select the next unresolved p020 audit item while completed p021 and p022/p023
remain regression inputs. Concurrent q042 source and focused-host
milestones originally used the colliding pre-merge identifiers p015 and p016;
their active Phase IDs are `ws001-p022` and `ws001-p023`.

Shared tests: [WS001 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/tests/README.md)

## Phase registry

| Combined ID | Phase | Status | Result |
| --- | --- | --- | --- |
| `ws001-p000` | [inventory and gates](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase000-inventory/phase.md) | Complete | Protected the inventory and acceptance gates |
| `ws001-p001` | [temporary failure commands](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase001-failure-commands/phase.md) | Complete | Installed deliberate provider-missing commands |
| `ws001-p002` | [low-dependency utilities](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase002-low-dependency/phase.md) | Complete | Added low-dependency commands and shell builtins |
| `ws001-p003` | [locale and terminal descriptions](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase003-locale/phase.md) | Complete | Added locale catalogs and terminal-description foundations |
| `ws001-p004` | [parsers and archives](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase004-parsers/phase.md) | Complete | Implemented the planned parser/editor/traversal/archive utilities |
| `ws001-p005` | [process and IPC tools](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase005-process-ipc/phase.md) | Complete | Added process, credential, and System V IPC tools |
| `ws001-p006` | [development utilities](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase006-development/phase.md) | Complete | Added the selected development utilities |
| `ws001-p007` | [compression utilities](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase007-compression/phase.md) | Complete | Added compression utilities and tests |
| `ws001-p008` | [SCCS suite](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase008-sccs/phase.md) | Complete | Added the planned SCCS subset |
| `ws001-p085` | [terminfo and base builds](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase085-terminfo/phase.md) | Complete | Legacy Phase 8.5: terminal packages and standalone builds |
| `ws001-p009` | [POSIX utility audit](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase009-audit/phase.md) | Complete as audit | Findings remain in this ledger |
| `ws001-p010` | [local reimplementation](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase010-local-reimplementation/phase.md) | Complete milestone | Imported `bc`, `ed`, and `m4` were replaced locally |
| `ws001-p011` | [bounded basename correction](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase011-basename/phase.md) | Complete milestone | Host semantics/failure test and native amd64 build pass; runtime conformance handoff remains |
| `ws001-p012` | [bounded dirname correction](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase012-dirname/phase.md) | Complete milestone | Host lexical/failure suite and native amd64 build pass; runtime/locale handoff remains |
| `ws001-p013` | [bounded link/unlink correction](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase013-link-unlink/phase.md) | Complete | Host identity/failure suite and native amd64 build pass; broad filesystem matrix remains |
| `ws001-p014` | [shell foreground job-control synchronization](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase014-shell-job-control/phase.md) | Complete (`q023`, 2026-08-28) | Foreground pipelines gate every member until TTY handoff; `fg` hands off before `SIGCONT`; background/non-TTY and cleanup regressions pass |
| `ws001-p015` | [base C coding-style adoption](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase015-base-c-style-adoption/phase.md) | Complete (`agent2-q001`, 2026-08-31) | New/refactored base source has an executable style gate; the final inventory is 7 compliant and 234 historical files |
| `ws001-p016` | [direct PDF printing over LPD](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase016-direct-lpd-printing/phase.md) | Complete implementation milestone (`agent2-q001`, 2026-08-31) | Native PDF-only `lp`/`lpr` and fake-LPD protocol matrix pass; POSIX text/`-w` and guest network submission remain handoffs |
| `ws001-p017` | [bounded cmp conformance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase017-cmp-conformance/phase.md) | Complete implementation milestone (`agent2-q001`, 2026-08-31) | `-l`/`-s`, skip extension, independent short reads, output formats, and exit classes pass focused tests |
| `ws001-p018` | [bounded tee conformance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase018-tee-conformance/phase.md) | Complete implementation milestone (`agent2-q001`, 2026-08-31) | `-i`, dynamic outputs, robust writes, failure continuation, and statuses pass focused tests |
| `ws001-p019` | [canonical userland source headers](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase019-userland-file-headers/phase.md) | Complete (`agent2-q002`, 2026-08-31) | All 269 userland C-family files have the exact section 13 block and a separate explanation; body hashes, fixtures, assembler preprocessing, full build, and whitespace checks pass |
| `ws001-p020` | [complete userland C-style conformance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase020-complete-userland-c-style/phase.md) | Uncleared (`agent2-q003`, 2026-08-31) | All 2,454 functions pass public/static order, prototype, header-layout, comment, loop/switch, case-label, and build gates; the body audit records 731 mechanical residuals plus semantic-review handoffs in the 258-row ledger |
| `ws001-p021` | [ANSI C declarations and semantic layout](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase021-ansi-c-semantic-layout/phase.md) | Complete (`agent2-q006`, 2026-08-31) | All 214 implementations pass ANSI declaration, semantic paragraph, symmetric brace, loop block, entry spacing, indentation, build, automated audit, and user manual-review gates |
| `ws001-p022` | [credential-aware VFS object creation](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase022-credential-aware-vfs-creation/phase.md) | Complete (`q050`, 2026-09-01) | Production-linked UFS/overlay rollback faults, root/non-root backend matrix, and abrupt-stop/reopen/remount acceptance pass; tmpfs double link increment fixed |
| `ws001-p023` | [truthful and durable directory fsync](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase023-directory-fsync/phase.md) | Complete (`q050`, 2026-09-01) | VFS/UFS/overlay ordering and mutation gates plus five-launch abrupt-stop/remount durability pass; FAT/tmpfs directory sync stays explicitly unsupported |

### q042 pre-merge identifier migration

Historical q041/q042 documents and test output labels retain the original
`ws001-p015` credential-creation and `ws001-p016` directory-fsync names.  In
the active registry and all current dependencies, those records map to
`ws001-p022` and `ws001-p023`, respectively. Runtime `--path` is proven and
q050 completed their fault-injection and disposable-image/remount acceptance,
releasing both VFS dependencies of `ws005-p005`.

Original combined planning context is retained in the
[legacy Phase 0–10 plan](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/history/phase000-010-legacy-plan.md).

## Goals

- Provide a clearly declared POSIX.1-2024/SUS compatibility profile for the
  zedBSD base system.
- Implement required kernel, libc, shell, and utility behavior using local base
  source.
- Keep every known incompatibility visible until it is tested and resolved or
  explicitly excluded from the selected profile.

## WS completion conditions

WS001 is complete when every requirement in the declared profile is either
`reviewed` with repeatable evidence or explicitly marked non-applicable by a
documented profile decision; the utility/API matrices and this ledger agree;
and the full declared POSIX test set passes on the supported zedBSD targets.

## 1. Project objective

The POSIX compliance project incrementally brings the documented zedBSD
conformance environment to POSIX.1-2024 (Issue 8).  It covers kernel behavior,
system calls and private kernel interfaces, libc, locale and terminal data,
shell semantics, user utilities, packages, services, and executable evidence.

The project is evidence-driven.  A name in `/bin`, a successful build, a smoke
test, or a partially working implementation is useful progress but is not by
itself a conformance result.  An item becomes complete only when its required
semantics and failure behavior have been reviewed against Issue 8 and are
covered by repeatable tests on every relevant layer.

The project is deliberately iterative.  An unmet requirement shall be recorded
here and handed off rather than hidden, silently ignored, or forced into a
fragile implementation.  A safe partial implementation may remain pending for
multiple iterations.  Unsupported behavior must fail honestly, and progress
must never be manufactured by weakening a correct test or raising a status
without evidence.

No external implementation shall be imported into `userland/base`.  Official
standards and other documentation may be used to understand behavior, but
production source, generated parsers, implementation-specific tables,
compatibility ports, and copied upstream tests are outside the base policy.

## 2. Scope and sources of truth

This document is the project-level tracker.  It connects requirements that
cannot be represented by a utility-only CSV and records the current hand-off
state across components.

| Artifact | Authority |
|---|---|
| `tests/posix-2024-utilities.csv` (retired legacy artifact) | The removed machine-readable inventory was absorbed into this ledger; new shared fixtures must be copied into this WS `tests/` directory |
| this master | project objectives, cross-component dependencies, subsystem/API progress, embedded unmet utility work, completed Phase 0--10 results, and the backlog from which future phases are created |
| [`ws001-p009`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase009-audit/phase.md) | detailed evidence and rationale from the 2026-08-24 first audit pass |
| [`ws001-p010`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase010-local-reimplementation/phase.md) | detailed removal and local reimplementation design for `bc`, `ed`, and `m4` |
| [legacy Phase 0–10 plan](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/history/phase000-010-legacy-plan.md) | historical execution plan and phase acceptance policy |
| [WS002](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/ws.md) | completed post-Phase-10 service architecture and Phase 11–20 baseline |
| [`ws002-p020`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/phase020-network-service/phase.md) | completed synchronous net-service implementation milestone and its handoffs |
| Open Group Issue 8 pages | normative behavior to be reviewed; repository documents and tests do not replace the standard |

When these artifacts disagree, do not choose the more optimistic status.
Reconcile the discrepancy and retain the least-complete defensible state until
the implementation and evidence agree.

The kernel and API registers below currently contain dependencies discovered by
the Shell and Utilities work.  They are not yet a complete audit of the POSIX
Base Definitions and System Interfaces volumes.  Building that complete public
header/function/interface inventory is itself an open master item; new rows
shall be added rather than treating an unlisted interface as reviewed.

## 3. Status vocabulary

The utility CSV retains its existing status names.  This master uses the
following broader component states:

| State | Meaning |
|---|---|
| `reviewed` | applicable Issue 8 behavior and failures have executable evidence |
| `implemented-unreviewed` | useful implementation exists, but the full contract has not passed review |
| `implementation extension` | tested zedBSD-specific interface outside POSIX/SUS; retained for compatibility/security but not counted as conformance progress |
| `partial` | a required interface or semantic area is known to be incomplete |
| `missing` | no usable implementation exists |
| `policy-conflict` | implementation exists but violates the no-external-source policy |
| `deferred-provider` | command/API is intentionally waiting for a service or provider and remains a blocker |
| `disabled-profile` | requirement is outside the currently enabled option profile |
| `planned` | work is designed but implementation has not begun |
| `blocked` | work cannot proceed without an explicit decision or unavailable dependency; the blocker must be named |

Progress fractions count explicit gates, not estimated effort.  For example,
Phase 10 uses five replacement gates: imported files removed, local source
builds, host test passes, standalone/top-level build passes, and QEMU test
passes.  `5/5` replacement gates still does not mean full POSIX conformance.

## 4. Current dashboard

### 4.1 Utility inventory

| Measure | Current value | Meaning |
|---|---:|---|
| matrix rows | 155 | complete profile inventory |
| `reviewed` | 19 | utility-level review gate passed |
| `implemented-unreviewed` | 115 | includes the new local `at`, `batch`, `crontab`, and `logger` implementations |
| `deferred-stub` | 1 | remaining service/provider blocker: `mailx` |
| `option-disabled` | 20 | outside the selected option profile |
| historical Phase 9 P0 findings | 3 | the imported `bc`, `ed`, and `m4` findings were resolved by Phase 10 on 2026-08-24 |
| current policy conflicts | 0 | the declared `userland/base` provenance gate rejects the removed imported trees and fingerprints |
| current P1 known incompatibilities | 75 | the prior 77 minus the bounded `cmp` and `tee` incompatibilities closed by `agent2-q001`; full reviews remain open |
| Phase 9 P2 incomplete proof | 38 | no confirmed complete review; targeted evidence is missing |
| rows promoted by Phase 9 | 0 | no pending row satisfied the review checklist |

The advertised values remain `_POSIX2_VERSION=200809L` and
`_XOPEN_VERSION=700`.  They shall not be raised while required utilities,
providers, services, or cross-component semantics remain pending.

### 4.2 Phase progress

| Phase | State | Progress and hand-off |
|---|---|---|
| 0 | completed gate | matrix checks, deferred-stub state, evidence checks, and formatting policy established |
| 1 | implementation milestone complete | failure-only service commands installed; providers remain pending |
| 2 | implementation milestone complete | low-dependency tools and shell additions built/tested; Phase 9 findings remain |
| 3 | implementation milestone complete | locale/catalog/terminal foundation built; full locale/terminal compliance remains |
| 4 | policy correction complete | Phase 10 removed the imported `bc`, `ed`, and `m4`; the smaller local replacements remain non-conforming |
| 5 | implementation milestone complete | process/file-use/SysV IPC interfaces work in QEMU; full semantic review remains |
| 6 | implementation milestone complete | development utilities exist; parser/format/option review remains |
| 7 | implementation milestone complete | `.Z` compression path exists; Issue 8 and failure-path review remains |
| 8 | implementation milestone complete | SCCS local format exists; classic interoperability and option coverage remain |
| 8.5 | implementation milestone complete | standalone packages and terminal stack exist; consumer/format review remains |
| 9 | first audit pass complete | 111/111 pending rows inspected; remediation and conformance closure remain 0/111 |
| 10 | replacement milestone complete | local `bc`, `ed`, and `m4` pass provenance, host, standalone, `make -j16`, and amd64 QEMU gates; POSIX completion remains open |
| 11 | implementation milestone complete | administrative commands install in `/sbin`; the package interface accepts `PREFIX` and per-package install destinations |
| 12 | implementation milestone complete | native PID 1, `rc.conf`, service definitions/client, ordered startup, supervision, and PID-1-only power control boot in QEMU |
| 13 | implementation milestone complete | local libc syslog transport, `logger`, and `syslogd` deliver to `/var/log/messages`; live kernel streaming remains open |
| 14 | partial | getty acquires the console; the explicitly passwordless root account authenticates and starts `/bin/sh` in production QEMU; logout/utmpx/respawn evidence remains open |
| 15 | implementation milestone complete | kernel `lo0`, static networkd configuration, UNIX control, and `net` up/down pass QEMU; integrated DHCP/Wi-Fi remain open |
| 16 | partial | local durable at/crontab spools and at execution pass QEMU; full time/cron grammar, batch policy, periodic-job evidence, and output delivery remain |
| 17 | partial | bounded local `ntpdate` client exists and is boot-optional; controlled-server QEMU success/failure evidence remains open |
| 18 | partial | zedBSD power/administration behavior and `/etc/zinit.rc` were removed and `sh -c` works; the required POSIX grammar and semantic inventory is substantially incomplete |
| 19 | partial integration success | bounded amd64 QEMU service smoke passes through orderly poweroff initiation; the explicit hand-offs in Section 11 prevent a full integration claim |
| 20 | implementation milestone complete | focused host tests, `make -j16`, and four-CPU amd64 QEMU prove FD 3 readiness, synchronous `/sbin/net boot`, a real NE2000 DHCP lease through one-shot `dhcpc`, route/DNS installation, restart, direct-ifconfig recovery, interactive shell recovery after `ifconfig`, and shutdown |
| ws001-p011 | implementation milestone complete | `basename` empty/slash/suffix/`--`/stdout behavior corrected; focused host test and native amd64 ELF validation pass |

The Phase 0--10 series is closed as completed on 2026-08-24.  Here,
"completed" means that each phase's defined implementation, audit, or
replacement milestone was executed and its remaining work was handed off; it
does not mean that every affected component is POSIX conforming.  Phases
11--19 were defined on 2026-08-25 by the active system-services roadmap and
detailed execution plan.  They implement a single native init/service model,
logging, console sessions, networking, scheduled work, optional initial time,
POSIX shell remediation, and integrated QEMU acceptance.  Any incomplete
standards behavior discovered by those phases remains governed by this
master's conservative hand-off policy.

Phase 20 was selected from the Phase 15/19 networking and service-readiness
hand-offs and completed its implementation milestone on 2026-08-25.  All
focused host/build/QEMU gates in
[`ws002-p020`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/phase020-network-service/phase.md) passes. This narrows the
networking hand-offs but does not claim DHCP renewal, Wi-Fi, IPv6, exhaustive
startup-failure injection, or POSIX conformance.

## 5. Kernel subsystem tracker

These rows track the kernel-level capability needed by utilities.  A kernel row
may be implemented while its consuming utility remains non-conforming.

| ID | Subsystem | State | Consumers | Unmet requirement / next evidence |
|---|---|---|---|---|
| KERN-PROC-01 | process snapshot | implemented-unreviewed | `ps`, `top` | versioned snapshot works in amd64 QEMU; prove default/permission/race semantics and expose every field needed for POSIX/XSI `ps` |
| KERN-FILE-01 | file-use query | implemented-unreviewed | `fuser` | inode/mount/socket references work; verify all reference kinds, mount/block-device behavior, permissions, races, and multiple targets |
| KERN-IPC-01 | System V message queues | implemented-unreviewed | `ipcrm`, `ipcs` | create/stat/remove works; verify ownership, permissions, limits, stale IDs, races, enumeration, and error status |
| KERN-IPC-02 | System V semaphores | implemented-unreviewed | `ipcrm`, `ipcs` | create/stat/remove works; verify arrays/operations, undo/lifecycle semantics, limits, ownership, and concurrent removal |
| KERN-IPC-03 | System V shared memory | implemented-unreviewed | `ipcrm`, `ipcs` | create/attach/stat/remove path exists; verify attachment lifecycle, permissions, limits, stale IDs, and removal races |
| KERN-CRED-01 | credentials and process identity | partial | `id`, `chown`, `chgrp`, `newgrp`, `ps` | q050 proves effective-credential ownership before UFS1/UFS2/tmpfs/overlay publication, FAT representability rejection, set-GID inheritance, and safe read-only quarantine when rollback cleanup itself fails; broader real/effective IDs, supplementary groups, set-ID transitions, permission checks, and account-database integration remain |
| KERN-SIG-01 | signals and process groups | partial | `kill`, `sh`, `time`, `wait` | basic signaling works; prove process-group targets, job-control delivery, stopped/continued children, saved statuses, interruption, and permissions |
| KERN-WAIT-01 | child wait and accounting | partial | `wait`, `time`, `sh` | basic `waitpid()` works; multiple saved statuses, non-child behavior, signal status, stopped jobs, and user/system CPU accounting remain; missing-login exit/reap invalid-free remains tracked by [`ws002-p021`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/phase021-missing-login-session-teardown/phase.md) |
| KERN-TTY-01 | tty line discipline and termios | partial | `stty`, `sh`, `mesg`, `tty`, `newgrp` | canonical/raw and common flags exist; audit all required flags, speeds, control characters, VMIN/VTIME, drains/flushes, signals, and error atomicity |
| KERN-PTY-01 | pseudo terminals and controlling tty | implemented-unreviewed | shell/job control, terminal tests | UNIX98-style PTY path exists; prove session/controlling-terminal acquisition, foreground groups, hangup, permissions, and lifecycle; missing-login exit/reap invalid-free remains tracked by [`ws002-p021`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/phase021-missing-login-session-teardown/phase.md) |
| KERN-CLOCK-01 | clocks and clock setting | partial | `date`, `touch`, libc time | `clock_settime()` exists; prove privilege checks, valid ranges, clock selection, timezone-facing behavior, interruption, and filesystem timestamp integration |
| KERN-VFS-01 | pathname, metadata, and traversal semantics | partial | file utilities | q050 proves credential-aware object creation/rollback and truthful UFS1/UFS2/overlay directory `fsync`, with FAT/tmpfs directory sync explicitly `EOPNOTSUPP`; recursive symlink policies, mount boundaries, broader hard-link/metadata races, and family-wide error semantics remain |
| KERN-FSSTAT-01 | filesystem capacity/accounting | partial | `df`, `du` | provide and verify stable filesystem/device identity, portable block accounting, mount lookup, overflow behavior, and permission/error cases |
| KERN-RSRC-01 | priorities | reviewed | `nice`, `renice` | declared current scope has reviewed utility evidence; keep regression and permission/range tests |
| KERN-RSRC-02 | resource limits | reviewed | `ulimit`, shell | declared current scope has reviewed utility evidence; expand when new limit classes are exposed |
| KERN-BOOT-01 | init/service lifecycle | implemented-unreviewed | `/sbin/init`, service providers | native PID 1 boots and initiates ordered shutdown in QEMU; complete crash-loop, required-failure, stop-timeout, cycle, credential, and recovery evidence; missing-login exit/reap invalid-free remains tracked by [`ws002-p021`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/phase021-missing-login-session-teardown/phase.md) |
| KERN-NET-01 | loopback and interface control | implemented-unreviewed | `networkd`, `net`, socket users | four-CPU QEMU proves NE2000 receive/transmit, a real DHCP lease, default route, DNS, static `lo0`, up/down, and dp8390 SMP serialization; counters, aliases, IPv6, broader NICs, stress/race coverage, and full ioctl review remain |
| KERN-NET-02 | AF_UNIX peer identity | implementation extension | `networkd`, local control protocols | `SO_PEERCRED` returns one immutable connection-time 12-byte `zedbsd_peercred` snapshot for connected AF_UNIX streams; this is a zedBSD extension, not a POSIX/SUS conformance interface, and its pathname/socketpair/SCM_RIGHTS lifecycle evidence is owned by `ws005-p003` |
| KERN-POLL-01 | UNIX listener readiness | partial | `init`, `networkd` | listener `poll()` did not wake reliably after a queued AF_UNIX stream connection in Phase 19; daemons use a bounded one-second nonblocking accept loop pending a focused kernel repair |

## 6. System call and kernel-interface tracker

Private ioctls are included because they are part of the current implementation
dependency even when they are not POSIX public APIs.

| ID | API/interface | State | Implementation evidence | Unmet work |
|---|---|---|---|---|
| API-AUDIT-00 | complete POSIX public interface inventory | missing | utility-driven dependency audit only | enumerate every required header, type, constant, function, and semantic option from Base Definitions/System Interfaces, then add stable API rows and tests |
| API-MMAP-01 | `mmap()` fixed-address replacement | partial | a `MAP_FIXED` syscall/VM branch exists, but the public `mmap()` flag mask rejects `MAP_FIXED` before that branch | admit the public flag only after replacement semantics are defined; prove overlapping replacement, failure atomicity, exact alignment/protection/backing errors, and production guest evidence |
| API-CLOCK-01 | `clock_settime()` | implemented-unreviewed | `ZEDBSD_SYS_clock_settime`, `kern_clock_settime()` | range/privilege/error/QEMU cases and `date` setting operands |
| API-RSRC-01 | `getpriority()`, `setpriority()`, `nice()` | reviewed | priority syscalls and reviewed `nice`/`renice` rows | retain regression across user/process-group selectors and permissions |
| API-RSRC-02 | `getrlimit()`, `setrlimit()` | reviewed | resource-limit syscalls and reviewed `ulimit` row | retain current-shell inheritance and hard/soft-limit regression |
| API-CONF-01 | `sysconf()`, `pathconf()`, `fpathconf()`, `confstr()` | reviewed | reviewed `getconf` row for the declared mapping scope | update generated mapping when public constants or limits change; API-AUDIT-00 may expand the scope |
| API-IPC-01 | `msgctl()` family | implemented-unreviewed | kernel IPC plus `libc/sysv-ipc.c` | full command, permission, limit, removal, and malformed-ID review |
| API-IPC-02 | `semctl()` family | implemented-unreviewed | kernel IPC plus `libc/sysv-ipc.c` | operation/array/undo semantics and concurrent lifecycle review |
| API-IPC-03 | `shmctl()` family | implemented-unreviewed | kernel IPC plus `libc/sysv-ipc.c` | attach/remove lifecycle, permissions, limits, and enumeration review |
| API-NET-01 | `SO_PEERCRED`, `struct zedbsd_peercred` | implementation extension | fixed 12-byte PID/EUID/EGID ABI, connection-time AF_UNIX snapshot, and `ws005-p003` guest fixture | Explicitly non-POSIX/non-SUS; retain ABI layout, short-buffer atomicity, descriptor-transfer identity, and unconnected/non-AF_UNIX error regressions without counting this row toward POSIX conformance |
| API-SYSTEM-01 | process-snapshot system-device ioctl | implemented-unreviewed | `include/uapi/zedbsd/system.h`, `src/kern/system-device.c` | ABI evolution rules, race-consistent snapshots, permissions, all `ps` fields |
| API-SYSTEM-02 | file-usage system-device ioctl | implemented-unreviewed | `SYSTEM_IOC_FILE_USAGE`, `system_process_file_usage()` | all reference flags, path races, mount/socket cases, permissions, bounded output |
| API-TTY-01 | `TCGETS`, `TCSETS*`, termios libc API | partial | tty ioctl implementation and libc declarations | complete attribute/speed/control-character semantics, drain/flush/interruption tests |
| API-UTMPX-01 | `getutx*()`, `pututxline()` | partial | libc utmpx API, `write` and `who` consumers | session producer ownership, locking/atomicity, stale records, boot/login/logout lifecycle |

## 7. libc and shared-component tracker

| ID | Component | State | Consumers | Unmet requirement / next work |
|---|---|---|---|---|
| LIBC-LOCALE-01 | locale objects and artifact reader | partial | `locale`, `localedef`, text utilities | complete categories, environment precedence, keyword metadata, portability, malformed artifacts, and runtime switching |
| LIBC-COLLATE-01 | collation | partial | `sort`, `comm`, `ls`, `join`, regex consumers | implement and prove locale-defined collation, equivalence, ranges, stable ordering, and invalid data handling |
| LIBC-CTYPE-01 | multibyte and display-width behavior | partial | `cut`, `fold`, `expand`, `unexpand`, `wc`, `strings` | complete decoding/state/error rules and column-width behavior across buffer boundaries |
| LIBC-ICONV-01 | character conversion | partial | `iconv`, locale tools | UTF-8 validation exists; implement actual conversion pairs, aliases, stateful encodings, `-c`/`-s`, and streaming errors |
| LIBC-REGEX-01 | BRE/ERE engine | implemented-unreviewed | `awk`, `ed`, `find`, `grep`, `sed` | utility-level grammar integration, locale/collation, empty expressions, backreferences, limits, and malformed-input fuzzing |
| LIBC-STDIO-01 | robust stream I/O | partial | most utilities | standardize short read/write, `EINTR`, broken stdout, close/flush errors, and accumulated exit status |
| LIBC-ALLOC-01 | allocation/resource failure discipline | partial | parsers and recursive tools | add fault injection and checked size/growth paths; prohibit silent truncation and success after `ENOMEM` |
| LIBC-ACCT-01 | passwd/group lookup and group membership | partial | `id`, `chown`, `chgrp`, `newgrp`, `ps` | names, supplementary groups, reentrant/error behavior, missing records, and credential transition tests |
| LIBC-UTMPX-01 | session database | partial | `write`, `who`, login/service work | establish producer/lifecycle model, locking, corruption handling, stale tty cleanup, and time semantics |
| TERM-DB-01 | terminfo database and checked reader | implemented-unreviewed | `tabs`, `tput`, curses | broaden standard capability/parameter semantics, malformed data, aliases, install compatibility, and output failures |
| TERM-CURSES-01 | curses library | implemented-unreviewed | future full-screen programs | expand window/input/update semantics and define the POSIX/XSI scope before any conformance claim |
| ARCHIVE-01 | archive/ELF shared readers | implemented-unreviewed | `ar`, `nm`, `pax` | standard format variants, malformed data, overflow, metadata, symbol tables, non-ELF policy, and fuzz evidence |
| SCCS-CORE-01 | SCCS history/p-file/locking core | partial | ten SCCS commands | classic weave/control interoperability, full flags/MRs/SIDs, preservation, stale locks, interrupted atomic updates |
| SHELL-CORE-01 | shell lexer/parser/expansion/executor | partial | `sh`, cron, login | `sh -c`, simple lists/pipelines/expansion and scripts work; `ws012-p006` gates a single foreground external, and [`ws001-p014`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase014-shell-job-control/phase.md) gates every foreground-pipeline member until TTY handoff and makes `fg` hand off before `SIGCONT`; reserved words, multiline continuation, compound commands, functions, grouping/subshell grammar, here-documents, full redirects/expansions, strict mode, `ENV`, multiple-job selection, complete jobs/traps, and special-builtin semantics remain |
| BUILD-PKG-01 | standalone base package interface | implemented-unreviewed | all base packages | retain direct build/install coverage for source lists, libraries, headers, data-only packages, `PREFIX=/`, and ordinary prefixes |
| BUILD-PROV-01 | base source provenance gate | reviewed | `bc`, `ed`, `m4` replacement scope | `make phase10-local-source-check` enforces exact local manifests, Zlib headers, removed-file references, and known external fingerprints; extend the manifest when future source is added |

## 8. Service and provider tracker

| ID | Facility/command | State | Current behavior | Completion requirement |
|---|---|---|---|---|
| SVC-SCHED-01 | `at`, `batch`, `crontab`, `cron` | partial | local durable spools; QEMU proves at execution and crontab persistence | complete POSIX at time grammar, queue policy, batch load gating, cron ranges/lists/steps/environment, periodic-job QEMU evidence, locking/races, and mail/output delivery |
| SVC-LOG-01 | `logger` and system logging | implemented-unreviewed | `/run/log` datagrams reach local `syslogd` and `/var/log/messages` in QEMU | permissions/backpressure/rotation/storage failure, facility policy, live kernel stream, and durable boot-log review |
| SVC-MAIL-01 | `mailx` | deferred-provider | explicit failure command | required mail provider and Send Mode; Receive Mode for enabled XSI/UP environment |
| SVC-PRINT-01 | `lp`, `lpr` | partial | `ws001-p016` supplies direct PDF-over-LPD submission, no persistent spool, and host protocol/failure evidence | Issue 8 text input, `-w`, unspecified default destination, multi-file request semantics, timeout/early-close injection, and guest/physical-printer submission remain |
| SVC-TALK-01 | `talk` | disabled-profile | installed failure command | local rendezvous provider and service only if UP/XSI profile is enabled |
| SVC-INIT-01 | PID 1 and service manager | implemented-unreviewed | native `/sbin/init`, `/sbin/service`, `/etc/rc.conf`, and `/etc/service.d`; Phase 20 adds explicit `after`/`requires`, startup states, and FD 3 readiness, with networkd restart and orderly shutdown passing QEMU | prove crash loops, cycles, required/optional failures, malformed reload, stop timeout, persistence, scheduled-work restart, and the remaining shutdown actions |
| SVC-NOTIFY-01 | daemon startup readiness | implemented-unreviewed | private FD 3 READY/FAIL protocol, bounded timeout/parser, descriptor hygiene, terminal startup states, dependency propagation, and service status are implemented; QEMU proves networkd READY before `net boot` | add runtime fault injection for fragmented/malformed/duplicate/oversized records, FAIL, premature exit, timeout, and every descriptor-leak/restart edge |
| SVC-GETTY-01 | getty/login session | partial | production QEMU accepts the explicitly passwordless root account and starts `/bin/sh` in `/root` without daemon churn | prove utmpx transitions, logout, hangup, getty respawn, locked-account rejection, and hashed-password authentication; missing-login exit/reap invalid-free remains tracked by [`ws002-p021`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/phase021-missing-login-session-teardown/phase.md) |
| SVC-NET-01 | networkd/net | partial | synchronous `net boot` and lightweight networkd orchestrate local ifconfig/route/dhcpc; four-CPU QEMU proves NE2000 DHCP address/default route/DNS, restart, up/down, and direct-ifconfig recovery | DHCP renewal, Wi-Fi, IPv6, unprivileged reads, broad link events, resolver failure policy, and remaining failure/recovery evidence stay later work |
| SVC-TIME-01 | ntpdate | partial | local bounded NTPv4 client, disabled by default | controlled QEMU server, malformed/spoofed/unreachable cases, DNS timeout, clock privilege/error evidence; periodic `ntpd` remains a later project |

## 9. Cross-cutting unmet work

| ID | Area | State | Affected scope | Required evidence |
|---|---|---|---|---|
| CROSS-IO-01 | short reads/writes and `EINTR` | partial | stream, archive, filesystem, parser utilities | reusable host fault shim plus pipe/device/QEMU cases |
| CROSS-OUT-01 | broken stdout and close/flush errors | partial | every output-producing utility | exact non-zero status and no false success after partial output |
| CROSS-MEM-01 | allocation/size overflow | partial | dynamic arrays, parsers, recursion, binary formats | deterministic allocation injection, checked arithmetic, bounded nesting/input |
| CROSS-FS-01 | filesystem failures and atomic replacement | partial | editors, archives, SCCS, copy/move, generated databases | q050 adds production-linked allocation/publication/cleanup faults, cleanup-error precedence, rollback quarantine, directory-sync ordering, and abrupt-stop namespace survival for the bounded credential/VFS matrix; broader permissions, full-disk, interruption, utility-specific rollback, and destination-integrity evidence remain |
| CROSS-LOCALE-01 | locale/collation/multibyte | partial | most text and display utilities | non-C locale fixtures, invalid artifacts/sequences, boundary-split input, output verification |
| CROSS-SHELL-01 | current-shell state | partial | shell builtins | tests inside a running zshell for environment, cwd, umask, limits, traps, descriptors, and jobs; completed [`ws001-p014`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase014-shell-job-control/phase.md) supplies direct/pipeline/Ctrl-Z/`fg`/background/non-TTY job-control regression |
| CROSS-BINARY-01 | malformed binary formats | partial | locale/catalog/terminfo/archive/ELF/SCCS/compression | truncation, invalid offsets/counts, integer overflow, fuzz corpus, bounded failure |
| CROSS-QEMU-01 | zedBSD runtime evidence | implemented-unreviewed | kernel-, tty-, credential-, IPC-, process-, service-dependent behavior | q050 adds five bounded amd64 launches covering overlay and native UFS1 abrupt-stop/relaunch, external journaled UFS2, tmpfs, FAT rejection/remount, and frozen-source integrity; continue adding target-specific cells whenever host behavior is insufficient |
| CROSS-PROV-01 | external source exclusion | reviewed | Phase 10 `bc`, `ed`, `m4` scope | imported production/generated trees and the m4 host compatibility layer were removed; `make phase10-local-source-check` passes |

## 10. Phase 10 local replacement progress

Detailed implementation architecture and acceptance rules are defined in
[`ws001-p010`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws001-posix/phase010-local-reimplementation/phase.md).
This table is the final, closed progress record for Phase 10.  Later work may
resolve its hand-off items through newly planned phases, but shall update the
component and utility registers rather than reopen or renumber these gates.

| ID | Utility/work item | State | Replacement gates | POSIX review | Current hand-off |
|---|---|---|---:|---:|---|
| P10-GATE | provenance and transition gate | reviewed | 5/5 | n/a | resolved 2026-08-24: exact local manifests and source fingerprints are checked; imported files and obsolete m4 host compatibility files are absent |
| P10-BC | `bc` local reimplementation | partial | 5/5 | 0/checklist | local arbitrary-length integer arithmetic, precedence, scalar assignment, files/stdin, and safe failures work; decimal scale, base conversion, comparisons/control flow, functions, arrays, strings, standard functions, and `-l` remain |
| P10-ED | `ed` local reimplementation | partial | 5/5 | 0/checklist | local checked line vector, basic addresses/editing, BRE substitution, global selection, undo, and atomic sibling-file writes work; complete address/command grammar, backing storage, signals/recovery, newline fidelity, metadata, locale, and exact diagnostics remain |
| P10-M4 | `m4` local reimplementation | partial | 5/5 | 0/checklist | local scanner/rescanner, definitions, conditionals, includes, string/arithmetic builtins, quoting, and memory diversions work; definition stacks, remaining standard builtins, full expression/quote/comment rules, locale, system behavior, and temporary diversions remain |
| P10-INTEG | host, standalone, amd64, QEMU integration | reviewed | 5/5 | n/a | `make phase10-local-host-test`, three direct staged installs with `PREFIX=/`, `make -j16`, and `make posix-phase10-qemu-test` pass |

The five replacement gates for each utility are:

1. imported files and obsolete compatibility support removed;
2. local production source builds with warnings as errors;
3. local replacement host test passes, including safe unsupported behavior;
4. direct package build/install and top-level `make -j16` pass; and
5. the installed local binary passes the bounded amd64 Phase 10 QEMU test.

These fractions are frozen at the completed replacement milestone.  The
remaining conformance work in the hand-off column stays active in the master
register and may be selected when a future phase is defined.

## 11. Phase 11--19 execution result and hand-off

The 2026-08-25 execution produced a bootable native service system and a
repeatable `phase19-qemu-test` target.  The target boots the installed amd64
image with `qemu-system-x86_64`, exercises shell `-c`/simple-list behavior,
service list/status/policy reload, static loopback control, logger delivery,
at-job execution, crontab persistence, and requests orderly poweroff through
PID 1.  The production image was also booted separately and reached a stable
`login:` prompt with syslogd, networkd, and cron resident.

The following findings are deliberately not promoted to success:

| ID | Component | Observed limitation | Safe current state / next unit |
|---|---|---|---|
| P18-SH-GRAMMAR | `/bin/sh` | `if`/`then`, `for`, `case`, functions, grouping, subshell grammar, here-documents, and multiline continuation after operators are not parsed as POSIX reserved-word grammar | Phase 18 is partial.  Keep the safe simple-command/list executor, then replace the line-at-a-time parser with a token-stream AST parser and clause-mapped tests before claiming POSIX shell compliance. |
| P18-SH-SEMANTICS | `/bin/sh` | `ENV`, strict/extension mode separation, complete redirections and expansion ordering, special-builtin error rules, terminal-mode preservation, multiple remembered jobs, job selection, and full job control remain unproved or absent; Phase 20 fixed foreground tty restoration and EOF/error prompt spinning, `ws012-p006` fixed one-external pre-exec handoff, completed `ws001-p014` fixed foreground-pipeline and `fg` ordering, and `ws007-p001` fixed executable-script PATH lookup | Preserve the tested direct/pipeline foreground handoff, Ctrl-Z/`fg`, background/non-TTY behavior, script lookup, and libedit input; extract the remaining semantic families as separate master-derived phases. |
| P16-CRON | `cron`/`crontab` | QEMU proves durable crontab installation and proves the same daemon executes immediate at jobs, but periodic crontab execution and a restart attempted after scheduled work did not complete within the focused bound | Keep cron enabled as a minimal scheduler; add daemon reload/restart diagnostics, full field parser, deterministic clock fixture, locking, and periodic execution evidence. |
| P16-AT-BATCH | `at`/`batch` | accepted time syntax is only `now`, `now + N minutes/hours`, and `HH:MM`; batch does not wait for a load threshold | Retain honest subset behavior and durable jobs; implement the POSIX operand grammar, queue policy, environment, cancellation races, and output delivery next. |
| P13-KLOG | `syslogd` | boot messages are a snapshot, not a live kernel-log stream; rotation and storage failure policy are absent | Keep `/run/log` and `/var/log/messages`; add a pollable kernel reader and rotation/backpressure policy in a later logging phase. |
| P14-AUTH | getty/login | at the project owner's direction, the shipped root account has an empty password; production QEMU proves empty-password authentication and shell startup in `/root`, but logout/respawn and session accounting remain unproved | Treat passwordless root as a development-image policy only; before adding remote login, lock the account or provision a hashed password, and add locked/hashed authentication plus session/utmpx assertions. |
| P15-DHCP | networkd | Phase 20 renamed the local one-shot client to `dhcpc`; lightweight networkd invokes it synchronously, and QEMU proves an initial NE2000 lease plus address/default-route/DNS application with no resident DHCP process | Initial-acquisition milestone resolved.  Renewal/rebind/expiry/release and exhaustive timeout/NAK/spoof/rollback fault injection remain deliberate later hand-offs. |
| P17-NTP | ntpdate | implementation builds, is bounded, and is disabled by default, but no controlled NTP endpoint was available in the guest test | Add a deterministic QEMU network fixture for valid, malformed, spoofed, and timeout responses before enabling at boot. |
| P19-UNIX-POLL | kernel AF_UNIX/poll | listener readiness did not reliably wake init/networkd after a connection queued | Current daemons use nonblocking accept plus a one-second bounded loop.  Repair `poll_notify()`/listener readiness and restore event-driven waits in a focused kernel phase. |
| P19-VARRUN | overlay VFS | creating sockets through the root-image `/var/run -> ../run` symlink returned `EOPNOTSUPP` | Runtime endpoints live directly in tmpfs `/run`; repair overlay symlink traversal for create/bind, then provide `/var/run` compatibility. |
| P19-COVERAGE | integration | passwordless root login and shell startup pass in production QEMU; Phase 20 additionally proves NE2000 DHCP, route/DNS, networkd restart, direct ifconfig, interactive prompt recovery after an external command, and clean shutdown | Logout/respawn, ntpdate success, crash-loop/cycle/malformed-service recovery, reboot persistence, periodic cron, and exhaustive network failure injection remain outside passing markers. |

Phase 19 evidence used `make -j16`, `make -j16 phase19-qemu-test`, and a
separate bounded production-image boot.  Phase 20 adds its four focused host
targets, the vmspace overlapping-pin regression, `make -j16`,
`make phase20-qemu-test`, and
`make phase20-interactive-shell-qemu-test`, all passing under
`qemu-system-x86_64`.  The
aggregate `make check` and its ILP32 path were not used.  Phase 20-modified
userland/base C and header sources pass clang-format 19.1.7's dry-run check.
No source commit was created.

### 11.1 Re-ranked next-work tiers

This ordering controls selection, not conformance status. A lower tier may be
chosen when it directly blocks an active product milestone.

| Tier | Selection rule | Current examples |
| --- | --- | --- |
| 0 — platform blockers | Cross-component defects that invalidate many tests or the supported environment | complete public API inventory, shell grammar/state, tty/job control, credentials, AF_UNIX poll readiness |
| 1 — bounded proof/correction | Low-dependency utilities whose complete operand and failure surface can be isolated | `basename` (p011), then `dirname`, `link`, `unlink`, `cksum`, `true`, `false` |
| 2 — shared-library dependent | Closure depends on locale, robust stdio, regex, account/session, or filesystem semantics | `cat`, `comm`, `grep`, `mesg`, `logname`, `wc`, metadata/traversal utilities |
| 3 — language/service scale | Dedicated parsers, persistent providers, or broad algorithms are required | `awk`, `bc`, `ed`, `m4`, `sed`, `sh`, SCCS, cron/at, `mailx` |

`ws001-p011` was selected from tier 1 so the ledger resumes with a small,
honestly bounded result while tier-0 architectural work remains visible.

## 12. Phase 9 unmet utility register

This section embeds all 111 findings from the first Phase 9 audit and carries
their current hand-off state.  It is the human-readable work register; the
utility CSV remains the machine-readable status/evidence authority.  The three
historical P0 policy conflicts were resolved by Phase 10 and are now P1 because
their independent local replacements intentionally implement only a subset.
The current register therefore contains 0 P0, 73 P1, and 38 P2 findings.

| # | Utility | Finding | Hand-off |
|---:|---|---|---|
| 1 | [admin](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/admin.html) | P1 known incompatibility | Implements only `-i`, `-n`, `-r`, and `-y`; required user/flag/MR/descriptive-text administration, `-h`, `-z`, optional-option-argument rules, and classic SCCS interoperability are absent. |
| 2 | [alias](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/alias.html) | P2 incomplete proof | Basic set/list/query exists; quoting of displayed values, name/error cases, alias-substitution timing, and persistence in a running shell are not tested. |
| 3 | [ar](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ar.html) | P2 incomplete proof | Archive mutation and a SysV/GNU symbol index exist; complete operation/modifier interactions, `-C`, position/name edge cases, malformed archives, metadata, interruption, and output failures remain unproved. |
| 6 | [awk](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/awk.html) | P1 known incompatibility | Only trivial `print`/`$N` processing exists.  Implement the POSIX language locally, including options, grammar, EREs, variables, records/fields, arrays, functions, control flow, I/O, and diagnostics. |
| 7 | [basename](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/basename.html) | P2 narrowed by `ws001-p011` | Empty/all-slash/trailing-slash behavior, the chosen `//` result, suffix-equals/result removal, `--`, usage, and host broken-stdout cases pass; native runtime, allocation failure, and diagnostic-locale proof remain. |
| 9 | [bc](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/bc.html) | P1 known incompatibility (P0 resolved 2026-08-24) | The independent local replacement provides arbitrary-length integer literals, scalar assignment, precedence, `+ - * / % ^`, files/stdin, and checked failures.  Decimal scale, `ibase`/`obase` conversion, comparisons and control flow, functions, arrays, strings, standard functions, and the `-l` math library remain. |
| 13 | [cat](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cat.html) | P2 incomplete proof | `-u` and copying exist; repeated stdin, partial reads/writes, `EINTR`, same-file/error paths, close errors, and broken stdout are not proved. |
| 14 | [cd](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cd.html) | P1 known incompatibility | Missing `-L`, `-P`, and `-e`, `CDPATH`, logical `..`, `PWD`/`OLDPWD` updates, correct unset-`HOME` behavior, and shell-environment tests. |
| 15 | [cflow](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cflow.html) | P1 known incompatibility | Uses a token heuristic rather than a conforming C preprocessing/declaration analysis; macro/include options are accepted without providing full preprocessing semantics. |
| 16 | [chgrp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chgrp.html) | P1 known incompatibility | Numeric GIDs only; group names, `-h`, recursive `-R` with `-H`/`-L`/`-P`, symlink rules, and traversal/error behavior are absent. |
| 17 | [chmod](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chmod.html) | P2 incomplete proof | Numeric and substantial symbolic modes plus `-R` exist; omitted-who/umask semantics, symlink/traversal policy, special bits, race/error cases, and exhaustive grammar tests remain. |
| 18 | [chown](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chown.html) | P1 known incompatibility | Numeric UID/GID only; owner/group names, omitted components, `-h`, recursive link modes, and traversal/error semantics are absent. |
| 19 | [cksum](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cksum.html) | P2 incomplete proof | CRC path is plausible, but standard vectors at length boundaries, multiple files/stdin naming, read interruption, output/close failure, and accumulated exit status are not fully tested. |
| 20 | [cmp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cmp.html) | implemented-unreviewed (`ws001-p017`) | `-l`/`-s`, POSIX-locale formats, exit classes, independent short reads, same-stdin rejection, and a checked skip extension pass; deterministic I/O/close fault and locale review remain. |
| 21 | [comm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/comm.html) | P1 known incompatibility | Column suppression exists, but comparison uses byte ordering rather than `LC_COLLATE`; sorted-input assumptions, long lines, read/write errors, and locale behavior are unproved. |

続きはこのIssueの取り込み追記コメントに保持します。
