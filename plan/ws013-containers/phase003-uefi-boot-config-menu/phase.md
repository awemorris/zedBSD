# WS013 Phase 003: UEFI `boot.cfg` menu and translation

Last updated: 2026-08-29

Phase ID: `ws013-p003`

Status: planned; depends on `ws013-p002`

Parent: [WS013](../ws.md)

Tests: [WS013 review and test index](../tests/README.md)

## Objective

Read a bounded `/boot.cfg` from the selected payload FAT32, select one complete
section through timeout/default or keyboard input, and translate it into the
already implemented textual kernel parameters while preserving p002's
explicit `boot0=PARTUUID=...`.

## Initial grammar

```ini
timeout=5
default=zedBSD

[zedBSD]
rootfs=boot0:rootfs.img
datafs=boot0:data.img
swap=boot0:swapfile
```

Global keys are `timeout` and `default`. A unique section contains exactly
one native `rootpart`, or exactly one overlay `rootfs` plus `datafs`, and at
most one `swap`. Unknown keys, duplicates, missing default, duplicate section,
mixed root modes, malformed text, or an incomplete section fail visibly.

The implementation Phase must fix conservative file, line, title, key, value,
section-count, and final-parameter limits within the existing 3071-byte kernel
parameter ceiling. Parsing is ASCII and allocation-bounded; quoting and
escaping are not added.

## Menu and translation

- Display complete section titles, highlight the exact `default`, and count
  down `timeout` seconds.
- Minimal controls are up/down, Enter, and Escape to return to the menu; no
  file picker, version sorting, shell, or configuration editing is added.
- Overlay fields become `overlay-root=`, `overlay-data=`, and optional
  `swap0=`. Native fields become `rootpart=` and optional `swap0=`.
- The p002 `boot0=PARTUUID=...` token is always retained for the installed
  two-partition path and cannot be overridden by a hidden default.
- A missing `/boot.cfg` is an error for the installed path. The legacy
  single-partition default remains a separate compatibility branch.

## Completion conditions

- Parser fixtures cover every grammar and bound, missing versus invalid input,
  timeout boundaries, key sequences, final-string overflow, and exact emitted
  parameters.
- OVMF timed-default and manual-selection cells boot the corresponding
  complete section from the payload FAT32.
- The installer one-section configuration reaches the overlay root and swap;
  the ordinary legacy image remains usable.

## Reconsideration boundary

Return to planning if the menu needs a second manifest, new kernel handoff ABI,
or any filesystem mutation.
