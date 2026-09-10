# Filesystem and swap formatters

Status: current; command/native amd64 acceptance and amd64/PCAT/PC98 builds.

`mkfs` and `mkswap` have image modes for an existing, pre-sized regular file.
`mkfs` also has explicit FAT32 and native-UFS block-device modes used by the
installer. Neither command partitions disks or activates a filesystem/swap. The maintained
filesystem name is `ufs`, the single 64-bit implementation; `ufs1` and `ufs2`
are rejected command arguments.

```sh
mkfs -t ufs FILE
mkfs -t ufs --profile=journal-snapshot FILE
mkswap FILE
```

Formatting obtains the kernel's exclusive file-format reservation, checks the
selected identity and size, writes deterministic metadata, flushes, reopens and
verifies before reporting success. Active/mounted or otherwise conflicting
backing ownership prevents formatting. The unused UFS data area and swap slots
are not generally erased; start with zero-backed files when an exact pristine
image is required.

The regular-file UFS profile accepts fragment-aligned sizes from 4,194,304 through 2,147,482,624 bytes.
The optional profile reserves journal/snapshot records within that same size.
Swap emits ZEDSWAP2, requires 4,096-byte alignment and at least two pages, and
rejects sizes above INT32_MAX. The header occupies the first page.

## Read-only pristine verification

```sh
mkfs -t ufs --verify-pristine FILE
mkfs -t ufs --profile=journal-snapshot --verify-pristine FILE
mkswap --verify-pristine FILE
```

UFS options may occur in either order before the filename, each only once.
A pathname beginning with `-` can be written as `./-name`.

This mode checks exact initial bytes, including all unused data, reserved gaps
and trailing bytes. It does not test general filesystem consistency or accept
a normally used filesystem merely because it is healthy. UFS compares all
deterministic formatter extents and checks their complement for zero; swap
compares its canonical header and every unused slot byte.

The command opens a reader without following the final symlink, rejects special
files and multiply linked objects, and checks descriptor/path identity and size
before and after reading. It performs no write, format reservation, truncation,
flush or activation. Success means the checked observations matched; it is not
a lock against a concurrent writer. An installer must revalidate its selected
files at publication and refuse differing existing content.

Exit status is 0 after successful verification/initialization and close, 1 on
an object, I/O or content error, and 2 for unsupported arguments. Successful
read-only output says `pristine`; normal formatting says `initialized`.

Implementation: `userland/base/common/format-file.c`,
`userland/base/mkfs/`, `userland/base/mkswap/`.
Acceptance: [WS019-p019](../../plan/ws019-installation/phase019-pristine-verification/phase.md).

## Block-device modes

The dedicated installer uses these explicit destructive modes on its newly
created, inactive partitions:

```sh
mkfs -t fat32 DEVICE
mkfs -t ufs --profile=native DEVICE
```

`DEVICE` names a canonical block device, optionally prefixed by `/dev/`.
The command obtains block geometry and exclusive administration ownership,
prints the identity/size, then requires the exact displayed
`FORMAT NAME:REGISTRATION` phrase. It writes and verifies filesystem metadata.
Failure after formatting begins may leave a partially formatted volume; this
is not secure erasure. It does not copy a bootable root tree or install a loader.
Use the [installer](../howto/boot-and-storage.md) for that complete workflow.

Native UFS uses geometry sized for the volume and is not limited by the
regular-image profile's 2-GiB ceiling. Its representable size also depends on
file offsets and filesystem geometry. The following queries do not open or
modify a device; they check geometry and report usable capacity:

```sh
mkfs -t fat32 --check-size BYTES
mkfs -t ufs --profile=native --check-size BYTES
```

The regular-file `--verify-pristine` command is not a general checker for an
installed native filesystem. FAT32 block formatting and native UFS are owned
by `userland/base/mkfs/block-command.c`, `fat32-format.c` and `ufs-format.c`;
acceptance is recorded in
[WS019 native integration](../../plan/ws019-installation/phase049-native-installer-integration/phase.md).
