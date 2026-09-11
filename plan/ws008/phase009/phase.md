# WS008 Phase 009: relocate target Noct under base and resume it

Last updated: 2026-09-02

WSID: `ws008`

Phase ID: `p009`

Combined ID: `ws008-p009`

Status: complete (`q063`, 2026-09-02)

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

## Objective

Replace the held target integration with a clean, reproducibly identified
release extraction at `userland/base/noct/noct/`, consolidate zedBSD-owned
Noct target integration
under `userland/base/noct/`, and re-enable the optional amd64 `/usr/bin/noct`
package only after the accepted upstream release passes the existing
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

These are upstream target-integration facts. The user selected the second
resume path on 2026-09-02:

1. a clean published upstream revision restores a buildable zedBSD amd64
   target and is selected as the common host/target pin; or
2. a tracked downstream patch overlay fixes only the zedBSD CMake/link
   integration while the downloaded release archive remains immutable.

## Retired existing checkout

- `userland/noct/NoctLang` is ignored by the zedBSD repository, detached at
  `ec9936a4b75bf3181b1dde8f8c55d9827f649098`, and contains numerous
  maintainer changes.
- The latest user request explicitly superseded the former path hold and
  required `userland/noct` to be removed. Before implementation the dirty
  checkout was moved without modification to
  `/home/awe/zedBSD-userland-noct-NoctLang-backup-20260902` so no maintainer
  work was destroyed. It is not an input to p009.
- Remove the old tracked acquisition Makefile and old ignore rule while adding
  the exact new `userland/base/noct/noct/` and `distfiles/` ignore rules.
- The new target source is extracted from the official archive. It is not a
  copy or rename of the retired checkout.

## Fixed source and package contract

- Canonical upstream: `https://github.com/awemorris/NoctLang`, release
  `v2.0.1`, tag commit
  `ed621e79139f55d06dd1a474243afbf0ce5efe0a`.
- Canonical target source directory: `userland/base/noct/noct/` exactly,
  including the lower-case final component.
- The tracked integration root is `userland/base/noct/`. Its Makefile owns
  both clean source acquisition and top-level package registration without
  introducing a gitlink or checked-in Noct source copy.
- Host and target use the same official `v2.0.1` source archive identified by
  its URL, tag commit, exact size, and SHA-256. Extracted trees contain no Git
  metadata; archive identity plus a strict patch manifest replaces the former
  detached-checkout test.
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
2. Set the extracted source root to `userland/base/noct/noct`, update
   `.gitignore`, and retain the separately backed-up dirty tree outside the
   repository.
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
- The canonical archive stays pristine. A tracked patch changes only two
  target-integration points: it updates the required adapter path to
  `userland/base/noct/zedbsd.cmake`, then enables ASM, includes that adapter,
  and invokes `noct_configure_zedbsd_target(noctcli)` when
  `NOCT_TARGET_ZEDBSD` is selected. It must not change BeUI, language/runtime,
  CLI, JIT, or public Noct headers.
- Apply the patch with zero fuzz to a temporary extraction and publish the
  ignored patched tree atomically. Existing unexpected source content is an
  error, never overwritten automatically.

## Work packages after the blocker is released

1. Record release `v2.0.1`, tag commit, archive size/digest, the narrow patch,
   backed-up old checkout metadata, and path-scoped zedBSD status.
2. Reuse p010's same verified archive and rerun the relevant host gates.
3. Create the new tracked integration root and strict patched extraction, then
   consolidate package/acquisition/adapter ownership.
4. Update target discovery, package registration, build artifacts, retained
   Remacs hold, platform references, tests, and current build documentation.
5. Configure and build the canonical amd64 zedBSD target from a clean target
   build directory.
6. Prove package artifact identity and run the existing non-JIT, JIT/RW-to-RX,
   and BeUI graphics/evdev QEMU gates using disposable disk images.
7. Remove only the Noct target hold, rebuild the configured amd64 image, and
   verify the optional package is selectable and installed only when selected.
8. Record exact release/archive identity, extraction manifests, artifact
   hashes, QEMU logs, and the audit proving no active reference uses the
   retired paths.

## Verification

- `NOCT-T070`: a source audit finds one tracked acquisition/package owner at
  `userland/base/noct/Makefile`, one ignored extraction at
  `userland/base/noct/noct/`, no gitlink, and no live reference to the retired
  integration paths.
- `NOCT-T071`: target and host archive identities are identical; both
  extractions match the recorded release plus target-only patch manifest; the
  backed-up old dirty checkout remains outside live build inputs.
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
- Host and target use the same accepted immutable upstream release archive.
- The clean canonical amd64 target passes non-JIT, real JIT, and BeUI QEMU
  acceptance and installs the exact tested artifact.
- Noct alone is released from the package hold; Remacs and unsupported target
  architectures remain honestly disabled.
- The old dirty checkout is preserved byte-for-byte and is no longer a live
  build input.

## Q063 result

Complete. Host and target source now come from the same official `v2.0.1`
archive, exact size `2524680`, SHA-256
`68588c84f508856474526be1c576cf6190ee99539cd81cc8453857d894f98f9f`.
The old dirty ignored checkout was preserved unchanged at
`/home/awe/zedBSD-userland-noct-NoctLang-backup-20260902` and no live build
input refers to the retired paths.

The tracked patch applies with zero fuzz and changes exactly two target CMake
integration points: the required path of `userland/base/noct/zedbsd.cmake`,
and the conditional ASM enable/include/invocation which attaches crt0, libc,
the static-link policy, and the zedBSD linker script. This is a final-link
adapter patch, explicitly not a BeUI adapter; canonical
`src/api/api-beui-zedbsd.c` is unchanged.

The `zedbsd-amd64` preset produced an ELF64 x86-64 static executable with
`_start` at entry `0x4066db`, no interpreter, and no dynamic segment. The CMake
artifact, package artifact, and staged `/usr/bin/noct` are byte-identical at
SHA-256
`e8ee34e05a79f89baefe30f57932cb7c543b4285edfddd61e9083e4e1ad92641`.
Noct is optional and amd64-only; forced PC-98/i386 selection remains absent.
Remacs remains held.

Disposable q35/xHCI USB-root runs passed `NOCT-T003` non-JIT,
`NOCT-T020`--`NOCT-T022` RW-to-RX/JIT/interpreter control, and
`NOCT-T011`--`NOCT-T013` canonical BeUI graphics and evdev interaction. The
first legacy IDE attempt stopped before Noct at the already recorded
intermittent `BUG-001` `BIO_FLUSH`; the same image passed on retry. The primary
q35/xHCI gates are therefore complete without treating that known IDE defect
as a Noct failure.

## Reconsideration boundary

Return for human judgment if the target requires changes outside the frozen
two-hunk CMake patch, if `/usr/bin` versus `/bin` or optional versus mandatory
package policy must change, or if i386/PC-98 target support is added. A future
upstream release containing the same integration may retire the patch after
the same gates pass.
