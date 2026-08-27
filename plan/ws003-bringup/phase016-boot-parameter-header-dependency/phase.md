# WS003 Phase 016: boot-parameter generated-header isolation

Last updated: 2026-08-27

WSID: `ws003`

Phase ID: `p016`

Combined ID: `ws003-p016`

Status: Planned; Queue-ready; not in the active Queue

Parent: [WS003](../ws.md)

Contract: [kernel boot parameters](../../../docs/reference/kernel-boot-parameters.md)

Shared tests: [WS003 test index](../tests/README.md)

## Objective

Make the generated boot-parameter header and every loader/image that consumes
it follow the currently selected `ZEDBSD_BOOT_PARAMETERS_FILE`, even when a
single platform build directory is reused across default and custom builds.
Changing the make-variable value must never leave a newer header generated
from the previous input authoritative.

## Defect and cause

During q017, an ordinary default `make -j16` reused
`build/amd64/generated/boot-parameters.h` from an earlier Queue's custom
`ZEDBSD_BOOT_PARAMETERS_FILE`. The generated target was newer than both the
custom and default text files, so the existing timestamp-only rule did not
run when the variable changed back to its default. The stale header then
contaminated an unrelated QEMU boot.

The defect is not in the boot-parameter grammar or loader handoff. Make's
dependency graph currently represents the selected file's timestamp, but not
the identity or generated content selected by the command-line variable.

## Scope

- `Makefile` dependency rules for
  `build/PLATFORM/generated/boot-parameters.h`;
- the existing boot-parameter header generator if content-aware replacement
  is needed to avoid unnecessary downstream rebuilds;
- amd64 BIOS and UEFI loader objects and the combined disk image that consume
  the generated header;
- one Phase-owned automatic default/custom/default switching fixture; and
- preservation of the user's `config.mk` and normal `make -j16` workflow.

## Required behavior

1. A default build generates the exact text from
   `bootloader/default-boot-parameters.txt`.
2. A subsequent build in the same `build/amd64` tree with
   `ZEDBSD_BOOT_PARAMETERS_FILE=CUSTOM` regenerates every affected loader and
   image with the custom text.
3. A third ordinary default build in that same tree, without cleaning,
   regenerates every affected loader and image with the default text again.
4. Selection is based on the effective input identity/content, not on which
   candidate file happens to have the newest modification time.
5. Repeating a build with unchanged selected content must not rewrite the
   header merely to advance its timestamp and must not cause perpetual loader
   rebuilds.

A valid implementation may use a selected-input provenance/content stamp or
an always-checked, compare-before-replace generator. It must make actual
content changes visible to Make dependencies while leaving an identical
generated header byte- and timestamp-stable. Deleting the build tree,
requiring `make clean`, touching an input, or teaching Queue scripts to remove
the header is not a fix.

## Work packages

1. Add BR-T47 under `plan/ws003-bringup/tests/` to reproduce the transition in
   one unchanged amd64 build directory: default, distinctive valid custom
   parameters, then default again.
2. Correct the generated-header dependency/provenance rule and, if necessary,
   make the generator replace its output only when the bytes differ.
3. At every transition, verify the generated header and extract the embedded
   parameter record from both the production amd64 BIOS loader and
   `BOOTX64.EFI`; neither loader may retain the previous transition's text.
4. Save a disposable copy of each of the three resulting disk images and use
   bounded `qemu-system-x86_64` boots to observe the expected parameter/init
   behavior from each copy. Runtime evidence must distinguish the custom
   image from both default images rather than relying only on source text.
5. Repeat the final default build once and prove that unchanged content does
   not rewrite the header or rebuild its loader consumers.
6. Hash `config.mk` before and after the fixture, run the normal `make -j16`
   gate, and run `git diff --check`. Do not run `make check` or consume
   `.internal/` material.

## Completion conditions

- BR-T47 fails against the stale-header behavior and passes after the fix;
- default/custom/default switching succeeds without a clean or targeted
  deletion between builds;
- each of the three saved build images contains and boots with the parameter
  text selected for that build;
- both amd64 BIOS and UEFI production loader records match on every
  transition;
- a repeated unchanged default build leaves the generated header and loader
  outputs unchanged rather than creating a rebuild loop;
- the pre/post `config.mk` hash is identical;
- `make -j16`, the automatic fixture, and `git diff --check` pass; and
- the result and exact evidence directory are synchronized into this Phase
  and WS003.

## Dependencies

Depends on completed `ws003-p011`, `p012`, and `p015`. It has no physical
hardware dependency and requires no new boot-parameter syntax or product
decision.

## Reconsideration boundary

Stop for human review only if correctness would require changing the public
boot-parameter grammar or making clean builds part of the supported workflow.
An ordinary Make/generator defect, fixture defect, or QEMU observation defect
inside the fixed default/custom/default contract remains implementation work
for this Phase.

