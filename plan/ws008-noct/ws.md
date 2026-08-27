# WS008: Noct and BeUI

Last updated: 2026-08-27

WSID: `ws008`

Status: planned; authoritative upstream tree is now available

Parent: [master plan](../master.md)

Last verified Phase: none

Resume point: extract NOCT-00 as an audit Phase against `/home/awe/NoctLang`
and the `userland/noct` submodule at pinned revision
`7d856856e16eb2d889ba49f557f2fda4dcaeea7e`.

Shared tests: [WS008 test index](tests/README.md)

## Phase registry

No Phase has started. The former missing-upstream blocker is cleared; NOCT-00
can now be extracted before implementation begins.

## Goals

- Add zedBSD as an upstream target system of the canonical Noct project.
- Add upstream BeUI backends for zedBSD graphics and evdev while retaining the
  existing SDL backend.
- Make the local Noct package a pinned clone-and-build integration only.

## WS completion conditions

WS008 is complete when upstream Noct builds and runs the declared runtime tests
for zedBSD, BeUI drawing and input tests pass on zedBSD public UAPIs, the changes
exist in the authoritative upstream tree, and the local package reproducibly
fetches the pinned revision, builds, installs, and records its licenses.

## 1. Objective

Make zedBSD an upstream target of the canonical Noct language project, add a
zedBSD BeUI backend using `/dev/graphics` and evdev, and reduce the local
`userland/packages/noct` package to a reproducible clone-and-build recipe.

## 2. Current baseline and prerequisite

The zedBSD tree contains a local Noct runtime/package prototype that uses
`/dev/graphics` and the legacy `/dev/console` event mode. The canonical
`~/NoctLang` tree and `userland/noct` upstream submodule are now present at the
same pinned revision. NOCT-00 must audit both before deciding which remaining
zedBSD runtime/BeUI changes move upstream and which local package glue is
deleted.

BeUI already has an SDL backend upstream according to the project direction;
the zedBSD backend is added alongside it rather than replacing it.

## 3. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| NOCT-00 | Ready to extract | Audit authoritative Noct tree, target model, runtime, BeUI, build, and tests | Upstream tree/repository supplied | Baseline and required upstream change list are recorded |
| NOCT-01 | Proposed | zedBSD target-system definition in upstream Noct | NOCT-00, stable zedBSD compiler/runtime interfaces | Compiler emits runnable zedBSD binaries and upstream target tests pass |
| NOCT-02 | Proposed | zedBSD runtime/syscall/platform layer upstream | NOCT-00/01, relevant UAPI | File, memory, process, time, and threading subset tests pass |
| NOCT-03 | Proposed | BeUI `/dev/graphics` backend upstream | NOCT-00, stable graphics UAPI | Drawing, surfaces, resize/mode behavior, and teardown pass |
| NOCT-04 | Proposed | BeUI evdev input backend upstream | IN-00–IN-05 | Keyboard/mouse event tests pass without console event mode |
| NOCT-05 | Proposed | Local package becomes pinned upstream clone-and-build only | NOCT-01–04 | Clean build from declared revision produces/install tested artifacts |

## 4. Upstream-first policy

Target-system and BeUI changes are made in the canonical Noct project first.
zedBSD does not maintain a divergent duplicate implementation under
`userland/base`. The package recipe may fetch external source because it is a
package, but it must record enough provenance for reproducibility:

- authoritative repository URL;
- tag or immutable commit revision;
- expected toolchain/build options;
- offline/cache behavior and clear network-failure reporting;
- installed files and licenses.

“Simply clone and build” describes the package's role, not an unpinned moving
target. Updating the revision is a deliberate package change.

## 5. Backend boundaries

BeUI's zedBSD backend uses public UAPIs only:

- `/dev/graphics` for the initial framebuffer path;
- `/dev/input/eventN` for keyboard and mouse;
- the future GPU API only through a separately designed optional backend.

The backend must not depend on the removed `/dev/console` continuous-event or
key-state interfaces. SDL remains a separate backend for other host systems and
development tests.
