# WS019 Phase 011: userspace existing-table editing and notification

Last updated: 2026-09-05

Phase ID: `ws019-p011`

Status: completed in finished q077; original q076 residual retained below

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Bounded command contract

Add explicit whole-device `add DISK SLOT START COUNT TYPE`, `delete DISK SLOT`
and `reload DISK` operations. Confirm writes by repeating the selected device
identity; never default a destructive target. Units are logical sectors and
slots one-based. GPT TYPE is a GUID, MBR TYPE a byte; GPT addition also requires
a user-supplied nonzero unique PARTUUID and optionally a validated name.
Freeze exact syntax/help before implementation tests.

Write only existing valid GPT with matching copies, or primary-only MBR.
Reject degraded/conflicting GPT, extended/hybrid MBR, unsupported layouts,
occupied slots, zero/overflow/out-of-bounds/overlapping extents and duplicate
GUIDs. Preserve boot code, disk identity, unchanged records and non-table
bytes. No initialization, formatting, moves/resizes or automatic repair.

Show exact old/new table and target before confirmation. Mount-list preflight
is advisory, not exclusion. Refuse deletion of a known mounted partition.
Addition on a mounted disk may preserve all existing extents, but reload
still returns EBUSY/reboot-required. Keep kernel raw-write/claim protections.

Re-read original metadata before writing; changed input aborts. This detects
change but is not a multi-writer transaction: assume one administrator and
do not claim crash-atomic GPT/MBR writes. GPT writes backup entries/header,
flushes, then primary entries/header and flushes. Preserve protective MBR.
Read back and verify before notification. Short write/flush/verification
failure stops without a success claim or speculative rollback.
Distinguish write failed/possibly partial, write persisted but reload failed
(EBUSY requires reboot), and persisted plus reloaded. Never roll back EBUSY.

## Verification / timebox

Production-linked disposable-image tests cover GPT/MBR add/delete,
metadata-only diffs, 512/4096 and >4-GiB offsets, exact writes/flush ordering,
confirmation, partition/read-only targets, malformed/unsupported/stale input,
and every injected short-write/flush/read-back fault. Mock reload tests cover
all outcome classes. Normal/sanitizer, amd64/i386 and shared q076 QEMU idle
plus mounted-add/reboot acceptance are required. Review at four active hours.
Physical media and installer formatting are outside this Phase.

## Result / resume

The frozen add/delete/reload syntax is implemented with explicit identity
confirmation and exit statuses 0 (success), 1 (failure), 2 (usage), and 3
(table persisted but reload failed). Normal/sanitized parser/writer/CLI tests
and maintained builds passed. Actual guest GPT/MBR add/delete/reload each
round-tripped the complete disposable image, not just table sectors.
Mounted-add exit 3 is host-CLI verified but its real guest/reboot acceptance
remains unexecuted after the nested-mount harness failure. See
[q076 evidence](../tests/q076-results.md). Resume in a newly approved bounded
Queue using `/q076`, without treating this partial result as full clearance.

## Q077 residual acceptance — 2026-09-05

Completed by final-source `q077-resume-02` after the p013/p014/p015 corrections.
Explicit root-level ro/rw/virtual mounts and mutation protection pass; whole-disk
reload returns EBUSY for ro, rw and the actual root disk. Mounted addition
persists and exits 3 without live replacement; reboot discovers p2 with no
automatic auxiliary mounts. MBR round-trip, GPT non-table preservation and
production-input immutability pass. [Q077 evidence](../../ws018-kernel-architecture/tests/q077-results.md)
records commands, hashes and both launches. Q076's earlier failure record is
unchanged; this is new acceptance, not a retroactive PASS for its failed cells.
