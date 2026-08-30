# WS019 Phase 004: existing-FAT overlay `/bin/zedinst`

Last updated: 2026-08-29

Phase ID: `ws019-p004`

Status: planned; blocked by stable installer-source decision

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Objective

Install exactly one overlay environment from the ordinary USB system into an
existing ESP plus an explicitly selected existing FAT32, without formatting,
partition-table writes, label changes, or firmware-variable writes.

## Dependencies

- `ws019-p002` and `ws019-p003`;
- WS013 p002 `zedbsd.cfg` discovery and p003 parameter translation;
- the stable NVMe block/GPT path from `ws004-p024`.

## Procedure contract

1. The user names one whole GPT disk and one payload partition; the installer
   never defaults either from enumeration order.
2. Preflight requires exactly one same-disk usable FAT32 ESP, a distinct
   writable same-disk FAT32 payload, enough free space, no mounted/root/swap
   alias, and no other same-disk `/zedbsd.cfg` marker. Although the loader can
   warn and choose its first candidate, the installer must guarantee that its
   selected payload is the only installed candidate.
3. Resolve and verify only stable source artifacts from the current USB boot
   filesystem, then generate the fixed direct `/zedbsd.cfg`. The live
   writable overlay upper and active swap are forbidden as copy sources.
4. Display source identity, destination identities, capacities, and all six
   managed destination paths and obtain one explicit confirmation.
5. Stage, flush, digest, and rename each new file. Accept an exact existing
   file; refuse any non-identical conflict.
6. Reopen and verify all six final files, their payload uniqueness, the
   unchanged GPT, unchanged labels, unchanged UEFI variables, and unmanaged
   sentinel files before success.

The Phase must define an option grammar no broader than this one transaction.
There is no whole-disk, native, format, overwrite, or noninteractive mode.

## Completion conditions

- Host/model fixtures cover every preflight and publication failure boundary,
  including power-loss-style interruption between files.
- A disposable QEMU guest executes the real target command from the ordinary
  USB root and produces exactly the fixed two-partition file layout.
- GPT bytes, filesystem format/labels, UEFI variable store, and unmanaged
  sentinels compare unchanged.
- Repeating the installer over byte-identical managed files succeeds
  idempotently; a single differing managed byte causes refusal.

## Reconsideration boundary

Stop if FAT cannot provide a bounded same-filesystem publication primitive,
if installer source identity cannot be proven, or if success would require
rewriting an existing non-identical file.

## Blocking source-safety decision

The ordinary USB system actively writes its `DATA.IMG` overlay upper and uses
its `SWAPFILE` as swap. Neither is a coherent installation template, even if a
single read sometimes completes. Copying active swap is specifically invalid.

Before this Phase enters a Queue, select one of these contracts:

- add unused immutable installer templates such as `/INSTALL/DATA.IMG` and
  `/INSTALL/SWAPFILE` to the source image and copy only those (recommended); or
- redesign p004 to create the data image and swap object on the target, with
  new bounded allocation/initialization and failure semantics.

`BOOTX64.EFI`, the kernel, and read-only `rootfs.img` may continue to be
verified from their existing source locations. This decision does not block
WS013 p002/p003 or WS019 p002/p003.
