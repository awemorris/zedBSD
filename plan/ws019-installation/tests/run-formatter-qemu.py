#!/usr/bin/env python3
"""Bounded q078 formatter acceptance on fresh disposable QEMU media."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
import struct
import subprocess
import uuid
import zlib

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
STORAGE = runpy.run_path(str(HERE / "run-storage-qemu.py"))
Guest = STORAGE["Guest"]
digest = STORAGE["digest"]
PARTUUID = "78190000-1111-4111-8111-111111111111"
SECTORS = 524288
PART_SECTORS = 520192


class FormatterGuest(Guest):
    def prompt(self, start):
        return self.wait(r"root@[^\s]*:[^\n]*\$ ?$", start, 120)[0]


def copy_image(source, destination):
    subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                    str(source), str(destination)], check=True)


def create_nvme(path):
    """Constructs only this test's pre-existing GPT/FAT32 destination."""
    mbr = bytearray(512)
    mbr[450] = 0xEE
    struct.pack_into("<II", mbr, 454, 1, SECTORS - 1)
    mbr[510:512] = b"\x55\xaa"
    entries = bytearray(16384)
    entries[:16] = uuid.UUID(STORAGE["TYPE"]).bytes_le
    entries[16:32] = uuid.UUID(PARTUUID).bytes_le
    struct.pack_into("<QQ", entries, 32, 2048, 2048 + PART_SECTORS - 1)
    entries[56:66] = "q078a".encode("utf-16le")
    with path.open("xb") as disk:
        disk.truncate(SECTORS * 512)
        disk.write(mbr)
        for lba, alternate, table in [(1, SECTORS - 1, 2),
                                       (SECTORS - 1, 1, SECTORS - 33)]:
            header = bytearray(512)
            struct.pack_into("<8sIIIIQQQQ16sQIII", header, 0, b"EFI PART",
                             0x10000, 92, 0, 0, lba, alternate, 34,
                             SECTORS - 34,
                             uuid.UUID("78190000-aaaa-4aaa-8aaa-aaaaaaaaaaaa").bytes_le,
                             table, 128, 128, zlib.crc32(entries))
            struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
            disk.seek(lba * 512)
            disk.write(header)
            disk.seek(table * 512)
            disk.write(entries)
    subprocess.run(["mformat", "-i", f"{path}@@1048576", "-F",
                    "-T", str(PART_SECTORS), "-N", "78190000",
                    "-v", "Q078DATA", "::"], check=True)
    subprocess.run(["mcopy", "-i", f"{path}@@1048576",
                    str(HERE / "README.md"), "::/sentinel.txt"], check=True)
    # Supply allocated, all-zero files, not filesystem or swap templates.
    # Creating/resizing is the caller's job, outside the formatter contract.
    for name, size in [("data.img", 33554432), ("swapfile", 67108864),
                       ("empty", 0), ("short", 8193)]:
        blank = path.parent / (name + ".blank")
        with blank.open("xb") as stream:
            stream.truncate(size)
        subprocess.run(["mcopy", "-i", f"{path}@@1048576",
                        str(blank), "::/" + name], check=True)


def protected_bytes(path):
    with path.open("rb") as disk:
        first = disk.read(34 * 512)
        disk.seek(-33 * 512, 2)
        last = disk.read()
        disk.seek(1048576)
        boot = disk.read(512)
    sentinel = subprocess.run(["mtype", "-i", f"{path}@@1048576",
                               "::/sentinel.txt"], capture_output=True,
                              check=True).stdout
    return {"tables": hashlib.sha256(first + last).hexdigest(),
            "fat_boot": hashlib.sha256(boot).hexdigest(),
            "sentinel": hashlib.sha256(sentinel).hexdigest()}


