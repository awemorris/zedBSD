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
- `LLVM-T020`--`T024`: installed host-tool completeness and incremental state.
- `LLVM-T025`--`T029`: deterministic host archive, disposable extraction,
  pinned digest, remote `rev-0` identity and cache-only bootstrap.
- `LLVM-T030`--`T039`: amd64/i386 sysroot header/startup/libc/builtins contract.
- `LLVM-T040`--`T049`: kernel/userland/target-Noct compile, link and no-fallback
  audit.
- `LLVM-T050`--`T059`: BIOS/UEFI loader and image-format/layout closure.
- `LLVM-T060`--`T065`: the single final six-cell big-bang QEMU matrix defined
  by `ws021-p006`.

P001--p005 do not require QEMU. Their tests reject source, compiler, ABI,
linker, PE/ELF and dependency defects before the final runtime campaign.

## Q064 final evidence

The ignored evidence root is
`plan/ws021-llvm-toolchain/temp/q064-final/`:

- `matrix-final-4/results.tsv`: all six amd64 firmware/Variant cells pass;
- `pcat-final/result.txt`: i386 PC/AT reaches the exact login checkpoint;
- `pc98-final/`: maintained qemu-pc98 reaches login and passes the Xzed mouse
  oracle without mutating the source image;
- `noct-smoke-final-2/results.tsv`, `noct-jit-final/results.tsv`, and
  `noct-beui-final-2/results.tsv`: target noct non-JIT, real RW-to-RX/JIT, and
  upstream BeUI graphics/evdev gates pass on amd64 UEFI;
- `ci-local/`: `make download`, `make toolchain-cache`, `make toolchain`, and
  all four `config/ci/` builds pass in workflow order.
