# Boot, root storage and installation

Status: how-to; boot and root modes on x86, and installing amd64 UEFI PCs
with the graphical installer.

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

Exactly one `kernel=` directive selects the kernel, as a path relative to the
chosen configuration filesystem. On amd64 UEFI the path may instead carry the
`rootpart:` prefix, which names an absolute path on the UFS root partition
that `rootpart=PARTUUID=` selects (`kernel=rootpart:/boot/vmunix`); `logo=`
takes the same prefix. The BIOS loaders do not read UFS and stop on such a
path. Remaining lines become kernel parameters. Missing configuration, invalid
kernel or over-limit records stop visibly; a missing file does not select a
hidden direct-kernel fallback. UEFI LoadOptions does not replace the
configuration file. See the exact
[parameter contract](../reference/kernel-boot-parameters.md).

UEFI may execute the loader from an ESP while reading configuration and kernel
from another FAT partition; the amd64 native image does so, with only the loader
on the ESP and `vmunix`, `zedbsd.cfg` and the logo on a FAT32 partition of the
Basic Data type labelled `ZEDBSD`. A system the installer installed instead has
the loader and `zedbsd.cfg` on its ESP and the kernel and logo in `/boot` of its
UFS root. The FAT the configuration came from is
`boot0` and is shown at `/boot/boot0`; the ESP the firmware started the loader
from is shown at `/boot/efi` (the same mount twice when one partition is both).
After boot, inspect:

```sh
sysctl kern.boot.firmware_partition
sysctl kern.boot.config_partition
sysctl kern.boot.config_matches
```

The [provenance reference](../reference/boot-provenance.md) explains PARTUUID,
`unavailable` and ambiguity. `boot0` is a kernel boot-filesystem selection;
it is not proof of which ESP firmware used. Multiple configurations on one disk
boot by discovery order: the loader uses the first `zedbsd.cfg` it finds, which
is why the installer does not add a second zedBSD to a disk that has one.

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
files in their image mode, and have block-device modes for inactive
partitions. Partition creation is owned by
[diskpart](../reference/block-administration.md). `--verify-pristine` is
read-only but intentionally rejects a previously used filesystem or swap
payload, even if it is otherwise valid.

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

