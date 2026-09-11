# WS008 Phase 006: maintainer API and source-layout review

Last updated: 2026-08-28

WSID: `ws008`

Phase ID: `p006`

Combined ID: `ws008-p006`

Status: Uncleared (`q024`; Principal Engineer review rejected implementation)

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

Evidence: [execution evidence](evidence.md)

## Objective

Bring the canonical Noct implementation back into alignment with the
maintainer-owned public API and source-layout policy without changing the
maintainer-edited `include/noct/noct.h`.  Terminal and BeUI implementations
become complete per-platform translation units, deleted public callback
backends disappear from implementation and tests, and the moved accelerator
and regex sources are selected from their new locations.

This is a review correction of an existing application.  It must preserve the
established style and public contract rather than invent a new abstraction.

## Entry state and protected user work

- The authoritative working copy for this Phase is the ignored checkout at
  `userland/noct/NoctLang`, detached at canonical commit
  `c1e4e0fcdbb7b8cdf1705601b13d57b787c61621` with maintainer edits present.
- `/home/awe/NoctLang` is a separate dirty checkout and is outside this Phase.
- The accepted `include/noct/noct.h` SHA-256 at planning time is
  `90c2115d53840fe3d6c1fdff6751676a35d473d2503fc9a3d9c179e1fb22a7b3`.
  The Phase must verify the same hash before and after implementation and must
  not stage, rewrite, format, regenerate, or otherwise modify this file.
- Existing maintainer changes that delete or move Term, accelerator, and regex
  sources are inputs to the work, not agent-owned changes to discard.
- Untracked editor/backup files, including `include/noct/#beui.h#`,
  `include/noct/.#beui.h`, and `include/noct/noct.h~`, are user material.  Do
  not read them as authoritative input, delete them, or stage them.
- Before editing, record a path-scoped status and hashes for every protected
  input.  Never use reset, checkout, clean, `git add -A`, or another broad
  operation in the canonical checkout.

## Fixed public-interface contract

- `include/noct/noct.h` is the maintainer-owned public API.  Any future change
  to it requires separate explicit maintainer permission.
- The only public Term registration entry point is
  `bool noct_register_api_term(NoctEnv *env)`.
- The only public BeUI registration entry point is
  `bool noct_register_api_beui(NoctEnv *env)`.
- Do not restore `NoctTermBackend`, `NoctTermStyle`,
  `noct_register_api_term_backend()`, `NoctDirectoryBackend`,
  `noct_set_directory_backend()`, public BeUI HAL injection, or platform-
  suffixed registration functions.
- `include/noct/beui.h` is redundant with the fixed aggregate header and is
  removed.  CLI and implementation sources include `<noct/noct.h>` directly.
- A configured executable contains one Term platform implementation and, when
  enabled, one BeUI platform implementation.  Supporting multiple backends in
  one executable is not a requirement.

## Fixed implementation design

### 1. Terminal implementations

- Rename `src/api/api-term.c` to `src/api/api-term-ansi.c`.
- Delete `src/api/api-term-backend.c` without replacing it with a shared
  backend, callback table, internal dispatcher, or private common header.
- `api-term-ansi.c` independently owns the Term FFI table, constants,
  registration, ANSI/POSIX terminal behavior, and the existing unsupported-
  target fallback where required by legacy presets.
- `api-term-win32.c` independently owns the same language-level Term contract
  and calls its Win32 implementation directly.  It must not construct a
  `NoctTermBackend` or call a generic backend registrar.
- CMake selects `api-term-win32.c` for the Win32 platform and
  `api-term-ansi.c` otherwise; it never compiles both for one target.

### 2. File API correction required by the fixed header

- Remove the callback directory backend, its global state, its setter, and its
  callback branch from `api-file.c`.  No production caller exists and the
  public type/function have deliberately been removed from `noct.h`.
- Retain each supported platform's normal File/FileUtil behavior.
- Retire the old callback-injection fixture.  Replace any still-useful API
  coverage with tests of public registration and platform behavior; preserve
  the unrelated object-model runtime check under an appropriate test entry.

### 3. Independent BeUI platform implementations

- `api-beui-sdl2.c` and `api-beui-zedbsd.c` each receive their own copy of the
  types, constants, image decoder, state/core logic, FFI binding, and platform
  implementation currently split through `beui-internal.h`, `beui-core.c`,
  and `beui-image.c`.
- Create one `src/api/api-beui-pc98.c` containing the same private BeUI
  contract plus the complete former `api-beui-pc98dos.c`,
  `beui-pc98-{auto,cirrus,gdc,glyph}.{c,h}`, and PC-98 JIS X 0208 table.
- Rename the colliding PC-98 GDC/Cirrus `pattern_bit()` helpers to backend-
  local names while combining the translation units.
- Delete the absorbed common/split BeUI files and make implementation symbols
  `static` wherever they do not cross the public `noct_register_api_beui()`
  boundary.
- Preserve the current SDL2 behavior, PC-98 GDC/Cirrus behavior, and zedBSD
  `/dev/graphics` plus capability-discovered evdev behavior.

### 4. JIS X 0208 ownership

- Delete standalone `src/api/jisx0208.c` after its data has two independent
  owners.
- Put one `static const` copy of the 7,896-entry table in
  `api-beui-pc98.c` for glyph conversion.
