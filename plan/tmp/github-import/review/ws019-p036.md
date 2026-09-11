# ws019-p036: native UFS initialization and wide geometry

Status: completed q169; timebox 90 active minutes
Parent: [WS019](../ws.md), prerequisite of p006/p007

Add a native UFS codec profile with an empty root directory, no overlay
journal files or root marker, and the existing journal/snapshot persistence
tail. Keep format layout entirely in userland. Widen total fragments and
summary arithmetic to 64 bits; native geometry is limited by host off_t,
on-disk inode-count range, per-group bitmap and supported minimum size, not
the historical 2-GiB image cap. Compute bounded group geometry, refusing a
final group too short for metadata. Keep memory bounded independently of
volume size for normal write/readback; pristine whole-volume comparison may
require additional coverage memory and is not the installer acceptance path.

Existing overlay/profile output remains byte-compatible on its accepted
range. Native root uses only reserved inodes 0/1 and root inode 2; initial
root link count, allocation maps and all free summaries must agree. Preserve
the tail format and independent kernel decoder contract. Check every transfer
against the medium bounds. Flush native metadata before primary publication.

Acceptance: legacy formatter fault/image/pristine regressions; native sparse
images below and above 2 GiB with independent root/inode/bitmap/summary/tail
inspection; geometry beyond 32-bit fragment counts, overflow and small-final-
group boundaries; failed/short writes/reads and corrupted metadata; three
target builds. Public reserved native mkfs and QEMU mount/copy belong to the
next bounded integration phase, not this codec-only acceptance.

Accepted 2026-09-09: [results](results.md). Native sparse/independent/fault
checks, legacy byte equality and pristine regressions, and three builds pass.
