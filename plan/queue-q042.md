# Queue: native VFS credential and durability prerequisites

Last updated: 2026-08-31

QID: `q042`

Queue status: finished

Queue finished: **Yes**

Authorization: the user directed continuous execution of the remaining
workstreams after the preceding fixes, with no time limit and with
human-decision items deferred. q041 discovered two independent, fully
designed kernel prerequisites and recorded both as the exact resume condition
for the otherwise implemented credential store. This finite Queue executes
that dependency chain without adding WLAN hardware or protocol scope.

Timebox: none. Process each item to `completed` or `uncleared`, synchronize
P/W/M, commit locally after each processed Phase, and continue while the next
dependency is satisfied. Stop expansion at every Phase reconsideration
boundary.

Parent: [master plan](master.md)

Previous Queue: [q041](queue-q041.md)

Canonical ID note: this historical Queue retains the pre-merge
`ws001-p015`/`ws001-p016` labels and test output names.  Their active Phase
documents are now
[`ws001-p022`](ws001-posix/phase022-credential-aware-vfs-creation/phase.md)
and [`ws001-p023`](ws001-posix/phase023-directory-fsync/phase.md).

## Purpose

Correct native filesystem object ownership and directory synchronization,
then use those exact semantics to finish the root/non-root transactional
`wifi.conf` acceptance which q041 could not honestly claim.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws001-p015` | [Canonical Phase](ws001-posix/phase022-credential-aware-vfs-creation/phase.md) | uncleared | Source/host milestone passes: atomic authorization/credential snapshot, all creation callers/backends, conservative rollback, 50 request checks, AF_UNIX publication, 883,564 FAT ordinary/sanitized checks, and UFS gates; two backend fault cells plus native/remount evidence remain |
| 2 | `ws001-p016` | [Canonical Phase](ws001-posix/phase023-directory-fsync/phase.md) | uncleared | Source implementation and 135 deterministic ordering/error checks pass; disposable-image remount and device-flush acceptance cannot run until `ws008-p010` restores Noct `--path` |
| 3 | `ws005-p005` | [Phase](ws005-networking/phase005-wifi-credential-store/phase.md) | uncleared | Its retained host implementation was not rerun as invalid native acceptance: both explicit WS001 prerequisites remain incomplete |

## Dependency and deferral decisions

- p015 and p016 are independent internally, but they are executed sequentially
  so each gets one coherent Phase commit and evidence record.
- p005 begins only if both prerequisites complete. If either is uncleared,
  record p005 as uncleared without repeating the already passing host-only
  implementation.
- FAT ownership representation and stacked-directory lock ordering use the
  explicit reconsideration boundaries in their P books. They are not grounds
  to invent a format, weaken an error, or hide partial success.
- Physical Archer identity, WLAN UAPI/driver work, WS008 target Noct, Intel Mac
  hardware, and other manually held work remain outside q042.

## Fixed boundaries

- User-created ownership comes from effective credentials and the frozen
  set-GID-parent rule before namespace publication; post-publication `chown`,
  ambient current-process lookup in a backend, and root-default success are
  forbidden.
- Directory `fsync` means the owning namespace and backing flush were reached,
  or returns an explicit error. Missing callbacks never mean success for a
  directory.
- Use Phase-owned focused fixtures under `plan/`; do not consume `.internal/`
  or run aggregate `make check`. Use `make -j16`, disposable images, bounded
  QEMU cells, and `git diff --check`.
- Do not broaden q042 into general credential, ACL, journaling, WLAN protocol,
  or filesystem-format work.

## Completion definition

q042 is finished when all three rows have been processed. Full success means
both VFS prerequisites complete and the retained credential store passes its
native root/non-root and remount-durability gates. A Phase may be uncleared at
its documented reconsideration boundary; dependent rows then record the exact
resume condition rather than running invalid acceptance.

## Closure

q042 is finished with all three rows processed honestly. p015 and p016 retain
substantial reviewed source/host progress, including fixes found by independent
corrective review, but neither can claim its required native/remount evidence
while the pinned Noct interpreter rejects the production `--path` option.
p015 also records two missing deterministic backend failure-injection cells.
Because those prerequisites remain uncleared, p005 was not subjected to a
guest acceptance whose stated preconditions were false. Resume through
`ws008-p010`, complete the two WS001 Phases, then requeue p005.
