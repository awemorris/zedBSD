#!/usr/bin/env python3
"""Validate the PC-98/PC-AT BIOS and x64 UEFI disk layout."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import hashlib
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
HEADS = 8
SECTORS = 17


def fail(message: str) -> None:
    raise SystemExit("unified image check: " + message)


def pc98_chs(lba: int) -> bytes:
    cylinder, remainder = divmod(lba, HEADS * SECTORS)
    head, sector = divmod(remainder, SECTORS)
    return bytes((sector, head)) + struct.pack("<H", cylinder)


def check_zbl2(stream, lba: int, expected_machine: int) -> int:
    stream.seek(lba * SECTOR_SIZE)
    header = stream.read(SECTOR_SIZE)
    if len(header) != SECTOR_SIZE:
        fail(f"missing ZBL2 at LBA {lba}")
    magic, version, header_size, image_size = struct.unpack_from(
        "<IHHI", header, 0)
    count, machine, entry = struct.unpack_from("<HHI", header, 12)
    if (magic, version, header_size) != (0x324C425A, 1, 32):
        fail(f"invalid ZBL2 header at LBA {lba}")
    if machine != expected_machine:
        fail(f"ZBL2 machine mismatch at LBA {lba}")
    if not 1 <= count < SLOT_SECTORS:
        fail(f"ZBL2 exceeds slot at LBA {lba}")
    if not 32 <= entry < image_size <= count * SECTOR_SIZE:
        fail(f"invalid ZBL2 size or entry at LBA {lba}")
    stream.seek(lba * SECTOR_SIZE)
    data = stream.read(count * SECTOR_SIZE)
    total = sum(struct.unpack_from("<I", data, offset)[0]
                for offset in range(0, len(data), 4)) & 0xFFFFFFFF
    if total:
        fail(f"ZBL2 checksum mismatch at LBA {lba}")
    return count


def extracted_hash(image: Path, name: str, lba: int = PARTITION_LBA) -> bytes:
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / name.replace("/", "-").replace(".", "-")
        subprocess.run(["mcopy", "-n", "-i",
                        f"{image}@@{lba * SECTOR_SIZE}",
                        f"::{name}", str(output)], check=True,
                       stdout=subprocess.DEVNULL)
        return hashlib.sha256(output.read_bytes()).digest()


def extract_file(image: Path, name: str, output: Path,
                 lba: int = PARTITION_LBA) -> None:
    subprocess.run(["mcopy", "-n", "-i",
                    f"{image}@@{lba * SECTOR_SIZE}", f"::{name}", str(output)],
                   check=True, stdout=subprocess.DEVNULL)


def check(args: argparse.Namespace) -> None:
    size = args.image.stat().st_size
    if size == 0 or size % SECTOR_SIZE:
        fail("image size is not a positive sector multiple")
    total_sectors = size // SECTOR_SIZE
    with args.image.open("rb") as stream:
        mbr = stream.read(SECTOR_SIZE)
        if mbr[2:8] != b"\x90\x90IPL1":
            fail("missing PC-98 IPL1 marker in Stage 0")
        if mbr[510:512] != b"\x55\xaa":
            fail("missing MBR signature")
        if struct.unpack_from("<H", mbr, 508)[0] != 9:
            fail("missing PC-98 IPL metadata")
        status, ptype, start, blocks = (
            mbr[0x1BE], mbr[0x1C2],
            struct.unpack_from("<I", mbr, 0x1C6)[0],
            struct.unpack_from("<I", mbr, 0x1CA)[0])
        if (status, ptype, start, blocks) != (
                0x80, 0x0E, PARTITION_LBA, FAT16_BLOCKS):
            fail("first MBR entry is not the expected active FAT16 partition")
        status2, ptype2, start2, blocks2 = (
            mbr[0x1CE], mbr[0x1D2],
            struct.unpack_from("<I", mbr, 0x1D6)[0],
            struct.unpack_from("<I", mbr, 0x1DA)[0])
        if (status2, ptype2, start2) != (0x00, 0xEF, ESP_LBA):
            fail("second MBR entry is not the expected FAT32 ESP")
        if blocks2 < MIN_ESP_BLOCKS or start2 + blocks2 != total_sectors:
            fail("second MBR entry has an invalid extent")
        if any(mbr[0x1DE:0x1FC]):
            fail("MBR entry three or reserved entry-four bytes are used")

        stream.seek(SECTOR_SIZE)
        table = stream.read(SECTOR_SIZE)
        last_lba = ESP_LBA - 1
        expected = bytearray(32)
        expected[0:2] = b"\xa1\x91"
        expected[4:8] = pc98_chs(PARTITION_LBA)
        expected[8:12] = pc98_chs(PARTITION_LBA)
        expected[12:16] = pc98_chs(last_lba)
        expected[16:32] = b"BOOT".ljust(16, b" ")
        if table[:32] != expected or any(table[32:]):
            fail("LBA 1 is not the exact H=8/S=17 PC-98 mirror")

        stream.seek(PC98_STAGE1_LBA * SECTOR_SIZE)
        if not any(stream.read(SECTOR_SIZE)):
            fail("missing PC-98 Stage 1.5")
        pc98_count = check_zbl2(stream, PC98_STAGE1_LBA + 1, 2)
        stream.seek((PC98_STAGE1_LBA + 1 + pc98_count) * SECTOR_SIZE)
        if any(stream.read((PCAT_STAGE1_LBA - PC98_STAGE1_LBA - 1 -
                            pc98_count) * SECTOR_SIZE)):
            fail("PC-98 slot padding is not zero")

        stream.seek(PCAT_STAGE1_LBA * SECTOR_SIZE)
        if not any(stream.read(SECTOR_SIZE)):
            fail("missing PC/AT Stage 1.5")
        pcat_count = check_zbl2(stream, PCAT_STAGE1_LBA + 1, 1)
        stream.seek((PCAT_STAGE1_LBA + 1 + pcat_count) * SECTOR_SIZE)
        if any(stream.read((PARTITION_LBA - PCAT_STAGE1_LBA - 1 -
                            pcat_count) * SECTOR_SIZE)):
            fail("reserved pre-partition area is not zero")

        stream.seek(PARTITION_LBA * SECTOR_SIZE)
        bpb = stream.read(SECTOR_SIZE)
        if struct.unpack_from("<H", bpb, 11)[0] != SECTOR_SIZE:
            fail("FAT bytes/sector is not 512")
        if struct.unpack_from("<H", bpb, 22)[0] == 0:
            fail("volume is not FAT12/16")

        stream.seek(ESP_LBA * SECTOR_SIZE)
        esp_bpb = stream.read(SECTOR_SIZE)
        if struct.unpack_from("<H", esp_bpb, 11)[0] != SECTOR_SIZE:
            fail("ESP bytes/sector is not 512")
        if struct.unpack_from("<H", esp_bpb, 22)[0] != 0 or \
                struct.unpack_from("<I", esp_bpb, 36)[0] == 0:
            fail("ESP is not FAT32")

    if args.pc98_kernel and extracted_hash(args.image, "VMUNIX.98") != \
            hashlib.sha256(args.pc98_kernel.read_bytes()).digest():
        fail("VMUNIX.98 differs from the input kernel")
    if args.pcat_kernel and extracted_hash(args.image, "VMUNIX.AT") != \
            hashlib.sha256(args.pcat_kernel.read_bytes()).digest():
        fail("VMUNIX.AT differs from the input kernel")
    if args.amd64_kernel:
        expected_amd64 = hashlib.sha256(args.amd64_kernel.read_bytes()).digest()
        if extracted_hash(args.image, "VMUNIX.X64") != expected_amd64:
            fail("FAT16 VMUNIX.X64 differs from the input kernel")
        if extracted_hash(args.image, "VMUNIX.X64", ESP_LBA) != expected_amd64:
            fail("ESP VMUNIX.X64 differs from the input kernel")
    if args.arm64_kernel and extracted_hash(args.image, "VMUNIX.A64") != \
            hashlib.sha256(args.arm64_kernel.read_bytes()).digest():
        fail("FAT16 VMUNIX.A64 differs from the input kernel")
    for profile, source in (("i386", args.i386_arch_image),
                            ("amd64", args.amd64_arch_image),
                            ("aarch64", args.aarch64_arch_image)):
        if source is None:
            continue
        if extracted_hash(args.image, f"arch/{profile}.img") != \
                hashlib.sha256(source.read_bytes()).digest():
            fail(f"/arch/{profile}.img differs from the input image")
        with tempfile.TemporaryDirectory(prefix="zedbsd-unified-arch-") as work:
            inner = Path(work) / f"{profile}.img"
            extract_file(args.image, f"arch/{profile}.img", inner)
            checker = Path(__file__).with_name("check-arch-overlay-image.py")
            subprocess.run(["python3", str(checker), "--profile", profile,
                            "--image", str(inner)], check=True)
    if args.rpi4_config and extracted_hash(args.image, "config.txt") != \
            hashlib.sha256(args.rpi4_config.read_bytes()).digest():
        fail("/config.txt differs from the Pi 4 configuration")
    if args.rpi4_firmware_dir:
        names = ("start4.elf", "fixup4.dat", "bcm2711-rpi-4-b.dtb",
                 "LICENCE.broadcom")
        for name in names:
            source = args.rpi4_firmware_dir / name
            if extracted_hash(args.image, name) != \
                    hashlib.sha256(source.read_bytes()).digest():
                fail(f"/{name} differs from the pinned Pi firmware")
        overlay = args.rpi4_firmware_dir / "overlays" / "disable-bt.dtbo"
        if extracted_hash(args.image, "overlays/disable-bt.dtbo") != \
                hashlib.sha256(overlay.read_bytes()).digest():
            fail("/overlays/disable-bt.dtbo differs from pinned firmware")
    if args.bootx64 and extracted_hash(
            args.image, "EFI/BOOT/BOOTX64.EFI", ESP_LBA) != \
            hashlib.sha256(args.bootx64.read_bytes()).digest():
        fail("ESP BOOTX64.EFI differs from the input loader")
    if args.holoris and extracted_hash(args.image, "apps/holoris.nct") != \
            hashlib.sha256(args.holoris.read_bytes()).digest():
        fail("/apps/holoris.nct differs from the input script")
    if args.remacs and extracted_hash(args.image, "apps/remacs.nap") != \
            hashlib.sha256(args.remacs.read_bytes()).digest():
        fail("/apps/remacs.nap differs from the input bytecode")
    print("Unified BIOS/UEFI/Raspberry Pi 4 image check: PASS "
          f"(PC-98 ZBL2 {pc98_count} sectors, PC/AT ZBL2 {pcat_count} sectors)")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pc98-kernel", type=Path)
    parser.add_argument("--pcat-kernel", type=Path)
    parser.add_argument("--amd64-kernel", type=Path)
    parser.add_argument("--arm64-kernel", type=Path)
    parser.add_argument("--i386-arch-image", type=Path)
    parser.add_argument("--amd64-arch-image", type=Path)
    parser.add_argument("--aarch64-arch-image", type=Path)
    parser.add_argument("--rpi4-config", type=Path)
    parser.add_argument("--rpi4-firmware-dir", type=Path)
    parser.add_argument("--bootx64", type=Path)
    parser.add_argument("--holoris", type=Path)
    parser.add_argument("--remacs", type=Path)
    parser.add_argument("image", type=Path)
    check(parser.parse_args())


if __name__ == "__main__":
    main()
