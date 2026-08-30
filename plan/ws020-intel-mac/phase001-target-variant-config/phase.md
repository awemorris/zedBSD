# WS020 Phase 001: generic target Variant and image capacity

Last updated: 2026-08-30

WSID: `ws020`

Phase ID: `p001`

Combined ID: `ws020-p001`

Status: Planned; Queue-ready after WS018

Parent: [WS020](../ws.md)

## Objective

Extend the saved target model to Architecture -> Board -> Variant and add the
fixed target-disk-capacity choice without changing source selection or the
compiled amd64 kernel/loader set.

## Work

1. Represent variants as board-owned data rather than branches embedded in the
   menu.  Boards with one variant display and save that fixed default; amd64
   PC/AT offers `hybrid`, `bios`, and `uefi`.
2. Add `Variant:` below `Board:` in `Select target`, plus `Select disk image
   size` with 2/4/8/16/32/64/128/256-GiB choices.
3. Save validated `ZEDBSD_VARIANT` and `ZEDBSD_IMAGE_SIZE_GIB` in `config.mk`.
   Loading an old config supplies the board default (`hybrid` for amd64 PC/AT)
   and a documented default capacity without changing unrelated selections.
4. Reject invalid hand-edited values before image generation.  Keep the values
   out of kernel CPP flags and source/object composition.
5. Update the maintained menuconfig host fixture.  Do not add a new Python
   dependency; if the current menu implementation is migrated to Noct by a
   preceding accepted change, extend that owner rather than restoring Python.

## Verification

- Noninteractive round-trip tests cover every architecture/board, every amd64
  variant, all eight capacities, old-config defaults, and invalid values.
- With identical driver/userland settings, hashes of `vmunix`, BIOS loader
  artifacts, and `BOOTX64.EFI` are equal across all three amd64 variants.
- `make bootloader` produces both firmware families for every amd64 variant.
- `make -j16`, the menu fixture, and `git diff --check` pass; do not use
  `make check` or `.internal/`.

## Completion conditions

The hierarchy and saved values are generic, validated, backward-compatible for
old config files, and affect only image composition.
