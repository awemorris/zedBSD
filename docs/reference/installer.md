# Installer, install-admin and efiboot

Status: reference; the amd64 UEFI installer's image, the installer program,
its helper `install-admin`, the layout of an installed system and `efiboot`.

The procedure is in the
[installation how-to](../howto/boot-and-storage.md#install-zedbsd-with-the-installer).
The installer supports only amd64 PCs with UEFI and ACPI that were started
through UEFI.

## 1. The rule of the installer's image

- The installer's image is for installing only and must not be used as a
  system. Using it as a system means going on using the started image for
  anything but installing: starting the computer from it as an everyday
  system, copying it to an internal disk and running that, offering network
  services from it, or keeping a person's files on it. Using its desktop after
  closing the installer, as part of an installation, is installing.
- The image logs in automatically as the default user `user`, whose password
  is the published default `user` (the same accounts as every image). Root is
  locked as in a release image.
- A start of the installer's image is a start as root. `install-admin` writes
  disks for that user without a password, so every process of that user,
  including remote content in a browser, has the power of root. The system
  does not try to separate that user from root.
- What keeps that power out of other systems is that `install-admin` exists
  only in the installer's image (Section 4.1) and that the image has no
  network login service. Adding one (for example sshd) to the distributed
  image changes the premise of this section.

The installer shows the rule on its first and last screens.

## 2. The installer's image

### 2.1 Building

```sh
make ZEDBSD_CONFIG=CONFIG installer-image
```

| Condition | Result |
| --- | --- |
| `ZEDBSD_PLATFORM` is not amd64 or `ZEDBSD_VARIANT` is not `native` | make stops with an error |
| Another image goal (`disk-image`, `run`, `rootfs`, `world`, ...) on the same command line | make stops with an error |
| `ZEDBSD_EXTRA_FILES`, `ZEDBSD_TEST_EXTRA_FILES` or `ZEDBSD_TEST_RC_CONF` set | make stops with an error |
| `installer` or `install-admin` in `ZEDBSD_USER_PROGRAMS` of any other build | make stops with an error; they are not selectable in menuconfig |

The configuration selects the kernel, its drivers and the loader. The
programs are a fixed selection, whatever the configuration selects: every
base program, every desktop program, the firmware the kernel's drivers name
(`config/drivers/firmware.list`), and every amd64 package under
`userland/packages/`. Test programs and X11 are not included. Graphical boot
and the graphical login are always on; `kmsg=` and extra boot lines come from
the configuration.

The make runs itself again with `ZEDBSD_IMAGE_PROFILE=installer` and builds
under `$(BUILD)/installer/`, not `$(BUILD)/rootfs`, so an ordinary image in the
same `BUILD` is not disturbed. The output is `$(BUILD)/installer-image.img`.
Build order:

1. the whole root of that selection, without the installer and its helper;
2. a manifest giving each file of it exactly one unit;
3. the units and the boot files, then the index and its SHA-256;
4. `install-admin`, built with that SHA-256, and the installer;
5. the live root: the base, desktop and firmware units' files, the installer,
   `install-admin`, `/opt/installer` and the live-only files;
6. the image.

The build fails when a file belongs to no unit or two, when two packages
install different contents at one path, when a set-user-ID or set-group-ID
file is not in the build's allowed list, or when the live root does not fit
the image generator's limits (16 GiB, 65536 inodes).

### 2.2 Layout

| # | Partition | Contents |
| --- | --- | --- |
| 1 | ESP (FAT32) | `\EFI\BOOT\BOOTX64.EFI` |
| 2 | FAT32, Basic Data type, volume label `ZEDBSD` | `vmunix`, `logo.ppm`, `zedbsd.cfg` |
| 3 | UFS root | the live root and `/opt/installer` |
| 4 | swap | ZEDSWAP2 |

The root and swap PARTUUIDs are new for every build, and `zedbsd.cfg` names
them (`rootpart=PARTUUID=`, `swap0=PARTUUID=`), so an attached installed disk
or another image does not make the selection ambiguous. Two drives written
from the same image file are still ambiguous.

Only the live root has `/etc/keiland/autologin` naming `user`. Accounts,
passwords and the screen lock are the base defaults.

### 2.3 /opt/installer

```text
/opt/installer/index                  the index (2.4)
/opt/installer/boot/BOOTX64.EFI       the loader, for the target's ESP
/opt/installer/boot/vmunix            the kernel, for the target's /boot/vmunix
/opt/installer/boot/logo.ppm          the logo, for the target's /boot/logo.ppm
/opt/installer/units/base.tar.gz
/opt/installer/units/desktop.tar.gz
/opt/installer/units/firmware.tar.gz
/opt/installer/units/CATEGORY-NAME.tar.gz   one for each package, e.g. lang-clang.tar.gz
```

The files and their directories are owned by root, with no group or other
write permission; `install-admin` refuses them otherwise.

A unit is a ustar archive compressed with deflate (gzip format). Its entries
are regular files, directories and symbolic links, with relative paths and no
`..` component. Owners in the archive are ignored: everything is created as
0:0. Modes are normalized at build time: without an explicit mode in the
package's registration, group and other write permission are removed. A
set-user-ID or set-group-ID entry must be listed in the index.

The installer and `install-admin` are in no unit and are never installed.

### 2.4 Index

One record a line, fields separated by TAB; an empty list is `-`.

| Record | Fields |
| --- | --- |
| `format` | `1` |
| `unit` | NAME, LABEL, GROUP (`base`, `desktop`, `firmware`, `package`), VERSION, TAR-BYTES, BYTES (unpacked), ENTRIES, SHA-256, DEPENDENCIES (unit names separated by spaces) |
| `boot` | NAME, PLACE, BYTES, SHA-256 |
| `setid` | UNIT, PATH, MODE |
| `license` | UNIT, PACKAGE, PATH of the licence text |
| `item` | ID, PARENT, DEFAULT (`required`, `on`, `off`), LABEL, UNITS |

Dependencies come from the packages' requirements; a dependency on a package
inside the base or desktop unit becomes a dependency on that unit. The build
refuses cycles. `item` records are the choices of the installer's Install
page (`platform/amd64/installer-items`).

| Item | Units | Default |
| --- | --- | --- |
| Base System | `base` (kernel, loader, base programs and libraries, `/usr/include`) | required |
| Keiland Desktop | `desktop` | on |
| Firmware | `firmware` (`/lib/firmware` and licences) | on |
| Clang | `lang-clang` | off |
| OpenSSH | `network-openssh` | on |
| OpenSSL and CA certificates | `security-openssl`, `security-ca-certificates` | on |
| GTK4 | `desktop-gtk4` | off |
| Fonts | `fonts-noto-color-emoji`, `fonts-hand-hershey` | on |
| libavcodec | `multimedia-libavcodec` | off |
| curl | `network-curl` | off |
| Python | `lang-python3` | off |
| Other packages | each remaining package, one line each | off |

Packages install their headers, `.pc` files and development links with their
libraries.

## 3. The installer

`/bin/installer` is a Keiland application. On the installer's image the
desktop starts it once a session in place of the Welcome (when
`/opt/installer/index` exists), and App Home lists it. It runs as the
session's user, never opens a disk, and gets everything it reads from or
writes to a disk through `install-admin`.

| Screen | Purpose | Next is refused when |
| --- | --- | --- |
| Welcome | what the installer does, back-up warning, the rule of Section 1 | the probe failed (Retry is offered), or the computer was not started through UEFI |
| Disk | one disk; others are greyed with the reason | no disk is chosen |
| Partitions | Use the whole disk, or Choose partitions | nothing is chosen |
| Edit | Delete and Create, written at once | the disk has no sufficient ESP |
| Root | the UFS root partition | none chosen, or too small |
| Swap | a swap partition, or none (a swap file) | the partition is under 64 MiB |
| Install | the items of 2.4 and the firmware licences | the root is too small, or Firmware is on without accepting its licences |
| User | name, display name, passwords, automatic login, hostname | a field breaks its rule |
| Confirm | the whole plan | with the whole disk, the erase box is not ticked |
| Progress | step, bytes, unit, elapsed time | |
| Failure | step, reason, what the target now holds; Start over or Close | |
| Finished | how to start the installed system, the user, the rule of Section 1; Restart or Close | |

Rules the screens apply (`install-admin` checks them again):

- **Disks offered**: not used by the running system (any mount source, boot
  slot, `/boot/efi`, active swap or loop backing file, traced to its disk),
  not read-only, not file-backed, and at least the size of the smallest
  installation.
- **Choose partitions** is refused on a disk that is not GPT; whose GPT
  `diskpart` cannot edit (backup header not at the last LBA, entries not 128
  bytes, header not 92 bytes, hybrid MBR, damaged copy); with 16 used entries;
  with `/zedbsd.cfg` at the root of any FAT on it (another zedBSD), counting a
  FAT that cannot be mounted read-only as having one; or whose ESP is not
  sufficient.
- **Sufficient ESP**: an ESP type FAT16 or FAT32 with a nonzero serial, free
  space of at least the boot files plus 16 MiB, and no `zedbsd.cfg` or
  `\EFI\ZEDBSD\BOOTX64.EFI` already there. A new ESP is 512 MiB. The first
  sufficient ESP by slot is used.
- **Edit**: Create puts a partition of the given size (default: the largest
  free space) in the first free space that holds it, aligned to 1 MiB, with
  type `zedBSD-unassigned` and no name, and zeroes its first and last MiB.
  Without an ESP it also makes a 512 MiB ESP in front of it, in one table write,
  and formats it; if either cannot be done, the new entries are removed in one
  write. A write is refused while a partition of the disk stays mounted or the
  kernel cannot reload the table. After any write the screens show the table
  read back from the disk, and earlier Root, Swap and Confirm choices are
  cleared.
- **Root size**: at least 1.1 times the unpacked size of the chosen units,
  plus the swap file, plus 1 GiB; and enough inodes for 1.2 times their file
  count, as `mkfs -t ufs --profile=native --check-size` reports.
- **Whole disk**: 512 MiB ESP, a swap partition the size of the memory between
  1 GiB and 8 GiB (1 GiB on a disk under 32 GiB), the UFS root on the rest.
- **User**: login name of a lower-case letter followed by lower-case letters,
  digits, `-` and `_`, at most 32 bytes, not `root` or a system account;
  display name at most 64 bytes; passwords not empty and repeated; hostname of
  lower-case letters, digits and `-`, at most 63 bytes. Automatic login is
  offered only with the desktop.

The text is translated through the `installer` domain and follows the
desktop's language.

## 4. install-admin

### 4.1 Program

```text
/usr/libexec/install-admin probe
/usr/libexec/install-admin session
```

Mode 4550, owner root, group wheel; only in the installer's image. Before
anything it:

1. requires effective uid 0;
2. requires the caller to be root, or to be in the wheel group with the real
   uid of the user named in `/etc/keiland/autologin`;
3. reads `/opt/installer/index` once and requires its SHA-256 to be the one
   it was built with (it does nothing on any other image);
4. becomes root wholly (`setresuid`, `setresgid`, a new session), clears the
   environment, closes other descriptors, resets the umask and limits, and
   ignores SIGPIPE, SIGHUP, SIGINT and SIGTERM, so the caller cannot stop it or
   its children half way;
5. takes `/run/install-admin/lock` (directory mode 0700, made without
   following symbolic links). A second helper is refused while one holds it.

It asks for no password. Every request is logged to syslog `auth`, without
the passwords.

| Exit | Meaning |
| --- | --- |
| 0 | done |
| 1 | failure |
| 2 | usage |
| 3 | refused: not the image's user, or not the installer's image |
| 4 | busy: another helper holds the lock |

### 4.2 probe

Reads and changes nothing, then prints one record a line, fields separated by
TAB, ended by `end`. An empty field is `-`; TABs and control characters in
names and labels become `?`.

```text
firmware uefi|other
memory   BYTES
disk     NAME REGISTRATION KIND SECTOR-BYTES SECTORS FLAGS LIVE TABLE RESTRICTIONS ENTRIES DIGEST
part     DISK SLOT START SECTORS TYPE PARTUUID ATTRIBUTES DEVICE FSTYPE FSUUID MOUNTED SWAP NAME FSLABEL
fat      DISK SLOT FREE-BYTES CFG BOOT-LOADER ZEDBSD-LOADER
end
```

`firmware` is `uefi` when `kern.boot.firmware_partition` names a partition.
KIND is `nvme`, `removable` or `fixed`. LIVE is 1 for a disk the running
system uses. TABLE is `gpt`, `mbr` or `none`. DIGEST is the SHA-256 of the
partition table as read, which writing requests quote back. CFG and the
loaders are `yes`, `no` or `unknown`; each FAT is mounted read-only on
`/run/install-admin/probe` to look, and one that cannot be is `unknown`.

### 4.3 session

Reads requests from standard input, one line each, tokens separated by one
space, until `cancel` or end of input, and reports on standard output.

```text
probe
cancel
edit delete DISK part TUPLE
edit create DISK new START COUNT
edit create-with-esp DISK esp START COUNT new START COUNT
install DISK LAYOUT units N NAME... user FIELD fullname FIELD
        password FIELD root-password FIELD autologin yes|no hostname FIELD
```

- DISK is `disk REGISTRATION NAME SECTOR-BYTES SECTORS DIGEST`, the disk as
  the screen showed it.
- LAYOUT is `whole ESP-BYTES SWAP-BYTES` (no swap partition when SWAP-BYTES is
  0) or `chosen esp TUPLE root TUPLE swap TUPLE|none`.
- TUPLE is `REGISTRATION:START:SECTORS:PARTUUID`: the disk's registration,
  the partition's first sector and size in sectors, and its PARTUUID. A slot
  number or device name is never enough, since a delete and an add can reuse
  them for another range.
- FIELD, text a person typed, is `LENGTH:BYTES`, the decimal byte count, a
  colon and exactly that many bytes.
- A request is at most 16384 bytes.

The first writing request binds the session to its disk; a request about
another disk is refused. While a session holds the lock, `probe` is asked of
the session.

Output, one line each:

| Line | Meaning |
| --- | --- |
| `STEP N TOTAL NAME` | a step begins |
| `BYTES DONE TOTAL` | bytes unpacked so far |
| `UNIT NAME` | the unit being unpacked |
| `STATE TEXT` | what the disk holds now (for example `table written`, `formatted`, `reload-failed`, `loader EFI/BOOT`, `swap file`) |
| `DONE` | the request is done |
| `FAIL STEP ERRNO TEXT` | the request failed at that step |

`cancel`, or the end of input, is taken at the end of a step or between files
of a unit; a table write or a format is never left half done.

Before writing, every request is checked again against the disk as it is now:
the registration, size and table digest must match what the screen showed, the
disk must not be one the running system uses, partitions must not overlap or
leave the disk, untouched entries must not change, every tuple must be in the
current table, and the units must be in the index. Before each zeroing,
`mkfs` and `mkswap`, the partition's device is checked to be that tuple and the
kernel's table to match the disk's. Mounts of the disk's partitions are
removed before each child program; one that cannot be removed fails the step.
Confirmation phrases (`WRITE NAME:REGISTRATION`, `FORMAT NAME:REGISTRATION`)
are made from the identity just read, not from a child's output.

### 4.4 install steps

| # | Step | Work |
| --- | --- | --- |
| 1 | `check` | check the plan against the disk |
| 2 | `claim` | unmount the disk's partitions |
| 3 | `table` | whole disk: `diskpart init` with new random disk GUID and PARTUUIDs. Chosen: `diskpart batch` sets the root's and swap's type, name and a new PARTUUID, and clears their attributes |
| 4 | `format` | zero each new partition's first and last MiB; `mkfs -t fat32` a new ESP, `mkfs -t ufs --profile=native` the root, `mkswap` the swap partition and read its header back |
| 5 | `mount` | mount the root on `/run/install-admin/target`, `nosuid`, `nodev` |
| 6 | `unpack` | each unit in dependency order, checking its SHA-256 from the same descriptor it is read from; files opened component by component without following symbolic links; then the kernel and the logo to `/boot`, checked against the index |
| 7 | `settings` | accounts, hostname, swap file, sshd, records |
| 8 | `sync` | unmount and synchronize the root |
| 9 | `esp` | mount the ESP, write the loader, then `zedbsd.cfg` under a temporary name and rename it |
| 10 | `finish` | unmount the ESP; write the boot entry (Section 6) |

A failure stops at its step with nothing left mounted. Nothing is rolled back.
Because `zedbsd.cfg` is written last, a target whose step 9 did not finish has
no configuration and does not boot.

The `settings` step:

- replaces the base's default user (uid 1000) with the chosen user, renaming
  the user's group (gid 1000) and its membership of `wheel` and `network`, and
  `/home`'s owner; sets the user's and root's passwords (the base's fixed
  password hashes never remain);
