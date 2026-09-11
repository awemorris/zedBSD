# WS002: system services

<!-- awesome-plan-current:start -->

## Current state — 2026-09-11 user decision

Status: completed

Decision source: current user, 2026-09-11, this task.

既存サービス受け入れとp021のユーザー判断によるcompleted判定を維持し、今回の明示指示によりIssueを閉じる。BUG-012の履歴・再発条件とWS001への引き継ぎは保持。

今回の処理は計画上の受け入れ・閉鎖であり、ソース変更・ビルド・実行試験は行っていない。再開点なし。

[Master](https://github.com/awemorris/zedBSD/issues/1) · [Past Log](https://github.com/awemorris/zedBSD/issues/366)

<!-- awesome-plan-current:end -->

## 既存の計画・試行履歴

以下の旧状態・残件・再開条件は過去の記録。現在の状態と追加完了条件の扱いは上記ユーザー判断を優先する。


<!-- traceability:start -->

## Goal traceability

- Primary Milestone: **MG005 — シンプルで一貫したネットワーク/サービス管理を利用できる**
- Related Milestones: MG002
- Objectives: O1, O2, O3
- 貢献する成果: initとシステムサービスの実行・制御を整える。
- 上位定義: [MasterのObjectives / Milestone Goals](https://github.com/awemorris/zedBSD/issues/1)

既存Phaseは本WSを親として上位成果に接続する。Primaryは分類と責任の所在であり、
各PhaseがRelatedすべてを満たすという意味ではない。成果・検証・限界は各Phaseの
現行記録を根拠とする。今回の対応付けは状態変更・未定義作業の追加・実行許可ではない。

<!-- traceability:end -->


Current update (2026-09-09): **completed**. The user clears p021 based on passing
current acceptance and the likelihood that the old failure is already fixed.
BUG-012 preserves the unproved historical cause and recurrence condition.
See [completion reconciliation](completion.md) and
[p021 closure](phase021/closure.md).

Last updated: 2026-09-09

WSID: `ws002`

Status: completed; service baseline and corrective phases accepted

Parent: [master plan](../master.md)

Last verified Phase: `ws002-p024` (q142); p021 subsequently cleared by user decision

Resume point: no active phase. Preserve POSIX handoffs in WS001 and recurrence
conditions in the bug ledger; no current Priority list (removed by user on 2026-09-11).

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
[WS001](../ws001/ws.md), and the Phase acceptance result states
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
This completed baseline uses a strict `key=value` data format. WS012 owns the
planned migration to YAML; the baseline is not described as YAML before that
separately authorized migration completes.

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
| `ws002-p011` | [service foundation](phase011/phase.md) | Complete | Parsers, formats, path policy, tests, and `/sbin` classification are stable |
| `ws002-p012` | [init and service lifecycle](phase012/phase.md) | Complete | Native PID 1 boots, controls services, mounts filesystems, and shuts down orderly |
| `ws002-p013` | [system logging](phase013/phase.md) | Complete | `logger`, `/run/log`, `syslogd`, boot log persistence, and messages work |
| `ws002-p014` | [console sessions](phase014/phase.md) | Complete | Supervised `getty` reaches `login` and respawns safely |
| `ws002-p015` | [network management](phase015/phase.md) | Complete baseline | `networkd` and `net` configure static IPv4 and initial DHCP |
| `ws002-p016` | [scheduled work](phase016/phase.md) | Complete baseline | `cron`, `crontab`, `at`, and `batch` execute jobs with durable state |
| `ws002-p017` | [initial time](phase017/phase.md) | Complete optional feature | Bounded `ntpdate` works without making boot depend on network time |
| `ws002-p018` | [POSIX shell](phase018/phase.md) | Partial with handoffs | Shell is usable; remaining incompatibilities are recorded in WS001 |
| `ws002-p019` | [integrated QEMU acceptance](phase019/phase.md) | Complete minimum system | Boot, login, services, jobs, network, and shutdown were exercised and repaired |
| `ws002-p020` | [synchronous network service](phase020/phase.md) | Complete milestone | fd 3 readiness and synchronous `net` orchestration pass host/build/QEMU gates |
| `ws002-p021` | [missing-login session teardown](phase021/phase.md) | Complete by user acceptance | Current runtime passes; likely corrected historical failure retained as BUG-012 |
| `ws002-p022` | [intermittent console-login progress](phase022/phase.md) | Complete | USB submit-commit IRQ self-wait repaired; deterministic old-order regression, unchanged `MAC-T022`, and ordinary initial plus final five exact-login boots pass |
| `ws002-p023` | [USB boot halt](phase023/phase.md) | Complete q135 | QEMU reproduction and correction, subsequent paired regression; physical boundary retained |
| `ws002-p024` | [retirement heap integrity](phase024/phase.md) | Complete q142 | UHCI stale-link correction and heap/lifecycle acceptance |

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
7. update [WS001](../ws001/ws.md) with actual evidence and remaining
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

## USB boot shutdown follow-up (2026-09-09)

[ws002-p023](phase023/phase.md)を追加。BUG-010のQEMU再現と、再現した場合の修正を現在のゴールに含める。p021と独立して扱い、USB解析は実装可能な作業の後段で実行する。

### q135 USB boot halt correction

[p023](phase023/phase.md) completed its QEMU reproduction and
correction. Referenced root-media detach left the control worker alive, and
PC/AT halted only one CPU. Checked storage quiescence and all-CPU terminal
stop now pass xHCI USB-root halt, dirty-data persistence, reboot and single-CPU
checks. [Evidence](phase023/results.md). q137 also passes paired
EHCI/UHCI halt and persistent-data reboot regression after boot-context fixes.
Physical USB halt remains unverified. WS002-p021 remains.

### q136 session teardown verification

p021 remains **uncleared** with [new evidence](phase021/results.md):
three missing-login boots per PCAT/PC98, 100 owner-recovery cycles per platform,
and normal session regression pass. Old invalid-free provenance and controlled
small-heap fallback investigation remain. Continue WS006; WS002 is not closed.

### q137 heap-walk reproduction lead

Paired USB input/root-I/O now reaches a process-reaper heap-walk stall after
boot and hotplug succeed. [p024](phase024/phase.md)
plans bounded capture and first-failure diagnosis. The connection to p021's
historical invalid free remains unproven; neither Phase is complete.

## q140 / q141 heap-integrity continuation

q140's Daybreak diagnostics reproduced a structural heap-header overwrite in
paired UHCI retirement. q141 proved that non-head schedule unlink left the
predecessor's software successor stale; a later unlink followed freed/reused
memory and wrote a request pointer into the heap header. The narrow link repair
and regression pass all heap/USB gates. p024 remains uncleared solely because
the independent PCAT missing-login gate reports owner counts 9 to 8. See
[p024 progress](phase024/progress.md). The historic
p021 lifecycle requirement remains separately owned and must be repaired before
that last p024 completion gate can turn green.
