# WS008: Noct and BeUI

Last updated: 2026-09-02

WSID: `ws008`

Status: complete (`q063`)

Parent: [master plan](../master.md)

Last accepted Phases: `ws008-p010`, then `ws008-p009`

Resume point: no current Phase. Extract a new requirement before resuming;
Remacs and i386/PC-98 target Noct remain explicitly outside q063.

Planned compiler follow-up: WS021 p004 rebuilds the existing amd64 target Noct
package with `build/llvm/bin/clang`, `x86_64-unknown-zedbsd`, LLD, and
`build/amd64/sysroot`; p006 reruns its non-JIT, JIT/RW-to-RX, and upstream BeUI
runtime gates in the final consolidated matrix. Host Noct remains host-built.

Shared tests: [WS008 test index](tests/README.md)

## Phase registry

| Phase | Status | Deliverable / gate |
| --- | --- | --- |
| [`ws008-p001`](phase001-zedbsd-preset/phase.md) | Complete (`q019`) | Official Noct builds for zedBSD with `cmake --preset zedbsd`, and the resulting amd64 executable passes a non-JIT QEMU smoke |
| [`ws008-p002`](phase002-beui-zedbsd/phase.md) | Complete (`q020`) | Official BeUI zedBSD backend uses `/dev/graphics` and capability-discovered `/dev/input/eventN`; the downstream duplicate and console-event dependency are removed |
| [`ws008-p003`](phase003-amd64-jit/phase.md) | Complete (`q020`) | amd64 zedBSD proves Noct-generated code traverses RW `mmap` to RX `mprotect` and executes under QEMU |
| [`ws008-p004`](phase004-upstream-review/phase.md) | Complete (`q022`) | Maintainer review is published upstream, the BeUI/JIT/CMake contracts are cleaned up, and zedBSD uses one reproducible clone/build Makefile instead of a gitlink |
| [`ws008-p005`](phase005-independent-beui-backends/phase.md) | Complete (`q023`, 2026-08-28) | Canonical Noct removes `api-beui.c` and `api-beui-backend.c`; each selected platform source independently owns the complete `noct_register_api_beui()` implementation, with HAL/core details private |
| [`ws008-p006`](phase006-maintainer-api-layout-review/phase.md) | Uncleared (`q024`; manual review rejection) | Automated gates passed, but the Principal Engineer rejected the implementation quality and took ownership of the repair |
| [`ws008-p007`](phase007-target-package-hold/phase.md) | Complete (`q025`, 2026-08-28) | Target Noct and dependent Remacs are absent from menu, forced selection, and a fresh rootfs; the separate host Noct script runtime remains operational |
| [`ws008-p008`](phase008-latest-host-toolchain-pin/phase.md) | Complete (`q041`, 2026-08-31) | Host pin `3bf3d236...`, clean detached checkout, Process-enabled static build, stale-stamp invalidation, and clean/incremental toolchain smoke pass |
| [`ws008-p009`](phase009-base-noct-relocation-target-resume/phase.md) | Complete (`q063`, 2026-09-02) | Target integration is under `userland/base/noct/`; release 2.0.1, strict two-hunk final-link patch, static/package identity, and q35/xHCI non-JIT/JIT/BeUI acceptance pass |
| [`ws008-p010`](phase010-host-script-cli-contract-repair/phase.md) | Complete (`q063`, 2026-09-02; q047 attempt retained) | Verified release 2.0.1 passes the complete host CLI, toolchain, recovery, and ordinary-build contract, including compile/application `--path` |

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
  and release selection rather than a divergent Noct/BeUI implementation.
- Keep the host build-script interpreter current through one immutable,
  verified upstream release selected at Queue entry.
- Preserve the published `--path`/`require` module-loading contract used by
  repository build scripts, and validate it in an ordinary production build.
- Own target integration under `userland/base/noct/` and acquire its pristine
  canonical source at `userland/base/noct/noct/` without a gitlink or copied
  source tree.

## WS completion conditions

WS008 returns to complete when p001--p005 and p007--p010 are complete. p006 is
retained as an honestly uncleared historical review attempt and is superseded
by the clean accepted upstream release integrated through p010/p009; it does
not need to be replayed. The official Noct source tree must provide working
`zedbsd-amd64` configure and build presets, the installed amd64 artifact must
be built from that target, and the official BeUI backend must pass graphics
and evdev tests without legacy console event ioctls, while a
QEMU guest produces both correct JIT program output and positive
JIT-compilation evidence after an RW-to-RX mapping transition. The SDL backend
must retain its upstream tests, and the reviewed official release must be
reproducibly acquired from one verified archive without a zedBSD-owned source
copy or gitlink. Each
configured platform must own a complete BeUI implementation behind
`noct_register_api_beui()` without the shared `api-beui.c` dispatcher or
`api-beui-backend.c` implementation.  The maintainer-review correction must
also leave `include/noct/noct.h` byte-for-byte unchanged, eliminate the removed
Term/File callback interfaces, make each Term and BeUI platform source
independently complete, and build moved accelerator/regex sources from their
accepted directories. The host and target builds use the same immutable
accepted release archive at final WS completion, with the host extraction
under `build/NoctLang`, target integration under `userland/base/noct/`, and
the target extraction under `userland/base/noct/noct/`.

