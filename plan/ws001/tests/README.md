# WS001 shared test index

Parent: [WS001](../ws.md)

This directory owns the POSIX workstream's test-case catalog and its reusable
Phase-specific fixtures. Historical project-wide executables remain under
repository `/tests` where the top-level Makefile and tooling already use them;
new WS001-only evidence is kept here rather than copied into `.internal/`.

| Phase(s) | Test cases / executable evidence |
| --- | --- |
| `ws001-p000` | `tests/posix-2024-utilities.csv`, matrix/import/check tooling, build/provenance gates |
| `ws001-p001` | `tests/test-deferred-stubs-host.sh`, `tests/deferred-stub-rootfs-test.py`, `tests/deferred-stub-qemu-test.py` and fixtures |
| `ws001-p002` | `tests/test-posix-phase2a-host.sh`, `phase2b`, `phase2c-priority`, shell-builtin host tests, `tests/posix-phase2-qemu-test.py` and fixtures |
| `ws001-p003` | locale/gencat/localedef host tests, catalog/locale fixtures, `tests/posix-phase3-qemu-test.py` |
| `ws001-p004` | `test-posix-{bc,ed,find,m4,pax}-host.sh`, Phase 4 fixtures, `tests/posix-phase4-qemu-test.py` |
| `ws001-p005` | credential/priority/IPC focused tests and `tests/posix-phase5-qemu-test.py` |
| `ws001-p006` | `tests/test-posix-development-host.sh`, development fixtures, `tests/posix-phase6-qemu-test.py` |
| `ws001-p007` | `tests/test-posix-compress-host.sh`, compression fixture, `tests/posix-phase7-qemu-test.py` |
| `ws001-p008` | `tests/test-posix-sccs-host.sh`, SCCS fixtures, `tests/posix-phase8-qemu-test.py` |
| `ws001-p085` | terminal stack/tools host tests, curses/terminfo fixtures, `tests/posix-phase85-qemu-test.py` |
| `ws001-p009` | utility matrix check, source/test audit, format/provenance review evidence recorded in the Phase report |
| `ws001-p010` | `tests/test-phase10-local-source.sh`, local bc/ed/m4 host tests, standalone installs, top build, Phase 10 QEMU target |
| `ws001-p014` | `shell-job-control-test.sh` plus instrumented shell hooks/PTY probe; `qemu-shell-job-control.sh` against the installed amd64 `/bin/sh`（2026-09-24に両方削除。host の方は header の置き場所の変更で compile できず、QEMU の方は旧出力に依存していた。job control は `plan/ws034/tests/sh-jobs-interactive.py` が確かめる） |
| `ws001-p015` | `base-c-style-audit-test.sh`, `base-c-style-inventory.sh`, and `run-base-command-host-test.sh` |
| `ws001-p016` | `run-lpd-host-test.sh` plus `fake-lpd-test.py`; `qemu-base-utility-smoke.sh`（2026-09-23に削除。IDE・BIOS起動で古く、実機はUEFIのUSB起動しか使っていない。起動確認は `plan/tools/boot-test.sh`）proves the native guest binary/error path while guest networking remains a handoff |
| `ws001-p017` | `run-cmp-host-test.sh` and the shared `qemu-base-utility-smoke.sh` native guest cell（2026-09-23に削除。上記参照） |
| `ws001-p018` | `run-tee-host-test.sh` and the shared `qemu-base-utility-smoke.sh` native guest cell（2026-09-23に削除。上記参照） |
| `ws001-p019` | `userland-file-header-audit.py`, `userland-file-header-audit-test.sh`, `migrate-userland-file-headers.py`, the deterministic 269-file inventory and body hashes, assembler preprocessing, and the configured build gate |
| `ws001-p020` | `userland-c-style-audit.py`, `userland-c-style-audit-test.sh`, `userland-c-body-audit.py`, three idempotent refactoring tools, the deterministic 258-file review ledger, Phase 19 header regression, declaration-after-statement compiler gate, and configured build; Phase is honestly uncleared by recorded body/semantic residuals |
| `ws001-p021` | `refactor-userland-ansi-c.py`, `refactor-userland-semantic-layout.py`, `refactor-userland-control-blocks.py`, the extended zero-residual body audit and `userland-c-body-audit-test.sh`, the 269-file header gate, structural/header fixtures, the 258-row review ledger, configured `make -j16`, and whitespace validation; q005 adds semantic paragraphs and explicit call results, while q006 enforces symmetric decision braces, multi-line loop blocks, exact block-entry indentation, and nonempty block entries across all implementations |

The concurrent q042 branch used `ws001-p015` and `ws001-p016` before the
Principal-authored registry above was merged.  The two labels below are
pre-merge historical identifiers, not additional active uses of those combined
IDs.  Their evidence and output labels remain indexed here unchanged; the
active Phase documents and dependencies now use `ws001-p022` for credential-
aware VFS creation and `ws001-p023` for directory `fsync`.

