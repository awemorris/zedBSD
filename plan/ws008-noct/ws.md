# WS008: Noct and BeUI

Last updated: 2026-08-28

WSID: `ws008`

Status: Active; `ws008-p005` pending in q023

Parent: [master plan](../master.md)

Last verified Phase: `ws008-p004`

Resume point: execute q023 `ws008-p005` after `ws003-p016` is processed

Shared tests: [WS008 test index](tests/README.md)

## Phase registry

| Phase | Status | Deliverable / gate |
| --- | --- | --- |
| [`ws008-p001`](phase001-zedbsd-preset/phase.md) | Complete (`q019`) | Official Noct builds for zedBSD with `cmake --preset zedbsd`, and the resulting amd64 executable passes a non-JIT QEMU smoke |
| [`ws008-p002`](phase002-beui-zedbsd/phase.md) | Complete (`q020`) | Official BeUI zedBSD backend uses `/dev/graphics` and capability-discovered `/dev/input/eventN`; the downstream duplicate and console-event dependency are removed |
| [`ws008-p003`](phase003-amd64-jit/phase.md) | Complete (`q020`) | amd64 zedBSD proves Noct-generated code traverses RW `mmap` to RX `mprotect` and executes under QEMU |
| [`ws008-p004`](phase004-upstream-review/phase.md) | Complete (`q022`) | Maintainer review is published upstream, the BeUI/JIT/CMake contracts are cleaned up, and zedBSD uses one reproducible clone/build Makefile instead of a gitlink |
| [`ws008-p005`](phase005-independent-beui-backends/phase.md) | Pending (`q023`) | Canonical Noct removes `api-beui.c` and `api-beui-backend.c`; each selected platform source independently owns the complete `noct_register_api_beui()` implementation |

The old NOCT-00--NOCT-05 labels are superseded as scheduling units by these
immutable Phase IDs. Their concerns are retained inside p001--p003 rather than
requiring a preliminary audit-only Queue item.

## Goals

- Add zedBSD as an upstream target system of the canonical Noct project.
- Build that target through the public `zedbsd` CMake configure/build preset.
- Move the existing downstream BeUI adaptation into canonical Noct, using
  `/dev/graphics` and evdev while retaining the SDL backend.
- Prove on amd64 zedBSD that the canonical Noct JIT really executes generated
  native code through the supported `mmap`/`mprotect` path.
- Reduce the zedBSD package to target integration, installation, provenance,
  and revision selection rather than a divergent Noct/BeUI implementation.

## WS completion conditions

WS008 returns to complete when all five Phases are complete: the official Noct
source tree provides working `zedbsd` configure and build presets, the
installed amd64 artifact is built from that target, the official BeUI backend
passes graphics and evdev tests without legacy console event ioctls, and a
QEMU guest produces both correct JIT program output and positive
JIT-compilation evidence after an RW-to-RX mapping transition. The SDL backend
must retain its upstream tests, and the reviewed official revision must be
reproducibly acquired without a zedBSD-owned source copy or gitlink. Each
configured platform must own a complete BeUI implementation behind
`noct_register_api_beui()` without the shared `api-beui.c` dispatcher or
`api-beui-backend.c` implementation.

Publishing, committing, or pushing the canonical Noct changes requires the
explicit two-repository approval recorded by p004; it is not inferred from an
ordinary zedBSD Phase execution.
The implementation must nevertheless be authored in an official Noct checkout,
not copied into a new zedBSD-owned fork. A reproducible package revision cannot
be advanced until that revision exists; this is recorded honestly at Queue
closure rather than worked around with an untracked source copy.

## 1. Objective

Make canonical Noct a first-class zedBSD amd64 application, make canonical
BeUI consume zedBSD's public graphics/input interfaces, and validate the
language's native JIT execution path. This sequence deliberately starts with a
plain target build, then adds the graphical/input backend, then enables the
runtime feature whose VM permissions are the most security-sensitive.

