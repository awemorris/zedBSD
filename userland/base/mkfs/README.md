# UFS formatter ownership

The disk definitions, endian codec and superblock decoder in this directory
were copied from the zedBSD UFS driver during WS025 p031 (2026-09-08).
They are now maintained as userland code under their original Zlib license.
No source or private header from src/drivers is required to build mkfs.

ufs-format.c and the codec use ordinary host C/POSIX interfaces and are checked
against the maintained image generator, including fault injection and byte equality.
Keep format changes compatible with the kernel decoder through those tests;
do not reintroduce a kernel-source build dependency.

The command frontend also uses userland/base/common/format-file.c/.h and the
public zedbsd/fcntl.h interface. The zedBSD frontend requires its exclusive
format-reservation ioctl before writing. A port to another OS must supply an
appropriate host reservation frontend; compiling the portable codec does not
provide that OS-specific reservation service. Kernel implementation sources
are unnecessary in either case.

## FAT32 initializer

`fat32-format.c/.h` implements portable FAT32 geometry and metadata generation
for an already exclusively admitted descriptor. It supports sector sizes from
512 through 4096 bytes and clusters up to 32 KiB, refusing geometries outside
its FAT32 and host file-offset limits. It writes only reserved sectors, two
FATs and the empty root cluster. Unallocated data is preserved, not securely
erased. Metadata is flushed before backup and primary boot records are
published; this ordering does not promise atomic formatting or recovery of an
old filesystem after failure. Callers must check write, verify and close.

The independent readback mode compares all generated metadata and root bytes;
it is a freshly formatted volume check, not a filesystem consistency checker.
The codec does not open devices or supply mutation authorization. A block
frontend must retain BLKRESERVE through write, verification and final close.
`mkfs -t fat32 DEVICE` accepts a physical disk or direct partition with a
512-byte logical sector size, using one BLKRESERVE description. The zedBSD FAT
driver currently mounts 512-byte sectors only, despite the portable codec's
broader geometry support. Existing UFS regular-file syntax remains separate.

The command displays a destruction warning and requires the exact
`FORMAT NAME:REGISTRATION` phrase followed by a newline. Cancellation makes
no format writes. There is no force switch. An installer must first obtain
its explicit NO/YES confirmation and send only the identity it revalidated.
The descriptor stays reserved through metadata flush, readback and final
close; no partition-table reload is involved. A failure after writing starts
may leave partial media. The volume serial uses the current device registration
and is not a persistent boot selector; use the partition GUID for that purpose.

Format reference: [Microsoft FAT32 specification 1.03](https://www.pcjs.org/documents/papers/microsoft/MS_FAT_OVERVIEW_103-2000-12-06.pdf).
Tests independently inspect the on-disk fields and exercise mtools without
importing a production decoder.

## Native UFS codec

Capacity-only inspection is available before a partition exists:

```
mkfs -t fat32 --check-size 67108864
mkfs -t ufs --profile=native --check-size 268435456
```

These commands open no media and return one tab-separated record: `format`,
version `1`, profile (`fat32` or `ufs-native`), sector size `512`, requested
bytes, allocatable full-block bytes, free inodes, and allocation unit in bytes.
FAT reports zero free inodes because it has no fixed inode table. UFS excludes
its persistence tail, metadata, initial root and unusable partial blocks.
Check both bytes and inodes against the complete copy and swap requirements.
This query neither inspects an existing filesystem nor reserves a device;
normal formatting still requires identity revalidation and BLKRESERVE.

`ufs_format_native_validate_size`, `ufs_format_native_write` and
`ufs_format_native_verify` provide the dedicated-root initializer. The initial
namespace contains only root `.` and `..`, with no `.zovl*` files or marker.
The existing journal/snapshot persistence tail remains outside filesystem
allocation. Total fragments and free-space summaries use 64-bit arithmetic.
Geometry checks account for the caller's off_t, inode-number capacity,
per-group bitmaps and the final group's metadata room. Native metadata is
flushed before primary publication; formatting is not crash-atomic.

The historical 2-GiB bound still applies to the legacy overlay-image profile,
whose accepted byte output is unchanged. It does not limit the native codec.
Normal native generation/readback keeps fixed scratch space; it does not scan
or zero all free data. `mkfs -t ufs --profile=native DEVICE` shares the
block-command module with FAT32, retaining BLKRESERVE through confirmation,
write/readback and close. It requires the same exact FORMAT identity answer.
Byte-count multiplication is checked before the native codec is invoked.
Public native QEMU acceptance includes a 4-GiB partition and attribute/link
copy from a separate immutable UFS, compared after remount. A codec-only validation is not proof
that a particular kernel architecture supports the full geometry.
