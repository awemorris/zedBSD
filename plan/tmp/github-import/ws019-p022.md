<!-- awesome-plan project=zedbsd record=ws019-p022 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase022/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# WS019-p022: populated tmpfs teardown

Date: 2026-09-09
Status: completed (q155); [acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase022-tmpfs-unmount/results.md), [ownership design](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase022-tmpfs-unmount/design.md)
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)
Timebox: 120 active minutes

Q154's first native nested-mount run fails with EBUSY when unmounting a tmpfs
after closing its file and removing its child mount. The closed file and child
mountpoint directory remain in the namespace. Evidence:
`../temp/q154-source/guest.log` and `result.json`.

`tmpfs:publish_new` retains each directory entry's inode. Generic
`inode_cache_mount_busy` allows only the cache reference (plus the mount's root
reference), so it treats those internal owners as external busy users. Moreover,
`tmpfs_reclaim` does not release a populated directory's entries. Simply allowing
an extra reference or skipping the busy check would leak or free live state.

Design and implement a filesystem-aware teardown ownership contract. Distinguish
namespace references (including hard links) from external file/path/VM owners;
keep the mount DYING and namespace reserved while admitting teardown. Preserve
all directory contents and references on every failure-capable path. After the
last refusal point, retire entries and inode/page/xattr charges exactly once,
then free mount state only after every node is reclaimed. Avoid recursive
unbounded kernel-stack traversal and allocation-dependent partial destruction.
Do not infer an inode's reference allowance solely from linkcount: directory
link counts and hard-link ownership have different meanings.

Tests use actual tmpfs/inode/mount code: empty and populated filesystems, deep
directories, multiple hard links, symlinks, data pages and xattrs; open/removed
files, cwd and child mounts must refuse teardown, preserving content until retry.
Inject any supported sync/prepare failure before destruction and verify retry.
Compare node/page/commit/inode/mount ownership before and after repeated cycles.
Run host normal and ASan/UBSan, three maintained builds and disposable QEMU.

The q154 path-resolution fixture explicitly cleans its owned entries before
unmounting and tests an unlinked open file for genuine external ownership.
That acceptance does not close this populated-tmpfs defect. Schedule a finite
queue before changing the generic/filesystem teardown contract.
