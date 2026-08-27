# WS008 Phase 001: canonical Noct zedBSD CMake preset

Last updated: 2026-08-28

WSID: `ws008`

Phase ID: `p001`

Combined ID: `ws008-p001`

Status: Complete (`q019`)

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
- No canonical Noct commit, push, or submodule gitlink update is part of this
  Phase. The enclosing zedBSD Phase checkpoint is committed and pushed.

## Work packages

- [x] Record clean baseline revisions and path-scoped status for
      `/home/awe/NoctLang` and `userland/noct` before editing.
- [x] Add a `NOCT_TARGET_ZEDBSD` target selection, amd64 toolchain integration,
      and matching `zedbsd` configure/build presets to canonical Noct.
- [x] Select only APIs supported by the current zedBSD libc and fail configure
      for incompatible shared-library or host-only options.
- [x] Move or replace only the target entry/runtime pieces required to produce
      the canonical executable; do not migrate BeUI in this Phase.
- [x] Make the zedBSD Noct package consume the CMake target artifact, eliminate
      obsolete per-platform manual Noct object/link lists for amd64, and retain
      explicit source provenance.
- [x] Add focused preset/artifact checks under `plan/ws008-noct/tests/`.
- [x] Run the non-JIT language smoke in `qemu-system-x86_64`, then run the
      supported `make -j16` image gate.
- [x] Produce a parity manifest for the official and integration Noct checkout
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

## Implementation result

- Canonical Noct now owns the `zedbsd` configure/build presets, target
  selection, target detection, generic CLI fixes, and zedBSD OS identity. The
  preset builds the real CLI/runtime with JIT compiled in and BeUI disabled.
- The zedBSD-owned adapter builds crt0 and the supported libc as a separate
  object target. This preserves libc header precedence, uses
  `platform/amd64/user.ld`, and prevents host headers, host crt objects, and
  host libraries from entering the final static ELF.
- The amd64 package copies `userland/noct/build-zedbsd/noct` verbatim to
  `build/amd64/bin/noct`; the obsolete empty/manual-link path was removed only
  for amd64. Existing i386 and PC-98 paths remain unchanged for later work.
- QEMU exposed the canonical desktop GC default as too large for the current
  64 MiB guest. The zedBSD target therefore retains the established
  `NOCT_MEMORY_SMALL` policy and software `fmaf` path. Host GCC Linux macros
  are explicitly suppressed, and direct Linux topology branches now use
  `NOCT_TARGET_LINUX` rather than compiler-host macros.
- `/home/awe/NoctLang` and `userland/noct` remain at
  `7d856856e16eb2d889ba49f557f2fda4dcaeea7e`, with the same nine changed
  paths. They intentionally remain uncommitted/unpublished; advancing the
  package revision remains release administration, not a hidden Phase action.

## Acceptance result

- `NOCT-T001` and `NOCT-T002`: PASS in
  `plan/ws008-noct/temp/p001-host-20260827T185736Z-2/`. Both root-error cases,
  the literal clean configure/build, zedBSD layout, `_start`, 1 MiB
  non-executable stack, host-contamination, and unresolved-symbol audits pass.
- Package identity: PASS. The integration CMake artifact, package artifact,
  and staged `/usr/bin/noct` are byte-identical. The clean-copy artifact is
  audited independently because debug information records its temporary
  absolute source path.
- Checkout parity: PASS for nine paths. The parity manifest SHA-256 is
  `72402db91ba157c5497df5a2efa634297f9145fe7b8d9e6b93cc2f299973a6f4`.
- `NOCT-T003`: PASS in
  `plan/ws008-noct/temp/q019-p001-noct.eqwMMU/`. The amd64/PC-AT image boots to
  production init/login, its embedded `/usr/bin/noct` matches host POSIX
  checksum `3310757182 2735224`, and
  `noct -j0 -e 'print("NOCT-P001-SMOKE")'` emits the exact marker with status
  zero and no fatal diagnostics.
- Final `make -j16`: PASS (`Nothing to be done for 'disk-image'`). The
  aggregate `make check` target and `.internal/` were not used.

All completion conditions are met. `ws008-p002` may start without redesigning
the target or package boundary.
