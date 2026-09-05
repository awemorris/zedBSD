# WS019 Phase 004: existing-FAT overlay `/bin/zedinst`

Last updated: 2026-09-05

Phase ID: `ws019-p004`

Status: planned Noct implementation; follows p002, p003, p008, and p009

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Objective

Install exactly one overlay environment from the ordinary USB system into an
existing ESP plus an explicitly selected existing FAT32, without formatting
either partition, changing its table or labels, or writing firmware variables.

## Dependencies

- `ws019-p002`, `ws019-p003`, `ws019-p008`, and `ws019-p009`;
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
3. Resolve and verify only `BOOTX64.EFI`, `vmunix`, and the immutable
   `rootfs.img` from the current USB boot filesystem. Create fixed-size empty
   regular-file staging objects for `data.img` and `swapfile`, format them
   through `/sbin/mkfs` and `/sbin/mkswap`, and generate the fixed direct
   `/zedbsd.cfg`. The live writable overlay upper and active swap are forbidden
   as copy sources.
4. Display source identity, destination identities, capacities, and all six
   managed destination paths and obtain one explicit confirmation.
5. Stage, flush, digest, and rename each copied or generated file. Accept an
   exact existing file; refuse any non-identical conflict. A child-command or
   generation failure removes only its unpublished staging object.
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

## Selected source and generation contract

The live `DATA.IMG` and `SWAPFILE` are never copied. Installer templates are
not introduced. P008 creates the same UFS1 format already consumed by the
overlay upper; p009 creates the same ZEDSWAP2 format already consumed by the
swap subsystem. For installer v1, `zedinst` creates a 32-MiB `data.img` and a
64-MiB `swapfile`, matching the current image defaults. Size selection is a
later installer feature.

## Reconsideration boundary

Stop if FAT cannot provide a bounded same-filesystem publication primitive,
if installer source identity cannot be proven, or if success would require
rewriting an existing non-identical file.
