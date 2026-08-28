# WS008 Phase 004: upstream review corrections and source delivery

Last updated: 2026-08-28

WSID: `ws008`

Phase ID: `p004`

Combined ID: `ws008-p004`

Status: Complete (`q022`)

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

## Objective

Apply the maintainer's review to canonical NoctLang, replace zedBSD's Noct
gitlink with one package Makefile that obtains a reproducible official source
checkout, and revalidate the already accepted zedBSD BeUI and JIT behavior.

## Fixed source-delivery contract

- The zedBSD repository tracks exactly `userland/noct/Makefile` below
  `userland/noct/`; it no longer tracks a submodule/gitlink or a copied Noct
  source tree there.
- `make -C userland/noct` clones `https://github.com/awemorris/NoctLang.git`
  into `userland/noct/NoctLang/`, checks out the Phase's accepted upstream
  commit, configures the `zedbsd` preset, and builds it. Repository and
  revision variables may override the defaults for testing.
- The fetched `userland/noct/NoctLang/` tree is ignored by the parent zedBSD
  repository. Package and acceptance rules consume that checkout rather than
  requiring `git submodule update`.
- The host `make toolchain` checkout under `build/NoctLang` remains separate
  and is updated to the same accepted upstream revision.

## Canonical NoctLang corrections

1. Merge all zedBSD evdev state/discovery implementation from
   `beui-zedbsd-input.c` and its private header into
   `api-beui-zedbsd.c`; delete the split files and update CMake/tests.
2. Make the zedBSD compiler flags define `__ZEDBSD__`. Teach
   `c89compat.h` to select `NOCT_TARGET_ZEDBSD` and `NOCT_TARGET_POSIX` from
   that compiler macro instead of requiring callers to define the Noct-private
   target macro directly.
3. Add `api-beui.c` as the sole target dispatcher. Public callers use
   `noct_register_api_beui(env)` once; the dispatcher selects zedBSD, PC-98
   DOS, SDL2, or the null backend at compile time. Rename or internalize the
   existing HAL-taking registrar so it is not confused with the public target
   dispatcher, and remove the repeated target `#ifdef` chains from CLI call
   sites.
4. Keep `jit_build`, `jit_commit`, and `jit_free` returning `bool` on every
   backend. The accepted upstream main already has this boundary; this Phase
   treats any `void` declaration/definition or ignored failure as a regression
   and retains forced allocation, protection, and release failure tests.
5. Delete `cmake/modules/Platform/zedBSD.cmake` and move its static-platform
   settings into `cmake/toolchains/zedbsd-amd64.cmake`. A clean
   `cmake --preset zedbsd` must work without a project-owned CMake Platform
   module.

## Verification

- `NOCT-T030`: from a parent tree containing only
  `userland/noct/Makefile`, `make -C userland/noct` creates the canonical
  checkout at the pinned revision and a second invocation is idempotent;
  invalid repository/revision failures are explicit.
- `NOCT-T031`: source and object audits find one zedBSD BeUI implementation,
  one public `noct_register_api_beui(env)` call per CLI path, no split input
  source/header, and no `cmake/modules/Platform/zedBSD.cmake`.
- `NOCT-T032`: clean static and `zedbsd` preset builds pass; compile commands
  contain `__ZEDBSD__`, `c89compat.h` derives the intended target macros, and
  no Linux host target macro leaks into the zedBSD artifact.
- `NOCT-T033`: BeUI host/sanitizer tests and SDL/PC-98 regressions pass after
  the merge and dispatcher change.
- `NOCT-T034`: every JIT backend preserves the Boolean boundary; forced
  allocation, publish/protection, and release failures propagate, while the
  existing amd64 RW-to-RX QEMU JIT acceptance remains positive.
- Re-run the canonical zedBSD non-JIT smoke and BeUI QEMU acceptance where
  affected, then run repository `make -j16` and `git diff --check`. Do not run
  `make check` or use `.internal/`.

## Completion conditions

- the canonical fixes exist in a published `awemorris/NoctLang` commit;
- zedBSD contains no Noct gitlink and tracks only the package Makefile below
  `userland/noct/`;
- both the package checkout and host toolchain select the accepted upstream
  revision reproducibly;
- the merged BeUI backend, single registration dispatcher, `__ZEDBSD__`
  target selection, Boolean JIT failure boundary, and toolchain-only CMake
  platform configuration pass NOCT-T030--T034;
- prior q019/q020 zedBSD build, BeUI, input, and amd64 JIT behavior remains
  accepted.

## Authorization boundary

This Phase changes two repositories. Execution approval must explicitly cover
commit and push to `awemorris/NoctLang` as well as the enclosing zedBSD
repository. Without upstream publication, record the Phase as `uncleared` and
do not pin zedBSD to an unpublished working-tree state.

## Reconsideration boundary

Stop for human review if a single `api-beui.c` dispatcher cannot retain a
public custom-HAL embedding entry point without an API rename, or if CMake
requires an external platform module to preserve the zedBSD target identity.
Do not silently keep the gitlink, duplicate the input backend, revert a JIT
failure result to `void`, or accept a host-target build as zedBSD evidence.

## Result

Completed on 2026-08-28.

- Canonical NoctLang commit
  `eba2043ca74b8601d68a405ecbbeca50ca8d5ac0` was committed and pushed to
  `awemorris/NoctLang` main. It contains the merged zedBSD BeUI source, the
  centralized `api-beui.c` dispatcher, compiler-defined `__ZEDBSD__`, the
  retained Boolean JIT boundary, and the toolchain-only zedBSD CMake setup.
- The zedBSD gitlink and its `.gitmodules` entry were removed. The only
  parent-owned file below `userland/noct/` is `Makefile`; it pins the published
  commit and obtains the official source in the ignored
  `userland/noct/NoctLang/` directory. A fresh official GitHub clone, detached
  revision selection, a second idempotent invocation, and explicit invalid
  repository/revision failures passed.
- `NOCT-T030`--`NOCT-T034` host acceptance passed in
  `plan/ws008-noct/temp/q022-p004-review.ZiCmTO`. A separate SDL2 shared-library
  build passed the SDL dummy-window, generic BeUI, PC-98 GDC, and PC-98 Cirrus
  tests; the merged zedBSD state engine also passed ASan/UBSan.
- The final post-relocation QEMU records passed:
  `NOCT-T003` in `q019-p001-noct.SgWSWw`, `NOCT-T011`--`T013` in
  `q020-p002-beui.KRdwVL`, and `NOCT-T020`--`T022` in
  `q020-p003-jit.ezkOBq`.
- `make -j16`, shell syntax checks, source/object audits, and
  `git diff --check` passed. `make -j16 toolchain` also advanced the separate
  `build/NoctLang` checkout to the same pinned commit and passed the WS010
  toolchain smoke. The QEMU runners now force regeneration of the
  selected boot-parameter header before their build so prior parameter-matrix
  artifacts cannot leak into a later acceptance cell.
