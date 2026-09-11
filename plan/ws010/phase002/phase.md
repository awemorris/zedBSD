# ws010-p002: x86 build-tool migration

WSID: `ws010`

Phase ID: `p002`

Status: complete

Parent WS: [WS010](../ws.md)

## Objective

Replace every Python script and invocation reached by amd64, PC-98 i386, and
PC/AT i386 `disk-image` builds with Noct equivalents while preserving their
binary-format validation and failure behavior.

## Work packages

- [x] Add shared Noct binary/endian/argument/process helpers.
- [x] Port ELF, loader, MZ, filesystem, overlay, and BIOS-image tools used by x86.
- [x] Port amd64 GPT/UEFI and PC-98 kernel patch/check tools.
- [x] Replace Makefile `$(PYTHON)` calls on the three build paths.
- [x] Build and structurally validate all three images.

## Frozen migration inventory

The dependency closure contains 15 repository scripts. This is the whole
WS010 migration set; repository Python outside this list is not part of this
workstream.

| Area | Scripts |
| --- | --- |
| Shared image formats | `make-data-image.py`, `overlay_journal_format.py`, `ufs1_format.py`, `make-arch-overlay-ufs.py` |
| Shared boot images | `make-bios-hdd-image.py`, `check-bios-hdd-image.py`, `make-swapfile.py`, `make-mz-exe.py`, `finalize-bios-stage2.py` |
| Shared ELF validation | `check-user-elf.py` |
| amd64 | `check-amd64-vmunix.py`, `check-bootx64.py`, `check-amd64-gpt-image.py` |
| PC/AT | `check-pcat-vmunix.py` |
| PC-98 | `patch-stage2.py` |

The inventory was derived from fresh forced dry-runs of the three production
disk-image targets plus imports/subprocess calls. It does not consult or include
`.internal/`.

## Completion conditions

- A dry-run and actual disk-image build for each x86 target contains no Python invocation.
- Each migrated tool rejects representative malformed input and validates its output.
- All three disk-image artifacts are produced and pass their Noct structural checks.

## Completion record

- Shared binary/endian/file helpers are implemented in `tools/build/zedbuild.noct`.
- `make-mz-exe`, `make-swapfile`, and `finalize-bios-stage2` match the prior
  Python output byte-for-byte on focused fixtures.
- `patch-stage2`, `check-user-elf`, `check-amd64-vmunix`,
  `check-pcat-vmunix`, and `check-bootx64` have Noct replacements; the amd64
  kernel, user ELF, UEFI loader, and swap-image production rules run them.
- The UFS/data/overlay writers, BIOS/GPT image builder, and all three platform
  validators now run through Noct. The format-heavy byte writer is the small
  repository-owned `zedimage-host` backend invoked by Noct.
- Fresh production builds produced and structurally validated amd64, PC/AT
  i386, and PC-98 i386 disk images.
- `.internal/` has not been used as a source or test dependency.
