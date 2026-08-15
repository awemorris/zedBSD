# PC-98 DOS zedBSD disk installer

`INST.EXE` provides three installation operations. It expects
`IPL-LBA0.IMG`, `IPL-LBA2.IMG`, `IPL-PART.IMG`, and `IO.SYS` beside the
executable:

```dos
INST /LBA0
INST /LBA2
INST /PART C:
```

The first two commands operate on the physical IDE disk containing the
current DOS drive. `/PART` accepts an explicit DOS drive, copies `IO.SYS` as
a normal hidden/system/read-only FAT16 file, verifies that its FAT chain is
contiguous, and writes the 1024-byte partition PBR while preserving the BPB.
The target must already be a FAT16 partition named `BOOT`, with one 1024-byte
reserved logical sector and identical IPL/data-start CHS values. `INST.EXE`
does not create a partition table, format FAT, copy the zedBSD kernel, or
erase another partition. Every raw PBR sector is read back after writing.

The reserved 1024 bytes and `IO.SYS` are separate objects. The reserved
logical sector is outside the FAT cluster area. No prefix of `IO.SYS` is
copied there; every byte of `IO.SYS` remains in its ordinary FAT cluster
chain. The contiguity requirement is the traditional DOS system-file rule.

Generated BOOT partitions contain the complete reusable installer kit in
their `INST` directory: `INST.EXE`, `IO.SYS`, `IPL-LBA0.IMG`, `IPL-LBA2.IMG`,
and `IPL-PART.IMG`. The root also retains the active boot copy of `IO.SYS`.
The PBR is deliberately kept as `IPL-PART.IMG`, rather than embedded in
`INST.EXE`, so it can be inspected, replaced, and installed by non-DOS
tooling.

Run the installer from the `INST` directory under plain real-mode DOS.
Maintainers can rebuild it with OpenWatcom 1.9 by running `make` in this
directory.
