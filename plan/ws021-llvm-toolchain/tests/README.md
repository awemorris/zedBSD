# WS021 test and evidence index

Last updated: 2026-09-02

This directory will own maintained LLVM acquisition, triple, sysroot,
tool-command audit, loader-format and final QEMU runners. Disposable LLVM
source/build trees, sysroots, disk images and raw logs remain ignored below
`build/` or a Phase `temp/` directory.

## Test groups

- `LLVM-T001`--`T009`: archive identity, unsafe-input, strict-patch and
  extraction fixtures.
- `LLVM-T010`--`T019`: triple, macro, ELF format and driver selection.
- `LLVM-T020`--`T029`: installed host-tool completeness and incremental state.
- `LLVM-T030`--`T039`: amd64/i386 sysroot header/startup/libc/builtins contract.
- `LLVM-T040`--`T049`: kernel/userland/target-Noct compile, link and no-fallback
  audit.
- `LLVM-T050`--`T059`: BIOS/UEFI loader and image-format/layout closure.
- `LLVM-T060`--`T065`: the single final six-cell big-bang QEMU matrix defined
  by `ws021-p006`.

P001--p005 do not require QEMU. Their tests reject source, compiler, ABI,
linker, PE/ELF and dependency defects before the final runtime campaign.
