# WS019 Phase 004: existing-FAT overlay `/bin/zedinst`

Last updated: 2026-08-29

Phase ID: `ws019-p004`

Status: planned; dependency-gated

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Objective

Install exactly one overlay environment from the ordinary USB system into an
existing ESP plus an explicitly selected existing FAT32, without formatting,
partition-table writes, label changes, or firmware-variable writes.

## Dependencies

- `ws019-p002` and `ws019-p003`;
- WS013 p002 payload discovery and p003 `boot.cfg` translation;
- the stable NVMe block/GPT path from `ws004-p024`.

## Procedure contract

1. The user names one whole GPT disk and one payload partition; the installer
   never defaults either from enumeration order.
2. Preflight requires exactly one same-disk usable FAT32 ESP, a distinct
   writable same-disk FAT32 payload, enough free space, no mounted/root/swap
   alias, and no second same-disk `/vmunix` plus `/boot.cfg` marker pair.
3. Resolve and verify the five source artifacts from the current USB boot
   filesystem, then generate the fixed one-section `boot.cfg`.
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
