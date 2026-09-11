# ws019-p033: partition administration reservations

Status: completed q166; timebox 90 active minutes
Parent: [WS019](../ws.md); prerequisite of p006 partition formatting
Depends on completed p031/p032. Standing autonomous execution authorization.

Extend BLKRESERVE to a direct partition of a physical disk, retaining exact
BLKGETINFO identity and final-description-close ownership. Continue refusing
file-backed devices and nested/unsupported partition ancestry. Store the owner
on the reserved logical disk. Compare canonical physical sector intervals for
new opens, registry changes, cache admission and BIO admission, so a whole-disk
alias or overlapping partition cannot bypass the gate. Nonoverlapping sibling
mounts/I/O remain usable; do not lock an entire disk merely to format one part.

At acquisition require the sole target open, no overlapping open/mount or
referenced overlapping child, no conflicting claim and an idle physical I/O/
cache frontier. The initial frontier check remains conservatively leaf-wide;
a transient sibling operation may return EBUSY, but a mounted idle sibling is
not permanently refused and disjoint I/O proceeds after acquisition. Protect
all checks/publication with existing registry/claim locks, without nesting the
mount namespace lock. Avoid scanning the registry on ordinary I/O when no
administrative reservation exists.

Each owner syscall retains the p031 temporary thread scope. Logical whole-
volume cache admission and mapped BIO intervals must use the same overlap
rules. Flush is authorized for its logical volume even though hardware flush
is physical. Reserved writes remain clipped by the block descriptor, use an
explicit raw claim, and invalidate overlapping cache lines under the gate.
Partition-owned BLKREREADPART remains invalid; only a whole-disk owner can
replace partition mappings. Preserve whole-disk init behavior and media-revoke
failure/close semantics.

Acceptance: production host tests for parent/overlap/new-open exclusion,
disjoint mounted/open sibling allowance, mapped BIO/cache access, wrong-thread
scope, two disjoint reservations, boundary writes and release/rollback; real
claim regressions; disposable QEMU probe while a sibling is mounted read-only
and writable, sibling access during reservation, owner read/write/fsync and
close/exit; unchanged protected image regions. Whole-disk regression and all
three target builds pass. No formatter or installer completion is claimed here.

Accepted 2026-09-09: [results](results.md). Host 41991 assertions in ordinary
and sanitizer modes, native partition/whole-disk probes, production claims
and amd64/pcat/pc98 builds pass.
