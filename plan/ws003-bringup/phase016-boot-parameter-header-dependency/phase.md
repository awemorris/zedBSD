# WS003 Phase 016: static image boot parameters and Python-regression removal

Last updated: 2026-08-28

WSID: `ws003`

Phase ID: `p016`

Combined ID: `ws003-p016`

Status: In progress (`q023`)

Parent: [WS003](../ws.md)

Contract: [kernel boot parameters](../../../docs/reference/kernel-boot-parameters.md)

Shared tests: [WS003 test index](../tests/README.md)

## Objective

Make the default parameter text embedded in each x86 boot image ordinary,
maintained source. Remove the generated `boot-parameters.h`, its Python
generator, its text input, and the `ZEDBSD_BOOT_PARAMETERS_FILE` build-time
selection mechanism. Restore the WS010 contract that supported x86 production
image builds do not invoke Python after `make toolchain` has built Noct.

## Defect and cause

WS010 completed the migration of the amd64, i386 PC/AT, and i386 PC-98 image
dependency graphs away from Python on 2026-08-25. The boot-parameter work added
`tools/build/make-boot-parameters-header.py` on 2026-08-27, after that
acceptance, and therefore reintroduced a Python dependency into all three
supported x86 loader builds.

The generated target also has an incorrect dependency model. Its fixed output
path does not encode which `ZEDBSD_BOOT_PARAMETERS_FILE` supplied the content,
so a custom/default transition can retain the preceding, newer generated
header. Source generation is unnecessary for a fixed legacy image default and
is contrary to the selected project style. This Phase removes the mechanism
rather than repairing its cache key.

## Fixed design

- `ZEDBSD_IMAGE_BOOT_PARAMETERS_TEXT` is defined directly in the maintained
  boot parameter header under `include/boot/`.
- `ZEDBSD_BOOT_PARAMETERS_DEFAULT_TEXT` aliases the same source definition so
  loader-image fallback and kernel fallback cannot drift.
- C derives the byte length as
  `sizeof(ZEDBSD_IMAGE_BOOT_PARAMETERS_TEXT) - 1U`.
- Assembly derives the byte length from labels around
  `.ascii ZEDBSD_IMAGE_BOOT_PARAMETERS_TEXT`; no manually synchronized numeric
  length macro is retained.
- amd64 UEFI stores its fallback as one fixed-size
  `zedbsd_boot_parameter_record`, with the C initializer length derived by
  `sizeof`. This gives every x86 loader the same bounded record representation.
- The production Makefiles do not generate or force-include a parameter header
  under `build/`, and do not accept `ZEDBSD_BOOT_PARAMETERS_FILE`.
- amd64 UEFI LoadOptions remain the supported immediate runtime override.
  Future UEFI `boot.cfg` menu selection remains a separate WS013 facility.
- Legacy i386 PC/AT, i386 PC-98, and amd64 BIOS images keep their fixed source
  default until an independently designed runtime configuration path exists.
- Test-only non-default loader fixtures patch validated BPR1 records in
  disposable loader copies with a Phase-owned Noct helper. They must not
  restore a production source generator, rewrite the maintained header in the
  user's working tree, alter a production artifact, or require Python.

## Scope

- the common source header and x86 BIOS, PC-98, and UEFI loader consumers;
- removal of `bootloader/default-boot-parameters.txt`,
  `tools/build/make-boot-parameters-header.py`, and their top-level Make rules;
- removal of generated-header prerequisites and forced includes from the x86
  platform Makefiles;
- adaptation of WS003 and WS016 reusable acceptance runners that currently set
  `ZEDBSD_BOOT_PARAMETERS_FILE`;
- one Phase-owned static-source/no-Python regression, `BR-T47`; and
- preservation of `config.mk`, the common public parameter grammar, and the
  normal `make toolchain` followed by `make -j16` workflow.

