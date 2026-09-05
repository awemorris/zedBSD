# WS019 Phase 002: basic block information and mount enumeration

Last updated: 2026-09-05

Phase ID: `ws019-p002`

Status: completed in q076, 2026-09-05

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

Queue: [q076](../../queue.md)

## Objective / supersession

Provide OS-owned disk geometry/identity and current mounts. The user replaced
the former comprehensive GPT/filesystem/use/boot snapshot proposal. Keep the
immutable directory slug, but table interpretation for diskpart belongs to
p003/p011. Kernel boot/reload validation remains for device discovery.

## Contract

- A versioned pointer-free query on an opened block FD returns registration
  identity/name, parent identity, logical sector size, 64-bit block count and
  parent-relative offset, and read-only/partition flags. No GPT parsing,
  filesystem identity or permission-to-mutate inference.
- A bounded coherent mount snapshot returns visible LIVE mounts, sources,
  targets, filesystem types and flags. Device-less/bind mounts do not invent
  physical origins; internal backing mounts are not ordinary visible paths.
- No-argument `mount` prints the snapshot. Preserve mount/-a/umount behavior.
  The snapshot is diagnostic, not an exclusion token for writes/reloads.
- Validate version/size/reserved/bounds, zero all output padding, and keep
  fixed-width ILP32/LP64 layouts. Queries grant no extra raw I/O privileges.
- Raw block reads/writes support 512/4096 sectors and existing 64-bit offsets,
  exactly-once full-sector writes and fsync.

## Verification / timebox

Production-linked host ordinary/sanitizer tests cover malformed queries,
empty/populated mount snapshots, ro/device-less mounts, 64-bit geometry and
raw 512/4096 I/O. Maintained amd64/i386 builds pass; shared q076 QEMU cells
exercise mount and block queries without query-induced target writes.
Review after two active hours; record actual remaining gates.

## Result

Implemented fixed-width BLKGETINFO and visible mount membership query, with
bounded getcwd-style path reconstruction (not concurrent-rename atomicity).
Normal/sanitized production-linked foundations, amd64/i386 ABI/builds, and
guest mount/list/show passed. No table records are returned by these APIs.
See [q076 evidence](../tests/q076-results.md); reload/writer runtime residuals
belong to p010/p011, not this diagnostic-query milestone.
