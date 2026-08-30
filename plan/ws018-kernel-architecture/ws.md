# WS018: kernel source ownership and interface consolidation

Last updated: 2026-08-30

WSID: `ws018`

Status: active in `q035`; `q026` ownership migration finished

Parent: [master plan](../master.md)

Last verified Phase: `ws018-p010`

Resume point: q035 processes p009's remaining graphics runtime matrix in
parallel with p011.  p010 is complete; migrate FAT to direct native VFS before
p012 removes the compatibility filesystem and startup residue.

Shared tests: [WS018 test index](tests/README.md)

## Goals

- Make source location state architectural ownership: kernel policy remains in
  `src/kern`, while device, bus, disk-label, and filesystem implementations
  live under `src/drivers`.
- Reduce each `src/kern/platform/<platform>.c` to the one translation unit that
  defines that platform's initialization hooks; keep all reusable or device-
  specific implementation outside the platform directory.
- Preserve stable external contracts while allowing platform and driver
  implementations to be independent, duplicated, and replaceable in full.
- Consolidate the boot public API into one deliberately large and stable
  `include/kern/boot.h`, and consolidate its implementation without changing
  the already accepted four-platform boot-parameter behavior.
- Remove legacy `bootfs`, `/dev/mouse`, and shared graphics/mouse frontends only
  after their consumers have crossed to the replacement interfaces.

## Objective

Reorganize the kernel around explicit ownership boundaries rather than its
historical platform directories and convenience abstractions.  This is a
staged migration, not a bulk rename: every Phase must leave supported targets
buildable, and a legacy interface is deleted only after its replacement has
passed its own consumer and integration gates.

The intended end state is:

```text
src/kern/                 kernel mechanisms and policy
src/kern/platform/        README.md plus exactly one C file per platform
src/drivers/              device, bus, disk-label and filesystem implementations
include/kern/boot.h       one stable public boot contract
```

The project explicitly permits implementation duplication below stable
interfaces.  In particular, mouse and graphics backends own complete frontend
behavior instead of inheriting a generic implementation merely to reduce
source duplication.

## Fixed design decisions

### Source-tree ownership

- Move the repository's top-level `drivers/` tree to `src/drivers/` and update
  every supported build manifest and include path atomically.
- Place `input-*.c` directly under `src/drivers/`.  The
  `src/drivers/hid/` name denotes human/input devices in general, not only USB
  HID; suitable keyboard, mouse, bus-mouse, PS/2, and USB HID implementations
  should move there.
- Move the console character-device implementation to
  `src/drivers/fs/console.c`.
- Move MBR, PC-98, and Sun partition-table parsers under
  `src/drivers/disklabel/`.  The generic partition registry and policy remain
  kernel core code.
- Move platform font data into the graphics drivers that own it.
- `src/kern/platform/` contains one `.c` file per supported platform and a
  `README.md`, with no shared helper source.  Each platform file defines
  `kern_platform_init()` and the complete platform hook set.  The historical
  `src/kern/{pc98,pcat,sun4u,rpi4,x68k}/` directories are removed after their
  contents have acquired real owners.

### Input and mouse ownership

- `/dev/input/eventX` is the sole mouse event interface after migration.
- Migrate Xzed keyboard and relative/absolute pointer input completely to
  capability-discovered evdev before deleting the legacy `/dev/mouse` path
  and Xzed's console continuous-event fallback; no hidden compatibility path
  remains.  The later kernel/UAPI deletion of console continuous-event and
  key-state ioctls remains WS006 `IN-06`; WS018 does not duplicate that owner.
- Eliminate `mouse-device.c`.  Copy the required producer/lifecycle logic into
  the PC-98 bus-mouse and PS/2 mouse modules, make it backend-local, and emit
  evdev events directly.  Truly generic event-queue logic belongs to evdev,
  not to a common mouse frontend.

### Graphics ownership

- Eliminate `graphics-device.c` as a shared implementation.  Each graphics
  backend contains its own complete `/dev/graphics` character-device/frontend
  implementation behind the existing public UAPI.
- The amd64/PC-AT driver keeps its boot-framebuffer, VGA, and Cirrus selection
  private.  PC-98 drivers independently own their GDC/Cirrus implementation.
- A platform without an implemented graphics backend does not receive a
  fabricated generic `/dev/graphics` node.

### Boot API stability