- writes `/etc/keiland/autologin` (the name, or empty);
- sets `hostname` in `/etc/rc.conf`;
- without a swap partition, writes a zero-filled `/swapfile` of
  min(memory, 2 GiB − 4 KiB), formats it with `mkswap`, and tries `swapon` and
  `swapoff` on it with a 30-second limit. If the kernel accepts it,
  `/etc/fstab` gets `/swapfile none swap sw 0 0`; otherwise the file is
  removed and the system has no swap (`STATE swap none`). If `swapoff` fails,
  the installation stops, as the running system is using the target;
- with OpenSSH, makes every `PermitRootLogin` line of `sshd_config`
  `PermitRootLogin no`;
- records the installed units and versions in `/var/db/installer/units` and
  each unit's files in `/var/db/installer/UNIT.files`.

Settings of the running system are not copied to the target.

## 5. The installed system

| Partition | Type GUID | Name |
| --- | --- | --- |
| ESP | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` | `EFI System` |
| root (UFS) | `516E7CB6-6ECF-11D6-8FF8-00022D09712B` | `zedBSD installed root` |
| swap | `516E7CB5-6ECF-11D6-8FF8-00022D09712B` | `zedBSD installed swap` |
| made on the Edit screen, not yet root or swap | `7EF6A3E2-F243-445D-B77A-C37CA9605418` (`zedBSD-unassigned`, [`<uapi/gpt-type.h>`](../../include/uapi/gpt-type.h)) | none |

The names differ from the ordinary image's `zedBSD-root` and `zedBSD-swap`, and
the configuration selects by PARTUUID, so neither image nor installed disk
makes the other's selection ambiguous.

The loader goes to `\EFI\BOOT\BOOTX64.EFI`, or to `\EFI\ZEDBSD\BOOTX64.EFI`
when the former exists (it is never overwritten). The ESP's root holds
`zedbsd.cfg`:

```text
kernel=rootpart:/boot/vmunix
rootpart=PARTUUID=ROOT
swap0=PARTUUID=SWAP          (with a swap partition)
logo=rootpart:/boot/logo.ppm
login=graphical              (with the desktop)
kmsg=quiet
```

There is no `boot0=`: the loader adds `boot0=UUID=` from the ESP's random
volume serial. The kernel and the logo are in the root's `/boot`; the loader
reads them from UFS ([parameters](kernel-boot-parameters.md#9-handoff-and-acceptance-requirements)).
How to replace the kernel is in the
[how-to](../howto/boot-and-storage.md#replacing-the-kernel-of-an-installed-system).

## 6. efiboot

`efiboot` edits the UEFI boot entries (`Boot####` and `BootOrder`) through
`/dev/system`. Changes need root.

