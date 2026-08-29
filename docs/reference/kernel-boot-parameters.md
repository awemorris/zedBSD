# Kernel boot parameters

Status: implemented for the four x86 production-loader paths

This document defines the implemented textual kernel-parameter contract for
selecting boot filesystems, the root mode, swap sources, and PID 1. The common
parser and kernel consumers are used by these production paths:

- i386 PC/AT;
- i386 PC-98;
- amd64 legacy BIOS; and
- amd64 UEFI.

## 1. Implementation scope

The four x86 loaders pass one bounded parameter string through their
architecture handoff. The HAL copies it into kernel-owned storage before
temporary loader memory can be released, and the common parser implements
`boot0=` through `boot3=`, `rootpart=`, `overlay-root=`,
`overlay-data=`, `swap0=` through `swap3=`, and `init=`.

The implemented VFS path provides four private boot-filesystem slots, explicit
native-root versus overlay-root selection, configurable image paths, file and
raw-partition swap, and atomic activation of up to four swap sources. The old
`boot=` and `root=` names are not compatibility aliases. The singular
`swap=` name is not introduced.

This strict contract applies to every explicit parameter source on every
architecture. A separate, narrowly bounded compatibility path exists only
when a non-x86 architecture supplies a NULL parameter-source pointer; Section
9.1 documents that boundary.

## 2. Text format

The parameter string is a sequence of ASCII `name=value` tokens separated by
one or more ASCII spaces. The maximum complete string is 3071 bytes excluding
the terminating NUL.

- Names and values are case-sensitive unless the selected filesystem or block
  identity format says otherwise.
- Quoting, escapes, embedded whitespace, and empty values are not supported in
  the initial contract.
- A known name may occur at most once. A duplicate known name is an error.
- Unknown names are ignored with a bounded diagnostic so future independent
  parameters do not make an older kernel unbootable.
- A malformed known parameter or an invalid combination stops root
  initialization with a visible error.

The complete initial name set is:

```text
boot0= boot1= boot2= boot3=
rootpart=
overlay-root= overlay-data=
swap0= swap1= swap2= swap3=
init=
```

## 3. Block-device selectors

`bootN=`, `rootpart=`, and the block-device form of `swapN=` accept the common
block selector grammar:

```text
/dev/sda1
sda1
UUID=6740-911D
LABEL=ZEDBOOT
PARTUUID=...
PARTLABEL=...
```

`/dev/NAME` is parsed as an early-boot device selector; the kernel does not
open a devfs pathname before devfs exists. UUID, label, PARTUUID, and PARTLABEL
matching is case-insensitive where the current block-identity resolver already
defines it that way. No match is an error. More than one match is an ambiguous
selector error.

## 4. Boot filesystem slots

`boot0=` through `boot3=` name up to four FAT16 or FAT32 filesystems whose
files may be used by root and swap parameters.

```text
boot0=UUID=6740-911D
boot1=/dev/sdb1
```

The slots are independent names, not an overlay order.

- If `boot0=` is omitted, `boot0` denotes the loader-origin boot partition
  identified by the architecture handoff.
- `boot1` through `boot3` have no implicit value.
- Sparse definitions are valid; for example, `boot0` and `boot3` may be
  defined without `boot1` or `boot2`.
- A `bootN:` file reference to an undefined slot is an error.
- Two slots resolving to the same partition are an error.
- A resolved initial boot slot must contain a supported FAT16 or FAT32
  filesystem. Other boot-filesystem types require a later contract revision.

A file reference has this form:

```text
boot0:rootfs.img
boot1:/images/data-work.img
```

The path is rooted at the selected boot filesystem. One leading `/` after the
colon is optional and normalized away. An empty path, `.` or `..` component,
backslash, control character, or path of 256 bytes or more is rejected. FAT
case matching follows the mounted FAT implementation.

Boot filesystems are mounted only in the kernel's private boot namespace for
these lookups. This contract does not itself require exposing them at `/boot`.

## 5. Root modes

Exactly one of `rootpart=` and `overlay-root=` must be effective after loader
defaults are applied.

### 5.1 Native root partition

`rootpart=` selects one filesystem-bearing block device and disables the
overlay-root path.

```text
rootpart=UUID=0123456789ABCDEF
```

In this mode:

- `overlay-root=` is forbidden;
- `overlay-data=` is forbidden;
- boot slots may still be used by `swapN=bootM:FILE`; and
- the selected root filesystem is mounted directly as `/`.

### 5.2 Overlay root

`overlay-root=` enables the overlay-root path and names its immutable lower
image. `overlay-data=` names the writable upper image.

```text
overlay-root=boot0:rootfs.img
overlay-data=boot0:data.img
```

Both parameters are required in the initial overlay mode.

