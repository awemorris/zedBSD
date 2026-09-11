# WS018 Phase 009: independent graphics frontends

Last updated: 2026-08-30

WSID: `ws018`

Phase ID: `p009`

Combined ID: `ws018-p009`

Status: Complete (`q035`)

Parent: [WS018](../ws.md)

Shared tests: [WS018 test index](../tests/README.md)

## Objective

Replace the shared `graphics-device.c` plus runtime backend-ops registry with
complete, independent `/dev/graphics` implementations owned by the PC/AT and
PC-98 graphics drivers.  Source duplication below the stable UAPI is deliberate:
each platform implementation can be ported, audited, or replaced in full
without modifying a cross-platform frontend implementation.

## Dependencies

- `ws018-p001` has moved the driver tree to `src/drivers/` and established the
  source paths used here.
- The current `<zedbsd/graphics.h>` UAPI is the frozen behavioral boundary.
- `ws018-p002` waits for this Phase before deleting historical `src/kern/pcat`
  and `src/kern/pc98` directories.

WS017 may later extend the UAPI for LFB mapping.  If that extension lands before
this Phase, both independent frontends implement the accepted expanded UAPI.
If it lands afterward, WS017 must deliberately update and test both frontends.
This Phase neither invents nor pre-approves that UAPI.

## Target ownership and interface boundary

- PC/AT owns one graphics implementation under `src/drivers/graphics/` that
  contains the entire cdev/frontend state and directly dispatches to its private
  boot-framebuffer, VGA, or Cirrus implementation.
- PC-98 independently owns the same external cdev behavior and directly
  dispatches to its private GDC or Cirrus implementation.
- Fold the PC-98 CG-ROM glyph/cache implementation currently in
  `src/kern/pc98/font.c` into the PC-98 graphics implementation.  It is no
  longer a platform-kernel service or a public `kern/pc98` interface.
- Move the PC/AT font and VGA-font sources/header contracts under the PC/AT
  graphics ownership subtree.  HAL console users may continue to call that
  platform graphics/font interface, but the implementation no longer resides
  in `src/kern/pcat` and is not a generic cross-backend helper.
- Keep `<zedbsd/graphics.h>` unchanged.  Keep a small, stable kernel call
  boundary for post-`cdev_reset()` device registration and text restoration;
  the same symbols may be defined independently by the one backend selected at
  build time.  A common production frontend body is forbidden.
- Targets built without an implemented graphics backend do not register a
  `/dev/graphics` cdev.  They do not link a generic frontend merely so opens can
  return `ENODEV`.

## Frontend behavior duplicated into each backend

Each backend must privately own and test the current behavior, including:

- single-open ownership and `EBUSY` rejection;
- open failure when that platform backend was not prepared;
- `ENTER`, `LEAVE`, and close-time console restoration;
- mode enumeration/selection and exact copyin/copyout validation;
- rectangle bounds and overflow checks;
- fill, line, pattern, indexed/RGB24/mono blit, glyph, flush, row staging, and
  palette staging limits;
- mutex serialization across user faults and hardware calls;
- cleanup after partial initialization, failed ioctl, and process close.

The implementations may start as faithful copies, then replace ops calls with
direct private function calls.  They must not share a new `graphics-frontend.c`,
macro-generated body, included `.inc` implementation, or registration table to
remove textual duplication.  Shared public UAPI declarations are an interface,
not a shared implementation, and remain appropriate.

## Registration sequencing

Graphics hardware discovery/preparation currently runs from platform
initialization, while devfs/cdev state is reset later during VFS initialization.
The migration must preserve that order:

1. platform initialization prepares/selects the platform hardware backend but
   does not register a cdev that `cdev_reset()` would erase;
2. after `cdev_reset()`, VFS invokes the stable graphics-device registration
   entry point only when a graphics backend is configured;
3. the selected platform's graphics translation unit installs its own cdev ops
   and initializes its own frontend ownership state;
4. backend-disabled targets skip registration and expose no node.

Do not retain `graphics_driver_register()` or an ops registry to bridge these
steps.  Exactly one platform implementation supplies the stable registration
symbol in a graphics-enabled build.

## Detailed procedure

1. Snapshot the existing shared frontend's externally observable UAPI/error,
   ownership, locking, and cleanup behavior in a backend-neutral black-box
   fixture under this WS.  The fixture may be shared test code; production
   frontend implementation may not be.
2. Establish the PC/AT graphics ownership subtree.  Move its hardware and font
   sources/headers there, preserving amd64 boot-framebuffer and i386/amd64
   VGA/Cirrus selection plus HAL console font users.
3. Copy the complete cdev/frontend behavior into the PC/AT implementation,
   replace runtime `graphics_driver_ops` dispatch with direct private calls,
   and preserve the post-cdev-reset registration boundary.
4. Establish the PC-98 graphics ownership subtree.  Fold the CG-ROM glyph
   implementation into it, keep GDC/Cirrus selection private, and remove the
   old `kern/pc98/font` dependency.
