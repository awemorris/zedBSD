# WS020 Phase 001: generic target Variant and image capacity

Last updated: 2026-08-30

WSID: `ws020`

Phase ID: `p001`

Combined ID: `ws020-p001`

Status: Completed (2026-08-30)

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
   and the documented 2-GiB default capacity without changing unrelated
   selections.
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

## Result

Completed in q036. `tools/menuconfig.py` now owns generic
Architecture -> Board -> Variant metadata keyed by `(architecture, board)`.
amd64 PC/AT exposes `hybrid`, `bios`, and `uefi`; every other maintained board
has the singleton `default` Variant. The independent 2/4/8/16/32/64/128/256-
GiB selector and both values are saved in menu version 3. Legacy configurations
default to their board Variant and 2 GiB without changing unrelated fields.

The top-level build has the same target table and validates the complete
Platform/Architecture/Board/Variant/capacity contract for default,
`disk-image`, `run`, `check-disk-image`, and direct `.img`/`.hd` goals. Empty,
multiword, wildcard, cross-board, nonnumeric, and out-of-range hand edits are
rejected before a standard image build begins. Neither setting is present in
kernel/loader CPP flags, source lists, object lists, or loader selection.

Evidence:

- `make menuconfig-host-test`: PASS for six maintained targets times all eight
  capacities, all three amd64 Variants times all capacities, all six old
  menu-version-2 target defaults, menu repair, Make fallback, and the negative
  validation matrix.
- `target-artifact-invariance.noct`: PASS in four fresh amd64 build trees
  (`hybrid`/`bios`/`uefi` at 2 GiB and `hybrid` at 256 GiB). Each tree contains
  `vmunix`, four BIOS artifacts, and `BOOTX64.EFI`; all six hashes, all 190
  object paths, and all 233 source/object/compile-flag contract rows are
  exactly equal.
- Representative hashes are `638dd19f...` (`vmunix`), `97e47d7c...`
  (`stage1.bin`), `24f38976...` (`stage2-chain.bin`), `c0086225...`
  (`partition-pbr.bin`), `6668c92d...` (`BOOTZBSD.EXE`), and `42e26920...`
  (`BOOTX64.EFI`).
- `make -j16`, Python syntax compilation, and `git diff --check`: PASS.

Reusable fixtures are under `../tests/`; disposable logs and manifests are
under `../temp/p001-artifact-invariance/`.
