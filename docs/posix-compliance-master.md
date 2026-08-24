# zedBSD POSIX.1-2024 compliance master

Last updated: 2026-08-24

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
| [`tests/posix-2024-utilities.csv`](../tests/posix-2024-utilities.csv) | machine-readable utility inventory, requirement/profile, status, implementation evidence, and test evidence |
| this master | project objectives, cross-component dependencies, subsystem/API progress, embedded unmet utility work, Phase 10 progress, and hand-off priorities |
| [`docs/phase9-posix-2024-audit.md`](phase9-posix-2024-audit.md) | detailed evidence and rationale from the 2026-08-24 first audit pass |
| [`docs/phase10-local-reimplementation.md`](phase10-local-reimplementation.md) | detailed removal and local reimplementation design for `bc`, `ed`, and `m4` |
| [`docs/posix2004.md`](posix2004.md) | historical execution plan and phase acceptance policy |
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
| `implemented-unreviewed` | 111 | all were inspected by the first Phase 9 audit |
| `deferred-stub` | 5 | service/provider blockers: `at`, `batch`, `crontab`, `logger`, `mailx` |
| `option-disabled` | 20 | outside the selected option profile |
| Phase 9 P0 policy conflicts | 3 | `bc`, `ed`, `m4` imported implementations |
| Phase 9 P1 known incompatibilities | 70 | source inspection confirms missing or incorrect required behavior |
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
| 4 | policy correction required | features were built, but imported `bc`, `ed`, and `m4` violate base policy |
| 5 | implementation milestone complete | process/file-use/SysV IPC interfaces work in QEMU; full semantic review remains |
| 6 | implementation milestone complete | development utilities exist; parser/format/option review remains |
| 7 | implementation milestone complete | `.Z` compression path exists; Issue 8 and failure-path review remains |
| 8 | implementation milestone complete | SCCS local format exists; classic interoperability and option coverage remain |
| 8.5 | implementation milestone complete | standalone packages and terminal stack exist; consumer/format review remains |
| 9 | first audit pass complete | 111/111 pending rows inspected; remediation and conformance closure remain 0/111 |
| 10 | planned | remove imported `bc`, `ed`, `m4` and replace them locally; see section 10 |
| 11 | deferred-provider | init and service-management work follows the local replacement milestone |

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
| KERN-CRED-01 | credentials and process identity | partial | `id`, `chown`, `chgrp`, `newgrp`, `ps` | numeric credentials work; audit real/effective IDs, supplementary groups, set-ID transitions, permission checks, and account-database integration |
| KERN-SIG-01 | signals and process groups | partial | `kill`, `sh`, `time`, `wait` | basic signaling works; prove process-group targets, job-control delivery, stopped/continued children, saved statuses, interruption, and permissions |
| KERN-WAIT-01 | child wait and accounting | partial | `wait`, `time`, `sh` | basic `waitpid()` works; multiple saved statuses, non-child behavior, signal status, stopped jobs, and user/system CPU accounting remain |
| KERN-TTY-01 | tty line discipline and termios | partial | `stty`, `sh`, `mesg`, `tty`, `newgrp` | canonical/raw and common flags exist; audit all required flags, speeds, control characters, VMIN/VTIME, drains/flushes, signals, and error atomicity |
| KERN-PTY-01 | pseudo terminals and controlling tty | implemented-unreviewed | shell/job control, terminal tests | UNIX98-style PTY path exists; prove session/controlling-terminal acquisition, foreground groups, hangup, permissions, and lifecycle |
| KERN-CLOCK-01 | clocks and clock setting | partial | `date`, `touch`, libc time | `clock_settime()` exists; prove privilege checks, valid ranges, clock selection, timezone-facing behavior, interruption, and filesystem timestamp integration |
| KERN-VFS-01 | pathname, metadata, and traversal semantics | partial | file utilities | core operations exist, but recursive symlink policies, mount boundaries, hard-link identity, metadata preservation, races, and exact error propagation need family tests |
| KERN-FSSTAT-01 | filesystem capacity/accounting | partial | `df`, `du` | provide and verify stable filesystem/device identity, portable block accounting, mount lookup, overflow behavior, and permission/error cases |
| KERN-RSRC-01 | priorities | reviewed | `nice`, `renice` | declared current scope has reviewed utility evidence; keep regression and permission/range tests |
| KERN-RSRC-02 | resource limits | reviewed | `ulimit`, shell | declared current scope has reviewed utility evidence; expand when new limit classes are exposed |
| KERN-BOOT-01 | init/service lifecycle | missing | deferred providers | real PID 1, reaping, startup/shutdown order, supervision, credentials, restart, and service failure reporting are Phase 11 work |

## 6. System call and kernel-interface tracker

Private ioctls are included because they are part of the current implementation
dependency even when they are not POSIX public APIs.