```text
efiboot list
efiboot create [-l LABEL] [-n] DISK PARTITION LOADER-PATH
efiboot delete NUMBER
efiboot order NUMBER[,NUMBER...]
efiboot next NUMBER
efiboot prune [-l LABEL]
```

- `list` prints `BootCurrent`, `BootNext`, `BootOrder` and each entry's
  number, an `*` when it is active, its label and its device path.
- `create` adds an entry labelled `LABEL` (`zedBSD` by default) whose device
  path is the GPT partition's `HD(number, GPT, PARTUUID, start, size)`
  followed by `File(LOADER-PATH)`, appends it to the end of `BootOrder`, and
  prints its number (`Boot####`). An existing entry with the same device path
  is reused and appended if missing. `-n` also sets `BootNext` to it.
- `delete` removes the entry and its number from `BootOrder`.
- `order` replaces `BootOrder`; `next` sets `BootNext`.
- `prune` removes every entry labelled `LABEL` (`zedBSD` by default) whose
  device path names a GPT partition that no present disk has, and prints the
  number of each one it removed. When a disk cannot be read it removes
  nothing.

It is built for amd64 and fails with `EOPNOTSUPP` ("Operation not
supported") where the system was not started through UEFI runtime services
(a BIOS boot, or a loader that did not hand them over).

The installer's `finish` step creates an entry labelled with the system's
name (`zedBSD`) for the loader it wrote, then prunes the entries of that
label whose partition is on no present disk. Failing to write the entry is
reported but does not fail the installation; the installed disk can still be
chosen in the firmware's boot menu.

## 7. Source

| Part | Source |
| --- | --- |
| Installer | [`userland/desktop/installer/`](../../userland/desktop/installer/) |
| install-admin | [`userland/desktop/installer/admin/`](../../userland/desktop/installer/admin/) |
| Image rules | [`platform/amd64/installer-image.mk`](../../platform/amd64/installer-image.mk), [`platform/amd64/installer-items`](../../platform/amd64/installer-items) |
| Manifest, units, index, live root | [`tools/build/`](../../tools/build/) (`installer-manifest.py`, `installer-units.py`, `installer-live.py`, `installer-root-size.py`) |
