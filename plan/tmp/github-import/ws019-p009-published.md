<!-- awesome-plan project=zedbsd record=ws019-p009 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws019/phase009/phase.md`

親: [ws019](https://github.com/awemorris/zedBSD/issues/20)

# WS019 Phase 009: target `/sbin/mkswap`

Last updated: 2026-09-06

WSID: `ws019`

Phase ID: `p009`

Combined ID: `ws019-p009`

Status: completed in q079 after q078 implementation

Parent: [WS019](https://github.com/awemorris/zedBSD/issues/20)

Tests: [WS019 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/tests/README.md)

## Objective

Add a target-side initializer for the existing ZEDSWAP2 file format so
installer v1 can create a fresh swap file instead of copying active swap or
shipping a template.

## Initial command contract

```text
mkswap FILE
```

- `FILE` must already exist, be a writable regular file, and have the exact
  desired page-aligned size. `mkswap` neither creates nor resizes it.
- A mounted, root, overlay, already active swap, non-regular, aliased, or
  otherwise busy object is refused using a descriptor-owned backing reservation. P002's completed
  diagnostic mount listing is not an exclusion token.
- The command opens without following a final symlink, validates the opened
  identity and size, obtains exclusive access, and writes the production
  ZEDSWAP2 header and page-aligned slot area used by WS016. The
  allocation bitmap is kernel memory, not an on-disk slot map. It does not invent a new swap
  format or silently fall back to ZEDSWAP1.
- Success is printed only after flush, reopen, production-parser validation,
  and a checked slot count derived from the file size.

## Scope limits

- no block-device/partition target, resize, label option, activation, repair,
  overwrite prompt, or format-version selector;
- no installer template and no copy from the source system's active
  `SWAPFILE`.

`zedinst` owns creation of its unpublished 64-MiB staging file and invokes
this command only after preflight and explicit installer confirmation.

## Completion conditions

- Focused fixtures validate the result with the production ZEDSWAP2 parser
  and compare all deterministic layout invariants with the maintained host
  `make-swapfile` path.
- Wrong object kind, short/unaligned size, symlink, identity change,
  mounted/root/overlay/active-swap alias, short write, flush failure, and
  parser failure produce nonzero status and never claim success.
- A QEMU target invocation creates the regular-file format on FAT32;
  `/sbin/swapon` accepts it, reports positive slots, and `/sbin/swapoff`
  deactivates it cleanly.
- Existing image-time swap generation and WS016 regressions remain passing.

## Reconsideration boundary

Adding block-device swap, labels, resize, activation inside `mkswap`, or a new
format version requires a later Phase and does not broaden installer v1.

## Q078 implementation boundary

The first implementation supports pre-sized FAT-backed regular files with
canonical identity and complete allocated extents; other containing
filesystems fail with `EOPNOTSUPP` before content writes. A descriptor-owned
reservation excludes other content/namespace mutations and swap/loop
activation until final close. Idle descriptors and read-only verification
opens remain possible; shared mappings/cache objects and in-flight mutations
prevent acquisition. Private read snapshots do not grant backing mutation.

The original descriptor retains its reservation while the command reopens
read-only and compares identity/size. Kernel production parsers are linked
into the target tools; no generic probe ioctl or comprehensive storage
snapshot is added. Failure never prints success and never resizes the file.

ZEDSWAP2 accepts 4096-byte-aligned sizes from 8192 through 2,147,479,552
bytes, bounded for the existing file-backed path on both user ABIs. Slot count
is `bytes / 4096 - 1`; installer v1 selects 64 MiB and 16,383 slots.

## Completion evidence

[Q078 implementation and diagnosed attempts](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/tests/q078-results.md) and
[Q079 final acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws019-installation/tests/q079-results.md) close this Phase. Production-
linked normal/sanitizer fixtures and all three supported builds pass. The
isolated QEMU cell passes target formatting, refusals, swap activation and
deactivation, generated overlay/swap boot, two-reboot persistence, and
production-input/GPT/FAT/sentinel invariance.

Only required reserved metadata and allocated initial contents are initialized;
unused areas are not an erasure guarantee. The fixture supplies pre-sized
zero files, so installer staging-allocation performance remains a p004 finding.