def boot_payload(source):
    with source.open("rb") as disk:
        disk.seek(512)
        header = disk.read(512)
        assert header[:8] == b"EFI PART"
        table, slots, size = struct.unpack_from("<QII", header, 72)
        assert 0 < slots <= 4096 and 128 <= size <= 4096
        disk.seek(table * 512)
        entries = disk.read(slots * size)
    candidates = []
    for slot in range(slots):
        entry = entries[slot * size:(slot + 1) * size]
        if entry[:16] == bytes(16):
            continue
        start, end = struct.unpack_from("<QQ", entry, 32)
        assert 34 <= start <= end < source.stat().st_size // 512
        result = subprocess.run(["mdir", "-i", f"{source}@@{start * 512}",
                                 "::/rootfs.img"], capture_output=True)
        if result.returncode == 0:
            candidates.append(start)
    assert len(candidates) == 1, "refuse ambiguous source payload"
    return candidates[0]


def prepare_boot(output, overlay, combined):
    source = REPO / "build/amd64/hdd-image.img"
    rootfs = REPO / "build/arch-images/amd64-ws019-formatters.ufs"
    assert rootfs.is_file(), "build ws019-formatter-qemu-fixture first"
    source_hash = digest(source)
    start = boot_payload(source)
    copy_image(source, output / "boot.img")
    image = f"{output / 'boot.img'}@@{start * 512}"
    subprocess.run(["mcopy", "-o", "-i", image, str(rootfs),
                    "::/rootfs.img"], check=True)
    if overlay:
        config = output / "zedbsd.cfg"
        config.write_text("kernel=vmunix\n"
                          f"boot1=PARTUUID={PARTUUID}\n"
                          "overlay-root=rootfs.img\n"
                          "overlay-data=boot1:data.img\n"
                          "swap0=boot1:swapfile\n")
        subprocess.run(["mcopy", "-o", "-i", image, str(config),
                        "::/zedbsd.cfg"], check=True)
    shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", output / "vars.fd")
    STORAGE["fixture"](output / "mbr.img", False)
    if combined:
        # Start from a native UFS root so the guest can mount and update its
        # inactive FAT payload volume without competing root/swap claims.
        config = output / "zedbsd-native.cfg"
        config.write_text("kernel=vmunix\nrootpart=sdb1\n")
        subprocess.run(["mcopy", "-o", "-i", image, str(config),
                        "::/zedbsd.cfg"], check=True)
        with (output / "mbr.img").open("r+b") as disk:
            disk.seek(458)
            disk.write(struct.pack("<I", rootfs.stat().st_size // 512))
            disk.seek(1048576)
            with rootfs.open("rb") as source_root:
                shutil.copyfileobj(source_root, disk)
    return {"production_sha256": source_hash, "rootfs_sha256": digest(rootfs),
            "boot_sha256_before": digest(output / "boot.img"),
            "native_root_sha256_before": digest(output / "mbr.img")}


def format_cell(guest):
    guest.login()
    guest.run("mount -t fat nvme0n1p1 /q078")
    guest.run("mkfs -t ufs1 /q078/data.img", "ufs1 initialized")
    guest.run("mkswap /q078/swapfile", "16383 slots")
    guest.run("mkfs -t ufs2 /q078/data.img", "usage:", 2)
    guest.run("mkfs -t ufs1 /dev/nvme0n1p1", status=1)
    guest.run("mkswap /q078", status=1)
    guest.run("mkswap /q078/empty", status=1)
    guest.run("mkfs -t ufs1 /q078/empty", status=1)
    guest.run("mkswap /q078/short", status=1)
    guest.run("ln -s /q078/data.img /q078-link")
    guest.run("mkfs -t ufs1 /q078-link", status=1)
    guest.run("swapon /q078/swapfile")
    guest.run("formatter-probe swap /q078/swapfile 1", "slots=16383")
    guest.run("mkswap /q078/swapfile", "busy", 1)
    guest.run("swapoff /q078/swapfile")
    guest.run("formatter-probe swap /q078/swapfile 0", "formatter-probe PASS")
    guest.run("umount /q078")


def overlay_cell(guest):
    guest.login()
    guest.run("mount", r" on / type overlay ")
    guest.run("formatter-probe swap boot1:swapfile 1", "slots=16383")
    guest.run("formatter-probe write /q078-persist", "formatter-probe PASS")
    at = len(guest.text())
    guest.send("reboot")
    guest.login(at)
    guest.run("formatter-probe read /q078-persist", "formatter-probe PASS")
    guest.run("formatter-probe swap boot1:swapfile 1", "slots=16383")


def combined_cell(guest):
    format_cell(guest)
    # sda2 is the verified unique payload in this ordinary two-partition image.
    guest.run("mount -t fat sda2 /q078-boot")
    guest.run("formatter-probe select-overlay /q078-boot/zedbsd.cfg",
              "formatter-probe PASS")
    guest.run("umount /q078-boot")
    at = len(guest.text())
    guest.send("reboot")
    guest.wait(r"login:\s*$", at, 120)
    # Consume only the new boot's login prompt, not the first boot's history.
    guest.login(at)
    guest.run("mount", r" on / type overlay ")
    guest.run("formatter-probe swap boot1:swapfile 1", "slots=16383")
    guest.run("formatter-probe write /q078-persist", "formatter-probe PASS")
    at = len(guest.text())
    guest.send("reboot")
    guest.login(at)
    guest.run("formatter-probe read /q078-persist", "formatter-probe PASS")
    guest.run("formatter-probe swap boot1:swapfile 1", "slots=16383")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cell", choices=["format", "overlay", "combined"])
    parser.add_argument("output", type=Path)
    parser.add_argument("--formatted", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.relative_to(REPO / "plan/ws019-installation/temp")
    if args.cell == "overlay":
        assert args.formatted, "overlay cell requires a completed format cell"
        previous = args.formatted.resolve()
        previous.relative_to(REPO / "plan/ws019-installation/temp")
        metadata = json.loads((previous / "result.json").read_text())
        assert metadata["result"] == "PASS format"
        assert digest(previous / "gpt.img") == metadata["nvme_sha256_after"]
    output.mkdir(parents=True, exist_ok=False)
    if args.cell == "combined":
        assert boot_payload(REPO / "build/amd64/hdd-image.img") == 133120
    metadata = prepare_boot(output, args.cell == "overlay", args.cell == "combined")
    if args.cell in ("format", "combined"):
        create_nvme(output / "gpt.img")
    else:
        copy_image(previous / "gpt.img", output / "gpt.img")
        metadata["formatted_source"] = str(previous)
        metadata["formatted_source_sha256"] = digest(previous / "gpt.img")
    before = protected_bytes(output / "gpt.img")
    metadata["nvme_sha256_before"] = digest(output / "gpt.img")
    (output / "inputs.json").write_text(json.dumps(metadata, indent=2) + "\n")
    guest = FormatterGuest(output)
    try:
        if args.cell == "combined":
            combined_cell(guest)
        elif args.cell == "format":
            format_cell(guest)
        else:
            overlay_cell(guest)
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
    except BaseException as failure:
        metadata["result"] = f"FAIL {args.cell}"
        metadata["failure"] = str(failure)
        (output / "result.json").write_text(json.dumps(metadata, indent=2) + "\n")
        raise
    finally:
        guest.stop()
    # Preserve failures after guest shutdown, including concurrent host rebuilds.
    metadata["guest_functional_result"] = "PASS"
    metadata["nvme_sha256_after"] = digest(output / "gpt.img")
    metadata["production_sha256_after"] = digest(REPO / "build/amd64/hdd-image.img")
    metadata["protected_before"] = before
    metadata["protected_after"] = protected_bytes(output / "gpt.img")
    try:
        assert before == metadata["protected_after"], "protected metadata changed"
        assert metadata["production_sha256_after"] == metadata["production_sha256"], \
            "production input changed during acceptance; do not build concurrently"
        if args.cell in ("format", "combined"):
            for name in ["data.img", "swapfile"]:
                subprocess.run(["mcopy", "-i", f"{output / 'gpt.img'}@@1048576",
                                f"::/{name}", str(output / name)], check=True)
                metadata[f"generated_{name}_sha256"] = digest(output / name)
    except BaseException as failure:
        metadata["result"] = f"FAIL {args.cell} postprocessing"
        metadata["failure"] = str(failure)
        (output / "result.json").write_text(json.dumps(metadata, indent=2) + "\n")
        raise
    metadata["result"] = f"PASS {args.cell}"
    (output / "result.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(metadata["result"])


if __name__ == "__main__":
    main()
