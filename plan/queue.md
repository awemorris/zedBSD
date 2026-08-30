# Queue: BIOS `zedbsd.cfg` convergence

Last updated: 2026-08-30

QID: `q032`

Queue status: completed

Queue finished: **Yes**

Authorization: the user explicitly requested that the three planned
`BOOTZBSD.EXE` configuration targets be placed in a Queue and implemented on
2026-08-30.

Timebox: no fixed wall-clock limit. Complete or honestly mark `uncleared` each
finite Phase; stop only for a real human decision that cannot be resolved from
the frozen p003 contract and existing loader behavior.

Parent: [master plan](master.md)

Previous Queue: [q031](queue-q031.md)

## Purpose

Converge all supported x86 BIOS `BOOTZBSD.EXE` loaders on the bounded
configuration language already implemented by amd64 UEFI. i386 PC/AT and
amd64 BIOS consume root `/zedbsd.cfg`; PC-98 consumes root
`/BOOTZBSD.CFG` under the earlier filename decision. The grammar, configured
kernel selection, shorthand, exact parameter record, and no-fallback behavior
are otherwise identical.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws013-p005` | [Phase](ws013-containers/phase005-pcat-bios-zedbsd-config/phase.md) | completed | i386 PC/AT and amd64 BIOS reach payload PBR -> `BOOTZBSD.EXE` -> `/zedbsd.cfg`; one amd64 hybrid image shares its payload configuration with UEFI |
| 2 | `ws013-p006` | [Phase](ws013-containers/phase006-pc98-bootzbsd-config/phase.md) | completed | PC-98 `BOOTZBSD.EXE` consumes `/BOOTZBSD.CFG` with exact p003 behavior and boots overlay/native roots |

## Dependency order

```text
q031 p002/p003 UEFI language and selected-FAT contract
        |
        +--> ws013-p005 PC/AT i386 + amd64 BIOS
        |
        `--> ws013-p006 PC-98 (reuses parser findings, not loader source)
```

The two Phases may share conformance vectors. Their loader implementations
remain independent unless an already-stable boundary makes reuse simpler.

## Frozen behavior

- `/zedbsd.cfg` and `/BOOTZBSD.CFG` use the p003 v1 language: printable ASCII,
  bounded `name=value` lines, exactly one loader-only `kernel=`, and every
  other line passed through the existing textual kernel-parameter ABI.
- Missing `boot0=` synthesizes the current payload FAT UUID. Relative
  `overlay-root`, `overlay-data`, and boot-file `swapN` values receive
  `boot0:`; explicit boot slots, raw swap selectors, `rootpart=`, and `init=`
  retain their p003 meaning.
- Missing/malformed/over-limit configuration, missing or invalid configured
  kernel, or parameter overflow stops visibly. No embedded parameter record,
  fixed `VMUNIX`, old direct-kernel path, or alternate configuration is a
  production fallback.
- i386 PC/AT and amd64 BIOS are separately accepted. amd64 must use its active
  payload FAT PBR and MZ `BOOTZBSD.EXE`, replacing the reserved-area direct
  Stage-2 kernel path while retaining valid hybrid GPT/MBR and UEFI boot.
- PC-98 retains only the requested legacy filename difference. It does not
  gain a second UEFI-style filename fallback.
- The current i386 PC/AT and PC-98 physical-disk publication failure is a
  preflight defect, not a relaxation of Phase completion. Repair it in a
  bounded shared prerequisite when the root cause is common, or record a
  platform-local repair in the owning Phase.

## Execution rules

- Do not inspect or modify `.internal/` or `userland/noct/NoctLang`.
- Preserve unrelated work. Use `make -j16` and focused Phase fixtures; do not
  use `make check`.
- Use disposable image copies for negative or destructive media cases.
- Validate parser parity with common host vectors and validate each production
  loader through its real image chain in QEMU.
- After each completed Phase, synchronize P/W/M/Q, commit `WIP`, and push.
  If push is unavailable, retain the local commit and continue.
- A new product/ABI decision makes only the affected item `uncleared`; record
  the decision request and continue independent work.

## Verification contract

- Host fixtures compare exact parameter records for the same p003 valid and
  invalid corpus across UEFI, PC/AT BIOS, and PC-98 policy.
- i386 PC/AT boots overlay and native-root cells through the active FAT PBR
  and configured `BOOTZBSD.EXE`.
- One amd64 hybrid image boots through SeaBIOS and OVMF using the same payload
  FAT configuration, kernel, images, and UUID.
- PC-98 boots overlay and native-root cells through `BOOTZBSD.EXE` and
  `/BOOTZBSD.CFG`.
- Negative configuration and kernel cases stop without embedded/fixed-name or
  direct-loader fallback; MZ/PBR size ceilings and image placement are checked.
- `make -j16` passes after both Phases.

## Completion evidence

- `ws013-p005` passed its 20/20 production-loader matrix: i386 overlay/native,
  amd64 SeaBIOS IDE, SeaBIOS xHCI USB, OVMF xHCI USB, LFN/subdirectory, missing
  and invalid configuration/kernel, PBR/BPB, and ELF bound failures.
- `ws013-p006` passed its 16/16 production-PBR matrix: overlay/native,
  1024-byte-sector LFN, missing and invalid configuration/kernel, executable
  entry, undersized reserved-area/volume, wrapped start LBA, and malformed
  PBR/cluster/chain cases.
- Shared configuration and FAT directory host tests passed ordinary,
  sanitizer, and available static-analysis gates. Migrated BR-T46 image-helper
  dry runs passed for PC/AT, amd64 BIOS, and amd64 UEFI.
- The dedicated chain-Stage-2 artifact rebuilt correctly with a stale legacy
  `stage2.o` present. Generic BIOS and GPT checkers rejected a one-byte-different
  declared Stage 2. Stage 1 executable/signature identity checks passed for
  PC/AT, amd64, and PC-98, and rejected one-byte and wrong-loader inputs.
  Atomic checker failure preserved the previous published image and left no
  unchecked temporary. FAT/GPT/native-UFS mismatch poisoning tests left no
  extraction temporary and their immediate correct-input retries passed;
  backend/checker failure paths also preserved the prior target and removed
  both unchecked siblings. Per-medium GPT GUID generation and primary/backup
  validation also passed.
- A normal amd64 `config.mk` rebuild regenerated the root staging tree,
  packaging tarball, UFS image, kernel, loader, and final hybrid image.
  `make -j16`, public PC/AT/PC-98 image checks, the amd64 GPT checker,
  shell syntax checks, and `git diff --check` passed.
- No Phase required a new product or ABI decision. The remaining PC-98 FAT32
  PBR layout and direct-DOS volume mapping questions are recorded as later
  WS013 backlog, outside the completed FAT16 production contract.

## Completion definition

q032 is finished when p005 and p006 are each `completed` or honestly
`uncleared`; exact implementation and QEMU evidence are synchronized into the
Phase, WS, Queue, and master books; and no supported BIOS image silently uses
the replaced fixed-name or embedded-parameter behavior.
