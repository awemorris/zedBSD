# WS019-p021: process-path mount and unmount

Date: 2026-09-09
Status: completed (q154); [acceptance and boundaries](results.md)
Parent: [WS019](../ws.md)
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
