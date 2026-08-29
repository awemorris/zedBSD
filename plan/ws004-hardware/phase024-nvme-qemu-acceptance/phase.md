# WS004 Phase 024: QEMU NVMe block acceptance

Last updated: 2026-08-29

Phase ID: `ws004-p024`

Status: planned; depends on `ws004-p022` and `ws004-p023`

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Accept the completed driver as a general block device in QEMU before any write
to the Latitude's internal storage. Cover data integrity, a strict GPT scan,
concurrent I/O, flush, reset, and regression boot paths on disposable media.

## Acceptance matrix

- fresh zeroed namespace and a namespace containing protective MBR plus valid
  primary/backup GPT headers and entry arrays;
- first partition published as `/dev/nvme0n1p1`;
- header/entry CRC, usable-range, overlap, type/unique GUID, UTF-16LE name, and
  primary-versus-backup validation with visible malformed-table rejection;
- unaligned user request edges translated into legal 512-byte block I/O;
- sequential and overlapping read/write workloads with explicit flush;
- queue wrap and a controlled reset or fault-injection path;
- guest power cycle followed by complete data verification;
- unchanged amd64 IDE and xHCI USB-storage boot to `login:`.

The later installer and `boot.cfg` Phases own native-root and overlay-root boot
from NVMe. This Phase proves the driver and partition substrate only.

## GPT substrate boundary

- Add an architecture-independent, read-only GPT partition implementation
  under `src/drivers/disklabel/` and a PC/AT disklabel selector that prefers a
  fully valid GPT while retaining legacy MBR support for existing media.
- Validate the protective MBR, primary and backup header signatures/revisions/
  sizes/locations/CRC32, entry-array size and CRC32, usable-LBA bounds,
  non-overlap, nonzero type and unique GUIDs, and bounded UTF-16LE names before
  publishing any entry.
- If the primary is damaged but the backup is fully self-consistent, report the
  condition and use the backup read-only; repair belongs to a later explicit
  `diskpart` operation, not boot-time code.
- Do not add a GPT writer, formatter, resize, hybrid-MBR reconciliation, or
  destructive repair in this Phase. WS019 owns the administrative write path.

## Completion conditions

- Every matrix cell passes on a disposable image without an I/O error, stale
  completion, leak, hang, or data mismatch.
- GPT discovery is deterministic and retains the current one-based partition
  naming contract.
- Valid primary and backup GPT metadata publish the same partitions and stable
  PARTUUID values; malformed, overlapping, or contradictory metadata fails
  visibly without publishing unsafe extents.
- `make -j16` and the two storage regressions pass; `make check` is not used.

## Reconsideration boundary

Return to p023 if a failure is within command/queue ownership. Create a
separate block/VFS Phase if correct NVMe completion exposes a pre-existing
generic cache, raw-device, or partition-lifetime defect.