- Put another `static const` copy in `api-file.c` for its EUC-JP decoder.
- The approximately 15.8 KiB duplicate in a build containing both modules is
  accepted deliberately.  PC-98 BeUI no longer depends on enabling File API.

### 5. CMake and moved sources

- Change all four accelerator source references from `src/api/accel*.c` to
  `src/accel/accel*.c`; no new include directory is required.
- Change the regex source reference from `src/api/regex.c` to
  `src/core/regex.c` and preserve its private include relationship.
- Replace the old Term and BeUI source lists with exact-one platform selection
  matching the independent translation units above.
- Remove install/source references to `include/noct/beui.h` and every absorbed
  or deleted source/header.

## Implementation procedure

1. Capture the canonical checkout HEAD, path-scoped dirty inventory, and
   protected-header hash.  Refuse to proceed if `noct.h` does not match the
   recorded contract.
2. Finish the CMake path corrections for accelerator and regex moves so later
   builds diagnose only API/layout work.
3. Remove the deleted File callback-backend dependency and split the Term API
   into independent ANSI and Win32 implementations.
4. Duplicate JIS data into File and PC-98 BeUI ownership, then consolidate the
   complete PC-98 implementation and its focused tests.
5. Consolidate SDL2 and zedBSD independently.  Do not construct a new shared
   include/source layer while doing so.
6. Update canonical tests, documentation, and CMake/install declarations to
   the new stable public header and source ownership.
7. Run the focused canonical build/test matrix and zedBSD integration gates.
8. Recheck the protected hash and path-scoped diff.  Do not commit, publish,
   or advance either zedBSD revision pin unless a Queue explicitly authorizes
   the two-repository publication step.

## Verification

- `NOCT-T050`: before/after SHA-256 proves `include/noct/noct.h` is byte-for-
  byte unchanged, and no backup/editor file is staged.
- `NOCT-T051`: source and symbol audit rejects every removed Term/File callback
  backend, shared BeUI core/header/source, redundant BeUI public header, old
  accelerator/regex path, and old split PC-98 source.
- `NOCT-T052`: clean static and shared host builds pass with the ANSI Term
  implementation; public Term registration/behavior and File/FileUtil/EUC-JP
  regressions pass without callback injection.
- `NOCT-T053`: a Win32/MinGW build compiles and links only the independent
  Win32 Term implementation and exports the fixed registration interface.
- `NOCT-T054`: independent SDL2 and PC-98 BeUI tests pass, including BMP/core,
  SDL dummy-driver, PC-98 GDC, Cirrus, glyph, and auto-selection coverage.
- `NOCT-T055`: zedBSD preset build and sanitized evdev/backend tests pass with
  only `api-beui-zedbsd.c` providing `noct_register_api_beui()`.
- `NOCT-T056`: accelerator source audit plus CPU/static accelerator tests pass;
  available OpenGL/Vulkan build gates compile the moved sources.  Hardware-
  dependent accelerator execution and unavailable DX12 toolchains are
  recorded rather than falsely claimed.
- `NOCT-T057`: outer `make -j16`, `git diff --check`, and affected existing
  Noct non-JIT/JIT/BeUI integration gates pass.  Do not run `make check` or
  consume `.internal/`.

## Completion conditions

- The fixed `noct.h` hash is unchanged and all implementation declarations
  match it.
- ANSI and Win32 Term implementations are standalone translation units, with
  no backend source, vtable, or public/private callback registrar.
- SDL2, PC-98, and zedBSD BeUI implementations each stand alone behind the
  single public registrar; all requested common and PC-98 split sources are
  gone.
- File API owns its JIS data and no longer exposes or consumes the removed
  directory callback backend.
- CMake selects only existing files at their accepted new locations, and all
  applicable `NOCT-T050`--`NOCT-T057` gates pass.
- Canonical local changes are ready for maintainer inspection.  Publication
  and zedBSD pin advancement are not required for local-review completion
  unless separately authorized by the executing Queue.

## Authorization boundary

Planning this Phase does not authorize edits in the ignored canonical checkout.
The executing Queue must separately authorize implementation.  Committing or
pushing `awemorris/NoctLang`, and updating the two zedBSD revision pins, require
explicit two-repository publication approval; ordinary zedBSD commit/push
authorization is insufficient.

## Automated result and subsequent rejection

All applicable automated `NOCT-T050`--`NOCT-T057` gates passed, including
public EUC-JP behavior and the non-JIT, BeUI, and JIT amd64 QEMU integrations.
The protected aggregate header retained its entry hash, and no editor backup
was consumed or staged. OpenWatcom and optional graphics-accelerator
toolchains were unavailable and are recorded without false pass claims in the
linked evidence.

The subsequent first Principal Engineer source review rejected the
implementation quality. Automated success is not engineering acceptance, so
this Phase is `uncleared`. The maintainer has taken ownership of the manual
repair. Agent work must not modify the canonical Noct tree until the
maintainer explicitly returns it.

## Reconsideration boundary

Stop and request maintainer judgment rather than changing `noct.h`, restoring a
deleted public backend interface, introducing a shared Term/BeUI implementation,
or altering the language-level Term/BeUI/File behavior.  A platform that truly
cannot implement the fixed registrar contract is an `uncleared` result, not a
reason to modify the public header casually.
