# WS019 Phase 009: target `/sbin/mkswap`

Last updated: 2026-09-05

WSID: `ws019`

Phase ID: `p009`

Combined ID: `ws019-p009`

Status: planned; follows p002 and precedes p004

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

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
  otherwise busy object is refused using the stable p002 storage snapshot.
- The command opens without following a final symlink, validates the opened
  identity and size, obtains exclusive access, and writes the production
  ZEDSWAP2 header and slot map used by WS016. It does not invent a new swap
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
