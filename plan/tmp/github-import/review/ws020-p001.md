# WS020 Phase 001: generic target Variant

Last updated: 2026-08-31

WSID: `ws020`

Phase ID: `p001`

Combined ID: `ws020-p001`

Status: Completed (revised 2026-08-31)

Parent: [WS020](../ws.md)

## Objective

Extend the saved target model to Architecture -> Board -> Variant without
changing source selection or the compiled amd64 kernel/loader set. Remove the
discarded target-disk-capacity setting completely.

## Work

1. Represent Variants as board-owned data rather than branches embedded in the
   menu. Boards with one Variant display and save that fixed default.
2. Offer the amd64 PC/AT values in this order and with these labels:
   `hybrid` = `UEFI + BIOS (for PC/AT)`, `uefi` = `UEFI (for Apple)`, and
   `bios` = `BIOS (for PC/AT)`.
3. Save and validate `ZEDBSD_VARIANT` in `config.mk`. Loading an old config
   supplies the board default (`hybrid` for amd64 PC/AT) without changing
   unrelated selections.
4. Remove `Select disk image size`, `ZEDBSD_IMAGE_SIZE_GIB`, its defaults,
   validation, persistence, fixtures, and documentation. An obsolete line in
   an old hand-written config is ignored and disappears on the next save.
5. Keep Variant out of kernel CPP flags, source/object composition, and loader
   selection. Both BIOS and UEFI loader families are built for every amd64
   PC/AT Variant.

## Verification

- Noninteractive round-trip tests cover every architecture/board, all three
  amd64 Variants, old-config defaults, and invalid Variant values.
- With identical driver/userland settings, hashes of `vmunix`, BIOS loader
  artifacts, and `BOOTX64.EFI` are equal across all three amd64 Variants.
- `make bootloader` produces both firmware families for every amd64 Variant.
- No production or maintained WS020 test refers to the removed capacity field.
- `make -j16`, the menu fixture, and `git diff --check` pass; do not use
  `make check` or `.internal/`.

## Completion conditions

The hierarchy and saved Variant are generic, validated, backward-compatible
for old configuration files, and affect image composition only. The removed
capacity selection has no active code, UI, test, or documentation contract.

## Prior result and revision

The original q036 implementation proved the generic board-owned Variant model,
old-config repair, and compiled-artifact invariance, but also introduced a
2--256-GiB capacity selector. The user removed that selector on 2026-08-31
after determining that Apple's relevant requirement is the pure Protective MBR
rather than capacity-matched GPT geometry. This Phase is reopened only to
remove that discarded axis and rerun the retained Variant evidence.

## Revised result

- `make menuconfig-host-test`: PASS for all six maintained targets, all three
  amd64 PC/AT Variants, legacy configuration defaulting, obsolete capacity-line
  removal on save, and invalid target/Variant rejection.
- `target-artifact-invariance.noct`: PASS in three fresh private build trees.
  `vmunix`, both BIOS Stage-1 forms, Stage 2, partition PBR,
  `BOOTZBSD.EXE`, and `BOOTX64.EFI` hashes, object manifests, and compile
  contracts are identical for `hybrid`, `uefi`, and `bios`.
- The build menu and top-level validation no longer define, display, save, or
  consume `ZEDBSD_IMAGE_SIZE_GIB`. The requested labels and order are exact.

Evidence is retained under `../temp/p001-revised-final/`.
