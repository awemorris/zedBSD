# WS019 Phase 002: read-only block and GPT administration

Last updated: 2026-09-05

Phase ID: `ws019-p002`

Status: pending in proposed `q076`; execution approval required

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

Queue proposal: [q076](../../queue.md)

## Objective

Expose enough immutable block/GPT state for `diskpart` and `zedinst` to make
truthful selections without introducing a raw-write or partition-editing UAPI.

## Dependencies

- `ws004-p024` strict GPT publication and QEMU NVMe block acceptance;
- existing block-identity, mount, swap, and `/dev/system` conventions.

## Scope

- versioned read-only records for whole disks and partitions;
- stable kernel name, parent identity, byte capacity, logical block size,
  start/length, GPT type GUID, PARTUUID, bounded PARTLABEL, and read-only state;
- FAT16/FAT32 filesystem identity and current mount/root/swap use;
- the loader-origin boot filesystem identity needed to locate installer source
  artifacts;
- enumeration with size negotiation, bounded record count, and snapshot/change
  detection;
- privilege policy consistent with disclosure of storage topology.

## Non-goals

- raw writes, GPT creation/editing, `mkfs`, rescan, exclusive mutation claims,
  or unmounting a consumer;
- inferring an install target or representing enumeration order as stable
  identity.

## Completion conditions

- Version/size/reserved-field checks and fixed-width amd64/i386 ABI agree;
  short buffers, count limits and arithmetic overflow fail without leakage.

- Focused tests cover 64-bit capacities, GUID/name bounds, missing and changed
  devices, mounted/root/swap flags, non-GPT media, and snapshot retry.
- Same-name re-registration, opened-object mismatch, full multibyte GPT labels,
  metadata-read failures, indirect root/swap backing, and distinct boot0/ESP
  provenance are covered. Query failures expose no partial success or target writes.
- `/dev/system` reports the QEMU NVMe GPT, exactly one ESP candidate, and the
  selected FAT32 candidate with matching kernel identities.
- The production amd64 build passes with no write-capable administration
  operation exposed by this Phase.
- Maintained i386 builds, directly affected regressions, documentation
  validators and `git diff --check` pass; no aggregate `make check`.

## Source audit and proposed implementation contract

The 2026-09-05 audit found reusable foundations, not a complete administration
interface. `disk_registry_snapshot()` exposes name/dev/flags/geometry only,
with an 80-disk bound. The GPT scanner validates type GUIDs but `struct
partition` does not retain them. It retains a full UTF-8 PARTLABEL of up to
108 bytes plus NUL, while the older blkid field is only 64 bytes.
`filesystem_identify()` can inspect formats without mounting. Mount, swap,
backing relationships, and retained boot-source slots supply the other state.

The following v1 policy is proposed for approval with q076:

1. Add version/struct-size checked, fixed-width, zero-reserved read-only query
   records. Export no kernel pointers. Negotiate count/capacity with bounded
   collection/retry, and reject unsupported versions or inconsistent epochs.
2. Preserve validated GPT type/scheme metadata at partition publication and
   expose the full label. Leave existing blkid ABI and GPT validation/degraded
   publication policy unchanged. Report checked 64-bit extents, parent and
   per-registration identity; device names alone are not stable identity.
3. Include filesystem validity/read-error state, mounted/read-only state,
   native/overlay-root backing, active swap backing, and boot-source use.
   Follow loop/file-backed ancestry. Unknown must not mean unused or usable FAT.
4. Match existing `/dev/system` GET visibility for read-only topology queries.
   Do not change device-node permissions or grant raw-device/write authority.
5. Collect reference-safe records without metadata I/O under registry
   spinlocks. Detect device and relevant mount/swap-use changes; return bounded
   retry/error instead of mixed records. Provide opened-block-object
   revalidation against the reported incarnation. This does not provide
   installation-time exclusion or replace later mutation claims.
6. Report the retained selected boot/config filesystem (`boot0`) explicitly;
   it is not necessarily the ESP from which firmware loaded the EFI program.
   Mark unavailable physical loader-origin provenance as unavailable, never
   substitute root or the first FAT. If the installer's required source cannot
   be identified through the retained contract, extract the missing foundation.

## Ordered work and proposed timebox

One Phase, estimated 2--3 hours active work, with a three-hour review point.
After execution approval:

- [ ] Freeze concrete v1 layouts/error/retry/use/provenance rules and host ABI
      checks before adding consumers.
- [ ] Retain validated GPT metadata and implement reference-safe collection,
      use/boot attribution, change tokens, and opened-object revalidation.
- [ ] Pass IN-T001 success/fault tests, normal/sanitizer modes, and directly
      affected GPT, disk lifetime, identity, mount/boot/swap regressions.
- [ ] Build maintained amd64/i386 configurations with `make -j16`; run at most
      one phase-owned amd64 OVMF/QEMU NVMe query cell (120-second boot,
      300-second total). Do not repeat a failure without a new budget.
- [ ] Record commands, identities and unchanged target/input digests; run
      documentation/whitespace gates and synchronize P/W/M/Q.

The runtime fixture boots a disposable ordinary-system copy and queries a
separate host-prepared NVMe GPT with one ESP and a distinct FAT32 payload.
A test-only query helper is allowed; it is not p003 diskpart. Open the target
read-only and compare its entire digest before/after. Do not modify a
production image, physical medium, partition table, or mounted target.

## Reconsideration boundary

Return to planning if truthful filesystem/boot-source reporting requires a
mount mutation or loader redesign, coherent collection needs a broad storage
lifecycle redesign, or an opened object cannot be safely revalidated against
reported identity. Changes after a completed query are expected; do not claim
a snapshot prevents them.
