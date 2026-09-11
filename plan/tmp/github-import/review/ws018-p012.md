# WS018 Phase 012: legacy bootfs and startup residue removal

Last updated: 2026-08-30

WSID: `ws018`

Phase ID: `p012`

Combined ID: `ws018-p012`

Status: Complete (`q035`)

Parent: [WS018](../ws.md)

Tests: [WS018 test index](../tests/README.md)

## Objective

Delete the now-unreferenced legacy `bootfs`/namespace stack, historical
startup shell and device-test path, and the broad `src/kern/internal.h`
wiring.  Leave `main.c` as the modern boot-parameter, VFS, VM, scheduler, and
init entry path, while preserving the small live handoff/device information
needed by `/dev/system` through an existing stable kernel interface.

## Preconditions and deletion gate

- `ws018-p002` has moved real partition/platform ownership out of historical
  platform directories, so old startup probing is not their last consumer.
- `ws018-p005` has removed the dead image loader, and `ws018-p006` has
  established the one boot implementation/header.
- `ws018-p011` has removed every FAT dependency on `struct bootfs` and uses
  native VFS for boot media, loops, overlays, runtime sources, and swap.
- Run a fresh whole-tree call/include/build audit before deleting each file.
  A declaration or object-list reference is not evidence of a required
  runtime contract; a live caller must be traced to its maintained use.

## Work packages

### 1. Inventory and classify the remaining legacy graph

Audit `src/kern/fs.c`, `namespace.c`, `device.c`, `startup.c`, `shell.c`,
`env.c`, `internal.h`, and their headers, globals, aliases, tests, make targets,
and linker entries.  Classify each remaining symbol as:

- already replaced by native mount/namei/file/boot-source code;
- used only by the historical startup shell or PC-98 M9 image;
- live handoff/device state used by `/dev/system`; or
- unexpectedly live and therefore a blocker to deletion.

Record the classification in the Phase result.  Do not retain a broad header
or obsolete translation unit merely because one live field remains.

### 2. Give live handoff/device state a focused existing owner

The copied boot handoff, platform boot-device table, and device count remain
read-only kernel runtime state used by `system-device.c`.  Declare focused
read-only accessors or declarations in the existing `include/kern/kernel.h`
and update `main.c` and `system-device.c` to use their explicit names.  Do not
create another catch-all `internal` header and do not expose mutable aliases.

Keep the handoff snapshot lifetime and the `/dev/system` values unchanged.
Remove the `ho`, `devs`, and `device_count` preprocessor aliases after their
consumers migrate.

### 3. Delete the legacy filesystem and namespace facade

After the zero-caller audit, delete:

- `src/kern/fs.c` and `include/kern/fs.h`;
- `src/kern/namespace.c` and `include/kern/namespace.h`;
- every `boot_volume`, `bootfs_result`, `bootfs_driver`, `bootfs`,
  `bootfs_file`, `bootfs_dirent`, and `bootfs_namespace` declaration or call;
  and
- `kern_mounted_fs`/`kern_mounted_namespace` storage and aliases in `main.c`
  and `internal.h`.

Do not fold this API into `mount.h` or rename it as compatibility.  Native
mount, namei, inode, path, and file interfaces are the only retained
filesystem path.

### 4. Retire the historical startup shell and M9 residue

Delete `src/kern/device.c`, `startup.c`, and `shell.c` once the audit confirms
that the production kernel no longer links them.  Remove the PC-98
`M9_STAGE2_OBJS`, `stage2-m9-test` link/image targets, special
`ZEDBSD_M9_WRITE_TEST` object rules, and the raw-sector write-test hooks.

Remove the old environment implementation (`env.c`/`env.h` and its global)
if, as in the current tree, it has no consumer outside that retired shell.
Do not migrate shell commands into the kernel or keep a second boot flow.
Should one diagnostic still be required, stop and extract it as a maintained,
bounded WS fixture using current disk/VFS interfaces rather than retaining the
old runtime.

### 5. Shrink and delete `internal.h`

Remove obsolete startup state, partition arrays, device selection, BIOS probe,
console/string helper declarations, environment/bootfs globals, M9 hooks, and
all alias macros.  Remove corresponding freestanding helper definitions from
`main.c` when they have no remaining caller.

Update production consumers to their owning public/internal subsystem header.
When only the handoff/device access moved in package 2 remains, delete
`src/kern/internal.h`.  This Phase must not replace it with several tiny
single-purpose headers; prefer the existing owning headers and stable APIs.

### 6. Clean every supported build and maintained test