| Pre-merge q042 identifier | Test cases / executable evidence |
| --- | --- |
| `ws001-p015` (credential-aware VFS creation) | `credential-creation-request-host-test.mk` verifies the exact production authorization/parent-attribute locking boundary; `credential-vfs-qemu.mk` builds native UFS1, UFS2, tmpfs, FAT, and normal overlay-root credential/object probes; `fat-native-vfs-host-test.c` retains the exact FAT production-path regression |
| `ws001-p016` (directory fsync) | the host fixture was removed on 2026-10-31 (it no longer built: the UFS driver fragments it linked are gone); the q050 QEMU runner below remains |

Canonical q050 completion extends those retained fixtures as follows:

| Active Phase | Additional executable evidence |
| --- | --- |
| `ws001-p022` | `credential-vfs-qemu.noct` drives the five-launch backend/reopen/remount matrix built by `credential-vfs-qemu.mk` |
| `ws001-p023` | the shared q050 QEMU runner supplies abrupt-stop/remount durability evidence |

When a new Phase fixes a ledger item, add its normative case, failure case,
executable path, and environment here before marking the row reviewed.

## ws001-p011 basename

`basename-test.sh` covers empty, all-slash, double-slash, trailing-slash,
suffix-equal, suffix-removal-to-empty, `--`, usage, and output-failure cases:

```sh
sh plan/ws001/tests/basename-test.sh
make -j16 build/amd64/bin/basename
```

## ws001-p012 dirname

`dirname-test.sh` covers empty/no-slash, root/all-slash/double-slash, repeated
and trailing slashes, a long operand, `--`, usage, and output failure:

```sh
sh plan/ws001/tests/dirname-test.sh
make -j16 build/amd64/bin/dirname
```

## ws001-p013 link and unlink

```sh
sh plan/ws001/tests/link-unlink-test.sh
make -j16 build/amd64/bin/link build/amd64/bin/unlink
```

## ws001-p014 shell foreground job-control ordering

`shell-job-control-test.sh` (a host copy of the shell with linker wrappers and
a PTY probe) and `qemu-shell-job-control.sh` were deleted on 2026-09-24
(ws034-p047): the host test no longer compiled after the header moves, and the
QEMU test depended on the shell's old job messages.  Job control is now checked
on the guest console by `plan/ws034/tests/sh-jobs-interactive.py`.

## q042 pre-merge ws001-p015: credential-aware object creation

[`credential-creation-request-host-test.mk`](./credential-creation-request-host-test.mk)
links the exact production creation-request helper and verifies that create
authorization and the set-GID/GID snapshot occur under one parent metadata
lock.  It also checks denial cleanup and the ordinary/set-GID ownership rules:

```sh
make -f plan/ws001/tests/credential-creation-request-host-test.mk run
```

[`credential-vfs-qemu.mk`](./credential-vfs-qemu.mk) adds a test-only native
guest program and three disposable amd64 images.  The guest drops to synthetic
effective UID/GID 123/456, exercises regular files, directories, FIFOs,
symlinks, pathname sockets, set-GID parent inheritance, collision, permission
denial, and FAT's explicit ownership-representation rejection, then verifies
persistent results after a later boot/remount where applicable.  Build only
the Phase targets by including the fragment after the ordinary project
Makefile; do not install the probe in production images.

The existing WS018 FAT native host test is also compiled against the new
creation request boundary.  It checks successful representable root creation,
non-destructive `EEXIST`, and the retained FAT lookup/read/write matrix.  The
guest fixture is the authority for non-root effective ownership; the host FAT
fixture is not a substitute for it.

[`unix-socket-publication-contract-test.mk`](./unix-socket-publication-contract-test.mk)
guards the pathname-socket publication boundary that cannot be paused from an
ordinary guest process.  It requires endpoint resolution to retain the socket
and then validate the exact committed bound path, and requires bind to acquire
path references before publishing `bound_path`, the printable path, and the
bound bit under one socket-lock critical section:

```sh
make -f plan/ws001/tests/unix-socket-publication-contract-test.mk run
```

The native guest remains the behavioral authority for successful tmpfs, UFS,
and overlay pathname binds.  This focused source contract specifically keeps
an early-published `i_special` from becoming usable before bind commits.

At q042 closure image generation depended on a Noct module-search CLI which
the pinned interpreter rejected. That was a separate `ws008-p010` blocker and
was not evidence that guest acceptance passed. q050 used the subsequently
restored runtime `--path=tools/build` contract and completed the guest matrix.

## q050 canonical p022/p023 completion

The host fault and ordering fixtures of p022/p023 (UFS socket, overlay and
directory fsync) were removed on 2026-10-31: they no longer built against the
current UFS and overlay sources.

