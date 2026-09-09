# WS019-p017: standard-command installer staging

Date: 2026-09-09
Status: completed (q133; q132 remains historical uncleared)
Parent: [WS019](../ws.md)

## Scope

Noct Process.spawn(argv)/read/write/wait/closeを使用し、shell文字列へpathを埋め込まない。cp/mv/truncate/mount/stat/df/blkid/cmp/mkfs/mkswapを対応付け、不足する標準的mktempと必要な既存コマンド機能を設計・実装する。unique staging、no-follow、inode identity、固定サイズ、flush/readbackとエラー処理を明示する。FAT作成modeを盲目的に0755へ偽装せず表現可能なmode/umask契約を決める。

## Acceptance

Noct上のargv保持、空白を含むpath、exit status、出力の上限/timeoutを検証。32MiB dataと64MiB swapの新規FAT stagingを実測し、過去120s超過を再評価。既存destination/symlink/途中失敗/容量不足の保存を検証。個別コマンド間で所有権を保持できない必須条件は推測で埋めずuncleared。

Timebox: 120 active minutes per execution queue. Queue投入前に詳細ABI・fixtureを確定する。未完了はunclearedとして他の独立作業へ進む。

## Read-only findings while closing p016

Current cp can leak its output descriptor if command_copy_fd fails: the OR
expression skips close, then sets output to -1. It also creates only mode 0666,
which existing FAT rejects because fresh entries are represented as root-owned
0755. These are concrete command gaps, not evidence of a Noct runtime failure.

Prefer extending existing cp with familiar --preserve=mode, --attributes-only,
-T and --update=none-fail operations. An exclusive attributes-only copy from a
known 0755 boot artifact can create an empty represented FAT staging file;
truncate sets its fixed size, mkfs/mkswap formats it, sync and mv publish it.
This avoids inventing a helper or claiming FAT can store private 0600 modes.
Noct chooses a candidate name, and atomic O_EXCL creation detects collisions;
a guessed unique pathname is not itself treated as ownership. A separate mktemp
command is not required if this existing-command route satisfies the contract.

Before Queue entry, specify preserve-mode behavior (including umask), first
error/close ownership, descriptor-based same-file checks, and the exact Noct
command runner/timeout/result contract. Validate freshly allocated 32/64MiB
files, not preallocated host fixture files. Retain any unsupported operation
as an explicit uncleared dependency.

## q132 selected command contract

Extend cp with -T/--no-target-directory, -n/--no-clobber,
--update=none-fail, --attributes-only and --preserve=mode. Unknown options fail.
Strict creation uses O_CREAT|O_EXCL; existing names, including symlinks, remain
untouched. Source metadata comes from the opened regular file. Ordinary new
files inherit source rwx bits filtered by umask; explicit mode preservation
ignores umask and applies fchmod after writing. Existing regular destinations
are compared by opened descriptor before ftruncate, so aliases never truncate
the source. Attributes-only leaves existing content unchanged. Both descriptors
are always closed and the first error survives cleanup. cp retains its usual
partial-file behavior on write failure; the caller must not infer ownership
merely from a failed command and pathname existence.

Noct orchestration uses argv arrays with Process.spawn/read/isAlive/wait/close.
Wrap commands in existing timeout -k 2 180; bound captured output to 1MiB.
Create a fresh FAT staging file exclusively with an attributes-only copy from
a known root-owned 0755 artifact, then truncate, mkfs/mkswap, sync, and atomic
mv. Do not pretend FAT represents private 0600 modes. If the explicit source
mode cannot be represented, fail. Standard mktemp is not needed for this route.

Host tests cover command ownership/error cleanup and actual existing-file,
symlink and mode behavior. Noct host/target checks argv with spaces, command
exit and timeout. QEMU measures actual fresh FAT growth to 32/64MiB and invokes
real formatters and publication; negative cells preserve existing objects.
Track capacity/partial-write failure with controlled host injection plus a
bounded target capacity refusal. Unproved cross-command identity requirements
remain explicit residuals for p004, never assumed from a pathname alone.

## q132 target fixture corrections

The host command test passed, but the first target fixture used unsupported
`sh printf %2048s`. Target diagnostics showed status 1 and the explicit
unsupported-conversion error, not a Noct output-loss bug. Generate the 2048-byte
argument in Noct and use supported `printf %s`; target q132-staging3 and later
pass argv, exit status, timeout and output-cap checks.

The initial FAT creation source `/bin/sh` inherited host build mode 0775. FAT
correctly refused preserving that unsupported mode. Use `/sbin/mkfs`, whose
production image manifest explicitly sets 0755, for attributes-only creation;
no forced mode or relaxed FAT semantics were introduced. The timing fixture
also assumed `time -p`, which this existing command does not implement; use its
existing `time command...` interface. q132-staging5 is the first fresh-allocation
run after these fixture corrections.

## q132 outcome

[Results](results.md): fresh creation/format/publication and Noct command cells pass;
capacity refusal times out and full acceptance remains unfinished. Resume after
a bounded FAT growth/capacity correction, then complete remaining runtime/build
gates. Noct itself is not shown to lack the required command APIs.

## q133 completion

[p018 evidence](../phase018-fat-growth-capacity/results.md) resolves the capacity
refusal and completes fresh staging, swap activation, generated-data reboot,
and three-architecture builds. Cross-command transaction/source identity is
implemented and accepted with the installer in p004/p005; p017 proves the
individual command mechanisms and bounded orchestration, not that transaction.