Remove deleted sources/objects/targets from all architecture manifests and
remove obsolete comments, dependency rules, and test references.  Preserve
the normal PC-98 image and boot path.  Historical planning records may name
the removed code, but no active build or test may depend on it.

## Verification

- KA-T110 performs a code/build audit with no definition, declaration, include,
  object, or active target for `struct bootfs`, the legacy namespace, old image
  API, startup shell/device path, M9 write mode, or `src/kern/internal.h`.
- Focused `/dev/system` coverage proves the BIOS ID and boot-device table/count
  remain identical through the new explicit kernel-state access.
- Existing WS003 boot-source/root/overlay/swap/init coverage and WS016 runtime
  swap/backing coverage pass without a legacy facade.
- i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI representative boots reach
  the configured init through the ordinary VFS path; the normal PC-98 image is
  still produced even though its obsolete M9 diagnostic image is not.
- Build all currently supported manifests represented by KA-T002, then run
  `make -j16` and `git diff --check`.  Do not use `make check` or `.internal/`.

## Completion conditions

- no legacy bootfs or bootfs namespace type, implementation, global, header,
  or active build reference exists;
- no obsolete startup shell/device/M9 runtime or target exists;
- the unused legacy environment disappears if its final audit remains empty;
- `internal.h` and its alias-based shared state are gone;
- `/dev/system` receives the same immutable boot metadata through an explicit
  existing kernel interface; and
- all supported builds and representative four-platform boots use the modern
  boot/VFS/init path successfully.

## Result and evidence

Completed in `q035` on 2026-08-30.  The fresh live-caller audit classified the
remaining graph as follows:

- `fs.c`/`namespace.c` and their mounted-state globals had already been
  replaced by native mount/namei/inode/file/VFS code;
- `device.c` and `shell.c` were reachable only from the historical PC-98 M9
  diagnostic graph; `env.c` was linked and `env_init` was called by production
  `main.c`, but the initialized state had no reader or writer outside that
  retired graph; `startup.c` was already absent;
- `/dev/system` still required the boot BIOS ID and platform boot-device
  table/count; and
- no unexpected maintained consumer was found.

`kernel.h` now owns three narrow read-only accessors for that live metadata.
`main.c` continues to copy the boot handoff, while retaining the old device
table lifetime contract as a borrowed const view of `entry.c`'s static
kernel-lifetime array.  Out-of-range lookup returns `NULL`.
`system-device.c` uses only those accessors; the mutable aliases and broad
`internal.h` boundary are gone.

The legacy filesystem/namespace facade, old environment, startup
device/shell sources, their headers, and `internal.h` were deleted.  All six
platform manifests were updated.  The PC-98 M9 object/rule/image graph, stale
libc host target, and maintained-test references were removed.  KA-T090 is now
only a native KA-T100/KA-T101 wrapper, so no active fixture recreates the
deleted interface.

Verification evidence:

- KA-T110 passed ordinary and ASan/UBSan production-linked runs with 73 checks
  each, including handoff copy, borrowed device lifetime/count, repeated host
  initialization, out-of-range lookup, and active source/build/test audits.
- The BR-T46 file-swap guest issued production `/dev/system` `GET_INFO`, every
  in-range `GET_DEVICE`, and the count-index `ENOENT` request before exercising
  overlay/data/swap.  i386 PC/AT, i386 PC-98, and amd64 BIOS passed with one
  boot device; amd64 UEFI preserved and passed the valid pre-enumeration value
  of zero devices.  All four paths emitted the metadata marker and final swap
  PASS.
- KA-T100/KA-T101 passed ordinary and ASan/UBSan runs with 441,528 checks per
  run.  KA-T030/KA-T031 passed 110 checks and ownership audit; maintained
  WS016 backing, runtime swap, boot-source, and command gates passed.
  BR-T42, BR-T43, BR-T44, KA-T050, and shutdown-order coverage also passed.
- Empty build directories produced amd64, i386 PC/AT, i386 PC-98,
  arm64/RPi4, SPARC V9/sun4u, and m68k/X68k kernels, with each architecture's
  contract checker passing.  Their artifact audit found no retired object;
  the amd64 global-symbol audit exposed only the three new metadata accessors
  and none of the old aliases.  The ordinary `make -j16` image build and
  `git diff --check` passed.

No reconsideration boundary was reached.

## Reconsideration boundary

Stop and return the Phase `uncleared` if the final audit finds a maintained
consumer of the legacy filesystem, namespace, startup shell, environment, M9
diagnostic, or a live `internal.h` service with no existing owner.  Report the
consumer and extract an explicit migration Phase; do not delete live behavior,
hide it behind aliases, or recreate a bootfs-compatible facade under a new
name.
