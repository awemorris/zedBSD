<!-- awesome-plan project=zedbsd record=ws001 -->

# WS001: POSIX.1-2024 compliance

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG002
Related Milestones: MG005
Objectives: O1, O2, O3
Parent: [Master](../master.md)
Queue: q834（2026-10-07、P2）
Resume point: 2026-10-07 q834 P2: p041 ls（host 24/24）→ p042 find（host 38/38）→ p043 tabs（host 55/55）、guest の回帰は 3 つの後に T1 にまとめる。それ以前: p024〜p040 cleared（2026-09-27）
Target: **ベータ3**（2026-10-07 ユーザー「下記をベータ3に移動します。・左手デバイスOSK、ゲームパッドOSK, 写真の続き, カレンダーの続き, IMEの続き、POSIX, NVMe, make, RTL8822C, Sleep」）
<!-- awesome-plan-current:end -->

Shared tests: [WS001 test index](tests/README.md)

## Phase registry

| Combined ID | Phase | Status | Result |
| --- | --- | --- | --- |
| `ws001-p000` | [inventory and gates](phase000/phase.md) | Complete | Protected the inventory and acceptance gates |
| `ws001-p001` | [temporary failure commands](phase001/phase.md) | Complete | Installed deliberate provider-missing commands |
| `ws001-p002` | [low-dependency utilities](phase002/phase.md) | Complete | Added low-dependency commands and shell builtins |
| `ws001-p003` | [locale and terminal descriptions](phase003/phase.md) | Complete | Added locale catalogs and terminal-description foundations |
| `ws001-p004` | [parsers and archives](phase004/phase.md) | Complete | Implemented the planned parser/editor/traversal/archive utilities |
| `ws001-p005` | [process and IPC tools](phase005/phase.md) | Complete | Added process, credential, and System V IPC tools |
| `ws001-p006` | [development utilities](phase006/phase.md) | Complete | Added the selected development utilities |
| `ws001-p007` | [compression utilities](phase007/phase.md) | Complete | Added compression utilities and tests |
| `ws001-p008` | [SCCS suite](phase008/phase.md) | Complete | Added the planned SCCS subset |
| `ws001-p085` | [terminfo and base builds](phase085/phase.md) | Complete | Legacy Phase 8.5: terminal packages and standalone builds |
| `ws001-p009` | [POSIX utility audit](phase009/phase.md) | Complete as audit | Findings remain in this ledger |
| `ws001-p010` | [local reimplementation](phase010/phase.md) | Complete milestone | Imported `bc`, `ed`, and `m4` were replaced locally |
| `ws001-p011` | [bounded basename correction](phase011/phase.md) | Complete milestone | Host semantics/failure test and native amd64 build pass; runtime conformance handoff remains |
| `ws001-p012` | [bounded dirname correction](phase012/phase.md) | Complete milestone | Host lexical/failure suite and native amd64 build pass; runtime/locale handoff remains |
| `ws001-p013` | [bounded link/unlink correction](phase013/phase.md) | Complete | Host identity/failure suite and native amd64 build pass; broad filesystem matrix remains |
| `ws001-p014` | [shell foreground job-control synchronization](phase014/phase.md) | Complete (`q023`, 2026-08-28) | Foreground pipelines gate every member until TTY handoff; `fg` hands off before `SIGCONT`; background/non-TTY and cleanup regressions pass |
| `ws001-p015` | [base C coding-style adoption](phase015/phase.md) | Complete (`agent2-q001`, 2026-08-31) | New/refactored base source has an executable style gate; the final inventory is 7 compliant and 234 historical files |
| `ws001-p016` | [direct PDF printing over LPD](phase016/phase.md) | Complete implementation milestone (`agent2-q001`, 2026-08-31) | Native PDF-only `lp`/`lpr` and fake-LPD protocol matrix pass; POSIX text/`-w` and guest network submission remain handoffs |
| `ws001-p017` | [bounded cmp conformance](phase017/phase.md) | Complete implementation milestone (`agent2-q001`, 2026-08-31) | `-l`/`-s`, skip extension, independent short reads, output formats, and exit classes pass focused tests |
| `ws001-p018` | [bounded tee conformance](phase018/phase.md) | Complete implementation milestone (`agent2-q001`, 2026-08-31) | `-i`, dynamic outputs, robust writes, failure continuation, and statuses pass focused tests |
| `ws001-p019` | [canonical userland source headers](phase019/phase.md) | Complete (`agent2-q002`, 2026-08-31) | All 269 userland C-family files have the exact section 13 block and a separate explanation; body hashes, fixtures, assembler preprocessing, full build, and whitespace checks pass |
| `ws001-p020` | [complete userland C-style conformance](phase020/phase.md) | Uncleared (`agent2-q003`, 2026-08-31) | All 2,454 functions pass public/static order, prototype, header-layout, comment, loop/switch, case-label, and build gates; the body audit records 731 mechanical residuals plus semantic-review handoffs in the 258-row ledger |
| `ws001-p021` | [ANSI C declarations and semantic layout](phase021/phase.md) | Complete (`agent2-q006`, 2026-08-31) | All 214 implementations pass ANSI declaration, semantic paragraph, symmetric brace, loop block, entry spacing, indentation, build, automated audit, and user manual-review gates |
| `ws001-p022` | [credential-aware VFS object creation](phase022/phase.md) | Complete (`q050`, 2026-09-01) | Production-linked UFS/overlay rollback faults, root/non-root backend matrix, and abrupt-stop/reopen/remount acceptance pass; tmpfs double link increment fixed |
| `ws001-p023` | [truthful and durable directory fsync](phase023/phase.md) | Complete (`q050`, 2026-09-01) | VFS/UFS/overlay ordering and mutation gates plus five-launch abrupt-stop/remount durability pass; FAT/tmpfs directory sync stays explicitly unsupported |
| `ws001-p024` | [実行系の utility（xargs・time・nohup・env・pwd）](phase024/phase.md) | cleared（2026-09-27） | host の差分 102/102・端末 7/7・amd64 guest 102/102、style 0。sh の `env` builtin を削除 |
| `ws001-p025` | [cp と mv](phase025/phase.md) | cleared（2026-09-27） | host の差分 118/118・端末 10/10・amd64 guest 116/116・installer の呼び方は旧と同じ、style 0。libc の `strerror` を全 81 error 番号に（coordinator の依頼） |
| `ws001-p026` | [id・chown・chgrp・chmod・mkdir・mkfifo・rmdir](phase026/phase.md) | cleared（2026-09-27） | host の差分 128/128・amd64 guest 128/128、style 0。記号 mode を chmod・mkdir・mkfifo で共有 |
| `ws001-p027` | [expand・unexpand・fold・nl・comm](phase027/phase.md) | cleared（2026-09-27） | host の差分 54/54（全 case 810/810）・amd64 guest 77/77、style 0 |
| `ws001-p028` | [split・csplit・pr](phase028/phase.md) | cleared（2026-09-27） | host の差分 62/62・amd64 guest 62/62、style 0。pr の header は POSIX の書式 |
| `ws001-p029` | [diff](phase029/phase.md) | cleared（2026-09-27） | Myers の最長共通部分列、全形式、host 29/29・乱数 2000/2000（GNU の patch/ed に当て、GNU --minimal と同じ変更行数）・amd64 guest 71/71、installer の呼び方は旧と同じ |
| `ws001-p030` | [date・sleep・uname・kill・pathchk・strings ほかの小さな utility](phase030/phase.md) | cleared（2026-09-27） | host の差分 44/44・期待値の case 5/5・amd64 guest 49/49、guest で date の時計の設定、style 0 |
| `ws001-p031` | [guest の回帰と台帳の照合](phase031/phase.md) | cleared（2026-09-27） | host 945/945、amd64 guest 948/948、boot test PASS、§12 を WS042・WS043 と照合（P1 73→27、P2 38→17）、q136 は再現しない |
| `ws001-p032` | [cat・cksum・dd](phase032/phase.md) | cleared（2026-09-27） | host 32/32・amd64 guest 38/38、dd の conv= と cbs= を全部、cksum は表の CRC、installer の `-a sha256` を保つ |
| `ws001-p033` | [patch](phase033/phase.md) | cleared（2026-09-27） | 全形式・全 option の書き直し、host 32/32・期待値 9/9・乱数 2000/2000（zedBSD diff の全形式を当てる）・amd64 guest 70/70、style 0 |
| `ws001-p034` | [df・du](phase034/phase.md) | cleared（2026-09-27） | df は mount 表の全部と operand の file system、du は全 option と hard link・loop、host du 15/15・amd64 guest 23/23、style 0 |
| `ws001-p035` | [who](phase035/phase.md) | cleared（2026-09-27） | 全 option と `am i`・file operand、`<utmpx.h>` に POSIX の定数、host who 5/5（glibc の記録を GNU と）・amd64 guest 28/28、style 0 |
| `ws001-p036` | [stty](phase036/phase.md) | cleared（2026-09-27） | 全 operand・`-a`・`-g`・窓の大きさ、`<termios.h>` に XSI の遅延、`<unistd.h>` に `_POSIX_VDISABLE`、host pty 89/89・amd64 guest 30/30、style 0 |
| `ws001-p037` | [dirname の複数の operand](phase037/phase.md) | cleared（2026-09-27） | ユーザーの決定（WS045 から）。各結果を 1 行ずつ、`-z`、dirname-test PASS・host 5/5・amd64 guest 28/28、style 0 |
| `ws001-p038` | [mktemp・install・base64](phase038/phase.md) | cleared（2026-09-27） | ユーザーの決定（WS045 から）。3 つの base utility を新設し package の一覧へ、host 11/11・11/11・13/13（GNU と）・amd64 guest 35/35、style 0 |
| `ws001-p039` | [xargs の GNU の option](phase039/phase.md) | cleared（2026-09-27） | ユーザーの決定（WS045 から）。`-d`・`-P`・`-a`・`-o`・旧い形・long option、host の build 一覧に xargs、host 54/54・11/11（POSIX と GNU の mode）・全 case 1080/1080・WS045 515/515・configure の比較 same・amd64 guest 65/65、style 0 |
| `ws001-p040` | [mesg](phase040/phase.md) | cleared（2026-09-27） | 書き直し（最初の端末の descriptor、`y`/`n`/`--`、他の bit を保つ、状態 0/1/2）、host 9/9、style 0、amd64 guest の pinned 31/31（console の case は BUG-067 の修正の後に PASS） |
| `ws001-p041` | [ls の XCU の option](phase041/phase.md) | in-progress（2026-10-07 P2） | -A・-c・-u・-f・-g・-o・-H・-k・-p・-s・-S、512 byte の block。host の差分 24/24、zedBSD の build。guest は T1 待ち |
| `ws001-p042` | [find の XCU の primary と式](phase042/phase.md) | in-progress（2026-10-07 P2） | -exec … {} + の束ね、-perm の symbolic mode、-L/-H の dangling link、loop の診断。host の差分 38/38、zedBSD の build。guest は T1 待ち |
| `ws001-p043` | [tabs の XCU の形と幅](phase043/phase.md) | in-progress（2026-10-07 P2） | -0、-1・-2、column 1 の `\E[0C` のずれ、幅（COLUMNS・端末・cols）、TERM の既定、複数の operand。host の比較 55/55、zedBSD の build。guest は T1 待ち |
| `ws001-p044` | [ps の XCU の形](phase044/phase.md) | test-wait（2026-10-09 P1） | -o の field=header（引数の終わりまで）、XCU の field と header、列の幅、選択の和と既定（実効 user と session）、-f の command line、`[dd-]hh:mm:ss`。host 15/15、zedBSD の build。pcpu・etime・実の user・tty の名前は kernel に無く記録。guest は T1 待ち |
| `ws001-p045` | [POSIX の header の全数の照合と補完](phase045/phase.md) | planned（2026-10-09 Q1、ユーザーの指示） | 全 header の型・定数・宣言を照合し、不足を補う。ws126-p002 の libc の不足が発端 |

