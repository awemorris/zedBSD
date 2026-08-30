# WS008 Phase 009: relocate target Noct under base and resume it

Last updated: 2026-08-30

WSID: `ws008`

Phase ID: `p009`

Combined ID: `ws008-p009`

Status: Blocked; upstream zedBSD-target fix or downstream patch-overlay
decision required

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

## Objective

Replace the held target integration with a clean, reproducibly pinned clone at
`userland/base/noct/noct/`, consolidate zedBSD-owned Noct target integration
under `userland/base/noct/`, and re-enable the optional amd64 `/usr/bin/noct`
package only after the latest accepted upstream revision passes the existing
non-JIT, JIT, and BeUI QEMU acceptance.

The path relocation changes source ownership, not the installed command or
package policy: this Phase preserves the optional `/usr/bin/noct` interface.
Remacs remains held until its own dependency and application tests are
explicitly scheduled.

## Blocking evidence

The 2026-08-30 planning audit tested a clean archive of locally known upstream
`main` revision `58bec083fd9926b386b30e02559d79db0178905a`.

- `cmake --preset zedbsd-amd64` configured, but its build failed immediately:
  the preset enabled Process, HTTPServer, and ANSI Term code that requires
  unavailable `<util.h>`, `<netinet/tcp.h>`, and `INPCK`; the Term source also
  assigned a function pointer to zedBSD's integer-valued `sa_handler` field.
- The canonical `CMakeLists.txt` no longer includes the zedBSD-owned adapter or
  calls `noct_configure_zedbsd_target(noctcli)`. Therefore the final crt0,
  libc, linker-script, and static-link policy from the previously accepted
  target path would not be attached even after compilation failures were
  removed.
- A `zedbsd-i386` preset is advertised but its referenced
  `cmake/toolchains/zedbsd-i386.cmake` does not exist.

These are upstream target-integration facts, not permission to modify the
canonical source. p009 cannot enter a Queue until one of these resume paths is
chosen:

1. a clean published upstream revision restores a buildable zedBSD amd64
   target and is selected as the common host/target pin; or
2. the user explicitly authorizes a tracked downstream patch-overlay design,
   after which this P book must be amended to define its ownership,
   application, cleanliness, update, and removal contracts.

## Protected existing checkout

- `userland/noct/NoctLang` is ignored by the zedBSD repository, detached at
  `ec9936a4b75bf3181b1dde8f8c55d9827f649098`, and contains numerous
  maintainer changes.
- Do not delete, move, rename, reset, clean, format, stage, build, or repurpose
  that directory. It is not an input to p009.
- Retain its existing ignore rule while adding the new
  `userland/base/noct/noct/` ignore rule. A later explicit human cleanup may
  retire the old tree after its ownership is resolved.
- The new target source must be a fresh clone. A filesystem rename or copy of
  the dirty old checkout is not an acceptable implementation shortcut.

## Fixed source and package contract

- Canonical repository: `https://github.com/awemorris/NoctLang.git`.
- Canonical target source directory: `userland/base/noct/noct/` exactly,
  including the lower-case final component.
- The tracked integration root is `userland/base/noct/`. Its Makefile owns
  both clean source acquisition and top-level package registration without
  introducing a gitlink or checked-in Noct source copy.
- The target pin is a full immutable commit ID. At completion it must equal
  `ZEDBSD_HOST_NOCT_REVISION`; p009 may advance the p008 host pin to a newer
  accepted target-fix revision, but it must rerun the p008 host gates when it
  does so.
- The checkout is detached and clean after acquisition. Local changes are an
  error, never overwritten automatically.
- zedBSD-owned runtime/link adapter material moves out of
  `userland/packages/lang/noct/` into `userland/base/noct/`. Canonical Noct
  continues to own its compiler, runtime, JIT, and BeUI implementation.
- The current external interface remains an optional amd64 command installed
  as `/usr/bin/noct`. Merely placing its integration under `userland/base`
  does not make it mandatory or move it to `/bin`.
- Remove `noct` from `ZEDBSD_TARGET_PACKAGE_HOLD` only after all acceptance
  gates pass. Keep `remacs` held.
- Do not advertise i386 or PC-98 target support in the package registry in
  this Phase. The missing upstream i386 toolchain and obsolete downstream
  manual object path require a separate accepted Phase.

## Required path migration

1. Consolidate the tracked source-acquisition Makefile, target package
   metadata, and the zedBSD-owned CMake/link adapter under
   `userland/base/noct/`.
2. Set the clone root to `userland/base/noct/noct` and update `.gitignore`
   without removing protection for the old dirty checkout.
3. Remove live build dependencies on `userland/noct/NoctLang`,
   `userland/noct/Makefile`, and `userland/packages/lang/noct/`.
