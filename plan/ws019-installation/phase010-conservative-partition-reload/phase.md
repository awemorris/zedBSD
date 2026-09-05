# WS019 Phase 010: conservative whole-disk partition reload

Last updated: 2026-09-05

Phase ID: `ws019-p010`

Status: uncleared in finished q076; implemented, final busy/reboot gate pending

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Contract

A privileged synchronous ioctl on a whole-disk FD requests re-reading.
No parsed table crosses the UAPI. Kernel discovery reads/validates but never
edits/repairs table bytes. Success replaces the whole child-device set.
Partition FDs and unsupported schemes are rejected.

Any mounted child means whole-disk EBUSY: rw, ro, internal backing, root and
unchanged partitions all count, even for a no-op/addition-only reload.
No force flag. Whole-disk mounts, indirect loop/swap claims, open child FDs,
pending opens and active users also prevent replacement. Exempt the requesting
whole-disk FD, not other whole-disk users. Cover preparing/dying mounts until
disk references and I/O are actually released.

Serialize admission through publication against new opens/mounts/claims,
I/O, teardown and competing reload. Never perform I/O under spinlocks.
Validate/allocate candidates and reserve resources before the namespace commit.
Invalidate old caches safely; old objects may never be retargeted to a new
partition. New registration identity distinguishes reused paths.
EBUSY, scan/read/flush errors and ENOSPC preserve the old published set.

Userspace fsync precedes notification. Reload failure does not undo disk
writes. Success means publication completed, not crash-atomic disk editing
or protection against future raw writes.

## Verification / timebox

Production-linked tests cover idle add/delete/no-op, mounted rw/ro/root,
unchanged p1 plus added p2/p3, swap/loop/open-child/pending-open,
concurrent admission, bad table/read/flush/allocation faults, repeated pool
reclamation, stale devfs nodes and privilege/invalid-target rejection.
Normal/sanitizer and amd64/i386 builds precede shared q076 QEMU idle and
busy/reboot cells. Review at four active hours; broader lifetime redesign
requires uncleared/re-planning, never weakened EBUSY or partial publication.

## Result / resume

Implementation and host/ABI/build gates passed, including 1,000 repeated
replacements and deterministic admission/fault tests. QEMU idle reload and
GPT/MBR add/delete passed; the initial old-policy guest also correctly refused
reload of its auto-mounted FAT. The final explicit rw/ro/root and reboot gate
was not reached because the fixture used an unsupported nested mount target.
See [q076 evidence](../tests/q076-results.md). Resume only in a newly approved
bounded Queue with the corrected root-level `/q076` fixture; no force option
or mount API expansion is authorized by this residual.