- Merge `boot-parameters.c`, `boot-source-contract.c`, and `boot-source.c` into
  `src/kern/boot.c`.
- Merge their public declarations into `include/kern/boot.h`, then delete the
  smaller public boot headers.  This aggregate header is an intentional,
  stable interface ledger: do not split or casually revise it for local
  implementation convenience.
- Preserve the q015 contract for `boot0`--`boot3`, exclusive `rootpart` versus
  overlay root/data, `swap0`--`swap3`, and `init` across i386 PC/AT, i386
  PC-98, amd64 BIOS, and amd64 UEFI.

### Filesystem ownership

- UFS1 and UFS2 are independent drivers behind the filesystem interface.  Each
  owns its endian, journal/snapshot, and any copied helper implementation; a
  small amount of duplication is preferred to a constraining common UFS
  layer.
- Add a filesystem-driver identity/probe function pointer.  FAT and UFS own
  their filesystem recognition; generic block, partition, and swap identity
  remain generic.
- Consolidate FAT12/16/32, LFN, and VFS implementation into
  `src/drivers/fs/fat.c` and use `fat_` terminology where behavior is not
  FAT16-specific.  The move/consolidation Phase must preserve the current
  `bootfs` compatibility path before semantics change.
- Migrate FAT boot-media access to the ordinary filesystem/VFS contract while
  preserving loop-backed root, writable overlay, and file-backed swap.
  Delete `struct bootfs` only after every loader and caller uses the native
  contract.

### Core consolidation and cleanup

- Merge `exec-prepare.c` into `exec.c` without changing exec semantics.
- Delete unused `image.c`/`image.h` after proving that no live image contract
  depends on them; do not merge dead code merely to preserve it.
- Retire obsolete startup/shell/device test residue or move the useful checks
  into maintained WS fixtures.
- Delete broad `internal.h` state only after live handoff/device state has a
  focused owner and all consumers have migrated.

## Scope

- source moves and build-manifest repair for all supported architectures;
- platform initialization ownership and platform-directory retirement;
- partition/disk-label, UFS, FAT, filesystem identity, exec, and boot cleanup;
- Xzed evdev consumer migration, independent mouse producers, and legacy
  `/dev/mouse` removal;
- independent `/dev/graphics` frontend ownership;
- shared, maintained acceptance fixtures under this WS.

## Non-goals

- changing the public evdev or `/dev/graphics` UAPI;
- implementing USB HID, a new graphics mode, mapped LFB, GPU acceleration, or
  native GPU ownership (WS006, WS017, and WS014 own those outcomes);
- changing the accepted x86 boot-parameter language or CPAR menu grammar;
- changing on-disk FAT/UFS formats;
- removing compatibility code before the phase ordering below reaches its
  explicit deletion gate;
- implementing any Phase as part of q024.

## Dependencies

- [WS003](../ws003-bringup/ws.md) owns the accepted four-platform boot and USB
  root behavior that boot/FAT changes must preserve.
- [WS006](../ws006-input/ws.md) owns evdev semantics and the eventual `IN-06`
  console legacy-UAPI deletion; WS018 changes source/backend ownership and
  removes Xzed's fallback only after the replacement semantics are available.
- [WS007](../ws007-graphics/ws.md) owns Xzed behavior and supplies the consumer
  migration gate before `/dev/mouse` deletion.
- [WS016](../ws016-swap-control/ws.md) supplies file-backed and partition-backed
  swap behavior that FAT/VFS cleanup must preserve.
- [WS017](../ws017-lfb-graphics/ws.md) may later extend the stable graphics
  interface; WS018 must not pre-empt its unresolved UAPI decision.

## Staged migration and ordering

The source-tree move comes first so later documents and manifests use one
canonical driver location.  UFS ownership and independent graphics work can
then proceed independently; the final platform-directory removal waits for
graphics/font ownership.  Filesystem identity follows the UFS ownership split.
Core and boot consolidation remain behavior-preserving steps.

Consumer-before-provider deletion is mandatory:

```text
Xzed keyboard/pointer evdev-only consumer
  -> independent mouse evdev producers
  -> delete /dev/mouse and mouse-device.c

FAT mechanical consolidation (bootfs retained)
  -> FAT native VFS/file-backed access
  -> remove struct bootfs and legacy boot/image state

graphics UAPI held stable
  -> duplicate frontend into each backend
  -> delete graphics-device.c
```