### q042 pre-merge identifier migration

Historical q041/q042 documents and test output labels retain the original
`ws001-p015` credential-creation and `ws001-p016` directory-fsync names.  In
the active registry and all current dependencies, those records map to
`ws001-p022` and `ws001-p023`, respectively. Runtime `--path` is proven and
q050 completed their fault-injection and disposable-image/remount acceptance,
releasing both VFS dependencies of `ws005-p005`.

Original combined planning context is retained in the
[legacy Phase 0–10 plan](history/phase000-010-legacy-plan.md).

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

## 2026-09-27 からの計画（p024〜p031）

2026-09-27 ユーザー指示（サブエージェントで WS001 に取り組む）により、台帳（下の §12）から依存を満たす utility を選び、
Phase に分けた。範囲の境界:

- WS043（完了）が作り直した `sed`・`awk`・`grep`・`cut`・`wc`・`head`・`tail`・`sort`・`uniq`・`tr`・`od`・`paste`・`join`・
  `rm`・`ln`・`touch`・`printf`・`echo`・`test`・`true`・`false` と、WS042（完了）の `sh` とその builtin は選ばない。
  GNU 拡張は WS045 の範囲（`sed`・`grep`・`expr`・`awk`・`tail`・`echo` ほか）で、ここでは POSIX の要求だけを扱う。
- 各 utility は `plan/coding-style.md` の全文で書き直す（`plan/tools/style-check.py` の違反 0）。
- 試験は host の差分試験（`plan/tools/utils/util-diff.py`、GNU の POSIX mode と比べる）を主とし、case は
  `plan/tools/utils/cases/<utility>.sh` に置く。host の build は [tests/build-host-ws001.sh](tests/build-host-ws001.sh)。
  guest（amd64 QEMU）の回帰は p031 で export した case を流す。
- 新しい program（`pwd`）は `userland/base/<name>/` と `config/ci/*.mk` の program の一覧に足す。

