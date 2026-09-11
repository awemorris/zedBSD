<!-- awesome-plan project=zedbsd record=ws019-p021 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase021/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# WS019-p021: process-path mount and unmount

Date: 2026-09-09
Status: completed (q154); [acceptance and boundaries](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/phase021-nested-mount/results.md)
Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)
Timebox: 120 active minutes

The public mount syscall incorrectly delegates to the bootstrap `mount()`
helper, which accepts only a single root-level component. The installer needs
owned nested paths under /run. Common mount_at already takes a referenced
parent path and name; implement the userspace boundary using that machinery
instead of moving installer mountpoints to global top-level names.

First audit reserve_mount, target lookup, mount-path recording and unmount's
reference/busy accounting. Resolve using the caller's retained cwd/root,
preserving chroot and relative-path semantics. Validate an existing directory
target, reject covered/busy/root targets appropriately and keep parent/target
references until publication. Do not pass a process-relative string into the
global mount_find_ref namespace. Unmount must resolve the actual mount root
with matching identity and account for its temporary reference; releasing a
reference and then unmounting by an unprotected global string is not sufficient.

Preserve the kernel bootstrap helper's existing callers. Retain root-only
authority, supported flags, disk/backing claims, mount reservations, sync and
teardown errors. Follow plan/coding-style.md for new C code. Do not change
filesystem format or relax claim checks to pass the fixture.

Implementation refinement: clone the caller's cwd/root for one operation,
resolve and retain the actual directory (including final symlinks), reconstruct
its canonical path within that root, then resolve its parent and reserve with
an expected covered-inode identity. Validate that identity under the existing
namespace transaction before publication. Bootstrap mount_at keeps its existing
missing-entry behavior. Unmount resolves the actual mount root and transfers
its held mount identity into a shared teardown helper, releasing only the
temporary inode/context references before the existing busy check.

Verification: focused real-code host lifecycle tests with sanitizers where
available; nested /run mounts, relative paths, missing/file targets, duplicate
mounts, child/open-reference EBUSY, failed-mount cleanup, failed/unmatched
unmount, path renaming/protection and process root containment. Run all three
maintained builds, then QEMU source-only q153 plus a bounded native lifecycle
scenario. Record any unverified race/architecture boundary explicitly. No
physical test or new public helper command is required.

Verification boundary: this OS does not expose a chroot syscall. Process-root
containment is exercised with actual cwdinfo/namei/mount code in the host
fixture; the native fixture covers the existing public syscall surface,
including root-only mount/unmount authority. Do not claim a native chroot test.
