# WS018 shared test cases

Parent: [WS018](../ws.md)

Reusable fixtures implemented by an authorized Phase live in this directory.
Future Queue execution adds only the fixtures needed by its selected Phase;
disposable build/QEMU evidence belongs under `../temp/` and remains untracked.

| Case ID | Owning Phase | Required observation |
| --- | --- | --- |
| KA-T001 | p001 | Every supported architecture resolves sources only below `src/drivers/`; no root `drivers/` dependency remains |
| KA-T002 | p001 | `make -j16` produces the supported amd64, i386 PC/AT, i386 PC-98, sun4u, rpi4, and x68k artifacts represented by current manifests |
| KA-T010 | p002 | [`run-disklabel-host-test.sh`](run-disklabel-host-test.sh) links the production legacy MBR, PC-98 native/auto, Sun, and X68k parsers against deterministic disk images, preserves count, indexes, extents, flags, UUIDs, and labels across relocation, and proves that MBR no longer decodes GPT identity |
| KA-T011 | p002 | [`run-platform-layout-audit.sh`](run-platform-layout-audit.sh) finds one C file per platform, no common platform source, no historical platform directory or header, driver-owned disk labels, and graphics-owned fonts |
| KA-T020 | p003 | [`run-ufs-independence-host-test.sh`](run-ufs-independence-host-test.sh) compiles and links UFS1 and UFS2 superblock/endian boundaries independently, preserves little-/big-endian decode, rejects malformed metadata, and rejects cross-format implementation symbols |
| KA-T021 | p003 | [`run-ufs2-consistency-host-test.sh`](run-ufs2-consistency-host-test.sh) exercises UFS2-owned journal commit/replay/rejection and snapshot create/preserve/reopen/read/delete behavior without UFS1 or retired common-UFS symbols |
| KA-T030 | p004 | [`run-filesystem-identity-host-test.sh`](run-filesystem-identity-host-test.sh) validates registry dispatch and real FAT12/16/32 plus little-/big-endian UFS1/UFS2 type, UUID, and label callbacks, including bounded mismatch, malformed, truncated, I/O, ambiguity, output-validation, cache, and UUID/LABEL unique/duplicate/case-fold/hard-error selector cases |
| KA-T031 | p004 | The same runner proves PARTUUID/PARTLABEL selector resolution bypasses filesystem callbacks, swap stays generic, filesystem+swap hybrids return `EEXIST`, and source audit finds no FAT/UFS format parser in `block-identity.c` |
| KA-T040 | p005 | [`run-exec-preparation-host-test.sh`](run-exec-preparation-host-test.sh) preserves shebang, script-vector, allocation, and credential boundaries before and after the source merge; the separate dead-image audit has no live consumer |
| KA-T050 | p006 | [`run-boot-header-aggregate-compile.sh`](run-boot-header-aggregate-compile.sh) compiles the sole `<kern/boot.h>` contract for kernel 32/64-bit, amd64/i386 HAL, PC-98 handoff, and X68k Stage 2 consumers without retired or private boot-header dependencies; maintained WS003 parser/source fixtures preserve all parameter and ownership behavior |
| KA-T051 | p006 | The existing 31-cell matrix passes on i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI production loaders |
| KA-T060 | p007 | [`run-xzed-input-host-test.sh`](run-xzed-input-host-test.sh) links Xzed's production evdev consumer and proves capability-only multi-device discovery, key/repeat/modifier/Caps translation, framed relative/absolute pointer input, dropped-event resynchronization, split reads, HUP, and absence of legacy/fixed-identity paths |
| KA-T070 | p008 | [`run-input-hid-host-test.sh`](run-input-hid-host-test.sh) links each production mouse driver directly and proves independent evdev lifecycle, ordered press/release/motion frames, close/reopen state, and late-IRQ exclusion; platform boots provide hardware evidence |
| KA-T071 | p008 | The same runner proves final input/console/HID source ownership, registration order, and no live `mouse-device.c`, `/dev/mouse`, registry symbol, or legacy UAPI implementation |
| KA-T080 | p009 | [`run-graphics-frontends-host-test.sh`](run-graphics-frontends-host-test.sh) links the production PC/AT and PC-98 frontends independently and preserves registration, ownership, mode, drawing, glyph, copy-fault, rollback, and restore behavior; [`run-graphics-runtime-matrix.sh`](run-graphics-runtime-matrix.sh) exercises production PC/AT VGA/Cirrus and PC-98 GDC/Cirrus Xzed entry/render/console restoration |
| KA-T081 | p009 | The host runner proves that both frontend copies remain explicit and behavior-identical, no registry/common implementation remains, and exactly two platform-owned registration sites exist; the runtime runner builds and boots an amd64 graphics-disabled image and proves `/dev/graphics` is absent |
| KA-T090 | p010 | [`run-fat-consolidation-host-test.sh`](run-fat-consolidation-host-test.sh) preserves the historical p010 checkpoint by delegating to the maintained native KA-T100/KA-T101 gate after p011; the removed compatibility fixture remains recorded in the p010 Phase and repository history rather than as an active legacy consumer |
| KA-T100 | p011 | [`run-fat-native-vfs-host-test.sh`](run-fat-native-vfs-host-test.sh) links the production FAT filesystem type against in-memory FAT12/16/32 disks and exercises native mount/inode/file lookup, I/O, mutation, sync, extents, and backing identity; boot media separately provides rootfs image, writable data overlay, and file-backed swap through the same native VFS path |
| KA-T101 | p011 | The same runner covers native inode/file lifetime, open-writer rename and path-truncate authority, mutable replacement/unlink orphans, remount persistence, mirrored FAT copies, read-only and no-space behavior, 1024-byte logical sectors, close/reclaim durability retry, directory-entry slot reuse, FAT32 LFN sector-boundary namespace rollback, and partial grow/shrink rollback under bounded faults; native partition-root and private/runtime mount publication remain runtime gates |
| KA-T110 | p012 | [`run-legacy-bootfs-removal-host-test.sh`](run-legacy-bootfs-removal-host-test.sh) links production [`main.c`](../../../src/kern/main.c) with [`kernel-boot-metadata-host-test.c`](kernel-boot-metadata-host-test.c), verifies copied handoff and borrowed kernel-lifetime device metadata through the focused accessors, and audits every active source/build/test owner; BR-T46 then exercises production `/dev/system` `GET_INFO` plus valid/out-of-range `GET_DEVICE` on all four x86 boot paths |

