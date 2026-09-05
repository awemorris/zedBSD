# WS018 Phase 014: mounted namespace mutation protection

Last updated: 2026-09-05

Combined ID: `ws018-p014`

Status: completed in finished q077

Parent: [WS018](../ws.md)

## Authorization and baseline finding

KA-T121 discovered that namei crosses virtual mount children, but the mutation
path resolves a parent then looks up the backing inode directly. A mounted
directory can therefore reach filesystem rmdir/rename callbacks. The same
production-linked memory-filesystem probe fails at baseline `d97e21c` and after
p013 cleanup: `rmdir=0 expected=17 callbacks=1` (zedBSD EBUSY is 17). Both normal
and ASan/UBSan runs fail. This is host evidence, not yet a real-FAT guest result.

The user's subsequent explicit request authorizes thorough audit and fixing
this bug, including directly related namespace/lifetime problems. [q077](../../queue.md)
selects this Phase; its scope supersedes p013's stop on the discovered defect.
Do not restore the dead synthetic rootfs or obsolete marker as a workaround.

## Contract and audit scope

- Refuse deletion or rename of a live covered directory and rename replacement
  of a mount attachment. Check source and destination, including backing-inode
  aliases through bind mounts and descriptor-relative lookup.
- Audit ancestor movement and cached mount-path/lifetime assumptions. Preserve
  namespace reachability, getcwd and coherent unmount; conservative EBUSY is
  acceptable where this architecture cannot safely move an attachment.
- Audit create/link/symlink/mknod collisions with virtual mount entries and
  lookup cache invalidation. Unrelated ordinary mutations must still work.
- Serialize checks and filesystem mutation against mount preparation,
  publication, bind publication and unmount/rollback. No check-then-act gap,
  duplicate publication, stale inode pointer or leaked reference/lock.
- Use explicit identity and documented lock ordering. Never hold a spinlock
  across filesystem I/O; do not weaken read-only, ROOT/SWAPFILE/LOOPFILE,
  configured root/overlay/swap or disk-wide reload EBUSY protections.
- Preserve explicit user-created `/disk1` names and existing virtual mount
  traversal. Public nested mount support and unrelated filesystem redesign
  are not goals of this Phase.

## Work and verification

1. Independently audit syscall/namei/inode/mount/file paths and actual driver
   callbacks; record concrete findings and the chosen protection/locking model.
2. Fix at the shared kernel boundary, with a second independent review of
   identity/aliases, allocation/error unwinding and lock order.
3. Extend KA-T121 using production-linked normal and ASan/UBSan tests. Cover
   rmdir/unlink/rename source and target, aliases, unaffected names, read-only,
   mount lifetime/rollback, and deterministic concurrent admission where
   feasible. Preserve the pre-fix reproducer/result, never xfail it to pass.
4. Rerun KA-T120, storage foundations, backing claims, filesystem identity,
   native FAT and boot-source gates, then amd64/PCAT/PC98 `make -j16` builds.
5. Use q077's shared two-launch QEMU budget for real FAT mutation denial,
   mount continuity, ro/rw reload EBUSY, persisted-add exit 3 and reboot/no-auto-
   mount acceptance. Fresh disposable media only; 120-second boot and
   600-second cell caps. Host race evidence is distinct from guest evidence.
6. Record residual audit limits and synchronize M/W/P/Q. Review at four active
   hours from extension approval; do not extend the budget silently or clear
   an acceptance gate with an unexecuted test.

## Completion

The baseline reproducer and newly identified in-scope regressions pass without
skipping protections; independent review has no unresolved actionable defect;
declared supported builds and runtime gates pass. Otherwise record uncleared
with exact evidence and a concrete resume condition.

## Final functional continuation result — 2026-09-05

Completed in q077. [Findings and coverage](../tests/q077-filesystem-audit.md)
and [exact results](../tests/q077-results.md) retain the original failures and
record final-source normal/sanitizer gates, amd64/PCAT/PC98 builds and the
passing second shared QEMU cell. No acceptance criterion was waived.

The working tree includes the inherited p013/p014 edits and this continuation's
UFS1/UFS2, overlay and tmpfs corrections plus maintained regressions. All remain
uncommitted. Q077 consumed two of two permitted runtime launches. No third
launch, real-media operation or installer/formatter work belongs to this Queue.
The bounded functional audit does not assert exhaustive concurrency, physical
power-loss, non-x86 runtime or security coverage.