4. Update the top-level Makefile's package discovery/exclusion, immutable pin,
   target hold, and artifact paths for the new single integration Makefile.
5. Update or remove the obsolete PC/AT i386 and PC-98 manual Noct object rules
   so no supported build names files in the retired package directory. This
   does not claim target Noct support on those platforms.
6. Update Remacs metadata only far enough that its retained hold has no stale
   source path. Do not re-enable or accept Remacs here.
7. Update active WS008 tests and build documentation from
   `userland/noct/NoctLang`/`build-zedbsd` to
   `userland/base/noct/noct`/`build-zedbsd-amd64`. Historical Queue and Phase
   evidence remains historical and is not rewritten.

## Target integration contract

- Use the canonical `zedbsd-amd64` configure and build presets and their
  `build-zedbsd-amd64/noct` artifact.
- The accepted upstream target configuration selects only APIs supported by
  current zedBSD. It cannot conceal missing headers or ABI defects by
  importing host headers.
- The canonical target attaches zedBSD-owned crt0, libc objects, public
  headers, linker script, and static-link policy through the explicit adapter
  boundary under `userland/base/noct/`.
- The produced executable is amd64 zedBSD static ELF, has no host interpreter
  or host dynamic dependency, and is byte-identical to the staged
  `/usr/bin/noct` artifact.
- The canonical clone stays pristine. If the selected upstream revision still
  requires source changes, stop at the blocking boundary rather than editing
  the ignored clone.

## Work packages after the blocker is released

1. Record the release decision, accepted upstream commit, both current pins,
   the old dirty checkout metadata, and path-scoped zedBSD status.
2. If necessary, advance the host pin to the same accepted commit and rerun
   all p008 host gates.
3. Create the new tracked integration root and fresh detached clone, then
   consolidate package/acquisition/adapter ownership without touching the old
   checkout.
4. Update target discovery, package registration, build artifacts, retained
   Remacs hold, platform references, tests, and current build documentation.
5. Configure and build the canonical amd64 zedBSD target from a clean target
   build directory.
6. Prove package artifact identity and run the existing non-JIT, JIT/RW-to-RX,
   and BeUI graphics/evdev QEMU gates using disposable disk images.
7. Remove only the Noct target hold, rebuild the configured amd64 image, and
   verify the optional package is selectable and installed only when selected.
8. Record exact source SHA, checkout cleanliness, artifact hashes, QEMU logs,
   and the audit proving no active reference uses the retired paths.

## Verification

- `NOCT-T070`: a source audit finds one tracked acquisition/package owner at
  `userland/base/noct/Makefile`, one ignored clone at
  `userland/base/noct/noct/`, no gitlink, and no live reference to the retired
  integration paths.
- `NOCT-T071`: target and host pins are identical full commit IDs; both clones
  are clean and detached at that ID; the old dirty checkout is unchanged.
- `NOCT-T072`: a clean `zedbsd-amd64` configure/build produces a static amd64
  zedBSD executable with the explicit zedBSD adapter attached and without host
  contamination or unsupported API-header fallthrough.
- `NOCT-T073`: the canonical target artifact, packaged artifact, and staged
  `/usr/bin/noct` are byte-identical.
- `NOCT-T074`: the existing deterministic non-JIT QEMU smoke passes.
- `NOCT-T075`: the JIT acceptance proves RW `mmap`, RX `mprotect`, native
  entry, expected result, and cleanup without interpreter fallback.
- `NOCT-T076`: the BeUI QEMU acceptance proves `/dev/graphics` drawing and
  capability-discovered evdev keyboard/pointer input without legacy console
  event ioctls.
- `NOCT-T077`: menu/effective-selection checks show Noct selectable only on
  amd64, absent unless selected, and installed at `/usr/bin/noct` when
  selected; Remacs remains held.
- `make -j16` and `git diff --check` pass. Do not run `make check` and do not
  consume `.internal/`.

## Completion conditions

- The target source is reproducibly acquired under the exact user-selected
  new path, and all active zedBSD Noct integration is owned under
  `userland/base/noct/`.
- Host and target use the same accepted immutable upstream revision.
- The clean canonical amd64 target passes non-JIT, real JIT, and BeUI QEMU
  acceptance and installs the exact tested artifact.
- Noct alone is released from the package hold; Remacs and unsupported target
  architectures remain honestly disabled.
- The old dirty checkout is preserved byte-for-byte and is no longer a live
  build input.

## Reconsideration boundary

Do not Queue this Phase while its status is blocked. Return for human judgment
if the target still needs canonical source edits, if a downstream patch
overlay is proposed, if `/usr/bin` versus `/bin` or optional versus mandatory
package policy is to change, or if i386/PC-98 target support is added to this
Phase. A published clean upstream fix matching the contracts above releases
the blocker without redesign.