- `overlay-root=` and `overlay-data=` accept only `bootN:PATH` references.
- The lower image is attached read-only.
- The upper image is attached read-write.
- Both files must be regular files and must not resolve to the same file.
- Existing loop-recursion and nested-overlay rejection remains mandatory.
- `overlay-data=` without `overlay-root=` is an error.
- `overlay-root=` without `overlay-data=` is an error; an ephemeral or
  read-only upper is not implied.
- `rootpart=` together with either overlay parameter is an error.

The overlay is enabled only by `overlay-root=`. The kernel no longer silently
selects an overlay merely because files named `rootfs.img` and `data.img`
exist.

To preserve the current generated-image behavior before Boot CPAR supplies a
menu selection, each of the four x86 production loaders supplies this default
root selection when it has no explicit replacement:

```text
overlay-root=boot0:rootfs.img overlay-data=boot0:data.img \
swap0=boot0:swapfile
```

`boot0` in this default is the loader-origin boot filesystem. `init=` is
omitted so its kernel default applies.

## 6. Swap sources

`swap0=` through `swap3=` select zero to four swap sources. Each value is
either a block-device selector or a file on a boot filesystem.

```text
swap0=boot0:swapfile
swap1=/dev/sda5
swap2=UUID=89ABCDEF01234567
swap3=boot1:swap-extra
```

- Sparse swap numbers are valid.
- A physical partition or file may appear in at most one swap slot.
- A raw swap partition must not overlap the selected `rootpart=` on the same
  physical leaf disk. This is rejected before the aggregate backend is
  published; a FAT swap file on the native-root filesystem remains valid.
- Every source must be writable and contain a valid zedBSD swap header at
  offset zero. An arbitrary partition is never accepted merely because the
  user named it.
- The first page is reserved for the swap header. Page slots begin at the
  second 4096-byte page.
- Existing `ZEDSWAP1` 32/64-MiB files remain readable. The implemented
  `ZEDSWAP2` header carries 64-bit capacity fields so larger files and raw
  partitions can be represented without truncation.
- The initial file-backed implementation accepts FAT files with at most 1024
  physical extents. A more fragmented file is rejected during atomic source
  preparation; it is never activated partially.
- Source validation is atomic: if any explicitly selected swap source is
  missing, ambiguous, read-only, malformed, duplicated, or cannot be opened,
  no selected swap source is activated and boot fails visibly.
- With no `swapN=` parameters, the kernel runs without swap. The generated
  legacy-layout loader default above explicitly supplies `swap0=`.

The active sources form one system swap pool. Allocation is deterministic:
the kernel fills available slots in `swap0`, then `swap1`, `swap2`, and
`swap3`. Freeing a page returns it to its original source. Statistics report
the aggregate total and free page counts. Flush and shutdown visit every
active source in numeric order and preserve the first error while still
attempting the remaining sources.

An I/O error after activation is reported for the affected page and source;
the kernel does not silently reinterpret that page as belonging to another
source.

The initial contract is boot-time activation only. A future runtime
`swapon`/`swapoff` facility must add a filesystem-wide backing-object claim
shared by ordinary write/truncate and loop attachment, prevent writable
aliases through a second mount of the same disk, and recalculate VM commit
limits when capacity changes. Those runtime synchronization rules are not
implicitly provided by the serialized private boot-filesystem path.

## 7. Init selection

`init=` selects the executable started as PID 1.

```text
init=/sbin/init
init=/bin/sh
```

- The value must be an absolute path shorter than 256 bytes.
- Arguments and environment assignments are not part of `init=`.
- If omitted, the value is `/sbin/init`.
- `init=/bin/sh` is the explicit rescue/single-process boot mechanism; no
  separate single-user-mode parameter is introduced.
- Failure to validate, open, or execute the selected path is reported and
  leaves the kernel in its diagnosable idle path. It does not silently select
  a different init.

The selection and validation code is architecture-independent. An explicit
parameter source without `init=` obtains the `/sbin/init` default. The
compile-time init paths retained by the non-x86 NULL-source compatibility path
are documented in Section 9.1.

## 8. Valid and invalid examples

Current-layout overlay root using loader-origin FAT:

```text
overlay-root=boot0:rootfs.img overlay-data=boot0:data.img \
swap0=boot0:swapfile
```

Native root plus two swap sources:

```text
rootpart=PARTUUID=01234567-01 swap0=/dev/sda5 \
swap1=boot0:swapfile init=/sbin/init
```

Root and data images on different boot filesystems:

```text
boot1=UUID=1111-2222 overlay-root=boot0:rootfs.img \
overlay-data=boot1:data-work.img swap0=boot1:swapfile
```

The following are errors:

```text
rootpart=/dev/sda2 overlay-root=boot0:rootfs.img
overlay-data=boot0:data.img
overlay-root=boot2:rootfs.img overlay-data=boot0:data.img
boot1=UUID=1111-2222 boot2=UUID=1111-2222
swap0=/dev/sda5 swap1=UUID=<the same sda5 UUID>
init=bin/sh
```

The third example fails because `boot2` is undefined.