| ID | API/interface | State | Implementation evidence | Unmet work |
|---|---|---|---|---|
| API-AUDIT-00 | complete POSIX public interface inventory | missing | utility-driven dependency audit only | enumerate every required header, type, constant, function, and semantic option from Base Definitions/System Interfaces, then add stable API rows and tests |
| API-CLOCK-01 | `clock_settime()` | implemented-unreviewed | `ZEDBSD_SYS_clock_settime`, `kern_clock_settime()` | range/privilege/error/QEMU cases and `date` setting operands |
| API-RSRC-01 | `getpriority()`, `setpriority()`, `nice()` | reviewed | priority syscalls and reviewed `nice`/`renice` rows | retain regression across user/process-group selectors and permissions |
| API-RSRC-02 | `getrlimit()`, `setrlimit()` | reviewed | resource-limit syscalls and reviewed `ulimit` row | retain current-shell inheritance and hard/soft-limit regression |
| API-CONF-01 | `sysconf()`, `pathconf()`, `fpathconf()`, `confstr()` | reviewed | reviewed `getconf` row for the declared mapping scope | update generated mapping when public constants or limits change; API-AUDIT-00 may expand the scope |
| API-IPC-01 | `msgctl()` family | implemented-unreviewed | kernel IPC plus `libc/sysv-ipc.c` | full command, permission, limit, removal, and malformed-ID review |
| API-IPC-02 | `semctl()` family | implemented-unreviewed | kernel IPC plus `libc/sysv-ipc.c` | operation/array/undo semantics and concurrent lifecycle review |
| API-IPC-03 | `shmctl()` family | implemented-unreviewed | kernel IPC plus `libc/sysv-ipc.c` | attach/remove lifecycle, permissions, limits, and enumeration review |
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
| SHELL-CORE-01 | shell lexer/parser/expansion/executor | partial | `sh` and shell builtins | full grammar, expansions, redirects/here-docs, compound commands/functions, jobs/traps, special-builtin semantics |
| BUILD-PKG-01 | standalone base package interface | implemented-unreviewed | all base packages | retain direct build/install coverage for source lists, libraries, headers, data-only packages, `PREFIX=/`, and ordinary prefixes |
| BUILD-PROV-01 | base source provenance gate | planned | `bc`, `ed`, `m4`, future base additions | Phase 10 shall reject the known imported trees/fingerprints and require complete source review |

## 8. Service and provider tracker

| ID | Facility/command | State | Current behavior | Completion requirement |
|---|---|---|---|---|
| SVC-SCHED-01 | `at`, `batch`, `crontab` | deferred-provider | explicit failure commands | scheduler service, durable spool, credentials/environment, process groups, output delivery, lifecycle tests |
| SVC-LOG-01 | `logger` and system logging | deferred-provider | explicit failure command | logging endpoint/provider, libc integration, permissions, backpressure/storage failure, service tests |
| SVC-MAIL-01 | `mailx` | deferred-provider | explicit failure command | required mail provider and Send Mode; Receive Mode for enabled XSI/UP environment |
| SVC-PRINT-01 | `lp` | reviewed | tested no-destination failure in the declared no-device profile | preserve provider replacement rules if CUPS is selected |
| SVC-TALK-01 | `talk` | disabled-profile | installed failure command | local rendezvous provider and service only if UP/XSI profile is enabled |
| SVC-INIT-01 | PID 1 and service manager | missing | current kernel/user startup path is not the planned service manager | implement Phase 11 lifecycle, supervision, shutdown, and recovery design |

## 9. Cross-cutting unmet work

| ID | Area | State | Affected scope | Required evidence |
|---|---|---|---|---|
| CROSS-IO-01 | short reads/writes and `EINTR` | partial | stream, archive, filesystem, parser utilities | reusable host fault shim plus pipe/device/QEMU cases |
| CROSS-OUT-01 | broken stdout and close/flush errors | partial | every output-producing utility | exact non-zero status and no false success after partial output |
| CROSS-MEM-01 | allocation/size overflow | partial | dynamic arrays, parsers, recursion, binary formats | deterministic allocation injection, checked arithmetic, bounded nesting/input |
| CROSS-FS-01 | filesystem failures and atomic replacement | partial | editors, archives, SCCS, copy/move, generated databases | permissions, full disk, rename/fsync failure, interruption, rollback, no corrupted destination |
| CROSS-LOCALE-01 | locale/collation/multibyte | partial | most text and display utilities | non-C locale fixtures, invalid artifacts/sequences, boundary-split input, output verification |
| CROSS-SHELL-01 | current-shell state | partial | shell builtins | tests inside a running zshell for environment, cwd, umask, limits, traps, descriptors, and jobs |
| CROSS-BINARY-01 | malformed binary formats | partial | locale/catalog/terminfo/archive/ELF/SCCS/compression | truncation, invalid offsets/counts, integer overflow, fuzz corpus, bounded failure |
| CROSS-QEMU-01 | zedBSD runtime evidence | implemented-unreviewed | kernel-, tty-, credential-, IPC-, process-, service-dependent behavior | bounded headless amd64 tests with complete markers; add a target whenever host behavior is insufficient |
| CROSS-PROV-01 | external source exclusion | policy-conflict | current `bc`, `ed`, `m4` | execute Phase 10; do not preserve imported fallback/generated/compatibility files |