5. Independently copy and integrate the complete cdev/frontend behavior into
   PC-98, replacing ops dispatch with direct PC-98 calls rather than calling
   PC/AT or common frontend code.
6. Change VFS/build selection so a configured backend supplies registration
   after cdev reset and an unsupported/disabled target supplies no node.  Keep
   hardware preparation failure bounded: boot may continue, but `/dev/graphics`
   registration/open behavior must be deterministic and logged.
7. Delete `src/kern/graphics-device.c`.  Delete the runtime
   `graphics_driver_register()` contract and obsolete `graphics_driver_ops`
   declarations once both implementations compile without them.  Retain only
   genuinely stable call/UAPI headers; do not add a convenience compatibility
   wrapper.
8. Update all architecture manifests and active focused tests.  Run the same
   black-box ioctl/lifecycle fixture against each independent frontend so
   duplication drift is detected at the interface boundary rather than hidden
   by shared production code.
9. Build all supported targets.  Exercise amd64 PC/AT under
   `qemu-system-x86_64` with the production boot framebuffer and available
   VGA/Cirrus configurations, and exercise PC-98 GDC/Cirrus with the maintained
   runner.  Start Xzed through `/dev/graphics`, render/flush, and verify normal
   text-console restoration on exit.
10. Build and boot representative graphics-disabled targets/configurations and
    prove `/dev/graphics` is absent rather than a generic node returning
    `ENODEV`.

## Verification contract

The implementation Queue must record at least:

```sh
test ! -e src/kern/graphics-device.c
test ! -e src/kern/pc98/font.c
rg -n 'graphics_driver_register|struct graphics_driver_ops' src include
rg -n 'cdev_register\("graphics"' src/drivers/graphics
make -j16
git diff --check
```

The first `rg` must find no live registry/ops implementation.  The cdev audit
must show independent PC/AT and PC-98 ownership, not one common source.  The
black-box fixture must apply identical current UAPI requests, invalid ranges,
ownership conflicts, enter/leave sequences, and close recovery to both
frontends.  KA-T080 covers the PC/AT boot-framebuffer/VGA/Cirrus and PC-98
GDC/Cirrus backend matrix; KA-T081 covers source deletion and backend-disabled
node absence.

Do not use `make check` or `.internal/`.  QEMU storage is copied before any
destructive runtime test.

## Completion conditions

- PC/AT and PC-98 each contain a complete, independently compiled
  `/dev/graphics` cdev/frontend plus their own hardware dispatch and font
  ownership.
- Both implementations preserve the current graphics UAPI, error behavior,
  locking, exclusive ownership, mode/render operations, and console restoration
  under the common black-box acceptance fixture.
- No production common frontend, backend ops registry,
  `graphics-device.c`, `src/kern/pc98/font.c`, or historical PC/AT font
  implementation remains under `src/kern`.
- Graphics-disabled targets expose no fabricated `/dev/graphics` node.
- KA-T080, KA-T081, applicable Xzed launch/render evidence, supported builds,
  and `git diff --check` pass.

## Reconsideration conditions

Mark the Phase `uncleared` and request human review if the stable graphics UAPI
itself must change, if cdev registration cannot occur after `cdev_reset()`
without a new platform-level contract, or if HAL console restoration/font use
cannot be preserved with driver-owned implementations.  Do not solve drift or
porting difficulty by restoring a shared frontend, macro-generating both
implementations from one body, or reintroducing a runtime backend registry.

## q026 execution record (2026-08-29)

Implementation completed the source and build-ownership migration:

- PC/AT and PC-98 now each own a complete, independently compiled frontend in
  `src/drivers/graphics/pcat/device.c` and
  `src/drivers/graphics/pc98/device.c`.  Each translation unit owns its cdev
  state and operations and directly calls only its platform-private backend.
- PC/AT font/VGA-font ownership and PC-98 display/CG-glyph ownership moved
  under the corresponding `src/drivers/graphics/` subtree.  The retired common
  frontend, backend-ops registry, and historical kernel font sources/headers
  were deleted.  `<zedbsd/graphics.h>` was unchanged.
- The build derives the internal `CONFIG_DRIVER_GRAPHICS_DEVICE` capability
  only for a configured PC/AT or PC-98 backend.  A content-stable build stamp
  invalidates guarded objects and the link when that capability changes, so an
  enabled and disabled kernel can be rebuilt correctly in the same `BUILD`.
  Unsupported targets keep the capability at zero and link no registration or
  platform graphics symbol.
- KA-T080/KA-T081 are owned under this WS.  The host fixture links each
  production frontend separately against a private fake backend and exercises
  registration/readiness, exclusive ownership, modes, enter rollback, fill,
  line, pattern, INDEX8/RGB24/MONO1 row staging, palette validation/faults,
  flush, glyph, backend failure, user-copy failure, restore, close, and reopen.
  The runner also normalizes and compares the two explicit frontend copies and
  audits retired paths and registration ownership.

Recorded verification:

