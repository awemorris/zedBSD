# Boot, root storage and installation status

Status: current boot procedure; full NVMe installation remains incomplete

Start with the [build guide](build-from-source.md) and select a BIOS-capable
amd64 image for the following disposable QEMU example. Commands run in the
repository root on the host:

```sh
make -j16 disk-image
cp build/amd64/hdd-image.img /tmp/zedbsd-trial.img
qemu-system-x86_64 -machine pc -m 512 -smp 4 \
  -drive file=/tmp/zedbsd-trial.img,format=raw,if=ide -boot c
```

Use an unused destination name if that temporary file already contains work.
The copy receives guest writes; the built image remains the reference. The
expected observation is loader/kernel progress followed by init and `login:`.
This IDE example verifies boot/root operation, not USB or NVMe transport.
UEFI-only images require OVMF and a disposable writable variables file; the
build guide links the maintained Variant runner. PC-98 requires the `pc9821`
QEMU fork and its own image, not this PC machine command.

## What the loader selects

| Path | Configuration and kernel selection |
| --- | --- |
| PC/AT i386 BIOS | PBR → BOOTZBSD.EXE → payload FAT `/zedbsd.cfg` |
| PC-98 BIOS | PBR → BOOTZBSD.EXE → payload FAT `/BOOTZBSD.CFG` |
| amd64 BIOS | PBR → BOOTZBSD.EXE → payload FAT `/zedbsd.cfg` |
| amd64 UEFI | BOOTX64.EFI → same-physical-disk FAT configuration discovery |

Exactly one `kernel=` directive selects the kernel relative to the chosen
configuration filesystem. Remaining lines become kernel parameters. Missing
configuration, invalid kernel or over-limit records stop visibly; a missing
file does not select a hidden direct-kernel fallback. UEFI LoadOptions does
not replace the configuration file. See the exact
[parameter contract](../reference/kernel-boot-parameters.md).

UEFI may execute the loader from an ESP while reading configuration and kernel
from another FAT partition. After boot, inspect:

```sh
sysctl kern.boot.firmware_partition
sysctl kern.boot.config_partition
sysctl kern.boot.config_matches
```

The [provenance reference](../reference/boot-provenance.md) explains PARTUUID,
`unavailable` and ambiguity. `boot0` is a kernel boot-filesystem selection;
it is not proof of which ESP firmware used. Multiple configurations can boot
by discovery order but are not acceptable installer admission evidence.

## File-backed overlay root

The generated configuration supplies the lower root image, writable data image
and swap file, for example these lines in addition to its `kernel=` directive:

```text
overlay-root=rootfs.img
overlay-data=data.img
swap0=swapfile
```

The loader qualifies those relative file values with its selected `boot0`.
The kernel privately mounts boot filesystems, attaches the lower image read-only
and upper image read-write, then mounts the overlay as `/`. Both root and data
are required; neither an ephemeral upper nor automatic filename detection is
implied. Aliasing, recursion and conflicting root selectors are errors.

Persistence belongs to `data.img` and the containing medium, not to the
immutable lower image. Do not format or replace an active data/swap backing
file. [mkfs/mkswap](../reference/image-formatters.md) take reserved regular
files; they do not partition a disk. `--verify-pristine` is read-only but
intentionally rejects a previously used filesystem or swap payload, even if
it is otherwise valid.

## Native root

An already prepared, bootable filesystem can instead be selected explicitly:

```text
rootpart=PARTUUID=01234567-01
init=/sbin/init
```

The selector here is an example MBR partition identifier; substitute the
uniquely resolved identity of the prepared root, not its device enumeration
order. Remove both overlay directives when using `rootpart`. The selected
filesystem must contain the target programs, loader-compatible kernel/config
arrangement and required runtime files. Formatting an empty UFS image alone
does not populate a bootable root. The supported filesystem is the single
64-bit `ufs` implementation, with no selectable UFS1 alternative.

The [retained root-mode acceptance](../../plan/queue-q015.md) covers native and
overlay roots and selector reordering; later
[configured-loader gates](../../plan/queue-q032.md) establish required-file
behavior. Those historical gates do not constitute acceptance of a new native
installer or every hardware storage controller. Current formatter and
publication gates are [q148](../../plan/queue-q148.md) and
[q149](../../plan/queue-q149.md).

## Mount paths

Public mount/unmount calls resolve existing directory paths using the caller's
root and working directory, including nested paths and symbolic links. Both
require superuser authority. A child mount or an open file keeps its mount busy;
an ordinary directory is not an unmount target. See
[path acceptance](../../plan/ws019-installation/phase021-nested-mount/results.md).

Populated tmpfs unmount reclaims its directory entries and data after all users
close; it does not require empty directories. A failed busy/sync/prepare check
preserves the contents for retry. See
[p022 acceptance](../../plan/ws019-installation/phase022-tmpfs-unmount/results.md).

## USB trial and installation boundary

A prepared USB boot medium can be selected through the machine's firmware
boot menu. Match the image's BIOS/UEFI Variant to the firmware path and retain
a separate recovery boot medium. This guide intentionally has no raw-device
write command: image creation, destructive media preparation and installing
onto an existing disk have different effects. Read-only
[diskpart/blkid output](../reference/block-command-output.md) helps establish
device identity before any separately authorized media preparation.

USB-root QEMU acceptance includes xHCI and paired EHCI/UHCI operation, input
and checked shutdown ([q141](../../plan/queue-q141.md),
[q147](../../plan/queue-q147.md)). Shut down through `halt`; wait for completed
shutdown before removing the medium. A failed storage sync or controller
quiesce is an error to investigate, not permission to report a clean halt.
These tests are not a new physical USB acceptance campaign.

The first-stage installer design targets existing GPT/ESP/FAT32 storage without
partitioning or formatting those partitions. Its six managed files and
config-last publication/recovery component passed q149, but public admission,
confirmation, packaging and installed-NVMe-only boot are still unfinished.
There is therefore no supported executable `zedinst` installation recipe yet.
[WS019](../../plan/ws019-installation/ws.md) owns p004/p005 completion; this
guide will add the actual invocation and recovery procedure after acceptance.

## Failure diagnosis

- No loader under SeaBIOS: check whether the image is UEFI-only.
- Configuration/kernel error before the kernel banner: inspect the selected
  FAT file, exact path, required directive and firmware path.
- Root-selection failure: check the complete parameter record, unique selectors,
  both overlay files and absence of conflicting `rootpart`.
- Init failure: check the target executable and ABI; `init=/bin/sh` is an
  explicit rescue selection, not an automatic fallback.
- A boot-device or storage error: retain the console log and test a disposable
  copy with the matching controller. Do not repair by formatting the live root.

q150 reconciles documentation only. It retains earlier clean-build evidence
from [WS009-p002](../../plan/ws009-documentation/phase002-build-guide/phase.md)
and the current incremental amd64/PCAT/PC98 gates from q149; it does not claim
a new clean-room build or new physical installation.
