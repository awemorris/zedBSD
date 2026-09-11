# WS019-p018: FAT growth capacity admission

Date: 2026-09-09
Status: completed (q133)
Parent: [WS019](../ws.md)
Timebox: 120 active minutes

## Evidence and scope

[p017 results](../phase017/results.md) show fresh 32/64MiB
creation and formatting work, but an impossible 256MiB truncate spends time
allocating/zeroing and returns timeout 124 rather than a prompt ENOSPC.
Implement capacity admission before growing FAT files via truncate. Retain
existing write-failure rollback; no partial-write semantics changes to write.

## Design

Reuse bounded full-chain validation to return the allocated cluster count.
Under the existing FAT mount mutation lock, count additional clusters required
by the new size, including clusters already owned past EOF. Refuse inconsistent
old size/chain as EIO. Scan allocation entries only until enough free clusters
are found or the bounded table ends. ENOSPC/read failure precedes requested
growth allocation, zeroing or directory/size publication. Prior pending closes
and dirty cache flush retain their existing ordering. No unlocked df estimate,
trust in FSInfo hints, new persistent cache or timeout increase.

## Acceptance

Actual consolidated FAT host tests: FAT12/16/32, empty/existing/preallocated
chains, partial cluster, exact fit, insufficient space, full volume, corrupt
chain, table-read error, injected write/barrier errors and rollback. Verify
unchanged original bytes/size/chain on refusal; ordinary and ASan/UBSan.
Re-run p017 fresh QEMU staging including status 1 capacity refusal, swap
activation and generated-data reboot/readback. Complete three architecture
builds with explicit configs, serially, make -j16. Correct time's negative
nanosecond formatting exposed by this measurement without inventing timings.

If essential conditions remain unproved, leave p018/p017 uncleared with concrete
resume conditions; do not call installer acceptance complete.

## Result

All selected gates passed. See [evidence](results.md).