No Queue should combine a semantic migration with the deletion of its only
known rollback path unless the Phase explicitly supplies and verifies the new
path in the same bounded change.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws018-p001` | [Driver source-tree relocation](phase001-driver-source-tree/phase.md) | Complete (`q025`) | All root `drivers/` content and manifests move to `src/drivers/`; every supported target still builds |
| `ws018-p002` | [Disk-label and platform ownership](phase002-disklabel-platform-layout/phase.md) | Complete (`q026`) | Disk labels/fonts gain driver owners and every platform has exactly one platform-init TU |
| `ws018-p003` | [Independent UFS1 and UFS2](phase003-ufs-independence/phase.md) | Complete (`q025`) | UFS1/UFS2 build and operate without a shared implementation directory |
| `ws018-p004` | [Filesystem-owned identity probes](phase004-filesystem-identity/phase.md) | Complete (`q025`) | FAT/UFS recognition is dispatched through the filesystem interface while generic identities remain generic |
| `ws018-p005` | [Core source consolidation](phase005-core-source-consolidation/phase.md) | Complete (`q025`) | exec preparation is internal to `exec.c` and proven-dead image code is gone |
| `ws018-p006` | [Boot implementation and public API consolidation](phase006-boot-api-consolidation/phase.md) | Complete (`q025`) | One `boot.c` and one stable public `boot.h` preserve the four-platform contract |
| `ws018-p007` | [Xzed evdev-only consumer](phase007-xzed-evdev-consumer/phase.md) | Complete (`q026`) | Xzed discovers and consumes keyboard plus relative/absolute pointer events only through `/dev/input/eventX` |
| `ws018-p008` | [Independent input/HID driver ownership](phase008-input-hid-driver-ownership/phase.md) | Complete (`q026`) | Input sources have final owners, mouse backends emit evdev directly, and `/dev/mouse` is removed |
| `ws018-p009` | [Independent graphics frontends](phase009-independent-graphics-frontends/phase.md) | Uncleared (`q026`; implementation complete, runtime matrix remains) | Each supported graphics backend independently supplies `/dev/graphics`; shared frontend is gone |
| `ws018-p010` | [FAT source consolidation](phase010-fat-source-consolidation/phase.md) | Complete (`q035`) | One driver-owned `fat.c` preserves current FAT/bootfs behavior |
| `ws018-p011` | [FAT native VFS migration](phase011-fat-native-vfs/phase.md) | Planned after p004, p010 | Boot-media files use the normal filesystem contract with overlay and swap intact |
| `ws018-p012` | [Legacy bootfs and platform residue removal](phase012-legacy-bootfs-removal/phase.md) | Planned after p002, p006, p011 | `struct bootfs`, obsolete internal state, and historical platform residue are absent without regressions |

## WS completion conditions

- There is no repository-root `drivers/` tree and no historical
  `src/kern/{pc98,pcat,sun4u,rpi4,x68k}/` implementation directory.
- `src/kern/platform/README.md` states and the tree enforces one C translation
  unit per platform and no shared platform helper.
- UFS1, UFS2, and FAT independently satisfy the filesystem interface; block
  identity dispatch does not encode filesystem-specific probes.
- One `src/kern/boot.c` implements the declarations in one stable
  `include/kern/boot.h`, and the 31-cell four-platform boot-parameter matrix
  remains green.
- Xzed and all mouse producers use evdev; its console-event fallback,
  `/dev/mouse`, `mouse-device.c`, and their compatibility paths are absent.
- Every supported graphics backend owns a complete `/dev/graphics` frontend;
  no shared `graphics-device.c` implementation remains.
- FAT root image, writable overlay, runtime mounts, and file-backed swap work
  through ordinary filesystem interfaces; `struct bootfs` is absent.
- All applicable KA-T001--KA-T110 gates, `make -j16`, and
  `git diff --check` pass without `make check` or `.internal/`.

## Reconsideration boundaries

Stop the affected Phase and request design review if an existing stable UAPI
must change, if one-platform-per-file cannot represent a platform without
shared policy code, if a driver cannot expose evdev or `/dev/graphics` without
a new cross-driver abstraction, or if native FAT/VFS access cannot preserve
loop root, overlay, and file-backed swap ownership.  Do not solve those cases
by silently restoring a generic mouse/graphics frontend or a new `bootfs`
facsimile.
