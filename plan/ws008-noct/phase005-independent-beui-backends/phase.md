# WS008 Phase 005: independent BeUI platform implementations

Last updated: 2026-08-28

WSID: `ws008`

Phase ID: `p005`

Combined ID: `ws008-p005`

Status: Completed (`q023`, 2026-08-28)

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

## Objective

Remove the premature shared BeUI implementation and dispatcher from canonical
NoctLang. Each selected platform source owns the complete implementation behind
the stable public `noct_register_api_beui(NoctEnv *env)` interface, so one
platform backend can be replaced without changing or constraining another.

This Phase applies the project's interface-based modularity and late-abstraction
preference. Source duplication introduced by separating the implementations is
accepted deliberately; it is not a completion defect.

## Fixed design

- Delete `src/api/api-beui.c` and `src/api/api-beui-backend.c` from canonical
  NoctLang.
- `api-beui-zedbsd.c`, `api-beui-sdl2.c`, and `api-beui-pc98dos.c` each provide
  the public `noct_register_api_beui(NoctEnv *env)` entry point and all BeUI FFI
  registration required by that platform.
- A configured build selects exactly one of those platform implementation
  sources. Supporting multiple BeUI platform backends in one executable is not
  a requirement.
- There is no target dispatcher, shared backend registrar, or public
  platform-suffixed registrar such as `noct_register_api_beui_zedbsd()`.
- The stable caller-facing interface remains
  `bool noct_register_api_beui(NoctEnv *env)`. Existing CLI callers do not gain
  platform conditionals.
- `noct_register_api_beui_with_hal()` is removed from the public header,
  exported symbols, and documentation. Its HAL is Noct's backend function
  table, not a supported embedding interface; any equivalent setup helper is
  private to the selected platform source.
- Backend-private helpers may remain inside their owning
  `api-beui-<platform>.c`. Do not introduce a replacement common implementation
  merely to remove duplicated code.
- Existing zedBSD evdev and `/dev/graphics` behavior, SDL2 behavior, and both
  PC-98 graphics modes remain behaviorally compatible.
- This Phase does not change the Boolean JIT interface, target macro policy,
  zedBSD UAPI, BeUI language API, or zedBSD's Makefile-only Noct source-delivery
  model.

## Implementation plan

1. In a clean canonical NoctLang checkout, inventory the FFI registration and
   backend-neutral helpers currently owned by `api-beui-backend.c`.
2. Move or reproduce the complete required implementation in each selected
   `api-beui-<platform>.c`, preserving that platform's existing HAL behavior.
3. Rename each selected backend's registrar to the one public
   `noct_register_api_beui()` symbol, internalize any platform-local setup
   helper, remove `noct_register_api_beui_with_hal()` from the public header
   and documentation, and remove the dispatcher and shared backend source.
4. Change CMake source selection so every supported configuration compiles
   exactly one complete platform source and never compiles two definitions of
   the public registrar.
5. Update canonical tests and zedBSD WS008 audits to enforce source ownership,
   single-backend linkage, and behavioral parity rather than shared-source
   reuse.
6. Publish the accepted canonical NoctLang revision, advance both
   `userland/noct/Makefile` and the separate `build/NoctLang` toolchain checkout
   to that revision, then rerun the affected zedBSD package/QEMU gates.

## Verification

- `NOCT-T040`: source/CMake audit proves `api-beui.c` and
  `api-beui-backend.c` are absent, each supported platform source defines
  `noct_register_api_beui()`, and each configured target selects exactly one
  definition.
- `NOCT-T041`: canonical static/SDL2 tests pass, including generic BeUI and
  dummy-window behavior, without a shared implementation source.
- `NOCT-T042`: canonical PC-98 GDC and Cirrus tests pass from the independent
  PC-98 implementation.
- `NOCT-T043`: the zedBSD BeUI host sanitizer/input corpus and source audit
  pass from the independent zedBSD implementation.
- `NOCT-T044`: the existing amd64 zedBSD BeUI QEMU acceptance passes and the
  installed Noct artifact is built from the newly published pinned revision.
