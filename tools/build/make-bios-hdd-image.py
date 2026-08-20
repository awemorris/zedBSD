#!/usr/bin/env python3
"""Create a native zedBSD BIOS MBR/ZBL2/FAT16 disk image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import os
import re
import shutil
import struct
import subprocess
import tempfile
import uuid
import zlib
from pathlib import Path


SECTOR_SIZE = 512
PC98_SECTORS = 17
GPT_STAGE2_LBA = 34
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
BIOS_BOOT_TYPE = uuid.UUID("21686148-6449-6e6f-744e-656564454649")


def run(*arguments: str) -> None:
    subprocess.run(arguments, check=True)


def pc98_chs(lba: int, heads: int) -> bytes:
    cylinder, remainder = divmod(lba, heads * PC98_SECTORS)
    head, sector = divmod(remainder, PC98_SECTORS)
    if cylinder > 0xFFFF:
        raise SystemExit("partition exceeds the PC-98 CHS range")
    return bytes((sector, head)) + struct.pack("<H", cylinder)


def gpt_entry(type_guid: uuid.UUID, unique_guid: uuid.UUID, first: int,
              last: int, name: str) -> bytes:
    return (type_guid.bytes_le + unique_guid.bytes_le +
            struct.pack("<QQQ", first, last, 0) +
            name.encode("utf-16le")[:72].ljust(72, b"\0"))


def gpt_header(current: int, backup: int, entries_lba: int, first_usable: int,
               last_usable: int, disk_guid: uuid.UUID,
               entries_crc: int) -> bytes:
    header = bytearray(92)
    struct.pack_into("<8sIIIIQQQQ16sQIII", header, 0, b"EFI PART",
                     0x00010000, 92, 0, 0, current, backup, first_usable,
                     last_usable, disk_guid.bytes_le, entries_lba,
                     GPT_ENTRY_COUNT, GPT_ENTRY_SIZE, entries_crc)
    struct.pack_into("<I", header, 16, zlib.crc32(header) & 0xffffffff)
    return bytes(header).ljust(SECTOR_SIZE, b"\0")


def create(args: argparse.Namespace) -> None:
    bin_files = {}
    for specification in args.bin_file:
        if "=" not in specification:
            raise SystemExit("--bin-file requires NAME=SOURCE")
        name, source_text = specification.split("=", 1)
        source = Path(source_text)
        if not re.fullmatch(r"[a-z0-9_]{1,8}(?:\.[a-z0-9_]{1,3})?", name):
            raise SystemExit(f"invalid FAT16 /bin name: {name}")
        if name in bin_files:
            raise SystemExit(f"duplicate /bin name: {name}")
        if not source.is_file():
            raise SystemExit(f"missing input: {source}")
        bin_files[name] = source
    for path in (args.stage1, args.stage2, args.kernel):
        if not path.is_file():
            raise SystemExit(f"missing input: {path}")
    if args.gpt and (args.machine != "pcat" or args.bootx64 is None):
        raise SystemExit("--gpt requires --machine pcat and --bootx64")
    if args.bootx64 is not None and not args.bootx64.is_file():
        raise SystemExit(f"missing input: {args.bootx64}")
    if args.partition_pbr is None or args.bootzbsd is None:
        raise SystemExit("BIOS images require --partition-pbr and --bootzbsd")
    if not args.partition_pbr.is_file() or not args.bootzbsd.is_file():
        raise SystemExit("missing partition or system loader input")
    for path in (args.shell, args.noct, args.nettest, args.holoris):
        if path is not None and not path.is_file():
            raise SystemExit(f"missing input: {path}")
    if (args.arch_profile is None) != (args.arch_image is None):
        raise SystemExit("--arch-profile and --arch-image must be used together")
    if args.arch_image is None and args.arch_format != "fat":
        raise SystemExit("--arch-format requires --arch-image")
    if args.arch_image is not None and not args.arch_image.is_file():
        raise SystemExit(f"missing architecture image: {args.arch_image}")
    if args.data_image is not None and not args.data_image.is_file():
        raise SystemExit(f"missing data image: {args.data_image}")
    if args.swapfile is not None and not args.swapfile.is_file():
        raise SystemExit(f"missing swapfile: {args.swapfile}")
    if args.arch_format == "ufs" and args.holoris is not None:
        raise SystemExit("--holoris cannot modify a UFS rootfs; include it when building the rootfs")
    if args.ufs_root is not None and (not args.ufs_root.is_file() or
                                     args.ufs_root.stat().st_size % SECTOR_SIZE):
        raise SystemExit("--ufs-root must be a sector-aligned image")
    if args.arch_image is not None and (args.shell or args.noct or
                                        args.nettest or bin_files):
        raise SystemExit("architecture image cannot be mixed with direct /bin files")
    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")
    total_sectors = args.size_mib * 2048
    start = 2048
    blocks = args.fat_size_mib * 2048 if args.ufs_root is not None else \
        total_sectors - start - (33 if args.gpt else 0)
    root_start = start + blocks
    root_blocks = (args.ufs_root.stat().st_size // SECTOR_SIZE
                   if args.ufs_root is not None else 0)
    stage2_lba = GPT_STAGE2_LBA if args.gpt else \
        (1 if args.machine == "pcat" else 2)
    if blocks <= 0:
        raise SystemExit("image is too small for the LBA 2048 partition")
    if args.ufs_root is not None and root_start + root_blocks > total_sectors:
        raise SystemExit("FAT16 and UFS1 partitions exceed the image")
    stage1 = bytearray(args.stage1.read_bytes())
    stage2 = args.stage2.read_bytes()
    if len(stage1) != 512 or stage1[510:512] != b"\x55\xaa":
        raise SystemExit("invalid Stage 1")
    if len(stage2) % 512 or stage2_lba + len(stage2) // 512 > start:
        raise SystemExit("invalid or oversized Stage 2")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=args.output.name + ".",
                                           dir=args.output.parent)
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        with temporary.open("r+b") as stream:
            stream.truncate(total_sectors * 512)
            if args.machine == "pcat":
                signature = zlib.crc32(stage2)
                signature = zlib.crc32(args.kernel.read_bytes(), signature)
                if signature == 0:
                    signature = 1
                struct.pack_into("<I", stage1, 0x1B8, signature)
            stage1[0x1BE:0x1CE] = struct.pack(
                "<BBBBBBBBII", 0x80, 0xFE, 0xFF, 0xFF,
                0x0E, 0xFE, 0xFF, 0xFF, start, blocks)
            if args.gpt:
                stage1[0x1CE:0x1DE] = struct.pack(
                    "<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFE, 0xFF, 0xFF,
                    1, min(total_sectors - 1, 0xffffffff))
            if args.ufs_root is not None:
                stage1[0x1CE:0x1DE] = struct.pack(
                    "<BBBBBBBBII", 0x00, 0xFE, 0xFF, 0xFF,
                    0xA5, 0xFE, 0xFF, 0xFF, root_start, root_blocks)
            stage1[510:512] = b"\x55\xaa"
            stream.seek(0)
            stream.write(stage1)
            if args.machine == "pc98":
                heads = 4 if args.size_mib <= 20 else 8
                entry = bytearray(32)
                entry[0] = 0xA1
                entry[1] = 0x91
                entry[4:8] = pc98_chs(start, heads)
                entry[8:12] = pc98_chs(start, heads)
                entry[12:16] = pc98_chs(total_sectors - 1, heads)
                entry[16:32] = b"BOOT".ljust(16, b" ")
                stream.seek(SECTOR_SIZE)
                stream.write(entry)
                if args.ufs_root is not None:
                    root_entry = bytearray(32)
                    root_entry[0] = 0x21
                    root_entry[1] = 0x01
                    root_entry[4:8] = pc98_chs(root_start, heads)
                    root_entry[8:12] = pc98_chs(root_start, heads)
                    root_entry[12:16] = pc98_chs(
                        root_start + root_blocks - 1, heads)
                    root_entry[16:32] = b"ROOT".ljust(16, b" ")
                    stream.write(root_entry)
            stream.seek(stage2_lba * SECTOR_SIZE)
            stream.write(stage2)
            if args.gpt:
                entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
                entries[:GPT_ENTRY_SIZE] = gpt_entry(
                    ESP_TYPE, uuid.uuid4(), start, start + blocks - 1,
                    "zedBSD EFI System")
                entries[GPT_ENTRY_SIZE:2 * GPT_ENTRY_SIZE] = gpt_entry(
                    BIOS_BOOT_TYPE, uuid.uuid4(), GPT_STAGE2_LBA,
                    start - 1, "zedBSD BIOS loader")
                entries_crc = zlib.crc32(entries) & 0xffffffff
                disk_guid = uuid.uuid4()
                backup_entries_lba = total_sectors - 33
                stream.seek(2 * SECTOR_SIZE)
                stream.write(entries)
                stream.seek(SECTOR_SIZE)
                stream.write(gpt_header(1, total_sectors - 1, 2, 34,
                                        total_sectors - 34, disk_guid,
                                        entries_crc))
                stream.seek(backup_entries_lba * SECTOR_SIZE)
                stream.write(entries)
                stream.seek((total_sectors - 1) * SECTOR_SIZE)
                stream.write(gpt_header(total_sectors - 1, 1,
                                        backup_entries_lba, 34,
                                        total_sectors - 34, disk_guid,
                                        entries_crc))
            if args.ufs_root is not None:
                stream.seek(root_start * SECTOR_SIZE)
                stream.write(args.ufs_root.read_bytes())
        offset = start * 512
        if args.machine == "pc98":
            logical_blocks = blocks // 2
            cluster = 1
            while logical_blocks // cluster >= 65525:
                cluster *= 2
            run("mformat", "-i", f"{temporary}@@{offset}", "-S", "3",
                "-c", str(cluster), "-h", str(heads), "-s", "17",
                "-H", str(start), "-T", str(logical_blocks),
                "-v", "BOOT", "::")
            bpb = temporary.read_bytes()[offset:offset + 1024]
            pbr = bytearray(args.partition_pbr.read_bytes())
            if len(pbr) != 1024:
                raise SystemExit("PC-98 partition PBR must be 1024 bytes")
            pbr[3:0x3e] = bpb[3:0x3e]
            with temporary.open("r+b") as stream:
                stream.seek(offset)
                stream.write(pbr)
            run("mcopy", "-i", f"{temporary}@@{offset}",
                str(args.bootzbsd), "::BOOTZBSD.EXE")
            run("mattrib", "-i", f"{temporary}@@{offset}",
                "+r", "+h", "+s", "::BOOTZBSD.EXE")
        else:
            run("mformat", "-i", f"{temporary}@@{offset}", "-R", "2",
                "-H", str(start), "-T", str(blocks),
                "-v", "BOOT", "::")
            bpb = temporary.read_bytes()[offset:offset + 1024]
            pbr = bytearray(args.partition_pbr.read_bytes())
            if len(pbr) != 1024:
                raise SystemExit("PC/AT partition PBR must be 1024 bytes")
            pbr[3:0x3e] = bpb[3:0x3e]
            with temporary.open("r+b") as stream:
                stream.seek(offset)
                stream.write(pbr)
            run("mcopy", "-i", f"{temporary}@@{offset}",
                str(args.bootzbsd), "::BOOTZBSD.EXE")
            run("mattrib", "-i", f"{temporary}@@{offset}",
                "+r", "+h", "+s", "::BOOTZBSD.EXE")
        if args.gpt:
            run("mmd", "-i", f"{temporary}@@{offset}", "::/EFI")
            run("mmd", "-i", f"{temporary}@@{offset}", "::/EFI/BOOT")
            run("mcopy", "-i", f"{temporary}@@{offset}",
                str(args.bootx64), "::/EFI/BOOT/BOOTX64.EFI")
            run("mcopy", "-i", f"{temporary}@@{offset}",
                str(args.kernel), "::/VMUNIX.X64")
        if args.fragment_kernel:
            with tempfile.TemporaryDirectory(prefix="zedbsd-fragment-") as work:
                hole = Path(work) / "hole.bin"
                blocker = Path(work) / "blocker.bin"
                hole.write_bytes(bytes(4096))
                blocker.write_bytes(bytes(128 * 1024))
                run("mcopy", "-i", f"{temporary}@@{offset}", str(hole),
                    "::HOLE.BIN")
                run("mcopy", "-i", f"{temporary}@@{offset}", str(blocker),
                    "::BLOCKER.BIN")
                run("mdel", "-i", f"{temporary}@@{offset}", "::HOLE.BIN")
                run("mcopy", "-i", f"{temporary}@@{offset}", str(args.kernel),
                    "::VMUNIX")
                run("mdel", "-i", f"{temporary}@@{offset}", "::BLOCKER.BIN")
        else:
            run("mcopy", "-i", f"{temporary}@@{offset}", str(args.kernel),
                "::VMUNIX")
        if args.arch_image is not None:
            rootfs = args.arch_image
            if args.holoris is not None:
                rootfs = Path(str(temporary) + ".rootfs")
                shutil.copyfile(args.arch_image, rootfs)
                run("mcopy", "-o", "-i", str(rootfs),
                    str(args.holoris), "::/usr/bin/holoris.nct")
            run("mcopy", "-i", f"{temporary}@@{offset}",
                str(rootfs), "::/ROOTFS.IMG")
            if rootfs != args.arch_image:
                rootfs.unlink()
        if args.data_image is not None:
            run("mcopy", "-i", f"{temporary}@@{offset}",
                str(args.data_image), "::/DATA.IMG")
        if args.swapfile is not None:
            run("mcopy", "-i", f"{temporary}@@{offset}",
                str(args.swapfile), "::/SWAPFILE")
        elif args.shell or args.noct or args.nettest or bin_files:
            run("mmd", "-i", f"{temporary}@@{offset}", "::/bin")
        run("mmd", "-i", f"{temporary}@@{offset}", "::/etc")
        if args.noct or (args.holoris and args.arch_image is None):
            run("mmd", "-i", f"{temporary}@@{offset}", "::/usr")
            run("mmd", "-i", f"{temporary}@@{offset}", "::/usr/bin")
        if args.shell:
            run("mcopy", "-i", f"{temporary}@@{offset}", str(args.shell),
                "::/bin/sh")
        if args.noct:
            run("mcopy", "-i", f"{temporary}@@{offset}", str(args.noct),
                "::/usr/bin/noct")
        if args.nettest:
            run("mcopy", "-i", f"{temporary}@@{offset}", str(args.nettest),
                "::/bin/nettest")
        for name, source in bin_files.items():
            run("mcopy", "-i", f"{temporary}@@{offset}", str(source),
                f"::/bin/{name}")
        if args.holoris and args.arch_image is None:
            run("mcopy", "-i", f"{temporary}@@{offset}",
                str(args.holoris), "::/usr/bin/holoris.nct")
        checker = Path(__file__).with_name(
            "check-amd64-gpt-image.py" if args.gpt else
            "check-bios-hdd-image.py")
        run("python3", str(checker), "--machine", args.machine,
            "--kernel", str(args.kernel),
            "--bootzbsd", str(args.bootzbsd),
            *( ["--bootx64", str(args.bootx64)] if args.gpt else []),
            *(["--arch-profile", args.arch_profile,
               "--arch-image", str(args.arch_image),
               "--arch-format", args.arch_format]
              if args.arch_image is not None else []),
            *(["--noct", str(args.noct)] if args.noct else []),
            *(["--holoris", str(args.holoris)] if args.holoris else []),
            *(sum((["--bin-file", f"{name}={source}"]
                   for name, source in bin_files.items()), [])),
            *(["--data-image", str(args.data_image)] if args.data_image else []),
            *(["--swapfile", str(args.swapfile)] if args.swapfile else []),
            *(["--ufs-root", str(args.ufs_root)] if args.ufs_root else []),
            str(temporary))
        os.replace(temporary, args.output)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--machine", choices=("pcat", "pc98"), required=True)
    parser.add_argument("--gpt", action="store_true")
    parser.add_argument("--bootx64", type=Path)
    parser.add_argument("--stage1", type=Path, required=True)
    parser.add_argument("--stage2", type=Path, required=True)
    parser.add_argument("--partition-pbr", type=Path)
    parser.add_argument("--bootzbsd", type=Path)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--shell", type=Path)
    parser.add_argument("--noct", type=Path)
    parser.add_argument("--nettest", type=Path)
    parser.add_argument("--holoris", type=Path)
    parser.add_argument("--arch-profile", choices=("i386", "amd64", "aarch64"))
    parser.add_argument("--arch-image", type=Path)
    parser.add_argument("--data-image", type=Path)
    parser.add_argument("--swapfile", type=Path)
    parser.add_argument("--arch-format", choices=("fat", "ufs"), default="fat")
    parser.add_argument("--bin-file", action="append", default=[])
    parser.add_argument("--size-mib", type=int, default=129)
    parser.add_argument("--fat-size-mib", type=int, default=128)
    parser.add_argument("--ufs-root", type=Path)
    parser.add_argument("--fragment-kernel", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    create(parser.parse_args())


if __name__ == "__main__":
    main()
