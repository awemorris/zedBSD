#!/usr/bin/env python3
"""Create one BIOS/UEFI HDD image for PC/AT and NEC PC-98."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

SECTOR_SIZE = 512
PARTITION_LBA = 2048
FAT16_BLOCKS = 262144
ESP_LBA = PARTITION_LBA + FAT16_BLOCKS
MIN_ESP_BLOCKS = 131072
PC98_STAGE1_LBA = 2
PCAT_STAGE1_LBA = 66
SLOT_SECTORS = 64
PC98_HEADS = 8
PC98_SECTORS = 17


def run(*arguments: str) -> None:
    subprocess.run(arguments, check=True)


def pc98_chs(lba: int) -> bytes:
    cylinder, remainder = divmod(lba, PC98_HEADS * PC98_SECTORS)
    head, sector = divmod(remainder, PC98_SECTORS)
    if cylinder > 0xFFFF:
        raise SystemExit("partition is outside PC-98 16-bit CHS range")
    return bytes((sector, head)) + struct.pack("<H", cylinder)


def mbr_chs(lba: int) -> bytes:
    cylinder, remainder = divmod(lba, 255 * 63)
    head, sector0 = divmod(remainder, 63)
    if cylinder > 1023:
        return b"\xfe\xff\xff"
    sector = sector0 + 1
    return bytes((head, sector | ((cylinder >> 2) & 0xC0),
                  cylinder & 0xFF))


def zbl2_sectors(path: Path, machine: int) -> int:
    data = path.read_bytes()
    if len(data) % SECTOR_SIZE or len(data) < 32:
        raise SystemExit(f"invalid padded ZBL2 image: {path}")
    magic, version, header_size = struct.unpack_from("<IHH", data, 0)
    sectors, declared_machine = struct.unpack_from("<HH", data, 12)
    if (magic, version, header_size) != (0x324C425A, 1, 32):
        raise SystemExit(f"invalid ZBL2 header: {path}")
    if declared_machine != machine or sectors != len(data) // SECTOR_SIZE:
        raise SystemExit(f"ZBL2 machine or size mismatch: {path}")
    if not 1 <= sectors < SLOT_SECTORS:
        raise SystemExit(f"ZBL2 does not fit its {SLOT_SECTORS}-sector slot")
    total = sum(struct.unpack_from("<I", data, offset)[0]
                for offset in range(0, len(data), 4)) & 0xFFFFFFFF
    if total:
        raise SystemExit(f"ZBL2 checksum mismatch: {path}")
    return sectors


def copy_kernel(image: Path, offset: int, kernel: Path, dos_name: str,
                fragment: bool) -> None:
    spec = f"{image}@@{offset}"
    if not fragment:
        run("mcopy", "-i", spec, str(kernel), f"::{dos_name}")
        return
    with tempfile.TemporaryDirectory(prefix="zedbsd-unified-fragment-") as work:
        hole = Path(work) / "hole.bin"
        blocker = Path(work) / "blocker.bin"
        hole.write_bytes(bytes(4096))
        blocker.write_bytes(bytes(128 * 1024))
        run("mcopy", "-i", spec, str(hole), "::HOLE.BIN")
        run("mcopy", "-i", spec, str(blocker), "::BLOCKER.BIN")
        run("mdel", "-i", spec, "::HOLE.BIN")
        run("mcopy", "-i", spec, str(kernel), f"::{dos_name}")
        run("mdel", "-i", spec, "::BLOCKER.BIN")


def create(args: argparse.Namespace) -> None:
    inputs = (args.stage0, args.pc98_stage1, args.pc98_stage2,
              args.pcat_stage1, args.pcat_stage2, args.pc98_kernel,
              args.pcat_kernel, args.amd64_kernel, args.arm64_kernel,
              args.i386_arch_image, args.amd64_arch_image,
              args.aarch64_arch_image, args.rpi4_config, args.bootx64)
    for path in inputs:
        if not path.is_file():
            raise SystemExit(f"missing input: {path}")
    for path in (args.holoris, args.remacs):
        if path is not None and not path.is_file():
            raise SystemExit(f"missing input: {path}")
    firmware_files = (
        args.rpi4_firmware_dir / "start4.elf",
        args.rpi4_firmware_dir / "fixup4.dat",
        args.rpi4_firmware_dir / "bcm2711-rpi-4-b.dtb",
        args.rpi4_firmware_dir / "LICENCE.broadcom",
        args.rpi4_firmware_dir / "overlays" / "disable-bt.dtbo",
    )
    for path in firmware_files:
        if not path.is_file():
            raise SystemExit(f"missing Raspberry Pi firmware input: {path}")
    if args.output.exists() and not args.force:
        raise SystemExit(f"output exists (use --force): {args.output}")

    stage0 = bytearray(args.stage0.read_bytes())
    pc98_stage1 = args.pc98_stage1.read_bytes()
    pcat_stage1 = args.pcat_stage1.read_bytes()
    if len(stage0) != SECTOR_SIZE or stage0[510:512] != b"\x55\xaa":
        raise SystemExit("invalid unified Stage 0")
    if len(pc98_stage1) != SECTOR_SIZE or len(pcat_stage1) != SECTOR_SIZE:
        raise SystemExit("each Stage 1.5 must be exactly one sector")
    zbl2_sectors(args.pc98_stage2, 2)
    zbl2_sectors(args.pcat_stage2, 1)

    total_sectors = args.size_mib * 2048
    esp_blocks = total_sectors - ESP_LBA
    if esp_blocks < MIN_ESP_BLOCKS:
        raise SystemExit("image is too small for the 128 MiB FAT16 and ESP")
    fat16_last_lba = ESP_LBA - 1
    esp_last_lba = total_sectors - 1

    # Entry four remains unavailable because offset 508 is PC-98 metadata.
    stage0[0x1BE:0x1FE] = bytes(64)
    stage0[0x1BE:0x1CE] = struct.pack(
        "<B3sB3sII", 0x80, mbr_chs(PARTITION_LBA), 0x0E,
        mbr_chs(fat16_last_lba), PARTITION_LBA, FAT16_BLOCKS)
    stage0[0x1CE:0x1DE] = struct.pack(
        "<B3sB3sII", 0x00, mbr_chs(ESP_LBA), 0xEF,
        mbr_chs(esp_last_lba), ESP_LBA, esp_blocks)
    stage0[508:510] = struct.pack("<H", 9)
    stage0[510:512] = b"\x55\xaa"

    pc98_table = bytearray(SECTOR_SIZE)
    entry = bytearray(32)
    entry[0] = 0xA1
    entry[1] = 0x91
    entry[4:8] = pc98_chs(PARTITION_LBA)
    entry[8:12] = pc98_chs(PARTITION_LBA)
    entry[12:16] = pc98_chs(fat16_last_lba)
    entry[16:32] = b"BOOT".ljust(16, b" ")
    pc98_table[:32] = entry

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=args.output.name + ".",
                                           dir=args.output.parent)
    os.close(fd)
    temporary = Path(temporary_name)
    rootfs_work = tempfile.TemporaryDirectory(prefix="zedbsd-rootfs-")
    try:
        with temporary.open("r+b") as stream:
            stream.truncate(total_sectors * SECTOR_SIZE)
            stream.seek(0)
            stream.write(stage0)
            stream.seek(SECTOR_SIZE)
            stream.write(pc98_table)
            stream.seek(PC98_STAGE1_LBA * SECTOR_SIZE)
            stream.write(pc98_stage1)
            stream.write(args.pc98_stage2.read_bytes())
            stream.seek(PCAT_STAGE1_LBA * SECTOR_SIZE)
            stream.write(pcat_stage1)
            stream.write(args.pcat_stage2.read_bytes())

        offset = PARTITION_LBA * SECTOR_SIZE
        esp_offset = ESP_LBA * SECTOR_SIZE
        run("mformat", "-i", f"{temporary}@@{offset}", "-T",
            str(FAT16_BLOCKS), "-v", "BOOT", "::")
        copy_kernel(temporary, offset, args.pc98_kernel, "VMUNIX.98",
                    args.fragment_kernels)
        copy_kernel(temporary, offset, args.pcat_kernel, "VMUNIX.AT",
                    args.fragment_kernels)
        copy_kernel(temporary, offset, args.amd64_kernel, "VMUNIX.X64",
                    args.fragment_kernels)
        copy_kernel(temporary, offset, args.arm64_kernel, "VMUNIX.A64",
                    args.fragment_kernels)
        for profile, source in (("98", args.i386_arch_image),
                                ("x86", args.i386_arch_image),
                                ("x64", args.amd64_arch_image),
                                ("rp4", args.aarch64_arch_image)):
            installed = source
            if profile in ("98", "x86") and \
                    (args.holoris is not None or args.remacs is not None):
                installed = Path(rootfs_work.name) / f"rootfs.{profile}"
                shutil.copyfile(source, installed)
                if args.holoris is not None:
                    run("mcopy", "-o", "-i", str(installed),
                        str(args.holoris), "::/apps/holoris.nct")
                if args.remacs is not None:
                    run("mcopy", "-o", "-i", str(installed),
                        str(args.remacs), "::/apps/remacs.nap")
            run("mcopy", "-i", f"{temporary}@@{offset}", str(installed),
                f"::/rootfs.{profile}")
        run("mmd", "-i", f"{temporary}@@{offset}", "::/overlays")
        run("mcopy", "-i", f"{temporary}@@{offset}",
            str(args.rpi4_config), "::/config.txt")
        for path in firmware_files[:-1]:
            run("mcopy", "-i", f"{temporary}@@{offset}", str(path),
                f"::/{path.name}")
        run("mcopy", "-i", f"{temporary}@@{offset}",
            str(firmware_files[-1]), "::/overlays/disable-bt.dtbo")

        run("mformat", "-F", "-i", f"{temporary}@@{esp_offset}", "-T",
            str(esp_blocks), "-v", "ZEDESP", "::")
        run("mmd", "-i", f"{temporary}@@{esp_offset}", "::/EFI")
        run("mmd", "-i", f"{temporary}@@{esp_offset}", "::/EFI/BOOT")
        run("mcopy", "-i", f"{temporary}@@{esp_offset}",
            str(args.bootx64), "::/EFI/BOOT/BOOTX64.EFI")
        run("mcopy", "-i", f"{temporary}@@{esp_offset}",
            str(args.amd64_kernel), "::/VMUNIX.X64")

        checker = Path(__file__).with_name("check-unified-hdd-image.py")
        run("python3", str(checker), "--pc98-kernel",
            str(args.pc98_kernel), "--pcat-kernel", str(args.pcat_kernel),
            "--amd64-kernel", str(args.amd64_kernel),
            "--arm64-kernel", str(args.arm64_kernel),
            "--i386-arch-image", str(args.i386_arch_image),
            "--amd64-arch-image", str(args.amd64_arch_image),
            "--aarch64-arch-image", str(args.aarch64_arch_image),
            "--rpi4-config", str(args.rpi4_config),
            "--rpi4-firmware-dir", str(args.rpi4_firmware_dir),
            "--bootx64", str(args.bootx64),
            *(["--holoris", str(args.holoris)] if args.holoris else []),
            *(["--remacs", str(args.remacs)] if args.remacs else []),
            str(temporary))
        os.replace(temporary, args.output)
    finally:
        rootfs_work.cleanup()
        if temporary.exists():
            temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage0", type=Path, required=True)
    parser.add_argument("--pc98-stage1", type=Path, required=True)
    parser.add_argument("--pc98-stage2", type=Path, required=True)
    parser.add_argument("--pcat-stage1", type=Path, required=True)
    parser.add_argument("--pcat-stage2", type=Path, required=True)
    parser.add_argument("--pc98-kernel", type=Path, required=True)
    parser.add_argument("--pcat-kernel", type=Path, required=True)
    parser.add_argument("--amd64-kernel", type=Path, required=True)
    parser.add_argument("--arm64-kernel", type=Path, required=True)
    parser.add_argument("--i386-arch-image", type=Path, required=True)
    parser.add_argument("--amd64-arch-image", type=Path, required=True)
    parser.add_argument("--aarch64-arch-image", type=Path, required=True)
    parser.add_argument("--rpi4-config", type=Path, required=True)
    parser.add_argument("--rpi4-firmware-dir", type=Path, required=True)
    parser.add_argument("--bootx64", type=Path, required=True)
    parser.add_argument("--holoris", type=Path)
    parser.add_argument("--remacs", type=Path)
    parser.add_argument("--size-mib", type=int, default=256)
    parser.add_argument("--fragment-kernels", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("output", type=Path)
    create(parser.parse_args())


if __name__ == "__main__":
    main()
