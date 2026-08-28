# WS008 Phase 005: independent BeUI platform implementations

Last updated: 2026-08-28

WSID: `ws008`

Phase ID: `p005`

Combined ID: `ws008-p005`

Status: Planned; Queue-ready

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
   `noct_register_api_beui()` symbol and remove the dispatcher and shared
   backend source.
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

- canonical NoctLang contains no `api-beui.c`, `api-beui-backend.c`, shared
  backend registrar, or platform-suffixed public BeUI registrar;
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
backends in one executable, if an external embedding API requires runtime HAL
injection, or if preserving a public BeUI language contract requires an API
change. Do not restore a shared implementation merely because the independent
sources contain substantial duplicate code.
