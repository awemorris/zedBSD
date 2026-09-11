# Boot, root storage and installation status

Status: current; accepted amd64 installation and PC98 FAT installation.

Start with the [build guide](build-from-source.md) and select a BIOS-capable
amd64 image for the following disposable QEMU example. Commands run in the
repository root on the host:

```sh
make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk disk-image
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
files in their image mode; the installer also uses explicit block-formatting
modes on inactive target partitions. Partition creation is owned by `diskpart`. `--verify-pristine` is read-only but
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

The dedicated installer below populates this native root and writes its GPT
PARTUUID selector. It creates `/swapfile` on UFS and an `/etc/fstab` swap entry;
normal startup activates the file after mounting root. There is no separate
swap partition or FAT `swap0` setting in this mode.

The [retained root-mode acceptance](../../plan/history/queue-q015.md) covers native and
overlay roots and selector reordering; later
[configured-loader gates](../../plan/history/queue-q032.md) establish required-file
behavior. Those historical gates do not constitute acceptance of a new native
installer or every hardware storage controller. Current formatter and
publication gates are [q148](../../plan/history/queue-q148.md) and
[q149](../../plan/history/queue-q149.md).

## Mount paths

Public mount/unmount calls resolve existing directory paths using the caller's
root and working directory, including nested paths and symbolic links. Both
require superuser authority. A child mount or an open file keeps its mount busy;
an ordinary directory is not an unmount target. See
[path acceptance](../../plan/ws019/phase021/results.md).

If a removable UFS medium has already been lost, ordinary `umount` preserves its
failed synchronization result. Root can use `umount -f /mount/point` to explicitly
discard that revoked attachment. This requires all files, mappings and working
directories on it to be released; root, bind mounts and unsupported filesystems
are refused, as is a still-live medium. Success reports local disposal, not saved
data. Physical buffers and replacement-media publication remain owned by disk
retirement after all old-device users are gone.

Populated tmpfs unmount reclaims its directory entries and data after all users
close; it does not require empty directories. A failed busy/sync/prepare check
preserves the contents for retry. See
[p022 acceptance](../../plan/ws019/phase022/results.md).

## USB trial and installation boundary

A prepared USB boot medium can be selected through the machine's firmware
boot menu. Match the image's BIOS/UEFI Variant to the firmware path and retain
a separate recovery boot medium. This guide intentionally has no raw-device
write command: image creation, destructive media preparation and installing
onto an existing disk have different effects. Read-only
[diskpart/blkid output](../reference/block-command-output.md) helps establish
device identity before any separately authorized media preparation.

USB-root QEMU acceptance includes xHCI and paired EHCI/UHCI operation, input
and checked shutdown ([q141](../../plan/history/queue-q141.md),
[q147](../../plan/history/queue-q147.md)). Shut down through `halt`; wait for completed
shutdown before removing the medium. A failed storage sync or controller
quiesce is an error to investigate, not permission to report a clean halt.
These tests are not a new physical USB acceptance campaign.

## Install from the running installation disk

Boot the installation medium, log in as `root`, and run one frontend from an
interactive console:

```sh
/sbin/zedinst
```

For the BeUI frontend, use:

```sh
/sbin/zedinst-graphic
```

Both launch `/bin/noct` and share the installation backend in `/lib/zedinst`.
They are included in the amd64 and PC98 installer packages. The graphical
frontend requires a 640x480 true-color framebuffer and keyboard or pointer
input. On amd64 UEFI, `video=640x480` in the selected boot configuration
requests the mode; the supplied configuration already includes it. RGB24
artwork can be displayed on the accepted QEMU XRGB32 framebuffer. On PC98,
use a supported 640x480 true-color CoreGraph configuration; the ordinary
low-color display is not sufficient.

1. Select **Installation disk**. HTTP is displayed as unavailable; there is
   no network-download installation path yet. The installer requires a mounted
   `rootfs.img` from that installation disk and checks its identity. Starting
   from a native installed root without that image produces an error.
2. Choose the installation mode, then the destination disk. The installation
   source is excluded. Read the displayed disk name and capacity; enumeration
   names alone are not persistent identities.
3. Review the selected destination and confirm using the frontend's prompt.
   Dedicated installation starts at **NO** and requires selecting **YES** to
   erase. Graphical coexistence and PC98 also require explicit confirmation.
   Text-mode amd64 coexistence asks for the displayed `INSTALL DISK PARTITION`
   phrase instead of the dedicated mode's NO/YES menu.
4. Wait for copy and verification to finish. Image copies show byte progress.
   Native-root copying counts files first and reports copied-file progress,
   preserving attributes. The source installation's writable data and active
   swap contents are not cloned: fresh destination data/swap are initialized.
5. After **Installation complete**, close the frontend and run `halt`. Once
   shutdown completes, remove the installation source and select the target
   disk in firmware. Log in on the target-only boot; inspect `mount` for the selected root and the boot console
   `swap: active sources=...` message for active swap.

### amd64 coexistence

Select **Coexist with an existing FAT filesystem**, then a destination payload
partition. The target needs a healthy supported GPT, exactly one usable FAT32
ESP and a separate writable FAT32 payload partition with enough free space.
Existing partition geometry and unrelated files are preserved. The installer
publishes `EFI/BOOT/BOOTX64.EFI` on the ESP and `vmunix`, `rootfs.img`, fresh
`data.img`, `swapfile` and `zedbsd.cfg` on the payload FAT filesystem. It selects
the installed files by partition identity and publishes configuration last.
It is not an arbitrary overwrite/update tool: conflicting existing managed
files are reported rather than silently replaced.

### amd64 dedicated disk

Select **Dedicated disk (erase all contents)** and the whole destination disk.
This destroys the selected disk's existing contents after explicit confirmation.
The current path requires a writable disk with 512-byte logical sectors.
Partitioning is automatic: a FAT32 EFI System Partition sized for the boot
files (at least 64 MiB), then a UFS root partition using the remaining aligned
space. There is no partition editor, separate home partition or swap partition.

The ESP receives `EFI/BOOT/BOOTX64.EFI`, `vmunix` and `zedbsd.cfg`. The installer
copies the mounted lower `rootfs.img` tree into UFS, preserving file attributes,
creates the native swap file, and configures startup swap activation. This path
installs a UEFI loader; it does not provision a new BIOS boot chain. Use the
firmware's target-disk boot entry or select `EFI/BOOT/BOOTX64.EFI` manually.

### PC98 FAT installation

PC98 is detected from `uname -a`. Use two IDE HDDs: the installation source
and a target with an existing PC98 partition layout, a working boot entry and
FAT16. Select the existing FAT partition. Its size must be at most 4 GiB and
every managed payload file at most 2 GiB; the UI also checks available space.

`BOOTZBSD.EXE`, `vmunix`, `rootfs.img`, fresh `data.img`, `swapfile` and
`BOOTZBSD.CFG` go on that FAT partition. No ESP or GPT is used. The existing
bootstrap and unrelated files are retained. Blank-disk bootstrap creation and
native-root installation are not implemented for PC98 in this installer.
The accepted QEMU run used a 486, 64 MiB, CoreGraph and two IDE disks, then
booted and logged in with the target alone. This does not resolve the separate
physical PC-9821V13 CF boot failure.

### Shell and recovery

**Open shell** leaves the wizard for `/bin/sh`; exiting it restarts source
admission and disk discovery. `diskpart` remains available for manual work;
the installer has no arbitrary partition-editing screen. Returning from a
shell does not preserve an old destination approval.

If installation reports failure or cleanup problems, retain the console's
`Published`, `Publication requires inspection` and `Preserved staging object`
paths. Inspect them before retrying. Do not delete staging or overwrite a
managed file merely to suppress a conflict. Dedicated formatting cannot restore
the previous disk contents. Cancellation before confirmation leaves installation
unauthorized; a later I/O failure is not equivalent to cancellation.

Accepted normal paths and target-only boots are recorded in
[amd64 native integration](../../plan/ws019/phase049/phase.md),
[graphical installation](../../plan/ws019/phase029/results.md)
and [PC98 FAT installation](../../plan/ws019/phase050/results.md).
[Multiple-NVMe acceptance](../../plan/ws004/phase050/results.md)
also verifies installed-root boot under both enumeration orders. There is no
longer a one-controller NVMe limit; current support remains one active namespace
per controller, bounded by resources and the shared disk registry.

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

q190 updates these instructions against current installer sources and retained
normal-path producer evidence. Earlier clean-build evidence remains in
[WS009-p002](../../plan/ws009/phase002/phase.md).
This documentation update performs no new physical installation or exhaustive
installer fault campaign.
