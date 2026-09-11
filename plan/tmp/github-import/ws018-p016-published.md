<!-- awesome-plan project=zedbsd record=ws018-p016 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws018/phase016/phase.md`

親: [ws018](https://github.com/awemorris/zedBSD/issues/19)

# ws018-p016: Integrate the refactored kernel with current fixes

Status: completed (q084)
Date: 2026-09-06

## Objective and authorization

The user explicitly requests importing `/home/awe/claude/zedBSD/src/kern`
into the current tree, using the refactor as the base and retaining subsequent
fixes. This authorizes implementation and verification of this bounded import.
Review progress after 90 active minutes; continue useful authorized integration
without dropping unresolved corrections or claiming incomplete gates passed.

## Inputs and preservation contract

Common ancestor: `a143d5a`. Refactor: `03436b8`. Current destination: `d99865c`.
Snapshot both kernel inputs before editing under this WS's ignored temp area.
Preserve namespace/mount admission, namecache generation, overlay invalidation,
partial-write EOF, disk/partition reload and mount query, format reservations,
shared VM ordering, swap-format extraction and W52 WLAN probe corrections.
Retain deletion of synthetic rootfs and auxiliary automatic mounts. Repair the
refactor's stale devfs-block-range include and its existing test consumer.
Destination headers, drivers, userland and platform manifests remain the current
versions unless an actual kernel integration dependency requires adjustment.
Do not import unrelated source-repository softfloat or planning changes.

## Procedure and verification

1. Inventory history and snapshot inputs. Compare overlapping functions against
   the common ancestor; integrate recent behavior into the refactored layout.
2. Review untouched-by-recent-fixes imports for behavioral differences, including
   exec/startup/ioctl/VFS helper extraction; resolve preprocessing/build defects.
3. Run focused existing filesystem, storage, format reservation, swap and WLAN
   host regressions, including their sanitizer/analysis modes where supplied.
   Adapt source-layout-sensitive fixtures to exercise the imported production code.
4. Run serialized `make -j16` with explicit amd64, PCAT and PC98 CI configs.
5. Run the maintained amd64 disposable storage/formatter boot acceptance because
   startup and VFS helpers changed. Do not build while runtime inputs are frozen.
6. Record provenance, conflict decisions, evidence and limitations in q084 results;
   synchronize Queue/W/M and run `git diff --check`.

No commits, aggregate `make check`, private `.internal` reads or hardware changes.
Completion requires the integrated source, preserved fixes and successful gates;
any remaining failures must be stated explicitly.

## Result

Completed with [q084 evidence](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws018-kernel-architecture/tests/q084-results.md). Refactored source and
current fixes are integrated; import defects in devfs includes, init PATH,
PTY flag rereading and a conditional overlay prototype are corrected. Focused
host gates, three configured x86 builds and two disposable amd64 QEMU combined
cells pass. Kernel/source snapshots and logs remain under ignored temp storage.
