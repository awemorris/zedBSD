# Queue: VFS credential and directory-durability closure

Last updated: 2026-09-01

QID: `q050`

Queue status: completed

Queue finished: **Yes**

Authorization: after completing, synchronizing, and pushing q049, the user
directed automatic continuation. The q042 VFS work is now dependency-ready:
the accepted Noct interpreter runs repository scripts with `--path`, and the
remaining compile/application CLI defect is unrelated to these image rules.

Timebox: none. Process both WS001 Phases to `completed` or honestly
`uncleared`. Record a newly discovered product decision and defer only that
Phase; otherwise continue automatically. A later Queue may consume the
completed prerequisites for `ws005-p005`.

Parent: [master plan](master.md)

Previous Queue: [q049](queue-q049.md)

## Purpose

Finish the two kernel/VFS prerequisites discovered while implementing the
local Wi-Fi credential store: creator-effective ownership for every object
creation path, and truthful durable directory `fsync`. This Queue also repairs
the pre-merge Phase-ID collision by using canonical IDs `ws001-p022` and
`ws001-p023`; historical q041/q042 records and old test labels remain evidence
under their original names.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws001-p022` | [Phase](ws001-posix/phase022-credential-aware-vfs-creation/phase.md) | completed | Exact UFS socket and overlay rollback faults, root/non-root creation, reopen, and abrupt-stop/remount acceptance pass |
| 2 | `ws001-p023` | [Phase](ws001-posix/phase023-directory-fsync/phase.md) | completed | Production-backed ordering/failure gates and abrupt-stop/remount durability pass; FAT/tmpfs remain explicitly unsupported for directory sync |

## Dependency decisions

- Runtime `noct --path=...` is proven by q047, the ordinary production build,
  q049's complete QEMU harness, and this Queue's guest-ELF probe. The remaining
  `NOCT-T082` `--compile --app --path` failure does not gate either Phase.
- p022 and p023 are otherwise independent. They share only disposable image
  construction and may share one bounded QEMU harness when each Phase's
  terminal evidence remains separately identifiable.
- `ws005-p005` is not smuggled into this Queue. It becomes the first candidate
  for the next Queue only after both prerequisites complete.

## Fixed boundaries

- Preserve effective-credential ownership, set-GID-parent inheritance,
  pre-publication representability, complete rollback while cleanup succeeds,
  and cleanup-error precedence. If rollback itself fails, detach borrowed
  endpoints, expose no partial cache object, preserve only a complete
  authoritative upper when removal failed, and quarantine the filesystem
  read-only. Do not substitute post-publication `chown` or root defaults.
- Directory `fsync` succeeds only after the owning namespace and required
  backing flush complete. FAT, tmpfs, and missing directory callbacks return
  `EOPNOTSUPP`; they do not inherit a false-success fallback.
- Remount evidence must not rely on a clean guest unmount, clean shutdown,
  sleep, or QEMU exit as proof. Stop a passing first-stage guest without a
  clean unmount, then verify the same writable disposable media on the next
  launch.
- Keep Phase-only fault controls in production-linked host fixtures. Do not add
  a public ABI, persistent on-disk format, or product-only test hook.
- Use private build/image paths, bounded timeouts, source-image hash checks,
  and evidence below `/tmp`. Do not use `.internal/` or aggregate
  `make check`; use `make -j16` for build gates.
- Existing repository Python debt is outside this Queue. Add no Python script
  or new supported Python dependency; use Noct for the QEMU runner.

## Required evidence

For p022:

1. Exact production creation-request and AF_UNIX publication gates.
2. UFS1/UFS2 cleanup-failure cells which prove endpoint detachment, cleanup
   errno precedence, read-only freeze, and no dangling cache publication.
3. Overlay existing-upper and lower-only materialization/copy-up failure cells
   which prove no residue when cleanup succeeds. Double-fault cells prove
   cleanup errno precedence, read-only quarantine, no partial cache
   publication, and either durable removal or retention of the complete inert
   upper as authoritative so stale lower content cannot reappear.
4. Five bounded launches: overlay stage 1 and stage 2 on the same writable
   copy, native UFS1 stage 1 and stage 2 on the same writable copy, and FAT
   creation/rejection plus external unmount/remount. The overlay stage also
   exercises tmpfs and external UFS2.

For p023:

1. Existing production-linked VFS, UFS1/UFS2, and overlay dispatch/order/error
   tests plus the missing namespace-write failure boundary.
2. Real syscalls for create/rename, mkdir/rmdir, link/unlink, symlink
   replacement, file `fsync`, rename, and directory `fsync`.
3. UFS1, UFS2, and overlay second-launch namespace/content verification after
   the first launch is stopped without a clean unmount; FAT/tmpfs directory
   `fsync` returns `EOPNOTSUPP` and regular-file `fsync` does not regress.

Both Phases also require configured production objects, ordinary `make -j16`,
focused sanitizer/analyzer gates where applicable, and `git diff --check`.

## Completion definition

q050 finishes when both rows have been processed. Full success means p022 and
p023 are complete and `ws005-p005` has no remaining VFS dependency. If testing
demonstrates that a new on-disk protocol, public ABI, or unresolved stacked
lock order is required, record that exact reconsideration in P/W/M and finish
the Queue with the affected row `uncleared` rather than weakening its contract.

## Execution result

Both rows completed without a new product decision or public ABI change.

- The production creation-request gate passes 50 checks; AF_UNIX publication,
  UFS1/UFS2 independence (23 checks each), UFS2 consistency (45 checks), and
  the FAT production path (441,782 ordinary plus 441,782 ASan/UBSan checks)
  pass.
- The new production-linked UFS1 and UFS2 socket rollback fixtures each pass
  64 ordinary, sanitizer, and fixture-scoped analyzer checks. The overlay
  create/materialization/copy-up fixture passes 2,667 checks in each of the
  same three modes. Rollback uncertainty detaches socket endpoints, preserves
  the first cleanup error, and quarantines the affected filesystem read-only.
- Directory synchronization passes 13 VFS, 36 UFS ordering, 79 UFS1 mutation,
  83 UFS2 mutation, and 86 overlay ordering checks. The UFS mutation cells
  also pass ASan/UBSan and fixture-scoped analyzer builds.
- Five bounded amd64 launches pass from one private build: overlay stage 1 and
  stage 2 on the same writable media, native UFS1 stage 1 and stage 2 on the
  same writable media, and FAT create/reject/remount. Each stage-1 success is
  followed immediately by QMP `quit`, with no guest unmount or shutdown; the
  next launch verifies the committed namespace. Source image hashes remain
  unchanged. Historical guest markers retain the pre-merge `WS001-P015`
  label and map to canonical `ws001-p022` as documented above.
- `make -j16` and `git diff --check` pass. The build emits only the retained
  PC-98 stage-2 RWX-segment warning.

Final disposable evidence: `/tmp/ws001-q050-final-006`.
The completed Phases release both VFS prerequisites of `ws005-p005`.