The supported build gate is `make -j16`; the aggregate `make check` target and
repository `.internal/` material are excluded.  amd64 runtime tests use
`qemu-system-x86_64`; destructive image tests use disposable copies.

Strict PC/AT GPT semantics added after the source-ownership move are exercised
by WS004 HW-T20's production parser fixture rather than duplicated here.
KA-T011 nevertheless audits `gpt.c` and `pcat-auto.c` as architecture-neutral
disk-label owners and requires PC/AT to select the strict GPT/legacy-MBR
dispatcher.

KA-T040's runner keeps `src/kern/exec-prepare.c` as its default so the same
command records the pre-merge baseline in the pre-merge tree.  After p005
removes that source, pass `src/kern/exec.c` as the runner's only argument (or
set `KA_T040_SOURCE`) to run the identical fixture against the consolidated
translation unit.

KA-T050 is intentionally a post-merge gate.  It includes `<kern/boot.h>` before
any other project header and compiles, but does not link, each freestanding
consumer class.  Set `M68K_CC` when the X68k cross compiler is not installed
under its default `m68k-linux-gnu-gcc` name.  Behavioral parser, boot-source,
swap-source, and loader-handoff coverage remains owned by the maintained
fixtures in [`plan/ws003-bringup/tests`](../../ws003-bringup/tests/README.md);
this WS does not copy or fork them.

KA-T020 has two explicit modes.  Its default strict mode requires the final
`src/drivers/fs/ufs1/` and `src/drivers/fs/ufs2/` owners, links each
superblock/endian fixture with only that driver's sources, and audits the
resulting symbols.  `--baseline` exists only for a pre-p003 tree: it records
the old UFS2 behavior while clearly reporting that UFS2 still links the UFS1
endian implementation.  A baseline result is not an independence pass.

KA-T021 likewise defaults to the final UFS2-private
`ufs2-consistency.h`/`ufs2-journal.c`/`ufs2-snapshot.c` boundary.
`--baseline` records the equivalent behavior in a historical tree that still
owns those routines below `src/kern/ufs/`; it is rejected after relocation.
The host fixtures deliberately keep the dependency surface small.  Full VFS
mount, namespace read/write, sync, and unmount behavior remains covered by the
supported kernel builds and representative UFS boot/mount gates required by
p003 rather than by duplicating the kernel's disk/inode/mount environment in a
host shim.

KA-T030/KA-T031 link the production registry dispatcher, generic block
identity composer, swap-header parser, native `fat_filesystem_type`, and the
UFS1/UFS2 identity callbacks.  The fixture provides only memory-disk,
registry, partition, allocation, VFS-lifetime, and lock shims; it does not
duplicate the identity decoders.  The FAT registry entry therefore exercises
the production filesystem-owned identity path without exposing its private
callback as public API.  Cache coverage treats a newly initialized
`struct disk` as the documented re-probe boundary; no test-only cache
invalidation API is assumed.

KA-T080/KA-T081 compile the same host scenario twice: once with the real
PC/AT frontend and a PC/AT fake backend, and once with the real PC-98 frontend
and a PC-98 fake backend.  The runner also compares normalized frontend source,
audits retired registry/common paths, and requires exactly one platform-owned
registration call per frontend.  Supported-target and graphics-disabled kernel
builds remain the link-time proof that only the selected frontend is present;
QEMU boots provide the device-node and hardware-backend evidence.