The [installer](#install-zedbsd-with-the-installer) populates this native root,
selects it by a GPT PARTUUID it generates, and keeps the kernel in its `/boot`.
Swap is either a swap partition, selected by `swap0=PARTUUID=`, or a
`/swapfile` on the UFS root with an `/etc/fstab` entry that normal startup
activates after mounting root.

### Replacing the kernel of an installed system

The loader reads the kernel from the UFS root without replaying the journal,
so the new kernel must be written to its final place on the disk before the
next boot. Copy it under a new name, flush it, rename it over the old one and
synchronize:

```sh
cp vmunix.new /boot/vmunix.tmp
sync
mv /boot/vmunix.tmp /boot/vmunix
sync
```

Keep the old kernel under another name (for example `/boot/vmunix.old`) until
the new one has booted, and keep a recovery medium.

## Mount paths

Public mount/unmount calls resolve existing directory paths using the caller's
root and working directory, including nested paths and symbolic links. Both
require superuser authority. A child mount or an open file keeps its mount
busy; an ordinary directory is not an unmount target.

If a removable UFS medium has already been lost, ordinary `umount` preserves its
failed synchronization result. Root can use `umount -f /mount/point` to explicitly
discard that revoked attachment. This requires all files, mappings and working
directories on it to be released; root, bind mounts and unsupported filesystems
are refused, as is a still-live medium. Success reports local disposal, not saved
data. Physical buffers and replacement-media publication remain owned by disk
retirement after all old-device users are gone.

Populated tmpfs unmount reclaims its directory entries and data after all
users close; it does not require empty directories. A failed busy/sync/prepare
check preserves the contents for retry.

## USB trial and installation boundary

A prepared USB boot medium, such as the installer's image, can be selected
through the machine's firmware boot menu. Match the image's BIOS/UEFI Variant to the firmware path and retain
a separate recovery boot medium. This guide intentionally has no raw-device
write command: image creation, destructive media preparation and installing
onto an existing disk have different effects. Read-only
[diskpart/blkid output](../reference/block-command-output.md) helps establish
device identity before any separately authorized media preparation.

Shut down through `halt`; wait for completed shutdown before removing the
medium. A failed storage sync or controller quiesce is an error to
investigate, not permission to report a clean halt.

## Install zedBSD with the installer

The installer is a Keiland application on its own USB image. It installs
zedBSD on an amd64 PC that has UEFI and ACPI and was started through UEFI.
BIOS, PC-98 and other architectures have no installer. It installs to a
writable physical disk the kernel registers: NVMe (also behind Intel VMD),
SATA, or USB Mass Storage, with 512-byte or 4096-byte logical sectors.
The [installer reference](../reference/installer.md) defines the image, the
screens and the helper `install-admin` exactly.

### The rule of the installer's image

**The installer's image is for installing only. Do not use it as your system.**
Do not keep starting the computer from it, copy it to an internal disk, offer
network services from it or keep your files on it.

A system started from the installer's image logs in automatically as the
default user `user`, whose password is the published default `user`. Treat
that start as a start as root: the installer's helper writes any disk without
asking for a password, so every program in that session, including web
content in a browser, has the same power as root. This is intended, as with
any installer that runs as root, and the image is safe only because of the
rule above. Closing the installer and using the desktop to finish an
installation is part of installing; using it afterwards is not.

### Build the installer's image

```sh
make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk installer-image
```

The configuration must be amd64 with the `native` variant, and
`installer-image` cannot share the command line with another image goal such
as `disk-image`. `make menuconfig` configures only the kernel and the loader
for this image. The programs and packages are a fixed selection: all of the
base, the Keiland desktop, the firmware the kernel's drivers name, and every
amd64 package under `userland/packages/`; tests and X11 are left out.

The result is `build/amd64/installer-image.img`. It holds an ESP with the
loader, a FAT32 partition labelled `ZEDBSD` with the kernel, the logo and a
`zedbsd.cfg` that names the image's root and swap by PARTUUIDs new for each
build, a UFS root with the installer and `/opt/installer`, and a swap
partition. `/opt/installer` holds the units an installed system is made of,
their index, and the loader, kernel and logo the installed system receives.

Write the image to a USB drive whose contents you can lose, using the host's
disk-image writer on the whole drive (not one of its partitions). Check the
drive's identity before writing; this guide gives no raw-device command.

### Install

1. Back up anything on the target disk that you want to keep.
2. Start the PC from the USB drive through the firmware's boot menu, choosing
   its UEFI entry. The desktop starts and opens the installer instead of the
   Welcome. If the PC was not started through UEFI, the first screen says so
   and the installer does not continue.
3. **Welcome**: read the warning and the image's rule, then **Next**.
4. **Disk**: choose the target disk. Disks the running system uses (the USB
   drive itself, its boot FAT, its swap), read-only disks, file-backed disks
   and disks too small are shown greyed with the reason. **Reload** reads the
   disks again after plugging one in. Names such as `nvme0n1` and `sda` are
   not persistent: check the kind and size.
5. **Partitions**: choose one:
   - **Use the whole disk** erases every partition, which the screen lists.
     The installer makes a 512 MiB ESP, a swap partition (the size of the
     memory, from 1 GiB to 8 GiB; 1 GiB on a disk under 32 GiB) and a UFS root
     on the rest.
   - **Choose partitions** keeps the existing partitions. It is offered only on
     a GPT disk that `diskpart` can edit, with fewer than 16 used entries, no
     `zedbsd.cfg` on any of its FAT filesystems (another zedBSD), and either no
     ESP or an ESP with enough free space and no file the installer would
     overwrite. A FAT that cannot be read counts as one with `zedbsd.cfg`.
6. With **Choose partitions**:
   - **Edit**: **Delete** and **Create** write the partition table **at once,
     when pressed**; **Cancel** later does not undo them. Delete asks first.
     Create asks only for a size and puts the partition, aligned to 1 MiB, in
     the first free space that holds it, with the type `zedBSD-unassigned`.
     When the disk has no ESP, Create first makes a 512 MiB ESP in the same
     free space. A partition of the target disk that is mounted (for example
     by Files) is unmounted first; if it cannot be, nothing is written.
   - **Root**: choose the partition for the UFS root; it must be large enough
     for what you install. Choosing a partition that is not unassigned asks
     first, showing its type and label, because its contents are erased.
   - **Swap**: choose a swap partition, or none. With none, the installer makes
     `/swapfile` on the root (the memory's size, at most 2 GiB − 4 KiB) after
     asking.
7. **Install**: choose what to install. Base System is required. Keiland
   Desktop and Firmware are on; leaving either out asks first (without the
   desktop the system has only the console login; without firmware some Wi-Fi
   and GPU devices may not work). Firmware is installed only after accepting
   the licences that **Read the licences** shows. Packages: OpenSSH, OpenSSL with CA
   certificates and Fonts are on; Clang, GTK4, libavcodec, curl, Python and
   the other packages are off. What a choice depends on is added and shown
   greyed. The page shows the total size against the root's room.
8. **User**: the login name, its display name and password, the root
   password, automatic login (on by default, off without the desktop) and the
   hostname (default `zedbsd`). The user is an administrator (groups `wheel`
   and `network`).
9. **Confirm**: check the disk, the partition table before and after, which
   partitions are formatted, what is installed and where the loader goes. With
   the whole disk, tick that its data may be erased. **Install** asks once
   more and starts writing.
10. **Progress**: do not turn the PC off or restart it. **Cancel** stops at the
    end of the current step (or between files while unpacking).
11. **Finished**: it says how to start the installed system. Restart, remove
    the USB drive, and choose the installed disk in the firmware's boot menu.
    When the disk already had `\EFI\BOOT\BOOTX64.EFI`, it was kept and the
    loader is `\EFI\ZEDBSD\BOOTX64.EFI`: choose that in the boot menu. The
    installer adds a `zedBSD` boot entry at the end of the boot order, so the
    firmware's default does not change; the firmware's settings can move it
    first. Until the USB drive is removed the firmware may start from it again.

The installed system starts from its own disk alone. It logs in to the desktop
automatically when that was chosen, and otherwise shows `login:`; log in with
the name and password you chose. Check the root and swap:

```sh
mount
sysctl kern.boot.config_partition
cat /boot/boot0/zedbsd.cfg /etc/fstab
```

`/` is the UFS root and the configuration partition is the target disk's ESP.
The configuration names the root (and a swap partition) by PARTUUID; with a
swap file, `/etc/fstab` has its `/swapfile` line instead. When the swap file
could not be used, the last screen said the system has no swap. OpenSSH, when installed,
refuses root logins.

### Cancel, failure and starting over

Every screen's **Cancel**, after a question, and any failure lead to the failure
screen. It names the step and the reason and lists what was written to the
target disk: the partitions created or deleted on the edit screen, a changed
table, formatted partitions. When nothing was written it says so. The
`zedbsd.cfg` is written last, so a target whose installation did not finish
does not boot. Nothing is rolled back. **Start over** reads the disks again: a
whole-disk installation remakes the table, and partitions made on the edit
screen stay `zedBSD-unassigned` and can be chosen as root or swap.

When the table was written but the kernel could not reload it (a partition of
the target disk was in use), the new partitions cannot be used until a
restart. Restart from the USB drive and start over.

### Tools for manual work

The installer's work is done by base tools you can also use as root:
[`diskpart`](../reference/block-administration.md) for GPT tables,
[`mkfs` and `mkswap`](../reference/image-formatters.md) for FAT32, native UFS
and swap partitions, and [`efiboot`](../reference/installer.md#6-efiboot) for
UEFI boot entries. Each destructive command asks for the exact phrase that names
the device and its registration.

## Kernel image placement (amd64 UEFI)

`vmunix` is linked for physical 2 MiB, but firmware may already own memory
there (OVMF keeps its own data from 8 MiB, so a kernel image larger than about
6 MiB no longer fits). The UEFI loader therefore places the image at the linked
address when that is free and otherwise at the lowest 2 MiB-aligned run of
conventional memory below 1 GiB that holds it; the HAL maps the fixed virtual
range onto whichever physical range was used. The loader prints
`A64 KERN LINK`, `A64 KERN SIZE` and `A64 KERN LOAD` (plus `A64 KERN
RELOCATED` when moved), and the HAL prints `A64 KERNEL link=... load=...`.
The optional `kernel_phys=` line in `zedbsd.cfg` controls the choice:

| Value | Effect |
|---|---|
| `auto` (default) | linked address first, otherwise the lowest free aligned run |
| `link` | linked address only; fails with `Place kernel` when it is busy (the historical behaviour) |
| `0x...` | that 2 MiB-aligned physical address only, e.g. `kernel_phys=0x2000000` to exercise relocation |

When placement fails the loader lists the firmware memory descriptors below
1 GiB (`A64 KERN MAP t=<type> <start> +<pages>`). The BIOS loader always
loads at the linked address and ignores this setting.

## Failure diagnosis

- No loader under SeaBIOS: check whether the image is UEFI-only.
- A graphical boot (logo, quiet kernel messages) that stops before the login:
  on amd64 UEFI, power on and hold Ctrl and Shift while tapping Space until
  kernel messages scroll.  That one boot shows the kernel's messages on the
  console and logs in on the console, so the last message before the stop
  stays on the screen.  Ctrl alone keeps the graphical login, Shift alone keeps
  the logo.  `zedbsd.cfg` is not changed.  See
  [kernel boot parameters](../reference/kernel-boot-parameters.md) Section 7c.
- Configuration/kernel error before the kernel banner: inspect the selected
  FAT file, exact path, required directive and firmware path.
- `A64 KERN UFS <reason>` before the kernel banner: the UEFI loader could not
  read a `rootpart:` path. `duplicate PARTUUID` means another attached disk
  has a partition with the root's PARTUUID (for example a copy of the
  installed disk): detach it. Other reasons name a missing root partition, an
  unreadable UFS volume or a missing `/boot/vmunix`; start from a recovery
  medium and inspect the installed root.
- Root-selection failure: check the complete parameter record, unique selectors,
  both overlay files and absence of conflicting `rootpart`.
- Init failure: check the target executable and ABI; `init=/bin/sh` is an
  explicit rescue selection, not an automatic fallback.
- A boot-device or storage error: retain the console log and test a disposable
  copy with the matching controller. Do not repair by formatting the live root.
