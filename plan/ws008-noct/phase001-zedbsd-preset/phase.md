# WS008 Phase 001: canonical Noct zedBSD CMake preset

Last updated: 2026-08-27

WSID: `ws008`

Phase ID: `p001`

Combined ID: `ws008-p001`

Status: Planned; Queue-ready

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

## Objective

Make the canonical Noct source configure and build an amd64 zedBSD executable
through `cmake --preset zedbsd`, then install that artifact through the existing
zedBSD package/image path and prove it is a zedBSD program rather than a host
binary.

## Fixed contract

- Canonical Noct provides configure and build presets both named `zedbsd`.
  The acceptance commands are:

  ```sh
  cmake --preset zedbsd
  cmake --build --preset zedbsd --parallel 16
  ```

- A required zedBSD source/sysroot location may be supplied through one clearly
  named environment/cache variable. The preset must fail with an actionable
  message when it is absent or invalid; it may not silently use host headers or
  host libc.
- The initial target is amd64 LP64, static, canonical Noct CLI/runtime, with JIT
  compiled in. JIT execution is deliberately accepted only in p003.
- p001 may leave BeUI disabled. It must not preserve a fake success by linking
  an empty upstream object list or by continuing to install the old hand-linked
  binary under the same path.
- The produced executable has zedBSD's ELF ABI/link layout, contains no host
  ELF interpreter or unresolved host dependency, and is the file installed as
  `/usr/bin/noct` when the package is selected.
- Canonical target/CMake changes live in the official Noct checkout. zedBSD
  retains only build inputs that it owns: UAPI/libc/linker/sysroot integration,
  package metadata, installation, and revision provenance.
- No commit, push, or submodule gitlink update is part of this Phase.

## Work packages

- [ ] Record clean baseline revisions and path-scoped status for
      `/home/awe/NoctLang` and `userland/noct` before editing.
- [ ] Add a `NOCT_TARGET_ZEDBSD` target selection, amd64 toolchain integration,
      and matching `zedbsd` configure/build presets to canonical Noct.
- [ ] Select only APIs supported by the current zedBSD libc and fail configure
      for incompatible shared-library or host-only options.
- [ ] Move or replace only the target entry/runtime pieces required to produce
      the canonical executable; do not migrate BeUI in this Phase.
- [ ] Make the zedBSD Noct package consume the CMake target artifact, eliminate
      obsolete per-platform manual Noct object/link lists for amd64, and retain
      explicit source provenance.
- [ ] Add focused preset/artifact checks under `plan/ws008-noct/tests/`.
- [ ] Run the non-JIT language smoke in `qemu-system-x86_64`, then run the
      supported `make -j16` image gate.
- [ ] Produce a parity manifest for the official and integration Noct checkout
      changes without committing either tree.

## Acceptance

- `NOCT-T001`: a clean configure with `cmake --preset zedbsd` succeeds; a
  missing or bad zedBSD root fails visibly before compilation.
- `NOCT-T002`: `cmake --build --preset zedbsd --parallel 16` succeeds and an
  ELF audit verifies x86-64, static zedBSD layout, no host interpreter, and no
  unresolved host library dependency.
- `NOCT-T003`: selecting the package installs that exact artifact at
  `/usr/bin/noct`; a QEMU guest runs a deterministic non-JIT script and returns
  its expected stdout/status.
- `make -j16` succeeds. The aggregate `make check` target is not used.
- Official/integration Noct path manifests match for the files changed by this
  Phase, and no duplicate target implementation is introduced under
  `userland/base`.

## Completion conditions

- The literal configure/build commands above build the canonical amd64 zedBSD
  target from a clean target build directory.
- The installed guest executable is the CMake target artifact and passes the
  non-JIT QEMU smoke.
- Host contamination and the old empty/manual-link false-success paths are
  excluded by machine-readable checks.
- p002 can add a zedBSD BeUI backend without redesigning target selection or
  package installation.

## Failure and resume rules

A missing ordinary libc symbol or CMake target flag may be fixed locally. If a
successful target requires a new ABI, general sysroot/install framework, shared
library policy, or a product choice between canonical and custom Noct CLIs,
mark the Phase `uncleared`, record the exact configure/link failure, and return
that decision to the WS rather than hiding it in package glue.

Resume at the first failing NOCT-T001--T003 gate using the preserved clean
configure log and artifact audit.
