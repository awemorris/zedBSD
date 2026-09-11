# WS008 Phase 007: disable target Noct package options

Last updated: 2026-08-28

WSID: `ws008`

Phase ID: `p007`

Combined ID: `ws008-p007`

Status: Complete (`q025`, 2026-08-28)

Parent: [WS008](../ws.md)

## Objective

Temporarily remove the zedBSD target Noct package and its Remacs dependent
from selectable/accepted image build options while the Principal Engineer
repairs Noct manually. Preserve the separate host Noct toolchain used to run
project-owned image-build scripts, and do not inspect or modify either
maintainer Noct source checkout.

## Fixed boundary

- `/usr/bin/noct`, its Holoris payload, and Remacs are not selectable target
  packages and cannot be reintroduced by a hand-edited
  `ZEDBSD_USER_PROGRAMS` value or dependency expansion.
- `build/NoctLang/build-static/noct` remains the host build-script runtime.
  Removing it would contradict the accepted WS010 scripting architecture and
  is not part of this Phase.
- Keep package recipes in place for a future explicit re-enable; disable their
  registration/selection and the final selected-package set instead of
  deleting maintainer integration code.
- Do not read, edit, format, stage, reset, clean, build, or test
  `userland/noct/NoctLang` or `/home/awe/NoctLang`.

## Implementation

1. Mark `noct` and dependent `remacs` as non-default and non-selectable in
   their zedBSD package registrations.
2. Add an explicit target-package hold in the top-level dependency-expanded
   selection path so stale or hand-edited configuration cannot pull either
   package into an image.
3. Keep host `NOCT`, `toolchain`, and Noct-script image rules unchanged.
4. Rebuild the normal configured amd64 image and verify that target Noct and
   Remacs payloads are absent while the host tool remains executable.

## Verification

- `make list-user-programs` contains neither `noct` nor `remacs`.
- A dry-run/variable audit with both names forced in
  `ZEDBSD_USER_PROGRAMS` shows both removed after dependency expansion.
- `tools/menuconfig.py --defaults` emits a saved configuration containing
  neither disabled package. (`menuconfig-host-test` is listed by the top-level
  Makefile but has no implementation, so it is not claimed.)
- `make -j16` passes using the existing host Noct toolchain.
- A freshly assembled rootfs/image contains none of `/usr/bin/noct`,
  `/usr/bin/holoris.nct`, or `/usr/bin/remacs.nap`.
- `build/NoctLang/build-static/noct` remains executable.
- `git diff --check` passes. Do not run `make check` or consume `.internal/`.

## Completion conditions

- Target Noct and Remacs are absent from both the menu and effective image
  package selection, including stale/manual configuration inputs.
- Normal image construction remains operational through the separate host
  Noct script runtime.
- Neither maintainer Noct source tree has been touched by the Phase.

## Resume condition for WS008

This Phase does not repair or accept Noct. Re-enable the target package only
after the Principal Engineer explicitly returns an accepted source revision
and authorizes a new bounded Phase.

## Execution record

- `make list-user-programs` omitted both target packages.
- A forced `ZEDBSD_USER_PROGRAMS='noct remacs cat'` evaluation retained `cat`
  and mandatory base services but removed `noct` and `remacs`.
- Non-interactive menu defaults contained neither name.
- A forced fresh amd64 rootfs assembly passed using
  `build/NoctLang/build-static/noct` and contained none of
  `/usr/bin/noct`, `/usr/bin/holoris.nct`, or `/usr/bin/remacs.nap`.
- `git diff --check` passed. Neither maintainer Noct checkout was inspected or
  modified.
