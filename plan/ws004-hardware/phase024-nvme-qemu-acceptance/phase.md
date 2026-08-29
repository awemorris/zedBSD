# WS004 Phase 024: QEMU NVMe block acceptance

Last updated: 2026-08-30

Phase ID: `ws004-p024`

Status: complete in `q030`

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

WS013/WS019 first own the existing-FAT overlay install/boot. Native-root and
destructive GPT administration follow separately. This Phase proves the driver
and partition substrate only.

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

### Selection and publication contract

- PC/AT selection is per disk. Media with no GPT evidence retains the pure
  legacy-MBR scanner. A protective `0xee` entry or a primary/last-LBA GPT
  signature is GPT evidence; once evidence exists, every protective-MBR or GPT
  failure is terminal for that scan and must never fall back to legacy MBR.
- The existing amd64 BIOS/UEFI image intentionally contains one active FAT MBR
  entry for the BIOS chain loader in addition to exactly one canonical
  protective entry. This compatibility entry may coexist with a valid GPT, but
  it is ignored completely: it is neither reconciled with nor published in
  preference to GPT. The protective entry itself must be inactive, start at
  LBA 1, cover `min(last LBA, UINT32_MAX)`, and be unique. This preserves the
  shared BIOS/UEFI image without weakening the no-fallback rule.
- Each primary/backup copy is validated independently. Either one fully valid
  copy may be used with a visible degraded diagnostic when its peer is damaged.
  If both are valid, disk GUID, usable range, entry count/size, and the exact
  entry-array bytes must agree; disagreement rejects the disk.
- GPT revision is 1.0; header size is at least 92 bytes and no larger than the
  logical block; both the fixed reserved field and the header tail through the
  logical-block boundary are zero. Header and entry CRCs cover the exact
  declared bytes. Header/table locations, the UEFI minimum 16-KiB entry-array
  reservation, checked byte/block arithmetic, usable bounds, and
  primary/backup roles must be self-consistent for both 512-byte and 4096-byte
  logical blocks.
- Entry count is bounded at 4096, and entry size is `128 * 2^n` no larger than
  4096 bytes. Every
  declared entry is read and validated before success, including entries after
  the caller's output capacity. Active entries require nonzero type and unique
  GUIDs, an inclusive extent inside the usable range, no duplicate unique GUID,
  no overlap, zero reserved extension bytes, and zero UEFI-reserved attribute
  bits 3--47. Type-specific attribute bits 48--63 remain available. More active
  entries than capacity returns `ENOSPC` without publication; no partial table
  is accepted.
- `p_index` is the original GPT slot, so a hole before slot N still publishes
  `/dev/nvme0n1pN`. PARTUUID uses the canonical lowercase EFI mixed-endian GUID
  spelling. The full 36-code-unit UTF-16LE name is validated (including paired
  surrogates) and converted to bounded UTF-8 before any partition is created.

The hybrid compatibility rule above is a selector rule only. GPT remains the
sole authority whenever GPT evidence exists, and malformed GPT can never expose
the BIOS compatibility MBR entry as a writable partition.

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

## Result

Completed on 2026-08-30.

- Added the architecture-neutral strict read-only GPT parser and a PC/AT
  per-disk selector. Pure MBR media retains the legacy parser; any protective
  MBR or GPT-header evidence selects GPT permanently for that scan.
- Both GPT copies are independently validated. One valid copy is accepted
  read-only with a degraded diagnostic; two valid but contradictory copies and
  two invalid copies are rejected without publishing a partition.
- Publication preserves the original GPT slot, canonical PARTUUID, complete
  bounded UTF-8 label, one-based arbitrary-width partition name, and 64-bit
  extent diagnostics. Capacity and allocation failures publish nothing.
- The ordinary, sanitizer, and analyzer host fixtures passed for 512/4096-byte
  sectors, valid/degraded/contradictory copies, malformed headers/tables,
  unreadable primary/backup recovery, minimum array reservation, reserved
  header/entry/attribute rejection, overlap/GUID/range/capacity rejection,
  UTF-16 conversion, MBR fallback, and partition-publication rollback.
- The final OVMF/q35 test wrote, flushed, and read back `/dev/nvme0n1p1` below
  and above 4 GiB, exercised 96-command stress and four concurrent workers,
  recreated QEMU/controller state, and verified every pattern. A separate
  namespace with both GPT headers damaged was rejected without MBR fallback or
  partition publication while the IDE-root system continued to `login:`.
- `make -j16`, the configured i386 PC/AT kernel build, amd64 IDE boot, and
  amd64 xHCI USB-root boot passed. `make check` was not used.

Retained evidence: [q030 NVMe GPT evidence](../tests/q030-nvme-gpt-evidence.md).
Physical SN740 inspection and all physical writes remain outside this Phase.