## 9. Handoff and acceptance requirements

The bootloader-to-HAL handoff carries one bounded, NUL-terminated parameter
string. The HAL copies it into kernel-owned storage before temporary loader
memory can be unmapped. No CPAR-specific binary handoff is added.

Each of the four x86 production loaders materializes the generated-image
default shown in Section 5.2 when its loader input is absent or empty. A
nonempty recognized loader parameter source is the complete final parameter
string and replaces that default; it is not merged token by token.
Consequently, an explicit override must include one complete root mode. This
keeps precedence identical across Multiboot, custom BIOS handoffs, and
recognized UEFI text `LoadOptions` and prevents hidden duplicate-key
resolution.

UEFI defines `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions` as an opaque,
length-delimited binary buffer sourced from a boot option's `OptionalData`; it
does not require that firmware terminate it as text. zedBSD recognizes a
bounded printable-ASCII `CHAR16` buffer, with or without a final NUL, only when
its first token begins with a known zedBSD parameter name. Empty or
unrecognized options use the image default. Some x86 firmware passes the
complete packed `EFI_LOAD_OPTION` descriptor instead of `OptionalData`; the
loader validates every description and Device Path boundary before extracting
that descriptor's `OptionalData`. Any other firmware-specific binary form is
diagnosed as ignored and never prevents removable-media boot. All multi-byte
fields are decoded bytewise because neither descriptor nor `OptionalData`
requires natural alignment.

All four x86 paths use the same common parser and observable semantics:

| Platform | Required transport and runtime result |
| --- | --- |
| i386 PC/AT | Loader/Multiboot input reaches the common parser and boots both overlay and `rootpart` modes |
| i386 PC-98 | Versioned loader handoff reaches the common parser and boots both root modes |
| amd64 BIOS | Versioned ZBL6 handoff reaches the common parser and boots both root modes |
| amd64 UEFI | Bounded UEFI/loader text reaches the common parser and boots both root modes |

### 9.1 Non-x86 NULL-source compatibility boundary

NULL at the kernel parameter-source interface is not the same condition as an
absent x86 loader option. The x86 HALs convert absent loader input to the
generated default before the kernel parser runs. On i386 and amd64, the kernel
therefore never invokes legacy automatic root selection.

Only a non-x86 build receiving a NULL parameter-source pointer uses the
retained legacy automatic-root path:

1. begin with the loader-origin boot partition;
2. inspect sibling partitions on that same physical disk for exactly one UFS1
   filesystem containing `/etc/zedbsd-root` with the exact marker
   `zedBSD ufs1 root v1\n`, rejecting ambiguity;
3. on ARM64, if no marked UFS1 partition exists, try `/rootfs.img` (then
   `/rootfs.rp4`) together with `/data.img` using the legacy overlay path;
4. otherwise mount the uniquely marked UFS1 partition, or the loader-origin
   boot partition when no marker is present, directly as root.

That compatibility path does not interpret the new `bootN=`, root-mode, or
`swapN=` contract and does not activate a new swap source. It retains the
architecture build's `ZEDBSD_INIT_PATH`: normally `/sbin/init`,
`/bin/sh` for sparcv9, and `/x68k/bin/sh` for x68k. Any non-NULL parameter
source, including on a non-x86 architecture, uses the strict common contract;
a non-NULL empty source does not enter legacy automatic root selection.

## 10. Verification contract

The implementation is divided into focused production-code gates:

- BR-T42 covers bounded parsing, owned storage, the complete key set,
  duplicates, malformed input, and `init=`;
- BR-T43 covers the shared record, all four x86 handoff layouts, and UEFI
  `LoadOptions` conversion;
- BR-T44 covers selectors, private boot slots, aliases, root-mode selection,
  path validation, and reverse-order failure unwind;
- BR-T45 covers `ZEDSWAP1`, `ZEDSWAP2`, four-source aggregation,
  allocation/free, first-error flush, and shutdown; and
- BR-T46 is the production-loader QEMU acceptance gate.

BR-T46 acceptance requires one fresh 31-cell matrix: six common cases on each
of i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64 UEFI (24 cells); mixed
file/raw swap, UUID disk reordering, and PARTUUID disk reordering on both
amd64 firmware paths (six cells); and one PC/AT native-root/raw-swap alias
rejection cell. The six common cases cover the generated default and normal
login, `init=/bin/sh`, native root, file swap, raw swap, and visible
rejection of invalid input.

Every positive swap cell must force at least 1024 pages (4 MiB) to page out,
read all touched anonymous pages back with their contents intact, and observe
a positive page-in counter. The alias-rejection cell must fail before swap
publication, root mount, or init. The UUID and PARTUUID reordering cells must
enumerate another disk first while still selecting the production boot image,
so loader-origin `boot0` and explicit secondary-slot resolution are both
proved. Artifacts, generated configuration, hashes, commands, guest logs, and
the result table must be preserved from that fresh run.