## 10. Phase 10 local replacement progress

Detailed implementation architecture and acceptance rules are defined in
[`docs/phase10-local-reimplementation.md`](phase10-local-reimplementation.md).
This table is the live progress summary and must be updated during each Phase
10 work unit.

| ID | Utility/work item | State | Replacement gates | POSIX review | Current hand-off |
|---|---|---|---:|---:|---|
| P10-GATE | provenance and transition gate | planned | 0/5 | n/a | define rejected imported manifest/fingerprints, remove obsolete m4 host compatibility layer, update evidence/notes |
| P10-BC | `bc` local reimplementation | policy-conflict | 0/5 | 0/checklist | imported Gavin D. Howard tree is still present; replace first with local arbitrary-length calculator, then add scale/language/math features |
| P10-ED | `ed` local reimplementation | policy-conflict | 0/5 | 0/checklist | imported OpenBSD tree is still present; replace with local line store/address/command/file engine, then complete BRE/global/undo/recovery |
| P10-M4 | `m4` local reimplementation | policy-conflict | 0/5 | 0/checklist | imported OpenBSD/generated parser tree is still present; replace with local streaming scanner/expander, then complete builtins/diversions/system behavior |
| P10-INTEG | host, standalone, amd64, QEMU integration | planned | 0/5 | n/a | add Phase 10 source, host, install, build, and `qemu-system-x86_64` gates without weakening conformance expectations |

The five replacement gates for each utility are:

1. imported files and obsolete compatibility support removed;
2. local production source builds with warnings as errors;
3. local replacement host test passes, including safe unsupported behavior;
4. direct package build/install and top-level `make -j16` pass; and
5. the installed local binary passes the bounded amd64 Phase 10 QEMU test.

Update `state`, the fraction, and the hand-off text as soon as a gate changes.
Do not wait for full conformance, and do not mark a gate early because an old
imported binary or stale image satisfies a test.

## 11. Phase 9 unmet utility register

This section embeds all 111 findings from the first Phase 9 audit.  It is the
human-readable work register; the utility CSV remains the machine-readable
status/evidence authority.  P0 means policy conflict, P1 means a confirmed
incompatibility, and P2 means that useful code exists but complete compliance
has not been proved.

