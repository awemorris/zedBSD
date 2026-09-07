# WS019 Phase 004: existing-FAT overlay `/bin/zedinst`

Last updated: 2026-09-06

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

## Readiness findings after q078/q079

The formatter prerequisites now have implementation and focused evidence;
their separate runtime results are recorded under p008/p009. They do not
resolve these pre-implementation boundaries for the installer:

- The loader knows the physical ESP and selected configuration filesystem,
  but the retained kernel data does not prove both identities. An explicitly
  configured `boot0` may differ from loader origin. Define the promised
  provenance interface before using it to choose immutable source artifacts.
- Noct 2.0.1 provides file I/O, process execution and hashing, but the present
  target bindings do not export the required descriptor ioctl, no-follow open,
  fsync, ftruncate, mount and rename operations. Specify a bounded native
  primitive helper or bindings extension while retaining Noct as the installer.
- Existing rename replaces a destination; a prior lookup does not provide
  atomic no-replace publication. Specify a primitive serialized with the
  filesystem namespace mutation, together with a durable publication barrier.
  FAT directory descriptors currently do not implement fsync.
- Blank FAT creation accepts the representable root-owned mode `0755` with
  zero umask; ordinary `touch` mode `0666` is refused. Target growth of a
  32-MiB staging file exceeded 120 seconds in q078 while allocating about
  20 MiB. Formatter acceptance uses preallocated zero inputs and therefore
  does not establish bounded installer staging performance.

These are concrete dependency/design findings, not installer acceptance.
Resolve them in bounded prerequisite contracts before selecting p004 code
implementation. Do not substitute mount listings for source provenance or
check-then-rename for no-replace publication.

## Unified formatter dependency (q101)

Use `mkfs -t ufs FILE` and the ordinary profile under the existing reservation
transaction. WS024 is implementing the driver/producer migration and retains
old-media rejection and fresh-file initialization boundaries. Do not infer
in-place conversion from the new public name. This update does not start the
installer phase; its existing prerequisites still apply.