The q035 runtime runner uses `qemu-system-i386` for production PC/AT VGA and
Cirrus cells, the maintained `pc9821` QEMU for forced GDC (`coregraph=off`) and
Cirrus (`coregraph=on`) cells, and `qemu-system-x86_64` for the
graphics-disabled node-absence cell.  Each graphics cell launches Xzed with
the packaged session, captures a rendered frame, sends `SIGTERM` to its job
process group, and captures the restored text console.  PC-98 prompt control
uses the maintained text-VRAM decoder because post-init output is not mirrored
to debugcon.  All QEMU disks are disposable copies under WS `temp/`.

KA-T090 recorded the p010 `bootfs` compatibility boundary in both an ordinary
build and an ASan/UBSan build.  Its historical mutations covered create and
create-truncate, offset write, append, sparse extension, truncate grow/shrink,
mkdir, non-empty and empty rmdir, unlink, same- and cross-directory rename,
replacement, directory reparenting, explicit flush/remount persistence,
allocation exhaustion, and read-only rejection.  Open-writer rename repair,
orphan-chain reclaim, `disk_sync`, and native inode/file lifetime are native
VFS contracts rather than `bootfs` operations and are therefore not claimed by
that historical fixture.  Once p011 removed the compatibility API, the
maintained KA-T090 runner began executing KA-T100/KA-T101 instead of retaining
a post-migration legacy consumer.  The old C fixture is absent from the active
tree; its checkpoint is preserved by the p010 Phase result and repository
history.

KA-T100/KA-T101 supersede KA-T090's maintained behavioral gate after p011
removes the FAT `bootfs` boundary.  The native runner repeats the FAT12/16/32
SFN/LFN, cross-cluster read, mutation, persistence, no-space, and read-only
checkpoint through production `filesystem_type`, `mount`, `inode`, and `file`
operations, then adds the lifetime, extent, backing-identity, unmount, and
fault/retry contracts which KA-T090 could not exercise.  Failure coverage
verifies pre-retry size/content/extents/free-space
authority, mount-sync durability for live and failed-close writers, deferred
orphan reclaim, deleted-direntry reuse isolation, replacement and
cross-directory rename rollback, FAT12/16 insertion rollback, 255-byte LFN
lifecycle, and atomic multi-slot LFN unlink/rename—including an SFN at the
next sector's first slot—before remount.  Generic namei
permission/transaction checks, private-mount
publication, loop/overlay/swap ownership, and the complete boot chain remain
the Phase's kernel-build and runtime acceptance gates.

KA-T110 keeps its host fixture at the state-owner boundary: it links the
production `main.c`, proves that the boot handoff is copied, proves that the
device table remains a borrowed const view of `entry.c`'s static
kernel-lifetime storage, checks count and out-of-range behavior, and audits
the production `system-device.c` wiring.  The maintained BR-T46 file-swap
guest supplies the character-device boundary coverage by issuing
`ZEDBSD_SYSTEM_GET_INFO`, every in-range `ZEDBSD_SYSTEM_GET_DEVICE`, and one
out-of-range request expected to fail with `ENOENT` on i386 PC/AT, i386 PC-98,
amd64 BIOS, and amd64 UEFI.

KA-T060 supplies fake descriptors only at the I/O boundary and directly links
`userland/X11/xzed/input.c`; directory traversal, event-name filtering,
capability classification, all state machines, and event translation remain
production code.  The runner executes both an ordinary `-Werror` build and an
ASan/UBSan build.  It includes arbitrary byte-boundary reads, multi-device
modifier/button aggregation, full signed absolute-axis ranges with an
`INT_MAX` screen extent, `SYN_DROPPED` state recovery through key/axis
snapshots, CapsLock toggle retention without an LED query, and drain-before-
remove HUP behavior.  Production PS/2 keyboard/mouse runtime coverage remains
the amd64 QEMU acceptance recorded by p007 rather than being simulated here.

KA-T070 compiles the real PC/AT PS/2 and PC-98 bus-mouse translation units
against only host I/O, IRQ, locking, thread, and evdev capture shims.  Both
ordinary and ASan/UBSan runs exercise first-open failure/retry, two readers,
last-close stop, signed relative motion, unchanged buttons, zero-motion button
edges, held-button release on close, state recovery on reopen, and complete
`SYN_REPORT` framing.  The PS/2 fixture requires publication and EOI while the
controller lock is held; the PC-98 fixture requires publication while its
lifecycle mutex is held, so final close cannot race a late frame.  KA-T071's
source audit is host-side; booted node absence and hardware delivery remain
part of the Phase's amd64 and applicable PC-98 runtime gates.
