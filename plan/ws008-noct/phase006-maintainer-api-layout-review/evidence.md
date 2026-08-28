# WS008 Phase 006 execution evidence

Recorded: 2026-08-28

Queue: `q024`

Automated result: passed. Engineering acceptance: rejected by subsequent
Principal Engineer review. Phase status: `uncleared`.

This file records why the automated gates initially appeared green. It is not
an assertion that the implementation is acceptable or complete.

## Protected inputs

- Canonical checkout: `userland/noct/NoctLang`
- Entry and exit `include/noct/noct.h` SHA-256:
  `90c2115d53840fe3d6c1fdff6751676a35d473d2503fc9a3d9c179e1fb22a7b3`
- The header remained the maintainer's pre-existing modified file relative to
  canonical HEAD. q024 did not rewrite, format, stage, or otherwise change it.
- `include/noct/#beui.h#`, `include/noct/.#beui.h`, and
  `include/noct/noct.h~` remained untracked maintainer files. They were not
  consumed as source, edited, deleted, or staged.
- The Noct index remained empty. No Noct or zedBSD commit, push, revision-pin
  update, reset, checkout, or clean operation was performed.

## Implemented result

- ANSI and Win32 Term each independently implement the fixed
  `noct_register_api_term()` interface. The common callback backend and its
  types/registrar are gone, and CMake selects exactly one Term source.
- File API no longer has a directory callback backend. Its private JIS X 0208
  table remains available for EUC-JP decoding, and the public
  `FileUtil.readTextEucJp()` regression covers ASCII, two-byte JIS X 0208, and
  half-width katakana input.
- SDL2, zedBSD, and PC-98 BeUI are complete per-platform translation units
  behind only `noct_register_api_beui()`. Shared BeUI core/image/internal
  sources and the redundant public `beui.h` are gone.
- PC-98 GDC, Cirrus, glyph/JIS, selector, and DOS glue are contained in one
  `api-beui-pc98.c`; enabling File API is no longer a BeUI dependency.
- Accelerator and regex sources are selected from `src/accel/` and
  `src/core/`. Documentation and maintained tests use the accepted paths and
  aggregate public header.

## Verification matrix

| Gate | Result | Evidence |
| --- | --- | --- |
| `NOCT-T050` protected inputs | Pass | Final header hash matches the entry hash; `git diff --cached --name-only` is empty; protected backup status is unchanged |
| `NOCT-T051` source/API audit | Pass | Source/CMake/test/doc scan finds no live removed callback interface, common BeUI implementation, redundant `beui.h`, old moved-source path, or split PC-98 implementation; absence assertions in maintained audit scripts are intentional |
| `NOCT-T052` host/API | Pass | Clean strict static and shared host builds; public File/Term registration, real directory listing, EUC-JP decoding, and VM-local object-model tests pass |
| `NOCT-T053` Win32 Term | Pass | MinGW release build links standalone Win32 Term and exports only the fixed registrar; the Debug whole-program build separately encountered a pre-existing unrelated unused local in `module.c`, while `noctapi` itself built |
| `NOCT-T054` SDL2/PC-98 BeUI | Pass | SDL2 dummy-video/audio core/BMP execution passes; PC-98 core/BMP, GDC, and Cirrus host fixtures pass; target-stub strict compilation exports only the fixed BeUI registrar |
| `NOCT-T055` zedBSD BeUI | Pass | `zedbsd` preset cross-build, sanitized evdev/backend fixture, source/wiring audit, and linked single-registrar audit pass |
| `NOCT-T056` accelerators | Pass for available gates | Moved-source audit and full CPU/static accelerator suite pass. OpenGL/Vulkan development packages and DX12 toolchain/hardware were unavailable and are not claimed as passes |
| `NOCT-T057` outer integration | Pass | `make -j16`, top-level/Noct `git diff --check`, install-header audit, and amd64 non-JIT, BeUI, and JIT QEMU gates pass |

The canonical QEMU results were:

- `qemu-noct-smoke.sh`: pass (`NOCT-T003`)
- `qemu-beui-zedbsd.sh`: pass (`NOCT-T011`--`NOCT-T013`)
- `qemu-noct-jit.sh`: pass (`NOCT-T020`--`NOCT-T022`)

One first non-JIT run failed before reaching Noct when the existing IDE path
reported `ata: sda op=2 ... error=5` during overlay-data initialization. A
fresh identical rerun passed every gate. This incidental, pre-Noct timing
observation is retained here rather than misclassified as a q024 API-layout
failure.

The host lacked enough free `/tmp` space for the final test binary, so the
existing clean build products and QEMU evidence were copied/generated under
the ignored `plan/ws008-noct/temp/` area. This did not change production
inputs. `make check` and `.internal/` were not used.

## Honest unavailable coverage

- OpenWatcom was not installed, so actual PC-98 compiler execution was not
  claimed; combined-PC-98 host fixtures and a strict target-stub compile
  passed.
- OpenGL/Vulkan development packages and a DX12 toolchain/device were absent.
  CPU/static accelerator coverage passed, and unavailable optional backends
  remain explicitly unclaimed.
- On non-POSIX targets, `FileUtil.listDirectory()` retains its existing empty
  result rather than the removed callback injection path. Expanding native
  Windows/DOS directory enumeration is a separate functional Phase, not a
  reason to restore the rejected public backend interface.