```text
run-graphics-frontends-host-test.sh
  KA-T080 PC/AT independent frontend: PASS
  KA-T080 PC-98 independent frontend: PASS
  KA-T080/KA-T081 graphics ownership audit: PASS

make -j16                                      amd64/PC-AT: PASS
make -j16 vmunix (i386/PC-AT)                  PASS
make -j16 vmunix (i386/PC-98)                  PASS
make -j16 vmunix (aarch64/Raspberry Pi 4)      PASS
make -j16 vmunix (sparcv9/sun4u)               PASS
make -j16 vmunix (m68k/X68000)                 PASS
git diff --check                               PASS
```

Fresh graphics-disabled amd64 builds passed and contained no frontend/backend
registration symbol.  PC/AT, PC-98, and amd64 were also rebuilt in place
through enabled -> disabled -> enabled transitions: the guarded VFS/platform
objects were recompiled, the kernels were relinked, disabled images contained
no graphics registration/platform symbol, and the re-enabled image contained
only the selected platform implementation.  The three unsupported target
images likewise contained no graphics registration or PC/AT/PC-98 backend
symbol.

For runtime evidence, an amd64 disk image was rebuilt, copied to a disposable
WS `temp/` image, and booted with `qemu-system-x86_64`.  Root login followed by
`startx` selected the production 1024x768x32 boot framebuffer; the captured
1024x768 frame contained rendered Xzed output.  A separate disposable run
started Xzed, sent `SIGTERM`, waited for exit, and observed the marker and
resumed shell prompt, providing console-resume evidence.

The remaining Phase-contract runtime matrix was not executed in q026: PC/AT
VGA and Cirrus, PC-98 GDC and Cirrus, and a graphics-disabled boot/node-absence
check still need their maintained runners.  Accordingly this record does not
claim the full KA-T080 hardware matrix or every Phase completion condition;
the Queue result must retain these as explicit residual verification unless
that evidence is supplied separately.

### q026 disposition

The Phase is `uncleared`, not failed: all planned source ownership, deletion,
focused fixture, supported-build, configuration-toggle, amd64 Xzed/render, and
console-return work is present and verified.  It is not marked complete because
the remaining PC/AT and PC-98 backend runtime matrix and a booted disabled-node
check were not executed.  Resume by adding or selecting maintained runners for
those configurations and recording the resulting KA-T080/KA-T081 evidence; no
known source change is required first.

The source-ownership dependency needed by `ws018-p002` was nevertheless
satisfied in q026: fonts and complete frontends no longer resided in historical
platform directories.  q026 could therefore continue with p002 without
claiming p009's then-outstanding runtime acceptance.

## q035 completion record (2026-08-30)

q035 added the maintained residual runtime runner
[`run-graphics-runtime-matrix.sh`](../tests/run-graphics-runtime-matrix.sh).
It builds isolated production PC/AT, PC-98, and graphics-disabled amd64 images
with `make -j16`, copies every writable QEMU disk into WS `temp/`, and drives
login/Xzed/exit from QEMU's monitor only after the relevant guest marker is
observable.  No `.internal/` material or `make check` target is used.

The complete runner passed from one invocation:

```text
pcat-vga                 PASS  Xzed 640x480x4; PC/AT text mode restored
pcat-cirrus              PASS  Xzed 640x480x24; PC/AT text mode restored
pc98-gdc                 PASS  coregraph=off; Xzed 640x400; prompt restored
pc98-cirrus              PASS  coregraph=on; Xzed 640x480; prompt restored
amd64-graphics-disabled  PASS  no frontend/backend link symbol;
                              stat /dev/graphics -> ENOENT
KA-T080/KA-T081 production graphics runtime matrix: PASS
```

The PC/AT cells require the backend-selection marker, a nonempty Xzed
screendump, process-group `SIGTERM`, the backend's text-restoration marker, and
a distinct restored-console screendump.  PC-98 emits post-init diagnostics to
its console rather than debugcon, so the runner uses the maintained PC-98 VRAM
decoder for login and restored-prompt synchronization.  `coregraph=off` forces
GDC and produces a 640x400 Xzed frame; `coregraph=on` selects Cirrus and
produces a 640x480 Xzed frame before restoring the 640x400 text console.  The
disabled cell boots with `qemu-system-x86_64`, verifies the internal capability
stamp is `n`, audits the kernel symbol table, and observes `ENOENT` from a guest
`stat /dev/graphics`.

One preliminary PC/AT attempt observed the already separated intermittent ATA
cache-flush failure (`BIO_FLUSH`, `error=5`) before graphics could run.  The
same frozen image reached `login:` on retry and the full invocation above then
passed.  This is recorded as [`BUG-001`](../../known-bugs.md), is outside this
graphics Phase, and is not treated as a graphics regression.

The focused host fixture was rerun after the matrix and passed both production
frontend instances plus its source-ownership audit.  Together with q026's
amd64 boot-framebuffer/Xzed evidence, the residual VGA/Cirrus and disabled-node
results satisfy every p009 completion condition.  The Phase is complete.
