# ws019-p049: complete native installer transaction and text flow

Status: in-progress q182; dependencies accepted through q181
Parent: [WS019](../ws.md), implements remaining p006/p007 product flow
Timebox: 180 active minutes

Integrate the accepted components into the actual installer now. Completion
means the installed command runs the complete native path, not another isolated
helper or a simulated screen. Retain the current text appearance and existing
coexistence invocation. Graphical frontend remains p029 after this path works.

## Product flow

Provide source -> coexistence/dedicated mode -> physical destination -> review
-> progress -> completion/error. Source is installation disk only; HTTP remains
unavailable. Source-image admission precedes target writes. Dedicated mode
automatically creates ESP and native UFS root, with swap as /swapfile. No Home
partition or arbitrary editor. Keep a shell escape; discard all observations
on return and restart discovery. Exclude source, readonly and virtual backing
devices from selectable destinations using actual device capabilities, not
name guesses. Extend existing machine inventory if needed.

Show the exact target identity, capacity and complete destruction warning with
NO/YES, initially NO. EOF/Escape/input errors cancel without writes. Reobserve
source and target after YES. Preserve existing explicit coexistence behavior;
factor frontend callbacks so p029 can reuse the same decisions and transaction.

## Transaction

1. Own the private workspace and read-only source mounts through existing
   preflight/rootview. Native discovery must accept a blank destination: do not
   require the old target to contain healthy GPT/FAT before dedicated install.
2. Freeze the filename-safe source census, obtain conservative logical byte/
   inode measurements without following source symlinks outside the tree, and
   build/admit the automatic layout with nativeadmit. Generate fresh per-run
   disk/partition GUIDs, reject collisions against the complete visible
   inventory and the three new identifiers. Do not rely on an unseeded default
   Math.random sequence or hard-coded fixture identifiers. Required generation
   capability work belongs here; no human-supplied GUID prerequisite.
3. After NO/YES and revalidation, drive existing reserved diskpart init and mkfs
   commands by argv/PTY. Send each exact identity challenge only once, only for
   the expected operation. Bound output and lifetime, drain/close children,
   propagate nonzero/uncertain outcomes. Set destructive-start state before
   sending the first authorization; do not promise rollback after that point.
4. Reconcile generated GPT and freshly registered partitions with exact planned
   GUIDs/extents after reload. Recheck identity before each formatter and mount.
5. Mount native root, run accepted cp -a/tree progress against the frozen census,
   reconcile completion and metadata. Use fresh read-only source/target views
   for final comparison so read-time atime changes do not invalidate the oracle.
   Release individual owned mounts safely; retain cleanup facts on errors.
6. Allocate and sync the complete swapfile, mkswap, verify and exercise activation/
   deactivation. Install /etc/fstab for root-file startup using q180 support.
   Perform these intentional additions after the copied-tree comparison.
7. Copy loader/kernel to ESP with existing Noct-controlled file copying. Publish
   boot configuration last using an explicit native managed-file profile;
   preserve the six-entry coexistence profile and its ordering tests. Native
   configuration uses ESP/root PARTUUID, no overlay fields or FAT swap selector.
8. Sync, release only owned resources, and report completion or accurate partial
   installation/recovery locations. Never delete copied root content to imply
   restoration of the disk's former contents.

## Verification

Focused Noct scenarios cover both modes, source refusal, empty/invalid lists,
shell-return invalidation, default NO/cancel, stale identity, malformed child
challenge/status, capacity/inode shortage, copy and publication failure and
cleanup ambiguity. Reuse accepted command/transaction tests instead of replacing
their behavior with mocks solely to pass a new frontend.

Build supported configurations. Disposable QEMU: actual public installer cancel
and refusal preserve target; dedicated install completes with file-count
progress; fresh source-free boot has native root and automatic UFS swap, and
halt/reboot preserve content. Capture and show actual installer screens. Retain
coexistence regression. If the timebox ends, record exact remaining integration
as uncleared with a concrete resume path; p006/p007 stay open. Final paging
stress/fault matrix and docs remain explicit acceptance work, not silently waived.