## 2. Initial verified baseline

- `/home/awe/NoctLang` and `userland/noct` are official Noct checkouts at
  `7d856856e16eb2d889ba49f557f2fda4dcaeea7e`; neither currently defines a
  `zedbsd` CMake preset.
- zedBSD currently carries target entry, NAPI, terminal, filesystem, memory,
  and BeUI glue under `userland/packages/lang/noct/runtime/`. The BeUI portion
  opens `/dev/graphics` but still reads event/key state through legacy
  `/dev/console` ioctls.
- Canonical Noct already owns the generic BeUI core/HAL and SDL2 backend. A
  zedBSD backend belongs beside those backends, not in `userland/base` and not
  as a second BeUI implementation in package glue.
- The public zedBSD UAPIs are `<zedbsd/graphics.h>` and
  `<zedbsd/input.h>`. Event-node numbering is dynamic and consumers must
  discover devices by capabilities rather than assume event0/event1 roles.
- Canonical Noct has positive JIT observability: with `NOCT_JIT_DEBUG=1`, a
  successful compilation emits `noct-jit: ...: compiled`; interpreter fallback
  is therefore distinguishable from a real JIT pass.
- The Noct JIT already maps writable anonymous memory and then calls
  `mprotect(..., PROT_READ | PROT_EXEC)`. zedBSD has libc entry points for
  `mmap`, `mprotect`, and `munmap`, but the exact end-to-end amd64 JIT path has
  not yet been accepted.

## 3. Dependency order

```text
ws006-p003/p004 evdev producer/core milestone
                  |
ws008-p001 zedBSD preset and executable
                  |
ws008-p002 canonical BeUI graphics + evdev backend
                  |
ws008-p003 amd64 mmap/mprotect JIT execution
```

p001 may build without enabling BeUI. p002 requires the implemented evdev core
and producers, but not USB HID: QEMU's existing console keyboard/mouse producers
are sufficient. p003 depends on the canonical executable from p001; it follows
p002 so that its final image is also the target WS artifact, but it must diagnose
VM/JIT failures independently of BeUI.

## 4. Upstream/downstream ownership

Canonical Noct owns:

- `zedbsd` CMake configure/build presets and target selection;
- portable Noct runtime/compiler/JIT changes;
- the BeUI zedBSD backend and its backend-local tests;
- target-facing documentation and build options.

zedBSD owns:

- its public UAPI headers, libc/syscalls, linker scripts, and image/QEMU tests;
- package metadata, provenance, install paths, and the immutable upstream
  revision;
- only the minimal adapter that is inherently part of zedBSD packaging and
  cannot reasonably live in canonical Noct.

The backend includes zedBSD UAPI headers from the selected sysroot/source tree;
it must not copy those definitions into Noct. Canonical source is published in
`awemorris/NoctLang` and acquired at the accepted revision under
`userland/noct/NoctLang`; the host build-tool checkout under `build/NoctLang`
is separate. q022 explicitly authorized and completed the two-repository
publication and removed the former submodule/gitlink.

## 5. Product boundaries

- Initial target and acceptance are amd64 only. i386/PC-98 Noct target support
  is not removed, but porting the new preset/backend/JIT acceptance to those
  ABIs is outside p001--p003.
- p002 uses the ioctl rendering path. Direct LFB mapping belongs to the
  dedicated graphics/LFB workstream and is not folded into Noct migration.
- The terminal/text API may continue to use ordinary tty or console services.
  What is forbidden after p002 is BeUI dependence on the removed continuous
  console-event, drain-input, or key-state ioctls.
- No general CMake sysroot framework, package manager, GPU backend, USB HID,
  Xzed migration, or broad POSIX audit is hidden in this WS.
- If p003 reveals a narrow defect in zedBSD anonymous mappings, executable
  protection, instruction-cache synchronization, or Noct's target branch, that
  fix is in scope. A VM redesign or policy change is returned to planning as an
  `uncleared` result.
