#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""Make a PowerPC Mac disk image that Open Firmware boots: APM, HFS, UFS.

The disk is Apple-partitioned: the Driver Descriptor Map, then the
partition map (63 blocks), then

  2  Apple_Bootstrap "bootstrap"   an HFS volume Open Firmware reads
  3  Apple_UNIX_SVR2 "zedBSD-root" the UFS root (marked etc/zedbsd-root)
  4  Apple_UNIX_SVR2 "zedBSD-swap" swap, when asked for

Partition 1 is the map itself, as Open Firmware numbers them.  The HFS
volume holds the kernel as \\vmunix and the CHRP boot script
\\ppc\\bootinfo.txt, of Finder type 'tbxi' in the blessed folder \\ppc, so
the firmware finds it by "<disk>:2,\\\\:tbxi" (the default boot path of
New World Macs and of OpenBIOS) or "<disk>:2,\\ppc\\bootinfo.txt".  The
script boots the kernel from the same partition of the same disk with
"&device;:&partition;".

Open Firmware loads an ELF program's segments at their physical
addresses (p_paddr) when it is in real mode, at their virtual ones in
virtual mode; the kernel's own ELF has p_paddr zero-based for QEMU's
-kernel loader, so the copy on the disk gets p_paddr = p_vaddr, which is
right in both modes since the kernel runs where it is linked.

  make-macppc32-image.py --kernel vmunix (--root-tree DIR | --root-image IMG)
                         [--boot-mib 16] [--root-mib 64] [--swap-mib 0]
                         [--bootargs TEXT] --output IMAGE

Values referenced: Apple, Inside Macintosh: Devices (1994), chapter 3,
the Driver Descriptor Record and the partition map entry; IEEE Std
1275-1994 and its PowerPC binding (the CHRP boot script's <CHRP-BOOT>,
<BOOT-SCRIPT> and the &device; and &partition; substitutions, the ELF
loading by p_paddr in real mode); the System V ABI (the ELF32 header and
program headers).
"""

import argparse
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import hfs  # noqa: E402

SECTOR = 512
MAP_BLOCKS = 63
FIRST_PARTITION = 1 + MAP_BLOCKS

# Partition status: valid, allocated, in use, readable, writable; boot adds valid boot information.
STATUS_DATA = 0x00000037
STATUS_BOOT = 0x0000007F

PT_LOAD = 1


def ddm(total_sectors):
    """The Driver Descriptor Map: signature, block size, block count, no drivers."""
    block = bytearray(SECTOR)
    struct.pack_into(">2sHIHHIH", block, 0, b"ER", SECTOR, total_sectors, 0, 0, 0, 0)
    return bytes(block)


def map_entry(map_count, start, count, name, kind, status):
    """One partition map entry."""
    block = bytearray(SECTOR)
    struct.pack_into(">2sHIII32s32sIII", block, 0, b"PM", 0, map_count, start, count,
                     name.encode("ascii"), kind.encode("ascii"), 0, count, status)
    return bytes(block)


def kernel_for_firmware(path):
    """The kernel with each loadable segment's physical address set to its virtual one."""
    data = bytearray(Path(path).read_bytes())
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
        raise SystemExit(f"{path}: not a big-endian ELF32 file")
    phoff, = struct.unpack_from(">I", data, 28)
    phentsize, phnum = struct.unpack_from(">HH", data, 42)
    for index in range(phnum):
        at = phoff + index * phentsize
        kind, _, vaddr = struct.unpack_from(">III", data, at)
        if kind == PT_LOAD:
            struct.pack_into(">I", data, at + 12, vaddr)
    return bytes(data)


def bootinfo(bootargs):
    """The CHRP boot script: boot the kernel from the partition the script is on."""
    command = "boot &device;:&partition;,\\vmunix"
    if bootargs:
        command += " " + bootargs
    lines = [
        "<CHRP-BOOT>",
        "<COMPATIBLE>",
        "MacRISC MacRISC3 MacRISC4",
        "</COMPATIBLE>",
        "<DESCRIPTION>",
        "zedBSD",
        "</DESCRIPTION>",
        "<OS-NAME>",
        "zedBSD",
        "</OS-NAME>",
        "<BOOT-SCRIPT>",
        command,
        "</BOOT-SCRIPT>",
        "</CHRP-BOOT>",
        "",
    ]
    return "\n".join(lines).encode("ascii")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--root-tree")
    source.add_argument("--root-image")
    parser.add_argument("--boot-mib", type=int, default=16)
    parser.add_argument("--root-mib", type=int, default=64)
    parser.add_argument("--swap-mib", type=int, default=0)
    parser.add_argument("--bootargs", default="")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    # The dates are the build's, for a reproducible image.
    when = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))

    # The boot volume: the kernel at the root, the script in the blessed \ppc.
    top = hfs.Folder("zedBSD")
    ppc = hfs.Folder("ppc")
    top.children["vmunix"] = hfs.File("vmunix", kernel_for_firmware(args.kernel))
    top.children["ppc"] = ppc
    ppc.children["bootinfo.txt"] = hfs.File("bootinfo.txt", bootinfo(args.bootargs), b"tbxi", b"chrp")
    boot = hfs.make_volume(args.boot_mib * 1024 * 1024, top, "zedBSD", blessed=ppc, when=when)

    # The root: a UFS image as given, or one made from a tree.
    if args.root_image:
        root = Path(args.root_image).read_bytes()
    else:
        from ufs_format import create  # noqa: E402
        root = create(args.root_mib * 1024 * 1024, args.root_tree)
    if len(root) % SECTOR:
        raise SystemExit("root image is not whole sectors")
    swap = b"\0" * (args.swap_mib * 1024 * 1024)

    # The partitions, one after another after the map.
    parts = [("bootstrap", "Apple_Bootstrap", STATUS_BOOT, boot),
             ("zedBSD-root", "Apple_UNIX_SVR2", STATUS_DATA, root)]
    if swap:
        parts.append(("zedBSD-swap", "Apple_UNIX_SVR2", STATUS_DATA, swap))
    map_count = 1 + len(parts)
    total = FIRST_PARTITION + sum(len(p[3]) // SECTOR for p in parts)

    # The image, written in place and published by a rename.
    output = Path(args.output)
    temporary = output.with_name(output.name + ".tmp")
    with open(temporary, "wb") as handle:
        handle.write(ddm(total))
        handle.write(map_entry(map_count, 1, MAP_BLOCKS, "Apple", "Apple_partition_map", STATUS_DATA))
        start = FIRST_PARTITION
        places = []
        for name, kind, status, data in parts:
            count = len(data) // SECTOR
            handle.write(map_entry(map_count, start, count, name, kind, status))
            places.append((start, data))
            start += count
        for start, data in places:
            handle.seek(start * SECTOR)
            handle.write(data)
        handle.truncate(total * SECTOR)
    temporary.replace(output)
    print(f"{output}: {total} sectors; " +
          ", ".join(f"{index + 2}={name}@{places[index][0]}" for index, (name, _, _, _) in enumerate(parts)))


if __name__ == "__main__":
    main()