Publishing, committing, or pushing the canonical Noct changes requires the
explicit two-repository approval recorded by p004; it is not inferred from an
ordinary zedBSD Phase execution.
Q063 does not publish or edit upstream Noct. It consumes the official release
archive and applies only the explicitly authorized tracked target final-link
patch during strict extraction; the patch is not a BeUI adapter and is not a
new zedBSD-owned fork.

### Q063 completion record

Official release `v2.0.1`, tag commit
`ed621e79139f55d06dd1a474243afbf0ce5efe0a`, exact archive size `2524680`, and
SHA-256
`68588c84f508856474526be1c576cf6190ee99539cd81cc8453857d894f98f9f`
are now the common host/target identity. The host executable SHA-256 is
`db128557cacc7385976e491a26528bf14e3cfd47a2b9dbff78a63a64617653f6`.
The amd64 static target, package artifact, and staged `/usr/bin/noct` are
byte-identical at SHA-256
`e8ee34e05a79f89baefe30f57932cb7c543b4285edfddd61e9083e4e1ad92641`.

All host CLI, clean/incremental toolchain, recovery, ordinary-build, target
static/package, and disposable q35/xHCI USB-root non-JIT, RW-to-RX/JIT, and
canonical BeUI gates pass. The first legacy IDE attempt encountered known
intermittent `BUG-001` before Noct ran; it passed on retry and remains a
non-blocking known IDE defect. Target Noct is optional and amd64-only. Remacs
remains held, and no i386/PC-98 target result is claimed.

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

## 2.1 Re-entry baseline after maintainer repair

- The user's 2026-08-30 request releases the maintainer-only hold for bounded
  WS008 integration work. It does not make the old dirty target checkout safe
  to alter.
- Host and target integration still pin
  `c1e4e0fcdbb7b8cdf1705601b13d57b787c61621`; p008 advances the host pin
  independently and p009 restores a common accepted pin.
- The old ignored `userland/noct/NoctLang` checkout is detached at
  `ec9936a4b75bf3181b1dde8f8c55d9827f649098` with numerous maintainer changes.
  It is protected user material and is neither moved nor consumed.
- The clean planning candidate
  `58bec083fd9926b386b30e02559d79db0178905a` builds the host `static` preset,
  but its zedBSD target currently fails at unsupported API/header boundaries
  and no longer attaches the accepted zedBSD final-link adapter. p009 records
  the precise blocker and may not be Queued yet.

## 3. Dependency order

```text
ws006-p003/p004 evdev producer/core milestone
                  |
ws008-p001 zedBSD preset and executable
                  |
ws008-p002 canonical BeUI graphics + evdev backend
                  |
ws008-p003 amd64 mmap/mprotect JIT execution
                  |
ws008-p007 target package hold
                  |
ws008-p008 latest host toolchain pin
                  |
ws008-p010 host script CLI contract repair
                  |
ws008-p009 new target path and accepted target resume
```

p001 may build without enabling BeUI. p002 requires the implemented evdev core
and producers, but not USB HID: QEMU's existing console keyboard/mouse producers
are sufficient. p003 depends on the canonical executable from p001; it follows
p002 so that its final image is also the target WS artifact, but it must diagnose
VM/JIT failures independently of BeUI.

p008 depends only on the completed scripting bootstrap and is selected by
q041.
p010 depends on p008's historical host-selection workflow and records the
production-path compatibility regression discovered after p008's bounded
smoke. Q063 resolves it with the verified `v2.0.1` archive. P009 depends on
p010 and uses that same archive; the user's explicit two-hunk downstream
target final-link patch decision released its remaining blocker.

## 4. Upstream/downstream ownership

Canonical Noct owns:

- `zedbsd` CMake configure/build presets and target selection;
- portable Noct runtime/compiler/JIT changes;
- the BeUI zedBSD backend and its backend-local tests;
- target-facing documentation and build options.

zedBSD owns:

- its public UAPI headers, libc/syscalls, linker scripts, and image/QEMU tests;
- package metadata, provenance, install paths, and the immutable upstream
  release identity;
- only the minimal adapter that is inherently part of zedBSD packaging and
  cannot reasonably live in canonical Noct.

The backend includes zedBSD UAPI headers from the selected sysroot/source tree;
it must not copy those definitions into Noct. Canonical source is published in
`awemorris/NoctLang`. The completed target integration extracts official
release `v2.0.1` under `userland/base/noct/noct/`; the host extraction under
`build/NoctLang` is separate but comes from the same verified archive. The old
dirty `userland/noct/NoctLang` tree was preserved unchanged outside the
repository and is unused. q022 explicitly authorized and completed the earlier
two-repository publication and removed the former submodule/gitlink.

## 5. Product boundaries

- Initial and resumed target acceptance is amd64 only. i386/PC-98 target Noct
  is not claimed by the resumed package. Completing the missing canonical i386
  toolchain and porting target/backend/JIT acceptance to those ABIs is a
  separate later Phase.
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
- p008 does not edit, publish, or test target Noct. P009 preserves the optional
  `/usr/bin/noct` policy, keeps Remacs held, backs up the old dirty checkout
  without modification, and permits no untracked downstream source change.