| # | Utility | Finding | Hand-off |
|---:|---|---|---|
| 1 | [admin](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/admin.html) | P1 known incompatibility | Implements only `-i`, `-n`, `-r`, and `-y`; required user/flag/MR/descriptive-text administration, `-h`, `-z`, optional-option-argument rules, and classic SCCS interoperability are absent. |
| 2 | [alias](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/alias.html) | P2 incomplete proof | Basic set/list/query exists; quoting of displayed values, name/error cases, alias-substitution timing, and persistence in a running shell are not tested. |
| 3 | [ar](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ar.html) | P2 incomplete proof | Archive mutation and a SysV/GNU symbol index exist; complete operation/modifier interactions, `-C`, position/name edge cases, malformed archives, metadata, interruption, and output failures remain unproved. |
| 6 | [awk](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/awk.html) | P1 known incompatibility | Only trivial `print`/`$N` processing exists.  Implement the POSIX language locally, including options, grammar, EREs, variables, records/fields, arrays, functions, control flow, I/O, and diagnostics. |
| 7 | [basename](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/basename.html) | P2 incomplete proof | Basic suffix removal exists; `//`, root-only paths, trailing slashes, suffix-equals-result, empty results, locale, usage, and broken stdout need exact tests. |
| 9 | [bc](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/bc.html) | P0 policy conflict | The current package is an imported upstream implementation.  Remove it from base and replace it with a project-local arbitrary-precision parser/VM before conformance review. |
| 13 | [cat](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cat.html) | P2 incomplete proof | `-u` and copying exist; repeated stdin, partial reads/writes, `EINTR`, same-file/error paths, close errors, and broken stdout are not proved. |
| 14 | [cd](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cd.html) | P1 known incompatibility | Missing `-L`, `-P`, and `-e`, `CDPATH`, logical `..`, `PWD`/`OLDPWD` updates, correct unset-`HOME` behavior, and shell-environment tests. |
| 15 | [cflow](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cflow.html) | P1 known incompatibility | Uses a token heuristic rather than a conforming C preprocessing/declaration analysis; macro/include options are accepted without providing full preprocessing semantics. |
| 16 | [chgrp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chgrp.html) | P1 known incompatibility | Numeric GIDs only; group names, `-h`, recursive `-R` with `-H`/`-L`/`-P`, symlink rules, and traversal/error behavior are absent. |
| 17 | [chmod](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chmod.html) | P2 incomplete proof | Numeric and substantial symbolic modes plus `-R` exist; omitted-who/umask semantics, symlink/traversal policy, special bits, race/error cases, and exhaustive grammar tests remain. |
| 18 | [chown](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/chown.html) | P1 known incompatibility | Numeric UID/GID only; owner/group names, omitted components, `-h`, recursive link modes, and traversal/error semantics are absent. |
| 19 | [cksum](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cksum.html) | P2 incomplete proof | CRC path is plausible, but standard vectors at length boundaries, multiple files/stdin naming, read interruption, output/close failure, and accumulated exit status are not fully tested. |
| 20 | [cmp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cmp.html) | P1 known incompatibility | Missing `-l` and `-s`; independent short reads can be compared incorrectly, and `EINTR`, offsets, diagnostics, same-stdin, and close/error paths are incomplete. |
| 21 | [comm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/comm.html) | P1 known incompatibility | Column suppression exists, but comparison uses byte ordering rather than `LC_COLLATE`; sorted-input assumptions, long lines, read/write errors, and locale behavior are unproved. |
| 22 | [command](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/command.html) | P1 known incompatibility | Only ordinary dispatch and `-v` are recognized; `-p`, `-V`, lookup/reporting rules, special-builtin behavior, and 126/127 statuses are incomplete. |
| 23 | [compress](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/compress.html) | P1 known incompatibility | Classic `.Z` LZW is implemented, but Issue 8 algorithm-selection interfaces and complete overwrite, metadata, signal, full-disk, corrupted-stream, and replacement semantics remain. |
| 24 | [cp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cp.html) | P1 known incompatibility | Regular-file copying only; required options, directories, recursion, symlinks, special files, metadata preservation, interactive/force behavior, alias detection, and robust I/O are absent. |
| 26 | [csplit](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/csplit.html) | P1 known incompatibility | Supports one numeric split only; regex operands, repeats, `-f`/`-n`/`-s`/`-k`, multiple sections, cleanup rules, line zero/errors, and output failure handling are absent. |
| 28 | [cut](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cut.html) | P1 known incompatibility | `-b` and `-c` are byte-equivalent and `-n`/multibyte semantics are missing; field delimiter/suppression and list grammar need correction and boundary/I/O tests. |
| 29 | [cxref](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/cxref.html) | P1 known incompatibility | Token-based references are not a complete C translation-unit analysis; preprocessing options, declarations/scopes, output formats, width, and diagnostics need full implementation. |
| 30 | [date](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/date.html) | P1 known incompatibility | Always uses UTC, implements only a small format subset, and cannot set time; `-u`, operands, locale/timezone behavior, conversions, invalid dates, and permissions are incomplete. |
| 31 | [dd](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/dd.html) | P2 incomplete proof | A substantial operand/conversion path exists, but record accounting, `conv=` combinations, skip/seek/truncation, partial records, signals, `EINTR`, full-disk, and exact diagnostics need a dedicated matrix. |
| 32 | [delta](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/delta.html) | P1 known incompatibility | Only `-y` is parsed; required options, MR/comment rules, p-file selection, SID/permission cases, weave interoperability, signals, and transactional recovery are incomplete. |
| 33 | [df](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/df.html) | P1 known incompatibility | Only `-k` exists; `-P`/`-t`, correct filesystem/device identity, block accounting, operand resolution, default operand safety, locale formatting, and errors are incomplete. |
| 34 | [diff](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/diff.html) | P1 known incompatibility | Performs line-at-the-same-index comparison rather than a difference algorithm; output forms, context/unified hunks, whitespace/recursive options, binary/errors, and exit status 2 semantics are absent. |
| 35 | [dirname](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/dirname.html) | P2 incomplete proof | Basic lexical reduction exists; double slash, all-slash, trailing-slash, empty/long operand, locale, usage, and broken stdout cases need proof. |
| 36 | [du](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/du.html) | P1 known incompatibility | Only `-a` exists; `-s`/`-k`/`-x` and `-H`/`-L`, hard-link deduplication, mount/symlink/cycle rules, overflow, permissions, and traversal errors are absent. |
| 37 | [echo](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/echo.html) | P2 incomplete proof | Simple joining/newline exists; implementation-defined `-n`/backslash cases must be documented, and NUL/locale/output-error behavior needs running-shell tests. |
| 38 | [ed](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ed.html) | P0 policy conflict | The current package is imported OpenBSD `ed`.  Replace it with a project-local line editor before reviewing the extensive address, BRE, global, undo, file, signal, and recovery contract. |
| 39 | [env](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/env.html) | P1 known incompatibility | Implemented as a shell builtin that only prints the environment and rejects arguments; `-i`, assignments, utility execution, PATH lookup, and 126/127 statuses are absent. |
| 41 | [expand](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/expand.html) | P1 known incompatibility | Accepts only one tab width, not a tab list; multibyte/column handling, file-boundary state, malformed options, read/write errors, and locale semantics are incomplete. |
| 43 | [false](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/false.html) | P2 incomplete proof | Status behavior is trivial, but operand handling in the real shell, redirection failure, traps, and special-builtin execution context are not cited or tested. |
| 46 | [file](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/file.html) | P1 known incompatibility | Hard-coded recognition and `-L` only; required magic-file options and processing, MIME mode, default database, locale descriptions, special files, and error behavior are absent. |
| 47 | [find](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/find.html) | P2 incomplete proof | A useful expression parser/traversal exists; precedence/action edge cases, `-exec ... +`, `-ok` locale prompt, link loops, races, permissions, mount boundaries, and interruption need full review. |
| 48 | [fold](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/fold.html) | P1 known incompatibility | Byte-oriented width handling is incomplete for screen columns and multibyte characters; `-s`, backspace/tab/carriage return, long lines, and I/O errors need implementation/tests. |
| 49 | [fuser](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/fuser.html) | P2 incomplete proof | Kernel query works in QEMU; permissions, all reference kinds, mount/block-device `-c` semantics, `-f`, `-u` identity, races, multiple operands, and exact output/status remain. |
| 51 | [get](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/get.html) | P1 known incompatibility | Parses only `-e`, `-k`, `-p`, `-s`, and `-r`; the remaining selection/listing/cutoff/include/exclude behavior, keyword rules, permissions, and classic histories are incomplete. |
| 53 | [getopts](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/getopts.html) | P2 incomplete proof | Cluster scanning and silent errors exist; option arguments across argv, `OPTIND` resets, `OPTARG` unsetting, invalid variable/optstring, explicit args, and running-shell state need tests. |
| 55 | [grep](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/grep.html) | P1 known incompatibility | Only a subset of required options is accepted; multiple `-e`/`-f`, complete BRE/ERE/fixed semantics, binary/NUL, locale matching, long lines, output errors, and status 2 need work. |
| 57 | [head](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/head.html) | P2 incomplete proof | `-n` and `-c` exist; legacy syntax, multiple-file headers, zero/large counts, non-seekable short reads, repeated stdin, read/write errors, and locale diagnostics need tests. |
| 58 | [iconv](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/iconv.html) | P1 known incompatibility | Supports UTF-8 validation/copy only; conversions, `-c`, `-s`, `-l`, encoding aliases/state, incomplete sequences, locale defaults, and streaming boundary behavior are absent. |
| 59 | [id](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/id.html) | P1 known incompatibility | Numeric current-process `-u`/`-g`/`-G` only; user operands, names, real/effective selection, group names, combinations, and lookup errors are absent. |
| 60 | [ipcrm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ipcrm.html) | P2 incomplete proof | ID/key removal works in QEMU; complete option combinations, invalid/stale IDs, ownership/permission, races, multiple objects, diagnostics, and partial-failure status remain. |
| 61 | [ipcs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ipcs.html) | P2 incomplete proof | Object enumeration works; every selection/detail option, field units/headings, users/groups/times, removed/racing objects, permission errors, and locale formatting need review. |
| 63 | [join](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/join.html) | P1 known incompatibility | Only `-1` and `-2` exist; `-a`, `-v`, `-e`, `-o`, `-t`, repeated/unpairable fields, locale collation, long records, and read/write errors are absent. |
| 64 | [kill](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/kill.html) | P1 known incompatibility | Basic numeric/name signals and `-l` exist; required `-s` form, complete symbolic set/listing, process-group operands, range/parse rules, permission errors, and shell-builtin context need work. |
| 66 | [link](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/link.html) | P2 incomplete proof | Direct `link()` wrapper exists; exact operand count, directories, existing targets, cross-filesystem, permissions, diagnostics, and status are not cited by tests. |
| 67 | [ln](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ln.html) | P1 known incompatibility | Only two-operand `-s` exists; `-f`, `-L`/`-P`, directory/multiple operands, target-directory basename rules, overwrite/diagnostic behavior, and alias cases are absent. |
| 68 | [locale](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/locale.html) | P1 known incompatibility | Shared artifact metadata exists, but supported-name discovery is restricted, and `-a`/`-m` plus `-c`/`-k` output, quoting, category/keyword coverage, environment precedence, and locale errors need completion. |
| 69 | [localedef](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/localedef.html) | P1 known incompatibility | Checked artifact writing exists, but charmap/source grammar, symbolic characters, all category keywords, collation rules, `copy`, ellipses, diagnostics, `-u`, portability, and atomic installation are incomplete. |
| 71 | [logname](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/logname.html) | P2 incomplete proof | `getlogin()` path exists; no-login/session cases, stray operands, locale diagnostics, output failure, and QEMU login-session behavior are not tested. |
| 73 | [ls](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ls.html) | P1 known incompatibility | Several common options exist, but required Issue 8 option/output combinations, locale collation/character display, owner/group/time formats, symlink operands, recursion cycles, and errors are incomplete. |
| 74 | [m4](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/m4.html) | P0 policy conflict | The current package is imported OpenBSD `m4`.  Replace it locally before reviewing standard builtins, quoting/diversions, expression grammar, locale, nesting limits, signals, and output failures. |
| 78 | [mesg](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mesg.html) | P2 incomplete proof | Basic tty mode query/change exists; no-controlling-terminal, `y`/`n` parsing, unrelated permission-bit preservation, diagnostic/status, and QEMU tty tests remain. |
| 79 | [mkdir](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mkdir.html) | P2 incomplete proof | `-p` and `-m` exist; full symbolic mode grammar/umask, intermediate modes, existing paths, slash/symlink/race cases, partial failure, and diagnostics need tests. |
| 80 | [mkfifo](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mkfifo.html) | P2 incomplete proof | `-m` exists but numeric parsing alone is insufficient; symbolic modes/umask, multiple operands, existing paths, permissions, cleanup/error accumulation, and filesystem support need proof. |
| 83 | [mv](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/mv.html) | P1 known incompatibility | Rename-only subset with simple destination directories; `-f`/`-i`, cross-filesystem copy/remove, directories, symlinks/specials, metadata, self/subtree checks, and failure recovery are absent. |
| 84 | [newgrp](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/newgrp.html) | P2 incomplete proof | Membership/password and login mode paths exist; real set-ID/session behavior, supplementary groups, environment reset, shell replacement, audit/security failures, and interactive QEMU cases remain. |
| 87 | [nl](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/nl.html) | P1 known incompatibility | Only a few body/start/increment options exist; header/footer styles, delimiters, width/format/separator, regex numbering, blank grouping, section resets, and locale/I/O behavior are absent. |
| 88 | [nm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/nm.html) | P1 known incompatibility | Checked ELF/archive parsing exists, but several accepted options are ignored or incomplete; standard output formats, radix/sort/undefined/dynamic symbols, archive labels, malformed objects, and non-ELF policy need review. |
| 89 | [nohup](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/nohup.html) | P2 incomplete proof | Signal ignoring and execution exist; tty-dependent `nohup.out` redirection, mode/ownership, stderr duplication, HOME fallback, open failures, messages, and 126/127 statuses need tests. |
| 90 | [od](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/od.html) | P1 known incompatibility | Fixed hexadecimal dump only; `-A`, `-j`, `-N`, `-t`, `-v`, legacy operands, typed formats, duplicate suppression, endianness, offsets, and read/output failures are absent. |
| 91 | [paste](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/paste.html) | P2 incomplete proof | Parallel/serial and a delimiter string exist; escaped delimiters, empty delimiter/list cycling, unequal/empty files, repeated stdin, long lines, descriptor limits, and I/O errors need proof. |
| 92 | [patch](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/patch.html) | P1 known incompatibility | Minimal single-file unified-patch handling only; standard options, context/normal/ed formats, reversed/fuzzed hunks, pathname selection, rejects/backups, timestamps, safety, atomicity, and signals are absent. |
| 93 | [pathchk](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pathchk.html) | P1 known incompatibility | `-p` exists but Issue 8 `-P`, empty/leading-hyphen checks, component/path limits for the relevant directory, searchability/non-directory cases, locale, and status aggregation need work. |
| 94 | [pax](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pax.html) | P1 known incompatibility | Useful ustar/pax read/write/copy exists, but most standard options and formats, pattern selection, ownership/modes/times/links/specials, append/update semantics, substitutions, volume/error recovery, and security cases remain. |
| 95 | [pr](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pr.html) | P1 known incompatibility | Only a simple header/numbering path exists; columns/merge, page ranges, widths/lengths/margins, separators, form feeds, tabs, double spacing, locale/time, and error handling are absent. |
| 96 | [printf](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/printf.html) | P2 incomplete proof | Many conversions and escapes exist; format reuse, missing/extra arguments, numeric character constants, precision/width, locale, overflow/domain errors, `%b` corner cases, NUL, and output failure need running-shell tests. |
| 97 | [prs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/prs.html) | P1 known incompatibility | Only `-d` and `-r` are parsed and the data-spec set is partial; cutoff/all-delta selection, every keyword/escape, locale/time, malformed/classic histories, and diagnostics remain. |
| 98 | [ps](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/ps.html) | P1 known incompatibility | A kernel snapshot and common selection/output fields exist; complete POSIX/XSI option semantics, default selection, tty/session/group/user lists, field widths/headings, time/state values, races, and permissions remain. |
| 99 | [pwd](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/pwd.html) | P1 known incompatibility | Always calls `getcwd()` and accepts no options; `-L`/`-P`, valid logical `PWD`, removed/unsearchable directories, shell state, and output errors are absent. |
| 100 | [read](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/read.html) | P1 known incompatibility | Reads one bounded line into one variable/`REPLY`; `-r`, `-d`, multiple variables, `IFS` splitting, backslash/continuation, NUL/EOF, long input, and current-shell semantics are absent. |
| 104 | [rm](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/rm.html) | P1 known incompatibility | `-f` and recursive removal exist, but `-i`, `-d`, `-v`, write-protected prompting, root/dot protection, symlink traversal, deep/racing trees, locale prompts, and error aggregation are incomplete. |
| 105 | [rmdel](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/rmdel.html) | P1 known incompatibility | Basic `-r` SID removal exists; full leaf/branch/release constraints, ownership, pending edits, MR/history preservation, classic weave, locking interruption, and diagnostics remain. |
| 106 | [rmdir](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/rmdir.html) | P2 incomplete proof | `-p` exists; slash/dot/root handling, parent stopping and diagnostics, symlinks/nonempty paths, permissions/races, multiple operands, and partial-failure status need tests. |
| 107 | [sact](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sact.html) | P2 incomplete proof | Shared p-file display exists; multiple/no pending edits, malformed/classic files, operand naming, permissions, output failure, diagnostics, and locale behavior need review. |
| 108 | [sccs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sccs.html) | P1 known incompatibility | Basic argument-safe dispatch exists, but standard options, directory/project-prefix rewriting, command-specific flags, bulk operands, exit propagation, and full subcommand set/format compatibility are incomplete. |
| 109 | [sed](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sed.html) | P1 known incompatibility | Supports `-n`, `p`, `d`, and a narrow substitution syntax only; addresses, script files/`-e`/`-f`, BRE commands, hold/pattern spaces, branches, multiline behavior, writes, and errors are absent. |
| 110 | [sh](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sh.html) | P1 known incompatibility | Not a complete POSIX shell language or environment.  Implement and test full grammar, expansions, assignments, redirections/here-documents, compound commands/functions, execution/search, traps/jobs, special builtins, and error/status rules. |
| 111 | [sleep](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sleep.html) | P2 incomplete proof | One numeric duration and `nanosleep()` exist; accepted syntax/range, fractional precision, overflow, signal interruption/remainder policy, locale decimal point, stray operands, and diagnostics need tests. |
| 112 | [sort](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/sort.html) | P1 known incompatibility | In-memory bytewise ordering with only a small option subset; keys, locale collation, numeric/month/dictionary/fold/nonprinting modes, merge/check/output, stable ties, large data, and I/O errors are absent. |
| 113 | [split](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/split.html) | P1 known incompatibility | Basic `-l`/`-b` exists; `-a`, suffix exhaustion, size suffix grammar, zero/huge values, exact line/byte boundaries, file cleanup, existing outputs, short writes, and signals are incomplete. |
| 114 | [strings](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/strings.html) | P1 known incompatibility | Printable-byte scan with `-n` only; `-a`, `-t`, object-file sections, offset radices, locale/multibyte printability, long sequences, malformed objects, and I/O errors are absent. |
| 116 | [stty](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/stty.html) | P1 known incompatibility | Only a few flags and `raw` are handled; `-g`, speeds, control characters, rows/columns, complete modes, parse/application atomicity, non-tty errors, and exact restorable output are absent. |
| 117 | [tabs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tabs.html) | P2 incomplete proof | Major predefined forms, explicit lists, `-T`, and terminfo output exist; exact historical layouts, `+m`, terminal width/margins, tty errors, malformed data, output interruption, and runtime terminal tests remain. |
| 118 | [tail](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tail.html) | P1 known incompatibility | Buffers input and supports only `-n`; `-c`, `-f`, `-r`, origin/sign forms, legacy syntax, growing/truncated files, pipes, large inputs, overflow, and robust I/O are absent. |
| 120 | [tee](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tee.html) | P1 known incompatibility | `-a` exists, but `-i` is absent, outputs are capped at 32, and short writes, `EINTR`, broken outputs, signal behavior, descriptor/open failures, and continuation/status rules are incomplete. |
| 121 | [test](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/test.html) | P1 known incompatibility | Implements only small unary/binary arities; compound negation/parentheses/AND/OR, all primaries, precedence by argument count, integer errors, symlinks, permissions, and `[` form are incomplete. |
| 122 | [time](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/time.html) | P1 known incompatibility | No `-p`, reports only elapsed time, omits user/system CPU, mishandles normalized time subtraction, and lacks signal/exec status, locale format, and redirection tests. |
| 124 | [touch](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/touch.html) | P1 known incompatibility | Sets both times to now and always permits create; `-a`/`-m`/`-c`, `-r`, `-t`, `-d`, parsing/ranges/timezones, permissions, symlink policy, and partial failures are absent. |
| 125 | [tput](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tput.html) | P1 known incompatibility | Shared checked terminfo lookup/expansion exists, but only the local capability vocabulary is supported; standard operand forms, `clear`/`init`/`reset`, booleans/numbers/statuses, parameter language, tty/output errors, and broad database compatibility remain. |
| 126 | [tr](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tr.html) | P1 known incompatibility | Byte ranges with `-d`/`-s` only; `-c`/`-C`, character classes, equivalence classes, repetitions, escaping, multibyte/locale semantics, empty sets, and robust I/O are absent or unsafe. |
| 127 | [true](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/true.html) | P2 incomplete proof | Status behavior is trivial, but real-shell operand/redirection/trap/special-builtin context is not cited or exercised. |
| 129 | [tty](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/tty.html) | P2 incomplete proof | Basic terminal-name/status behavior exists; non-tty stdin, inaccessible names, exact diagnostics, stray options/operands, output failure, and QEMU console/pty cases need proof. |
| 130 | [type](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/type.html) | P1 known incompatibility | Basic builtin/PATH reporting exists, but aliases/functions/keywords, multiple names, command hashing, not-found diagnostics/status, quoting, and running-shell resolution order are incomplete. |
| 132 | [umask](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/umask.html) | P1 known incompatibility | Numeric query/set only; `-S`, symbolic masks, omitted-who semantics, invalid input without mutation, and persistence/child inheritance in a running shell are absent. |
| 133 | [unalias](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unalias.html) | P2 incomplete proof | Named removal and `-a` exist; option termination, multiple-name partial failure, exact status/diagnostic, and alias expansion/cache effects in a running shell need tests. |
| 134 | [uname](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/uname.html) | P2 incomplete proof | Required option letters are present; combined/default ordering, field values, unknown options, locale diagnostics, kernel failures, and broken stdout need exact tests. |
| 135 | [uncompress](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/uncompress.html) | P2 incomplete proof | Streaming `.Z` decoding exists; all name/stdin/stdout/overwrite/metadata cases, malformed code transitions, truncation, short I/O, signals, full disk, and atomic replacement remain. |
| 136 | [unexpand](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unexpand.html) | P1 known incompatibility | `-a` and one integer width exist, but tab lists, leading-only rules across boundaries, backspace/multibyte columns, file boundaries, malformed lists, and I/O errors are incomplete. |
| 137 | [unget](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unget.html) | P2 incomplete proof | Basic pending-edit cancellation exists; `-n`/`-s`/`-r` combinations, multiple users/SIDs, q-file/locking recovery, permissions, classic files, and diagnostics remain. |
| 138 | [uniq](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/uniq.html) | P1 known incompatibility | `-c`/`-d`/`-u` exist, but `-f`/`-s`, locale collation, long/NUL records, input/output aliasing, close/output errors, and option combinations are incomplete; output stream cleanup is defective. |
| 139 | [unlink](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/unlink.html) | P2 incomplete proof | Direct `unlink()` wrapper exists; directory/symlink/missing/permission cases, `--`, exact diagnostics/status, and filesystem/race behavior lack cited tests. |
| 145 | [val](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/val.html) | P1 known incompatibility | Basic local checksum/SID validation exists; required options and diagnostic bitmask/status behavior, every structural error, long/binary data, multiple/classic histories, and output errors remain. |
| 147 | [wait](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/wait.html) | P1 known incompatibility | Supports at most one numeric PID and otherwise waits only tracked/available children; multiple operands, job IDs, saved statuses, unknown/non-child rules, signals, 127 behavior, and running-shell jobs are incomplete. |
| 148 | [wc](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/wc.html) | P1 known incompatibility | `-c`/`-l`/`-w` exist; `-m`, multibyte/locale word semantics, multiple-file totals/labels, repeated stdin, huge counts/overflow, read interruption, and output failure are absent. |
| 149 | [what](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/what.html) | P2 incomplete proof | Identification scanning and `-s` exist; binary/NUL/chunk-boundary markers, stdin/multiple files, malformed/long text, read/write errors, and exact no-match status need review. |
| 150 | [who](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/who.html) | P1 known incompatibility | Ignores options/operands and prints a narrow utmpx view with fixed UTC formatting; `am i`/`am I`, headings/state/writeability/idle/PID fields, locale/time, file operands, and errors are absent. |
| 152 | [xargs](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/xargs.html) | P1 known incompatibility | Tokenizes input then executes exactly once; batching and size limits, quoting/escaping correctness, EOF strings, `-I`/`-L`/`-n`/`-s`/`-x`/`-p`/`-t`/`-r`/`-0`, empty input, and 123--127 statuses are absent. |
| 155 | [zcat](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/zcat.html) | P2 incomplete proof | `.Z` stdout decoding exists; multiple operands, suffix lookup, stdin, malformed/truncated streams, read/write interruption, broken pipe, diagnostics/status, and compatibility vectors remain. |

## 12. Update protocol

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
6. during Phase 10, update the `P10-*` progress fraction in section 10;
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

## 13. Completion and iteration policy

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
