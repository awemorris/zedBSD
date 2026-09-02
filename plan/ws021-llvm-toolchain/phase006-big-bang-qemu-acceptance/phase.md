# WS021 Phase 006: consolidated x86 big-bang QEMU acceptance

Last updated: 2026-09-02

WSID: `ws021`

Phase ID: `p006`

Combined ID: `ws021-p006`

Status: planned

Parent: [WS021](../ws.md)

Depends on: `ws021-p001`--`ws021-p005`

## Objective

Perform one final consolidated runtime campaign after the LLVM, sysroot,
kernel, userland, target Noct, BIOS and UEFI migrations are all complete.
Intermediate Phases deliberately do not use repeated QEMU success as a gate.

## Frozen matrix

| Cell | Image/firmware | Required checkpoint |
| --- | --- | --- |
| `LLVM-T060` | amd64 hybrid / SeaBIOS | BIOS chain, configured root/swap, init and exact `login:` |
| `LLVM-T061` | amd64 hybrid / OVMF | UEFI chain, configured root/swap, init and exact `login:` |
| `LLVM-T062` | amd64 UEFI-only / OVMF q35+xHCI USB root | exact `login:`, target Noct non-JIT, JIT/RW-to-RX and BeUI graphics/evdev |
| `LLVM-T063` | amd64 BIOS-only / SeaBIOS | BIOS-only layout reaches exact `login:` without UEFI payload dependency |
| `LLVM-T064` | i386 PC/AT / BIOS | configured root/swap, init and exact `login:` |
| `LLVM-T065` | i386 PC-98 / maintained pc9821 QEMU | native IPL/BOOTZBSD path, configured root/swap, init and exact `login:` |

Every cell uses a fresh disposable copy of its production-checked image. No
source image is mutated by runtime acceptance.

## Work packages

1. Freeze compiler, patch, sysroot and artifact manifests before the first
   runtime cell.
2. Run the six cells without rebuilding between firmware variants except where
   a distinct image Variant is the subject of the cell.
3. In `LLVM-T062`, prove the installed `/usr/bin/noct` is the exact
   LLVM/sysroot-built package artifact before running non-JIT, explicit
   RW-to-RX/JIT, and canonical upstream BeUI input/graphics checks.
4. Compare boot diagnostics against the pre-migration contracts; a known,
   separately deferred hardware/IDE bug is not silently reclassified as a
   compiler success or failure.
5. If a defect appears, retain the full matrix result, debug with the narrowest
   relevant cell, and rerun the complete six-cell matrix once after correction.
6. Record hashes, logs, exact commands, tool versions and pass/fail outcomes;
   feed the target Noct result back to WS008 without reopening its upstream
   BeUI/API design.

## Completion conditions

- All six cells pass in one final matrix with no host target GNU/MinGW tool in
  the recorded build closure.
- The runtime-visible kernel, userland, dynamic/static startup, filesystem,
  swap, init and loader behavior matches the pre-migration contract.
- Target Noct passes non-JIT, real JIT/RW-to-RX and upstream BeUI acceptance
  as a normal sysroot-built userland package.
- WS021 records exact LLVM/archive/patch/tool/sysroot/image identities and can
  be marked complete without a physical-hardware checkpoint.