The QEMU runner makes all images in a private output directory and requires a
path which does not yet exist:

```sh
build/NoctLang/build-static/noct --path=tools/build \
    plan/ws001/tests/credential-vfs-qemu.noct \
    "$PWD" /tmp/ws001-q050-final-001
```

It performs five launches: overlay stage 1/stage 2, native UFS1 stage 1/stage
2, and FAT. The stage pairs use the same writable disposable media. The runner
sends QMP `quit` immediately after the stage-1 PASS marker, without guest
unmount or shutdown, and stage 2 verifies the namespace and content. The
overlay cell also exercises tmpfs and a journaled external UFS2 image. FAT
uses a raw external image with a test-only `/etc/unixmode` policy, verifies
representable root creation and explicit non-root rejection, and remounts the
same image. Source images are hashed before and after and must remain
unchanged. Historical guest markers use `WS001-P015` and map to canonical
`ws001-p022`.

## 2026-09-27 からの utility の Phase（p024〜）

| Phase | Test cases / executable evidence |
| --- | --- |
| 共通 | `build-host-ws001.sh`（host の build を `build/ws001/bin` へ）、`plan/tools/utils/util-diff.py --bin build/ws001/bin --only <utility>`（case は `plan/tools/utils/cases/<utility>.sh`、GNU の POSIX mode と比べる）、`tty-host-test.py`（端末が要る case）、`guest-run.sh OUTPUT [CASE...]`（lean guest の image を `config-amd64-lean-guest.mk` で作り、QEMU で serial console から `guest-cases.sh` で case を流す） |
| `ws001-p024` | cases `xargs`・`time`・`nohup`・`env`・`pwd` と `tty-host-test.py` |
| `ws001-p025` | cases `cp`・`cp-user`（root でない user が要る。guest では流さない）・`mv`・`files`、`tty-host-test.py` の mv の問い、`cp-installer-compare.sh OLD_CP NEW_CP`（installer の cp の呼び方を新旧で比べる）、`strerror-host-test.py [STRING_C]`（libc の strerror の全 error 番号） |
| `ws001-p026` | cases `id`・`chown`（chgrp を含む）・`chmod`・`mkdir`・`mkfifo`・`rmdir`。host の user と guest の root の両方で成り立つように書いてある |
| `ws001-p027` | cases `expand`（unexpand を含む）・`fold`・`nl`・`comm` |
| `ws001-p028` | cases `split`・`csplit`・`pr` |
| `ws001-p029` | case `diff`、`diff-random-host-test.py`（乱数の組を GNU の patch と ed に当て、GNU `--minimal` と変更行数を比べる）、`diff-installer-compare.sh OLD_DIFF NEW_DIFF`（installer の `-r -q --metadata`） |
| `ws001-p030` | cases `date`・`small`（sleep・uname・kill・pathchk・strings・link・unlink・tty・logname）、`pinned-cases.py`（期待値を書いた case、`pinned/*.sh`。GNU が POSIX と違う点と意図した違い。`guest-run.sh ... pinned` で guest にも流す） |
| `ws001-p031` | 全 case の guest の回帰（`guest-run.sh` に case file を列べる）、`status-after-not-found.py SERIAL_SOCKET`（q136 の観察）、`plan/tools/boot-test.sh` |
| `ws001-p032` | case `stream`（cat・cksum・dd）、`tty-host-test.py` の dd の ^C、`pinned/posix.sh` の `cksum -a sha256` |
| `ws001-p033` | case `patch`、`pinned/posix.sh` の patch（`-N`、normal の reject、標準出力）、`diff-random-host-test.py --patch build/ws001/bin/patch`（zedBSD diff の全形式を zedBSD patch で当てる） |
| `ws001-p034` | case `du`（host だけ。block の数は file system で違う）、`pinned/guest.sh`（guest だけの期待値。df と、UFS の上の du。`pinned-cases.py` は host では流さず export だけ）、`pinned/posix.sh` の `du -L` の loop |
| `ws001-p035` | case `who`（host だけ。glibc の記録の file を python で作る）、`pinned/guest.sh` の who、`pinned/posix.sh` の読めない file |
| `ws001-p036` | `stty-host-test.py [--bin DIR]`（pty の上で zedBSD stty を試し、termios と GNU stty で読み戻す）、`pinned/guest.sh` の stty（console に設定して戻す） |
| `ws001-p037` | `dirname-test.sh`（複数の operand と `-z` に更新）、case `dirname` |
| `ws001-p038` | cases `mktemp`・`base64`・`install`（GNU coreutils と比べる） |
| `ws001-p039` | case `xargs-gnu`（GNU の option）、`util-diff.py --gnu --only xargs-gnu`、`plan/tools/utils/configure-diff.sh build/ws001/bin`（`DISTFILES=/home/awe/zedBSD-rpi4/build/distfiles`） |