No new parameter spelling, loader handoff structure, root/swap behavior,
`boot.cfg` parser, or runtime configuration UAPI is in scope.

## Work packages

1. Move the image-default text into the maintained common header, alias the
   kernel fallback to it, and make both C and assembly derive the length.
2. Delete the text input and Python generator, remove their Make variables and
   targets, and remove all generated-header dependencies/forced includes.
3. Add a source/build audit that rejects the deleted paths and symbols and
   rejects Python execution in forced amd64, i386 PC/AT, and i386 PC-98
   production disk-image dependency traces after `make toolchain`.
4. Add a Phase-owned Noct artifact helper which finds exactly one valid,
   fixed-size BPR1 record in a disposable loader copy, validates its complete
   header/text/padding contract, and replaces only its length and text area.
   Patch PC/AT and PC-98 `bootzbsd.raw`, amd64 BIOS `stage2.raw` and
   `bootzbsd.raw`, and the fixed UEFI record in `BOOTX64.EFI`. Re-run the
   existing Noct finalizer/MZ builder for patched BIOS artifacts and the EFI
   checker for patched UEFI artifacts.
5. Adapt affected WS003/WS016 acceptance fixtures to build each production
   loader once and create parameterized disposable images from patched loader
   copies. Preserve the non-default parser, handoff, root, and swap coverage;
   verify pre/post hashes of every production artifact and do not expose a
   replacement production source-generation knob.
6. Extract and validate the embedded record from the production PC/AT, PC-98,
   amd64 BIOS, and amd64 UEFI loaders, then boot the default production image
   on all four QEMU paths and observe the exact static parameter string.
7. Remove the targeted generated-header workarounds from the WS008 Noct QEMU
   runners, rerun focused host parameter/handoff tests and affected WS008/WS016
   regressions, then run `make -j16` and `git diff --check`. Do not use
   `make check` or `.internal/`.

## Verification

- `BR-T47a`: source audit finds exactly one maintained image-default text,
  finds no generated header/text input/Python generator/build-file selector,
  and verifies C/assembly length derivation rather than a hand-maintained
  length constant.
- `BR-T47b`: after `make toolchain`, forced dry-run/dependency traces for the
  three supported x86 disk images contain no Python interpreter invocation.
- `BR-T47c`: the PC/AT, PC-98, amd64 BIOS, and amd64 UEFI production loader
  records contain the exact source default and each QEMU path reports that
  exact parameter string.
- `BR-T47d`: the Noct patcher rejects zero, duplicate, malformed, oversized,
  non-ASCII, unterminated, and unsupported BPR1 records; affected non-default
  WS003 parameter and WS016 swap fixtures still exercise their accepted
  behavior without a production generated-header mechanism or Python
  dependency, and production artifact hashes remain unchanged.

## Completion conditions

- all generated boot-parameter inputs, outputs, variables, rules, and Python
  code are absent;
- `ZEDBSD_IMAGE_BOOT_PARAMETERS_TEXT` and the kernel fallback have one source
  definition, with derived C and assembly lengths;
- all `BR-T47a`--`BR-T47d` gates pass, including the four x86 QEMU paths;
- the affected prior parameter and runtime-swap regressions remain usable;
- the pre/post `config.mk` hash is identical;
- `make -j16`, the Phase-owned fixtures, and `git diff --check` pass; and
- results and exact evidence paths are synchronized into this Phase, WS003,
  the master plan, and the executing Queue.

## Dependencies

Depends on completed `ws003-p011`, `p012`, `p015`, WS010, and `ws016-p004`.
It has no physical-hardware dependency and requires no new public syntax or
product decision.

## Reconsideration boundary

Stop for human review only if removing the production build-time selector
would eliminate a currently required user-facing legacy boot configuration
path, or if preserving the accepted parameter behavior requires changing the
public grammar or handoff ABI. A Makefile, test-fixture, C/assembly layout, or
Noct tooling defect inside the fixed static-source contract remains
implementation work.