| Phase | 対象の台帳の行 | 受け入れの核 |
| --- | --- | --- |
| p024 | #152 xargs、#122 time、#89 nohup、#39 env、#99 pwd | option・引用・状態 126/127 と 123/124/125・`-p`・stdout の失敗が XCU どおり。`/bin/pwd` の新設 |
| p025 | #24 cp、#83 mv | `-R`・`-H`/`-L`/`-P`・`-p`・`-i`・`-f`、directory、同じ file の検出、別 device の mv |
| p026 | #59 id、#18 chown、#16 chgrp、#17 chmod、#79 mkdir、#80 mkfifo、#106 rmdir | 名前の解決、`-R`・`-H`/`-L`/`-P`・`-h`、記号の mode と umask |
| p027 | #41 expand、#136 unexpand、#48 fold、#87 nl、#21 comm | tab の list、`-b`・`-s`、nl の全 option と節、comm の照合 |
| p028 | #113 split、#26 csplit、#95 pr | 接尾辞の長さと尽き、regex の operand と繰り返し、pr の段と頁 |
| p029 | #34 diff | 最長共通部分列の差分、`-b`・`-c`・`-C`・`-e`・`-f`・`-u`・`-U`・`-r`、状態 0/1/2 |
| p030 | #30 date、#111 sleep、#134 uname、#64 kill、#93 pathchk、#114 strings、#66 link、#139 unlink、#129 tty、#71 logname | 各 utility の XCU の option と状態 |
| p031 | 上の全部 | amd64 guest で export した case を流す。台帳の §12 と dashboard を照合する |
| p032 | #13 cat、#19 cksum、#31 dd（p031 の後に追加） | 流れの utility。dd の全 conv と cbs、統計、SIGINT |
| p033 | #92 patch（p032 の後に追加） | normal・context・unified・ed、file の決め方、`-p`・`-R`・`-N`・`-b`・`-o`・`-r`・`-D`・`-l`、fuzz と offset、reject、状態 0/1/2 |
| p034 | #33 df、#36 du（p033 の後に追加） | df の全 file system と operand の解決、du の `-a`/`-s`/`-k`/`-x`/`-H`/`-L`、hard link、loop |
| p035 | #150 who（p034 の後に追加） | option の全部、`am i`、file operand、端末の状態と idle、`<utmpx.h>` の定数 |
| p036 | #116 stty（p035 の後に追加） | 全 operand、`-a`・`-g`、設定の読み戻し、窓の大きさ |
| p037 | #35 dirname（ユーザーの決定、WS045 から） | 複数の operand、`-z`、dirname-test の更新 |
| p038 | 台帳の外（POSIX でない）: mktemp・install・base64（ユーザーの決定、WS045 から） | GNU の option、image に入れる、GNU と比べる case |
| p039 | #152 xargs（ユーザーの決定、WS045 から） | GNU の `-d`・`-P`・`-a`・`-o`・旧い形・long option、host の build 一覧 |

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
| [`ws001-p009`](phase009/phase.md) | detailed evidence and rationale from the 2026-08-24 first audit pass |
| [`ws001-p010`](phase010/phase.md) | detailed removal and local reimplementation design for `bc`, `ed`, and `m4` |
| [legacy Phase 0–10 plan](history/phase000-010-legacy-plan.md) | historical execution plan and phase acceptance policy |
| [WS002](../ws002/ws.md) | completed post-Phase-10 service architecture and Phase 11–20 baseline |
| `ws002-p020`（削除済み。git の履歴にある） | completed synchronous net-service implementation milestone and its handoffs |
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
| §12 の状態（2026-09-27、ws001-p036 の後） | 111 | implemented-unreviewed 75、P1 22、P2 14（表の行を数えた値。p032 で cat・cksum・dd、p033 で patch、p034 で df・du、p035 で who、p036 で stty）。下の行は 2026-08-31 の値で、この行が新しい |
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
`ws002-p020`（削除済み。git の履歴にある） passes. This narrows the
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
| KERN-WAIT-01 | child wait and accounting | partial | `wait`, `time`, `sh` | basic `waitpid()` works; multiple saved statuses, non-child behavior, signal status, stopped jobs, and user/system CPU accounting remain; missing-login exit/reap invalid-free remains tracked by `ws002-p021`（削除済み。git の履歴にある） |
| KERN-TTY-01 | tty line discipline and termios | partial | `stty`, `sh`, `mesg`, `tty`, `newgrp` | canonical/raw and common flags exist; audit all required flags, speeds, control characters, VMIN/VTIME, drains/flushes, signals, and error atomicity |
| KERN-PTY-01 | pseudo terminals and controlling tty | implemented-unreviewed | shell/job control, terminal tests | UNIX98-style PTY path exists; prove session/controlling-terminal acquisition, foreground groups, hangup, permissions, and lifecycle; missing-login exit/reap invalid-free remains tracked by `ws002-p021`（削除済み。git の履歴にある） |
| KERN-CLOCK-01 | clocks and clock setting | partial | `date`, `touch`, libc time | `clock_settime()` exists; prove privilege checks, valid ranges, clock selection, timezone-facing behavior, interruption, and filesystem timestamp integration |
| KERN-VFS-01 | pathname, metadata, and traversal semantics | partial | file utilities | q050 proves credential-aware object creation/rollback and truthful UFS1/UFS2/overlay directory `fsync`, with FAT/tmpfs directory sync explicitly `EOPNOTSUPP`; recursive symlink policies, mount boundaries, broader hard-link/metadata races, and family-wide error semantics remain |
| KERN-FSSTAT-01 | filesystem capacity/accounting | partial | `df`, `du` | provide and verify stable filesystem/device identity, portable block accounting, mount lookup, overflow behavior, and permission/error cases |
| KERN-RSRC-01 | priorities | reviewed | `nice`, `renice` | declared current scope has reviewed utility evidence; keep regression and permission/range tests |
| KERN-RSRC-02 | resource limits | reviewed | `ulimit`, shell | declared current scope has reviewed utility evidence; expand when new limit classes are exposed |
| KERN-BOOT-01 | init/service lifecycle | implemented-unreviewed | `/sbin/init`, service providers | native PID 1 boots and initiates ordered shutdown in QEMU; complete crash-loop, required-failure, stop-timeout, cycle, credential, and recovery evidence; missing-login exit/reap invalid-free remains tracked by `ws002-p021`（削除済み。git の履歴にある） |
| KERN-NET-01 | loopback and interface control | implemented-unreviewed | `networkd`, `net`, socket users | four-CPU QEMU proves NE2000 receive/transmit, a real DHCP lease, default route, DNS, static `lo0`, up/down, and dp8390 SMP serialization; counters, aliases, IPv6, broader NICs, stress/race coverage, and full ioctl review remain |
| KERN-NET-02 | AF_UNIX peer identity | implementation extension | `networkd`, local control protocols | `SO_PEERCRED` returns one immutable connection-time 12-byte `kern_peercred` snapshot for connected AF_UNIX streams; this is a zedBSD extension, not a POSIX/SUS conformance interface, and its pathname/socketpair/SCM_RIGHTS lifecycle evidence is owned by `ws005-p003` |
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
| API-NET-01 | `SO_PEERCRED`, `struct kern_peercred` | implementation extension | fixed 12-byte PID/EUID/EGID ABI, connection-time AF_UNIX snapshot, and `ws005-p003` guest fixture | Explicitly non-POSIX/non-SUS; retain ABI layout, short-buffer atomicity, descriptor-transfer identity, and unconnected/non-AF_UNIX error regressions without counting this row toward POSIX conformance |
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
| LIBC-ERRNO-01 | error descriptions (`strerror`, `strerror_r`, `perror`, `err`/`warn`) | implemented-unreviewed (`ws001-p025`) | every diagnostic | all 81 numeric error numbers of `include/uapi/errno.h` have distinct descriptions; unknown numbers give `Unknown error N` and `strerror_r` gives `EINVAL`/`ERANGE` (`plan/ws001/tests/strerror-host-test.py`); locale-specific messages (`LC_MESSAGES`) remain |
| LIBC-STDIO-01 | robust stream I/O | partial | most utilities | standardize short read/write, `EINTR`, broken stdout, close/flush errors, and accumulated exit status |
| LIBC-ALLOC-01 | allocation/resource failure discipline | partial | parsers and recursive tools | add fault injection and checked size/growth paths; prohibit silent truncation and success after `ENOMEM` |
| LIBC-ACCT-01 | passwd/group lookup and group membership | partial | `id`, `chown`, `chgrp`, `newgrp`, `ps` | names, supplementary groups, reentrant/error behavior, missing records, and credential transition tests |
| LIBC-UTMPX-01 | session database | partial | `write`, `who`, login/service work | establish producer/lifecycle model, locking, corruption handling, stale tty cleanup, and time semantics |
| TERM-DB-01 | terminfo database and checked reader | implemented-unreviewed | `tabs`, `tput`, curses | broaden standard capability/parameter semantics, malformed data, aliases, install compatibility, and output failures |
| TERM-CURSES-01 | curses library | implemented-unreviewed | future full-screen programs | expand window/input/update semantics and define the POSIX/XSI scope before any conformance claim |
| ARCHIVE-01 | archive/ELF shared readers | implemented-unreviewed | `ar`, `nm`, `pax` | standard format variants, malformed data, overflow, metadata, symbol tables, non-ELF policy, and fuzz evidence |
| SCCS-CORE-01 | SCCS history/p-file/locking core | partial | ten SCCS commands | classic weave/control interoperability, full flags/MRs/SIDs, preservation, stale locks, interrupted atomic updates |
| SHELL-CORE-01 | shell lexer/parser/expansion/executor | partial | `sh`, cron, login | `sh -c`, simple lists/pipelines/expansion and scripts work; `ws012-p006` gates a single foreground external, and [`ws001-p014`](phase014/phase.md) gates every foreground-pipeline member until TTY handoff and makes `fg` hand off before `SIGCONT`; reserved words, multiline continuation, compound commands, functions, grouping/subshell grammar, here-documents, full redirects/expansions, strict mode, `ENV`, multiple-job selection, complete jobs/traps, and special-builtin semantics remain |
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
| SVC-GETTY-01 | getty/login session | partial | production QEMU accepts the explicitly passwordless root account and starts `/bin/sh` in `/root` without daemon churn | prove utmpx transitions, logout, hangup, getty respawn, locked-account rejection, and hashed-password authentication; missing-login exit/reap invalid-free remains tracked by `ws002-p021`（削除済み。git の履歴にある） |
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
| CROSS-SHELL-01 | current-shell state | partial | shell builtins | tests inside a running zshell for environment, cwd, umask, limits, traps, descriptors, and jobs; completed [`ws001-p014`](phase014/phase.md) supplies direct/pipeline/Ctrl-Z/`fg`/background/non-TTY job-control regression |
| CROSS-BINARY-01 | malformed binary formats | partial | locale/catalog/terminfo/archive/ELF/SCCS/compression | truncation, invalid offsets/counts, integer overflow, fuzz corpus, bounded failure |
| CROSS-QEMU-01 | zedBSD runtime evidence | implemented-unreviewed | kernel-, tty-, credential-, IPC-, process-, service-dependent behavior | q050 adds five bounded amd64 launches covering overlay and native UFS1 abrupt-stop/relaunch, external journaled UFS2, tmpfs, FAT rejection/remount, and frozen-source integrity; continue adding target-specific cells whenever host behavior is insufficient |
| CROSS-PROV-01 | external source exclusion | reviewed | Phase 10 `bc`, `ed`, `m4` scope | imported production/generated trees and the m4 host compatibility layer were removed; `make phase10-local-source-check` passes |

