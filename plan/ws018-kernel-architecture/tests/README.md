# WS018 shared test cases

Parent: [WS018](../ws.md)

Reusable fixtures implemented by an authorized Phase live in this directory.
Future Queue execution adds only the fixtures needed by its selected Phase;
disposable build/QEMU evidence belongs under `../temp/` and remains untracked.

| Case ID | Owning Phase | Required observation |
| --- | --- | --- |
| KA-T001 | p001 | Every supported architecture resolves sources only below `src/drivers/`; no root `drivers/` dependency remains |
| KA-T002 | p001 | `make -j16` produces the supported amd64, i386 PC/AT, i386 PC-98, sun4u, rpi4, and x68k artifacts represented by current manifests |
| KA-T010 | p002 | MBR, PC-98, Sun, and X68k disk-label fixtures resolve the same partitions before and after relocation; PC-98 auto-selection and X68k native-label parsing are distinct cases |
| KA-T011 | p002 | Tree audit finds one C file per platform, no common platform source, no historical platform directory, and graphics-owned fonts |
| KA-T020 | p003 | [`run-ufs-independence-host-test.sh`](run-ufs-independence-host-test.sh) compiles and links UFS1 and UFS2 superblock/endian boundaries independently, preserves little-/big-endian decode, rejects malformed metadata, and rejects cross-format implementation symbols |
| KA-T021 | p003 | [`run-ufs2-consistency-host-test.sh`](run-ufs2-consistency-host-test.sh) exercises UFS2-owned journal commit/replay/rejection and snapshot create/preserve/reopen/read/delete behavior without UFS1 or retired common-UFS symbols |
| KA-T030 | p004 | [`run-filesystem-identity-host-test.sh`](run-filesystem-identity-host-test.sh) validates registry dispatch and real FAT12/16/32 plus little-/big-endian UFS1/UFS2 type, UUID, and label callbacks, including bounded mismatch, malformed, truncated, I/O, ambiguity, output-validation, cache, and UUID/LABEL unique/duplicate/case-fold/hard-error selector cases |
| KA-T031 | p004 | The same runner proves PARTUUID/PARTLABEL selector resolution bypasses filesystem callbacks, swap stays generic, filesystem+swap hybrids return `EEXIST`, and source audit finds no FAT/UFS format parser in `block-identity.c` |
| KA-T040 | p005 | [`run-exec-preparation-host-test.sh`](run-exec-preparation-host-test.sh) preserves shebang, script-vector, allocation, and credential boundaries before and after the source merge; the separate dead-image audit has no live consumer |
| KA-T050 | p006 | [`run-boot-header-aggregate-compile.sh`](run-boot-header-aggregate-compile.sh) compiles the sole `<kern/boot.h>` contract for kernel 32/64-bit, amd64/i386 HAL, PC-98 handoff, and X68k Stage 2 consumers without retired or private boot-header dependencies; maintained WS003 parser/source fixtures preserve all parameter and ownership behavior |
| KA-T051 | p006 | The existing 31-cell matrix passes on i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI production loaders |
| KA-T060 | p007 | Xzed discovers keyboard and relative/absolute pointer capabilities through `/dev/input/eventX`, receives keys/movement/buttons, and contains no `/dev/mouse` or console-event fallback |
| KA-T070 | p008 | PC/AT PS/2 and PC-98 bus-mouse boots each expose dynamic evdev nodes and correct press/release/motion events |
| KA-T071 | p008 | Source/node/symbol audit finds final input ownership and no `mouse-device.c` or `/dev/mouse` implementation |
| KA-T080 | p009 | PC/AT boot-framebuffer/VGA/Cirrus and PC-98 GDC/Cirrus backends each provide the stable `/dev/graphics` behavior independently |
| KA-T081 | p009 | Backend-disabled targets do not fabricate `/dev/graphics`, and no `graphics-device.c` implementation remains |
| KA-T090 | p010 | FAT12/16/32, LFN, file read, and directory traversal fixtures are behavior-identical after consolidation while compatibility `bootfs` remains |
| KA-T100 | p011 | FAT boot media provides rootfs image, writable data overlay, and file-backed swap through native filesystem/VFS calls |
| KA-T101 | p011 | Native partition root and runtime FAT mounts remain usable, with bounded failures for missing/corrupt image files |
| KA-T110 | p012 | All supported builds and representative boots pass with no `struct bootfs`, legacy boot-source header, broad internal state, or obsolete platform residue |

The supported build gate is `make -j16`; the aggregate `make check` target and
repository `.internal/` material are excluded.  amd64 runtime tests use
`qemu-system-x86_64`; destructive image tests use disposable copies.

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
identity composer, swap-header parser, and link-visible private
`fat_identify`, `ufs1_identify`, and `ufs2_identify` callbacks.  The fixture
provides only memory-disk, registry, partition, allocation, and lock shims; it
does not duplicate the identity decoders.  Its FAT driver tables delegate to
the production `bootfat_probe` decoder while omitting unrelated file
operations.  Cache coverage treats a newly initialized `struct disk` as the
documented re-probe boundary; no test-only cache invalidation API is assumed.
