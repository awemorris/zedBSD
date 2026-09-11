#!/usr/bin/env python3
"""Prepare an immutable-tree native root, then boot it without installation USB."""
import json
from pathlib import Path
import re
import runpy
import shutil
import struct
import subprocess
import sys
import time
import uuid
import zlib

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
B = runpy.run_path(str(HERE / "run-installed-boot-qemu.py"))
F, REPO = I["F"], I["REPO"]
ROOT_GUID = "78190000-3333-4333-8333-777777777777"
ROOT_START, ROOT_COUNT = 524288, 524288
SECTORS = ROOT_START + ROOT_COUNT + 2048


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    disk = out / "gpt.img"
    F["create_nvme"](disk, sectors=SECTORS)
    with disk.open("r+b") as stream:
        stream.seek(1024)
        entries = bytearray(stream.read(16384))
        entries[:16] = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b").bytes_le
        entries[128:144] = uuid.UUID("516e7cb6-6ecf-11d6-8ff8-00022d09712b").bytes_le
        entries[144:160] = uuid.UUID(ROOT_GUID).bytes_le
        struct.pack_into("<QQ", entries, 160, ROOT_START, ROOT_START + ROOT_COUNT - 1)
        for lba, table in [(1, 2), (SECTORS - 1, SECTORS - 33)]:
            stream.seek(lba * 512)
            header = bytearray(stream.read(512))
            struct.pack_into("<I", header, 88, zlib.crc32(entries))
            struct.pack_into("<I", header, 16, 0)
            struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
            stream.seek(lba * 512); stream.write(header)
            stream.seek(table * 512); stream.write(entries)
    result["result"] = "FAIL native swap startup"
    result["cases"] = []
    guest = None
    try:
        guest = I["PublicInstallGuest"](out, usb_boot=True)
        guest.deadline = time.monotonic() + 1800
        guest.login()
        observed = guest.run("sysctl kern.boot.root_image")
        identity = re.search(r"^kern.boot.root_image: 1:15:(\d+):", observed, re.M)
        assert identity, observed
        records = guest.run("diskpart --machine list")
        loop = re.search(r"^device\t(\S+)\t" + identity[1] + r"\t0\t9\t", records, re.M)
        part = re.search(r"^device\tnvme0n1p2\t(\d+)\t", records, re.M)
        assert loop and part, records
        position = len(guest.text())
        guest.send("mkfs -t ufs --profile=native nvme0n1p2; echo storage-result-$?")
        guest.wait("to continue:", position, 120)
        guest.send("FORMAT nvme0n1p2:" + part[1])
        guest.command_status(position)
        guest.run("mkdir /run/source /run/native")
        guest.run(f"mount -t ufs -r {loop[1]} /run/source")
        guest.run("mount -t ufs nvme0n1p2 /run/native")
        guest.run("cp -a -T /run/source /run/native")
        guest.run("dd if=/dev/zero of=/run/native/swapfile bs=65536 count=32 conv=sync")
        guest.run("sync /run/native/swapfile")
        guest.run("mkswap /run/native/swapfile", "initialized")
        guest.run("chmod 600 /run/native/swapfile")
        guest.run("echo '/swapfile none swap sw 0 0' > /run/native/etc/fstab")
        guest.run("sync /run/native/etc/fstab")
        guest.run("umount /run/native")
        guest.run("umount /run/source")
        result["cases"].append("native tree and swap prepared")
        guest.stop(); guest = None

        # Provision only the disposable ESP after the preparation VM has exited.
        image = f"{disk}@@1048576"
        subprocess.run(["mmd", "-i", image, "::/EFI", "::/EFI/BOOT"], check=True)
        for source, destination in [(REPO / "build/amd64/uefi/BOOTX64.EFI", "::/EFI/BOOT/BOOTX64.EFI"),
                                    (REPO / "build/amd64/vmunix", "::/vmunix")]:
            subprocess.run(["mcopy", "-i", image, str(source), destination], check=True)
        config = out / "native.cfg"
        config.write_text("kernel=vmunix\nboot0=PARTUUID=" + F["PARTUUID"] +
                          "\nrootpart=PARTUUID=" + ROOT_GUID + "\n")
        subprocess.run(["mcopy", "-i", image, str(config), "::/zedbsd.cfg"], check=True)
        variables = out / "native-vars.fd"
        shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)
        for attempt in range(2):
            guest = B["InstalledGuest"](out / f"native{attempt}", disk, variables)
            guest.login()
            guest.run("mount", r" on / type ufs ")
            if attempt == 0:
                assert "init: swapon -a failed" not in guest.text()
            else:
                assert "init: swapon -a failed" in guest.text()
                assert "/missing-swap" in guest.text()
            # This refusal precedes any manual activation: init activated this file.
            guest.run("truncate -s 0 /swapfile", "Device or resource busy", status=1)
            guest.run("swapoff /swapfile")
            guest.run("swapon -a", status=attempt)
            guest.run("swapon -a", status=attempt)
            guest.run("top -b -n 1", "MiB Swap:")
            if attempt == 0:
                guest.run("echo '/missing-swap none swap sw 0 0' > /etc/fstab")
                guest.run("echo '/swapfile none swap sw 0 0' >> /etc/fstab")
                guest.run("sync /etc/fstab")
            result["console"] = guest.capture_screen("native-swap-console")
            guest.halt_checked()
            guest.stop(); guest = None
            result["cases"].append(f"source-free boot {attempt}, active file swap, repeat activation, halt")
        result["result"] = "PASS native swap startup"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        if guest is not None:
            guest.stop()
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        if result["production_after"] != result["production_sha256"]:
            result["result"] = "FAIL production image changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS native swap startup", result
    print(result["result"])


if __name__ == "__main__":
    main()