- Run repository `make -j16` and `git diff --check`. Do not run `make check` or
  consume `.internal/`.

## Completion conditions

- canonical NoctLang contains no `api-beui.c`, `api-beui-backend.c`, public
  `noct_register_api_beui_with_hal()`, shared backend registrar, or
  platform-suffixed public BeUI registrar;
- every supported build links one platform-owned implementation of
  `noct_register_api_beui()` and all `NOCT-T040`--`NOCT-T044` gates pass;
- the accepted canonical commit is published and zedBSD reproducibly pins it
  for both the userland package and build toolchain;
- p004's compiler-defined `__ZEDBSD__`, Boolean JIT boundary, toolchain-only
  CMake setup, and Makefile-only source acquisition remain intact.

## Authorization boundary

Execution changes canonical `awemorris/NoctLang` and then advances the zedBSD
pin. The Queue that executes this Phase must authorize verification, commit,
and publication in both repositories. Planning this Phase alone does not alter
either implementation.

## Reconsideration boundary

Return for human review if canonical Noct must support two BeUI platform
backends in one executable or if preserving the public BeUI language contract
requires another API change. Runtime HAL injection is explicitly not a public
contract. Do not restore a shared implementation merely because the
independent sources contain substantial duplicate code.

## Execution result

Completed in q023 without reaching the reconsideration boundary.

- Canonical NoctLang commit
  `c1e4e0fcdbb7b8cdf1705601b13d57b787c61621` was committed and pushed to
  `awemorris/NoctLang`. Both the host toolchain pin in the top-level Makefile
  and the userland package pin now select that exact revision.
- `api-beui.c` and `api-beui-backend.c` were deleted. The PC-98, SDL2, and
  zedBSD platform sources each own their complete FFI registration layer and
  define exactly one `bool noct_register_api_beui(NoctEnv *)`; CMake rejects a
  BeUI configuration that does not select exactly one implementation.
- `noct_register_api_beui_with_hal()` and every platform-suffixed registrar
  were removed from source, headers, documentation, and linked artifacts.
  The installed `include/noct/beui.h` exposes only
  `noct_register_api_beui()`. HAL types, `noct_beui_bind()`, core, and image
  contracts moved to non-installed `src/api/beui-internal.h`.
- The shared-library visibility target was corrected from `noct` to
  `noctapi`. The final `readelf` audit records the generic registrar as
  `GLOBAL DEFAULT` and every `noct_beui_*` implementation symbol as `LOCAL`;
  unrelated `NOCT_DLL` APIs remain global.
- `NOCT-T040` and `NOCT-T043` passed at
  `plan/ws008-noct/temp/q023-p005-backends.P8ZsxV`. Canonical zedBSD
  ASan/UBSan/wiring tests, exact-one negative CMake configurations, installed
  header boundary, and shared-symbol audits passed.
- `NOCT-T041` and `NOCT-T042` passed through the canonical `run-beui.sh`
  corpus: generic core, SDL2 dummy video/audio, PC-98 GDC, and PC-98 Cirrus.
- `NOCT-T044` passed through the complete BeUI QEMU acceptance at
  `plan/ws008-noct/temp/q020-p002-beui.unH7qL`. Drawing, evdev keyboard and
  pointer input, post-BeUI console use, artifact identity, screenshot pixels,
  and the unsuffixed-only linked registrar audit all passed.
- The pinned revision also passed the non-JIT QEMU smoke at
  `plan/ws008-noct/temp/q019-p001-noct.Q5DH4P`, the JIT/RW-to-RX QEMU corpus at
  `plan/ws008-noct/temp/q020-p003-jit.0mYri3`, and the updated p004 host
  regression at `plan/ws008-noct/temp/q022-p004-review.KBH0on`.
- `make -j16 toolchain`, the canonical zedBSD build, focused outer acceptance,
  script syntax checks, and `git diff --check` passed. `make check` and
  `.internal/` were not used.
