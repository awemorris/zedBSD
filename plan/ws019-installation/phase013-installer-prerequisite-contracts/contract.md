# Installer prerequisite contracts — current design

Date: 2026-09-09
Status: prerequisite decomposition completed; p015–p017 implementation not yet claimed

## Command orchestration

Noct is available. It owns selection, display, explicit confirmation, command
sequencing and failure reporting. Invoke argument vectors through its process
API; do not interpolate user paths into shell command text. No new private
native helper or ioctl binding is planned. Existing commands may gain missing
operations; ordinary UNIX/POSIX tools missing from userland/base may be added.

| Operation | Existing surface / remaining work |
| --- | --- |
| Inspect disk/partition/filesystem | diskpart, blkid, stat, df; preserve stable identifiers and provide parseable fields where needed |
| Prove firmware/config origins | Missing retained identity, not a Noct failure. Extend loader/kernel provenance and expose read-only through existing sysctl/blkid surface after its ABI is designed |
| Mount selected existing filesystem | mount/umount already exist; no format or partition edits |
| Create unique staging | Existing commands lack exclusive temporary creation. Consider standard mktemp, not a private installer helper |
| Copy immutable input | cp exists; add appropriate no-follow/exclusive behavior and handle descriptor/close errors. Current stat then open does not establish source identity |
| Set fresh image size | truncate exists but only handles existing files; evaluate FAT allocation cost under current implementation |
| Format data and swap files | mkfs -t ufs, mkswap exist; retain descriptor reservations and p014 current-format acceptance |
| Compare staged content | cmp and Noct hashing; verify reopened objects and size, never treat pathname existence as proof |
| Publish without replacing | Extend mv with no-clobber operation backed by atomic VFS primitive; userspace existence checks are insufficient |
| Flush file and publication | File fsync exists, FAT directory fsync is missing. A general UNIX sync command with per-file support is a candidate, with explicit error propagation |

The command boundary must preserve identity across processes. If a contract
requires a retained descriptor or claim that separate invocations cannot
preserve, document that exact gap and mark its dependent phase uncleared.
Do not assume repeated path resolution is equivalent to descriptor ownership.

## Atomic publication

src/kern/inode.c inode_namespace_enter joins a mount namespace transaction;
src/kern/mount.c mount_vfs_transaction_enter locks the mount mutex. Rename's
source and destination lookup are inside that transaction. A no-replace flag
can therefore be consumed at the VFS layer: if destination lookup succeeds,
return EEXIST (including aliases/same inode), otherwise call the existing FS
rename while retaining the lock. Unknown flags fail. Ordinary rename retains
its current replacement behavior. Confirm every mutation joins the same lock
before implementation. Case-insensitive FAT lookup determines existence.

Expose an explicit renameat2-style ABI rather than reinterpret unused arguments
of existing renameat. Test simultaneous publishers, same inode, FAT case aliases,
wrong mount, unknown flags, directory targets, busy objects and old rename.
FAT directory fsync must flush filesystem metadata then its disk barrier and
propagate either error. A successful rename followed by failed fsync is reported
as published-but-durability-unconfirmed; never remove a published file on error.

## Boot provenance

The UEFI loader retains the selected configuration volume serial in the common
handoff. That is not evidence for the firmware's BOOTX64.EFI ESP identity.
Configured boot0 can also differ from loader origin. Preserve and expose both
identities explicitly, including validity and partition identity, and resolve
against current registered devices. Ambiguous/missing identity refuses install.
No fallback to the first mounted FAT or first ESP. The exact handoff layout and
source-stability ownership remain to be designed in their prerequisite phase.

## Evidence boundary

p014 supplies current-format formatter evidence. q078 growth exceeding 120 s
was measured before current I/O work and must be remeasured, not assumed fixed
or treated as a permanent block. Disposable QEMU media only. Noct functionality
is not an outstanding human decision. Essential unsupported operations are
named and deferred as uncleared rather than hidden behind a helper.