## 10. Phase 10 local replacement progress

Detailed implementation architecture and acceptance rules are defined in
[`ws001-p010`](phase010/phase.md).
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
| 2 | [alias](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/alias.html) | implemented-unreviewed (WS042) | The shell and its builtins were rewritten by WS042 against dash (`plan/tools/sh/sh-diff.py`: host 1417/1425 at WS042 completion, 1437/1458 in the ws001-p024 rerun; amd64 guest 1388/1425 with environment differences only). |
| 3 | [ar](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ar.html) | P2 incomplete proof | Archive mutation and a SysV/GNU symbol index exist; complete operation/modifier interactions, `-C`, position/name edge cases, malformed archives, metadata, interruption, and output failures remain unproved. |
| 6 | [awk](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/awk.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). The whole POSIX language is implemented; gawk extensions are WS045's; locale collation in comparisons remains. |
| 7 | [basename](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/basename.html) | P2 narrowed by `ws001-p011` | Empty/all-slash/trailing-slash behavior, the chosen `//` result, suffix-equals/result removal, `--`, usage, and host broken-stdout cases pass; native runtime, allocation failure, and diagnostic-locale proof remain. |
| 9 | [bc](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/bc.html) | P1 known incompatibility (P0 resolved 2026-08-24) | The independent local replacement provides arbitrary-length integer literals, scalar assignment, precedence, `+ - * / % ^`, files/stdin, and checked failures.  Decimal scale, `ibase`/`obase` conversion, comparisons and control flow, functions, arrays, strings, standard functions, and the `-l` math library remain. |
| 13 | [cat](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cat.html) | implemented-unreviewed (`ws001-p032`) | Unbuffered copying (`-u` changes nothing), `-` and repeated stdin, short writes and EINTR, failures that continue and a write failure that stops pass the host cases and the amd64 guest. |
| 14 | [cd](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cd.html) | implemented-unreviewed (WS042) | The shell and its builtins were rewritten by WS042 against dash (`plan/tools/sh/sh-diff.py`: host 1417/1425 at WS042 completion, 1437/1458 in the ws001-p024 rerun; amd64 guest 1388/1425 with environment differences only). |
| 15 | [cflow](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cflow.html) | P1 known incompatibility | Uses a token heuristic rather than a conforming C preprocessing/declaration analysis; macro/include options are accepted without providing full preprocessing semantics. |
| 16 | [chgrp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chgrp.html) | implemented-unreviewed (`ws001-p026`) | Group names or IDs, `-h`, and `-R` with `-H`/`-L`/`-P` pass the shared chown cases on the host and the amd64 guest. |
| 17 | [chmod](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chmod.html) | implemented-unreviewed (`ws001-p026`) | The full symbolic grammar (clauses, several actions, `X`, `s`, `t`, permission copy, umask for omitted who), octal modes, directory set-ID retention, `-w` as a mode, and `-R` without following inner links pass 19 host cases and the amd64 guest. |
| 18 | [chown](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chown.html) | implemented-unreviewed (`ws001-p026`) | Names or IDs, `owner:group`, `:group`, `owner:`, `-h`, and `-R` with `-H`/`-L`/`-P` (shared with chgrp) pass 18 host cases and the amd64 guest. |
| 19 | [cksum](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cksum.html) | implemented-unreviewed (`ws001-p032`) | The POSIX CRC (table driven) with length bytes, standard vectors, length boundaries up to 65537, files and nameless stdin pass the host cases and the amd64 guest; the installer's `-a sha256` format is kept (pinned case). |
| 20 | [cmp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cmp.html) | implemented-unreviewed (`ws001-p017`) | `-l`/`-s`, POSIX-locale formats, exit classes, independent short reads, same-stdin rejection, and a checked skip extension pass; deterministic I/O/close fault and locale review remain. |
| 21 | [comm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/comm.html) | implemented-unreviewed (`ws001-p027`) | `strcoll` comparison, column tabs only for shown columns, `-123` combinations, duplicates, stdin, and missing newlines pass 9 host cases and the amd64 guest; non-C collation depends on LIBC-COLLATE-01. |
| 22 | [command](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/command.html) | implemented-unreviewed (WS042) | The shell and its builtins were rewritten by WS042 against dash (`plan/tools/sh/sh-diff.py`: host 1417/1425 at WS042 completion, 1437/1458 in the ws001-p024 rerun; amd64 guest 1388/1425 with environment differences only). |
| 23 | [compress](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/compress.html) | P1 known incompatibility | Classic `.Z` LZW is implemented, but Issue 8 algorithm-selection interfaces and complete overwrite, metadata, signal, full-disk, corrupted-stream, and replacement semantics remain. |
| 24 | [cp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cp.html) | implemented-unreviewed (`ws001-p025`) | `-R`/`-r`, `-H`/`-L`/`-P`, `-f`, `-i`, `-p` (set-ID bits dropped when the owner cannot be kept), new-file and new-directory modes, same-file and into-itself refusal, directories without `-R`, special files with and without `-R`, and the installer's extensions pass 47 host cases and the amd64 guest; the installer's calls give the same tree and report as before. yesexpr locale remains. |
| 26 | [csplit](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/csplit.html) | implemented-unreviewed (`ws001-p028`) | `/rexp/` and `%rexp%` with offsets, line numbers, `{num}` repetition, `-f`/`-n`/`-s`/`-k`, search start rules, and error cleanup pass 21 host cases and the amd64 guest. |
| 28 | [cut](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cut.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). `-b`/`-c`/`-f`/`-d`/`-s`/`-n` pass; multibyte `-c` remains (LIBC-CTYPE-01). |
| 29 | [cxref](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cxref.html) | P1 known incompatibility | Token-based references are not a complete C translation-unit analysis; preprocessing options, declarations/scopes, output formats, width, and diagnostics need full implementation. |
| 30 | [date](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/date.html) | implemented-unreviewed (`ws001-p030`) | `-u`, every `strftime()` conversion of the libc with E/O modifiers, the default format, TZ zones, and clock setting from `mmddhhmm[[cc]yy]` with range checks pass 12 host cases, the amd64 guest, and a guest clock-setting check; Issue 8 field widths/flags and LC_TIME remain. |
| 31 | [dd](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/dd.html) | implemented-unreviewed (`ws001-p032`) | Every operand and `conv=` value (POSIX conversion tables, block/unblock with truncation counts, swab, case, sync, notrunc, noerror), expressions, seek truncation, statistics, and SIGINT reporting pass the host cases, a pty case, and the amd64 guest; read-error recovery with noerror is unproved. |
| 32 | [delta](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/delta.html) | P1 known incompatibility | Only `-y` is parsed; required options, MR/comment rules, p-file selection, SID/permission cases, weave interoperability, signals, and transactional recovery are incomplete. |
| 33 | [df](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/df.html) | implemented-unreviewed (`ws001-p034`) | Every mounted file system from the kernel mount table without operands, the file system of each operand (same device, or a mounted device special file), `-k`, `-P`, `-t` (XSI, same output), 512/1024-byte units without overflow, rounded-up capacity, and failures pass guest cases; non-`-P` formats beyond the portable one are not provided. |
| 34 | [diff](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/diff.html) | implemented-unreviewed (`ws001-p029`) | A minimal longest-common-subsequence difference (Myers, linear space) with normal, `-c`/`-C`, `-u`/`-U`, `-e`, and `-f` output, `-b`, `-r` headers, directory/file pairing, stdin, missing-newline notes, and statuses 0/1/2 passes 29 host cases, 2000 random pairs applied by GNU patch/ed with GNU `--minimal` change counts, and the amd64 guest; the installer's `-r -q --metadata` output is unchanged. |
| 35 | [dirname](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/dirname.html) | implemented-unreviewed (`ws001-p012`, `ws001-p037`) | Empty/no-slash, chosen double-slash, all/trailing/repeated slash, long operand, `--`, usage, and host broken-stdout cases pass. Localized diagnostics, allocation-failure injection, and direct guest failure evidence remain. Several strings (one result per line) and GNU `-z` are taken by the user's decision for WS045 (`ws001-p037`). |
| 36 | [du](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/du.html) | implemented-unreviewed (`ws001-p034`) | `-a`/`-s`, `-k` (rounded up), `-x`, `-H`/`-L`, post-order directories, hard links and followed links counted once across operands, loop diagnostics, unreadable directories, and statuses pass host cases against GNU du, a pinned loop case, and guest cases on UFS/devfs/tmpfs; devfs `/dev/fd/N` appears as a loop (finding in phase034). |
| 37 | [echo](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/echo.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). Also a command built from the shell's source. |
| 38 | [ed](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ed.html) | P1 known incompatibility (P0 resolved 2026-08-24) | The independent local replacement provides a checked memory line store, basic addresses and edit commands, BRE substitution, `g`/`v`, single-operation undo, reads, and atomic sibling-file writes.  Relative/mark/BRE addresses, the remaining commands, temporary backing storage, signal recovery, exact newline/byte-count/diagnostic behavior, metadata preservation, and locale remain. |
| 39 | [env](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/env.html) | implemented-unreviewed (`ws001-p024`) | `/usr/bin/env` handles `-i`/`-`/`--`, assignments, PATH of the new environment, scripts without `#!`, write errors, and 125/126/127 (18 host cases, amd64 guest); the incomplete sh `env` builtin was removed. |
| 41 | [expand](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/expand.html) | implemented-unreviewed (`ws001-p027`) | Repeating and listed tab stops (commas or blanks), one space past the last stop, backspace, several files and stdin, and invalid lists pass the host cases and the amd64 guest; multibyte column widths remain (LIBC-CTYPE-01). |
| 43 | [false](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/false.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). Also a command. |
| 46 | [file](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/file.html) | P1 known incompatibility | Hard-coded recognition and `-L` only; required magic-file options and processing, MIME mode, default database, locale descriptions, special files, and error behavior are absent. |
| 47 | [find](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/find.html) | P2 incomplete proof | A useful expression parser/traversal exists; precedence/action edge cases, `-exec ... +`, `-ok` locale prompt, link loops, races, permissions, mount boundaries, and interruption need full review. |
| 48 | [fold](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/fold.html) | implemented-unreviewed (`ws001-p027`) | Terminal columns (tab, backspace, carriage return), `-b`, `-s`, over-wide characters, missing final newline, and invalid widths pass 13 host cases and the amd64 guest; multibyte widths remain. |
| 49 | [fuser](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/fuser.html) | P2 incomplete proof | Kernel query works in QEMU; permissions, all reference kinds, mount/block-device `-c` semantics, `-f`, `-u` identity, races, multiple operands, and exact output/status remain. |
| 51 | [get](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/get.html) | P1 known incompatibility | Parses only `-e`, `-k`, `-p`, `-s`, and `-r`; the remaining selection/listing/cutoff/include/exclude behavior, keyword rules, permissions, and classic histories are incomplete. |
| 53 | [getopts](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/getopts.html) | implemented-unreviewed (WS042) | The shell and its builtins were rewritten by WS042 against dash (`plan/tools/sh/sh-diff.py`: host 1417/1425 at WS042 completion, 1437/1458 in the ws001-p024 rerun; amd64 guest 1388/1425 with environment differences only). |
| 55 | [grep](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/grep.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). GNU options are WS045's; multibyte/locale matching remains. |
| 57 | [head](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/head.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). |
| 58 | [iconv](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/iconv.html) | P1 known incompatibility | Supports UTF-8 validation/copy only; conversions, `-c`, `-s`, `-l`, encoding aliases/state, incomplete sequences, locale defaults, and streaming boundary behavior are absent. |
| 59 | [id](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/id.html) | implemented-unreviewed (`ws001-p026`) | Default format, `-G`/`-g`/`-u` with `-n`/`-r`, group-list order without duplicates, user operands from the passwd/group databases, and option errors pass 15 host cases and the amd64 guest. |
| 60 | [ipcrm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ipcrm.html) | P2 incomplete proof | ID/key removal works in QEMU; complete option combinations, invalid/stale IDs, ownership/permission, races, multiple objects, diagnostics, and partial-failure status remain. |
| 61 | [ipcs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ipcs.html) | P2 incomplete proof | Object enumeration works; every selection/detail option, field units/headings, users/groups/times, removed/racing objects, permission errors, and locale formatting need review. |
| 63 | [join](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/join.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). Locale collation remains. |
| 64 | [kill](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/kill.html) | implemented-unreviewed (`ws001-p030`) | `-s`, `-name`/`-number` (any case, optional SIG), the null signal, `-l` with exit statuses above 128, negative process groups after `--`, and failures pass the host cases, a pinned case, and the amd64 guest; realtime signal names remain. |
| 66 | [link](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/link.html) | implemented-unreviewed (`ws001-p013`, `ws001-p030`) | Rewritten to the coding standard; operand count, existing targets, missing sources, and directories pass `link-unlink-test.sh`, the host cases, and the amd64 guest. |
| 67 | [ln](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ln.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). |
| 68 | [locale](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/locale.html) | P1 known incompatibility | Shared artifact metadata exists, but supported-name discovery is restricted, and `-a`/`-m` plus `-c`/`-k` output, quoting, category/keyword coverage, environment precedence, and locale errors need completion. |
| 69 | [localedef](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/localedef.html) | P1 known incompatibility | Checked artifact writing exists, but charmap/source grammar, symbolic characters, all category keywords, collation rules, `copy`, ellipses, diagnostics, `-u`, portability, and atomic installation are incomplete. |
| 71 | [logname](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/logname.html) | implemented-unreviewed (`ws001-p030`) | Operands are refused and the missing-login case fails; the host cases and the amd64 guest pass; a real login session in QEMU remains unproved. |
| 73 | [ls](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ls.html) | P1 known incompatibility | Several common options exist, but required Issue 8 option/output combinations, locale collation/character display, owner/group/time formats, symlink operands, recursion cycles, and errors are incomplete. |
| 74 | [m4](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/m4.html) | P1 known incompatibility (P0 resolved 2026-08-24) | The independent local replacement provides scanning/rescanning, positional arguments, definitions, conditionals, includes, common string/arithmetic builtins, quote changes, and memory diversions.  `changecom`, definition stacks/introspection, indirect invocation, wrapping/temp/system/trace builtins, complete checked `eval`, exact quote/comment/locale semantics, signals, and temporary-file diversions remain. |
| 78 | [mesg](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mesg.html) | P2 incomplete proof | ws001-p040 (2026-09-27): terminal search over fds 0–2, `y`/`n`/`--` parsing, permission-bit preservation, diagnostics and statuses pass the host pty test (9/9). After the BUG-067 devfs fix the QEMU console case passes (pinned 31/31). |
| 79 | [mkdir](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mkdir.html) | implemented-unreviewed (`ws001-p026`) | `-m` from a=rwx set exactly, `-p` parents with u+wx, existing directories, slashes, and failures pass 15 host cases and the amd64 guest. |
| 80 | [mkfifo](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mkfifo.html) | implemented-unreviewed (`ws001-p026`) | `-m` from a=rw set exactly (permission bits only), umask default, and failures pass 8 host cases and the amd64 guest. |
| 83 | [mv](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mv.html) | implemented-unreviewed (`ws001-p025`) | `-i`/`-f` (last wins), the terminal prompt for unwritable destinations, same-file refusal, directory/non-directory rules, and cross-file-system moves (copy as `cp -pRP` keeping hard links, then removal) pass 29 host cases, 3 pty cases, and the amd64 guest. |
| 84 | [newgrp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/newgrp.html) | P2 incomplete proof | Membership/password and login mode paths exist; real set-ID/session behavior, supplementary groups, environment reset, shell replacement, audit/security failures, and interactive QEMU cases remain. |
| 87 | [nl](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/nl.html) | implemented-unreviewed (`ws001-p027`) | Every option, `a`/`t`/`n`/`pBRE` types, `-l` blank grouping, logical-page delimiters with per-section restarts and `-p`, and number formats pass 15 host cases and the amd64 guest. |
| 88 | [nm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/nm.html) | P1 known incompatibility | Checked ELF/archive parsing exists, but several accepted options are ignored or incomplete; standard output formats, radix/sort/undefined/dynamic symbols, archive labels, malformed objects, and non-ELF policy need review. |
| 89 | [nohup](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/nohup.html) | implemented-unreviewed (`ws001-p024`) | `nohup.out`/`$HOME/nohup.out` (0600, append), stderr routing, terminal stdin, and 126/127 pass 9 host cases, 6 pty cases, and the amd64 guest. |
| 90 | [od](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/od.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). |
| 91 | [paste](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/paste.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). |
| 92 | [patch](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/patch.html) | implemented-unreviewed (`ws001-p033`) | Normal, context, unified and ed differences, several files per patch, filename determination with `-p` and `Index:`, `-b`/`-d`/`-D`/`-i`/`-l`/`-N`/`-o`/`-r`/`-R`, offset and fuzz, context-format rejects, atomic replacement keeping the mode, statuses 0/1/2, and diagnostics only on stderr pass the host cases against GNU patch, pinned POSIX cases, 2000 random diff/patch pairs, and the amd64 guest; SCCS retrieval and the interactive filename prompt are not done. |
| 93 | [pathchk](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pathchk.html) | implemented-unreviewed (`ws001-p030`) | System limits from `pathconf`, non-directory and unsearchable prefixes, `-p` portability limits and characters, and `-P` pass the host cases and the amd64 guest. |
| 94 | [pax](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pax.html) | P1 known incompatibility | Useful ustar/pax read/write/copy exists, but most standard options and formats, pattern selection, ownership/modes/times/links/specials, append/update semantics, substitutions, volume/error recovery, and security cases remain. |
| 95 | [pr](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pr.html) | implemented-unreviewed (`ws001-p028`) | All options, the POSIX header format and date, page filling/trailers/form feeds, balanced and across columns, merge, numbering, tab expansion/compression, offsets, and `+page` pass 28 host cases (header spacing and column tabs normalized against GNU) and the amd64 guest; terminal pauses (`-p`, `-f`) and multibyte widths remain unproved. |
| 96 | [printf](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/printf.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). Also a command (`/bin/printf`) built from the shell's source. |
| 97 | [prs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/prs.html) | P1 known incompatibility | Only `-d` and `-r` are parsed and the data-spec set is partial; cutoff/all-delta selection, every keyword/escape, locale/time, malformed/classic histories, and diagnostics remain. |
| 98 | [ps](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ps.html) | implemented-unreviewed (`ws001-p044`) | -o name=header to the end of the argument, the XCU fields and headers, column widths, additive selections and the default (effective user in the caller's session), -f's command line (BUG-274) and `[dd-]hh:mm:ss` pass 15 host cases against a fake /dev/system. The kernel keeps no start time (pcpu, etime are "-"), no real IDs (the effective ones stand for them) and no terminal names (-t is not taken). |
| 99 | [pwd](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pwd.html) | implemented-unreviewed (`ws001-p024`) | New `/bin/pwd` with `-L`/`-P` and `$PWD` validation passes 11 host cases and the amd64 guest; the shell builtin is WS042's. |
| 100 | [read](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/read.html) | implemented-unreviewed (WS042) | The shell and its builtins were rewritten by WS042 against dash (`plan/tools/sh/sh-diff.py`: host 1417/1425 at WS042 completion, 1437/1458 in the ws001-p024 rerun; amd64 guest 1388/1425 with environment differences only). |
| 104 | [rm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/rm.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). Terminal prompts are not covered by the cases. |
| 105 | [rmdel](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/rmdel.html) | P1 known incompatibility | Basic `-r` SID removal exists; full leaf/branch/release constraints, ownership, pending edits, MR/history preservation, classic weave, locking interruption, and diagnostics remain. |
| 106 | [rmdir](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/rmdir.html) | implemented-unreviewed (`ws001-p026`) | `-p` prefix removal stopping at the first failure, slashes, non-empty/non-directory/missing operands pass 10 host cases and the amd64 guest. |
| 107 | [sact](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sact.html) | P2 incomplete proof | Shared p-file display exists; multiple/no pending edits, malformed/classic files, operand naming, permissions, output failure, diagnostics, and locale behavior need review. |
| 108 | [sccs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sccs.html) | P1 known incompatibility | Basic argument-safe dispatch exists, but standard options, directory/project-prefix rewriting, command-specific flags, bulk operands, exit propagation, and full subcommand set/format compatibility are incomplete. |
| 109 | [sed](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sed.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). GNU extensions are WS045's; locale-dependent BRE behavior remains. |
| 110 | [sh](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sh.html) | implemented-unreviewed (WS042) | The shell and its builtins were rewritten by WS042 against dash (`plan/tools/sh/sh-diff.py`: host 1417/1425 at WS042 completion, 1437/1458 in the ws001-p024 rerun; amd64 guest 1388/1425 with environment differences only). Interactive job control and line editing pass 41/41 on the guest console. |
| 111 | [sleep](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sleep.html) | implemented-unreviewed (`ws001-p030`) | Integer and fractional operands, invalid operands, and SIGALRM ending the sleep normally pass the host cases and the amd64 guest. |
| 112 | [sort](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sort.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). Locale collation beyond the POSIX locale remains (LIBC-COLLATE-01). |
| 113 | [split](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/split.html) | implemented-unreviewed (`ws001-p028`) | `-l`, `-b` with k/m, `-a`, prefixes, suffix exhaustion (status 1, pieces kept), empty input, stdin, and missing final newlines pass 13 host cases and the amd64 guest. |
| 114 | [strings](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/strings.html) | implemented-unreviewed (`ws001-p030`) | `-a`, `-n`, `-t d/o/x`, stdin, and invalid options pass the host cases and the amd64 guest; multibyte printability remains. |
| 116 | [stty](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/stty.html) | implemented-unreviewed (`ws001-p036`) | Every mode and its negation, the character-size and delay groups, speeds, control characters (`^X`, `^?`, `^-`, `undef`), `min`/`time`, every combination mode, `-g` round trips, `-a`, `rows`/`cols`/`size`, applying all operands at once with read-back verification, and errors pass a host pty test (cross-checked with GNU stty) and guest cases on the console; `<termios.h>` gained the XSI fill/delay bits and `<unistd.h>` `_POSIX_VDISABLE`; the kernel accepts CS8 only. |
| 117 | [tabs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tabs.html) | P2 incomplete proof | Major predefined forms, explicit lists, `-T`, and terminfo output exist; exact historical layouts, `+m`, terminal width/margins, tty errors, malformed data, output interruption, and runtime terminal tests remain. |
| 118 | [tail](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tail.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). `-f` on growing files is not covered by the cases. |
| 120 | [tee](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tee.html) | implemented-unreviewed (`ws001-p018`) | `-a`/`-i`, dynamic output count, robust writes, open/write continuation, and final status pass; deterministic partial-I/O, allocation, close, descriptor, and locale review remain. |
| 121 | [test](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/test.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). Also a command built from the shell's source; the `[` executable is Future Work F-005. |
| 122 | [time](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/time.html) | implemented-unreviewed (`ws001-p024`) | `-p` format, user/system CPU from `wait4`, 126/127 and signal statuses pass 10 host cases and the amd64 guest. |
| 124 | [touch](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/touch.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). |
| 125 | [tput](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tput.html) | P1 known incompatibility | Shared checked terminfo lookup/expansion exists, but only the local capability vocabulary is supported; standard operand forms, `clear`/`init`/`reset`, booleans/numbers/statuses, parameter language, tty/output errors, and broad database compatibility remain. |
| 126 | [tr](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tr.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). Equivalence classes and multibyte sets remain (LIBC-CTYPE-01). |
| 127 | [true](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/true.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). Also a command. |
| 129 | [tty](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tty.html) | implemented-unreviewed (`ws001-p030`) | Statuses 0/1/2 (3 for a write error), the historical `-s`, and invalid arguments pass the host cases and the amd64 guest; console/pty names in QEMU remain unproved. |
| 130 | [type](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/type.html) | implemented-unreviewed (WS042) | The shell and its builtins were rewritten by WS042 against dash (`plan/tools/sh/sh-diff.py`: host 1417/1425 at WS042 completion, 1437/1458 in the ws001-p024 rerun; amd64 guest 1388/1425 with environment differences only). |
| 132 | [umask](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/umask.html) | implemented-unreviewed (WS042) | The shell and its builtins were rewritten by WS042 against dash (`plan/tools/sh/sh-diff.py`: host 1417/1425 at WS042 completion, 1437/1458 in the ws001-p024 rerun; amd64 guest 1388/1425 with environment differences only). |
| 133 | [unalias](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unalias.html) | implemented-unreviewed (WS042) | The shell and its builtins were rewritten by WS042 against dash (`plan/tools/sh/sh-diff.py`: host 1417/1425 at WS042 completion, 1437/1458 in the ws001-p024 rerun; amd64 guest 1388/1425 with environment differences only). |
| 134 | [uname](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/uname.html) | implemented-unreviewed (`ws001-p030`) | Field order, `-a`, the default, and invalid options/operands pass the host cases and the amd64 guest. |
| 135 | [uncompress](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/uncompress.html) | P2 incomplete proof | Streaming `.Z` decoding exists; all name/stdin/stdout/overwrite/metadata cases, malformed code transitions, truncation, short I/O, signals, full disk, and atomic replacement remain. |
| 136 | [unexpand](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unexpand.html) | implemented-unreviewed (`ws001-p027`) | Leading-blank conversion, `-a` (single spaces before a stop kept), `-t` lists implying `-a`, mixed blanks, and round trips with expand pass the host cases and the amd64 guest; multibyte widths remain. |
| 137 | [unget](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unget.html) | P2 incomplete proof | Basic pending-edit cancellation exists; `-n`/`-s`/`-r` combinations, multiple users/SIDs, q-file/locking recovery, permissions, classic files, and diagnostics remain. |
| 138 | [uniq](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/uniq.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). Locale collation remains. |
| 139 | [unlink](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unlink.html) | implemented-unreviewed (`ws001-p013`, `ws001-p030`) | Rewritten to the coding standard; missing files, directories, symbolic links, and operand count pass `link-unlink-test.sh`, the host cases, and the amd64 guest. |
| 145 | [val](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/val.html) | P1 known incompatibility | Basic local checksum/SID validation exists; required options and diagnostic bitmask/status behavior, every structural error, long/binary data, multiple/classic histories, and output errors remain. |
| 147 | [wait](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/wait.html) | implemented-unreviewed (WS042) | The shell and its builtins were rewritten by WS042 against dash (`plan/tools/sh/sh-diff.py`: host 1417/1425 at WS042 completion, 1437/1458 in the ws001-p024 rerun; amd64 guest 1388/1425 with environment differences only). |
| 148 | [wc](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/wc.html) | implemented-unreviewed (WS043) | Rewritten by WS043 against the GNU POSIX-mode differential cases (`plan/tools/utils/cases/`), which pass on the host and on the amd64 guest (ws001-p031 rerun: 948/948 with the WS001 cases). `-m` multibyte counting remains (LIBC-CTYPE-01). |
| 149 | [what](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/what.html) | P2 incomplete proof | Identification scanning and `-s` exist; binary/NUL/chunk-boundary markers, stdin/multiple files, malformed/long text, read/write errors, and exact no-match status need review. |
| 150 | [who](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/who.html) | implemented-unreviewed (`ws001-p035`) | Every option (`-abdHlmpqrstTu`), `am i`, the file operand, `%b %e %H:%M` local times, terminal state and idle time, headings, `-q`, and diagnostics pass host cases against GNU who on generated records, a pinned case, and guest cases on the live database; `<utmpx.h>` gained NEW_TIME/OLD_TIME/INIT_PROCESS, and nothing on zedBSD writes boot, clock or init records yet. |
| 152 | [xargs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/xargs.html) | implemented-unreviewed (`ws001-p024`, `ws001-p039`) | All options (`-0 -E -I -L -n -p -r -s -t -x`), quoting, logical lines, `-I` replacement, size limits counted both as `-s` and as the kernel counts, and statuses 123/124/125/126/127 pass 54 host cases, the `-p` pty case, and the amd64 guest. LC_MESSAGES yesexpr remains. GNU `-d`, `-P`, `-a`, `-o`, `-e`/`-i`/`-l` and the long options were added by the user's decision for WS045 (`ws001-p039`); host cases match GNU in POSIX and GNU mode and the guest. |
| 155 | [zcat](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/zcat.html) | P2 incomplete proof | `.Z` stdout decoding exists; multiple operands, suffix lookup, stdin, malformed/truncated streams, read/write interruption, broken pipe, diagnostics/status, and compatibility vectors remain. |

## 13. Update protocol

Every POSIX-related implementation turn shall update this master when it
changes the truth represented by a row.  A normal update consists of:

1. identify the stable subsystem/API/component/utility IDs affected before
   implementation;
2. update the implementation and tests without forcing unrelated pending work;
3. run the narrow host tests, required QEMU target, direct package/build gate,
   matrix checker, formatting check, and `git diff --check` applicable to the
   work;
4. update the corresponding subsystem/API/component row and cross-cutting row;
5. update the utility row here and its CSV implementation/test evidence;
6. update any phase-local progress table if the newly defined phase has one;
7. preserve remaining gaps as explicit hand-off text; and
8. mark an item `reviewed` only when no applicable Issue 8 checklist item is
   left without evidence.

When a finding is resolved, replace its hand-off with `resolved`, the test
target, and the resolution date; do not simply delete the historical row.  If
new work is found, add it immediately with a stable ID and the least-complete
defensible state.

The aggregate `make check` target is not used by this project unless the user
changes the current instruction.  Milestone-specific host and
`qemu-system-x86_64` targets are invoked directly.  No automatic commit is made.

Before implementation begins for a new phase, select its source rows from this
master and create a phase document that records the stable IDs, bounded scope,
dependency decisions, acceptance gates, and intended test targets.  Phase
numbers are planning labels assigned at that time; no Phase 11-or-later order
is reserved by the historical
[`phase000-010-legacy-plan.md`](history/phase000-010-legacy-plan.md) plan.

## 14. Completion and iteration policy

The project-level goal is reached only when:

- every required and enabled-option utility is reviewed, or a normative profile
  decision explicitly removes the requirement;
- every required service/provider is active in the documented conformance
  environment and has lifecycle/failure evidence;
- every supporting kernel, syscall, libc, locale, terminal, package, and shell
  row is reviewed for its declared scope;
- all cross-cutting failure classes have repeatable evidence;
- no external implementation remains in base;
- the conformance environment and package/service prerequisites are documented;
  and
- only then are Issue 8 advertisement macros considered for change.

Until that point, incomplete work is expected.  The correct iterative outcome
is a smaller verified step plus an accurate hand-off in this master, not an
unsupported completion claim.

### q136 shell-status observation (resolved 2026-09-27 by ws001-p031)

Resolved: on the amd64 guest with the current `/bin/sh`, `$?` in the next
interactive line is 127 after a command that was not found and 126 after
one that could not be run (`plan/ws001/tests/status-after-not-found.py`).
The original record follows.

The initial WS002 normal-session fixture attempted absent `/bin/true`; the
console printed `sh: /bin/true: not found` but the following interactive
`echo P032RESULT3 $?` printed zero. Evidence:
`plan/ws002/temp/q136-normal-pcat/run0/screen.log`. Do not count this
status-only fixture as command success. The replacement session test requires
actual pwd output. Investigate command-not-found status/interactive expansion
under WS001 before assuming a kernel teardown failure; no shell fix is claimed.
