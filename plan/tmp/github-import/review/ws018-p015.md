# WS018 Phase 015: filesystem-wide risk audit

Last updated: 2026-09-05

Combined ID: `ws018-p015`

Status: completed in finished q077

Parent: [WS018](../ws.md)

## User decision and boundary

The user explicitly broadened the audit from mounting to the whole filesystem
while p014 was in progress. Initially this Phase recorded the wider read-only audit,
coverage and evidence without claiming that one finite pass proves the absence
of bugs. P014's authorized critical correction continues in parallel.

Use the current q077 four-active-hour review window and shared two-launch
runtime cap; do not silently extend them. Newly discovered unrelated fixes
must receive a recorded scope/verification decision before implementation.
No actual media, formatter/installer execution or aggregate `make check`.

## Functional correction authorization — 2026-09-05

The user explicitly requests functional audit and correction across the
filesystem code. This resolves the former audit-only decision for FS-A05--A09.
Repair demonstrated update loss, allocation/pointer ordering, directory record
bounds, inode identity admission, and overlay postcommit cache consistency.
Preserve existing interfaces and filesystem formats. Add production-linked
regressions with ordinary/sanitizer, failure and controlled concurrency cells;
then run the existing related gates and supported builds. Record functional
coverage gaps honestly; no claim of exhaustive correctness follows from PASS.

## Audit matrix

- Core: pathname lookup/cache coherency, access/sticky/read-only checks,
  descriptor/path operations, inode/file references and cached/open lifetime.
- Mutation and I/O: create/link/unlink/rename, truncate/write/append, errors
  and partial writes, allocation rollback, dirty-data sync and final close.
- Namespace: p014 mount/bind/ancestor protection and concurrent admission,
  keeping cache and actual directory-entry identities coherent.
- Drivers: FAT, independent UFS1/UFS2, overlay copy-up/whiteouts/journal and
  backing resources. Inventory nodev/pseudo filesystems and their applicable
  lifetime/bounds/mutation contracts rather than assuming disk semantics.
- Storage boundary: block/cache/backing claims, loop/swap and partition reload;
  distinguish prior tested guarantees from newly audited caller assumptions.
- Validation: inventory maintained production-linked normal/sanitizer/fault
  tests and run relevant finite gates; track untested failure/concurrency paths.

## Deliverable and completion

Record reviewed files/boundaries, prioritized actionable findings with concrete
evidence, fixes owned by p014 or separately scoped, executed tests and residual
coverage limitations. A missing audit row or unresolved serious finding is
not a clean audit; mark uncleared with a resume condition if the review window
cannot establish the declared deliverable. Do not infer non-x86 runtime or
crash/power-loss guarantees from host tests and x86 build success.

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
