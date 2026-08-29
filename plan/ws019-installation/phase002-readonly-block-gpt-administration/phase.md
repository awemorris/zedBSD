# WS019 Phase 002: read-only block and GPT administration

Last updated: 2026-08-29

Phase ID: `ws019-p002`

Status: planned; dependency-gated

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

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

- Focused tests cover 64-bit capacities, GUID/name bounds, missing and changed
  devices, mounted/root/swap flags, non-GPT media, and snapshot retry.
- `/dev/system` reports the QEMU NVMe GPT, exactly one ESP candidate, and the
  selected FAT32 candidate with matching kernel identities.
- The production amd64 build passes with no write-capable administration
  operation exposed by this Phase.

## Reconsideration boundary

Return to planning if truthful filesystem type or loader-origin identity
requires a mount mutation, or if one snapshot cannot prevent target identity
from changing between display and open.
