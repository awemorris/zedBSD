#!/usr/bin/env python3
"""Real Noct file transaction on disposable FAT; no installation claim."""
from pathlib import Path
import json
import re
import runpy
import subprocess
import sys
import time
import struct
import uuid
import zlib
import hashlib

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
fixture = runpy.run_path(str(HERE / "run-formatter-qemu.py"))

def add_discovery_payload(path):
    """Add a second FAT32 partition to this disposable five-GiB fixture."""
    with path.open("r+b") as disk:
        disk.seek(512)
        primary = bytearray(disk.read(512))
        backup_lba = struct.unpack_from("<Q", primary, 32)[0]
        disk.seek(1024)
        entries = bytearray(disk.read(16384))
        entries[:16] = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b").bytes_le
        entries[128:144] = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7").bytes_le
        entries[144:160] = uuid.UUID("78190000-2222-4222-8222-222222222222").bytes_le
        struct.pack_into("<QQ", entries, 160, 526336, 526336 + 520192 - 1)
        entries[184:198] = "payload".encode("utf-16le")
        for lba in (1, backup_lba):
            disk.seek(lba * 512)
            header = bytearray(disk.read(512))
            table_lba = struct.unpack_from("<Q", header, 72)[0]
            struct.pack_into("<I", header, 88, zlib.crc32(entries))
            struct.pack_into("<I", header, 16, 0)
            struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
            disk.seek(lba * 512)
            disk.write(header)
            disk.seek(table_lba * 512)
            disk.write(entries)
    image = f"{path}@@{526336 * 512}"
    subprocess.run(["mformat", "-i", image, "-F", "-T", "520192", "-N", "78190002", "-v", "PAYLOAD", "::"], check=True)
    subprocess.run(["mcopy", "-i", image, str(HERE / "README.md"), "::/sentinel.txt"], check=True)

def payload_marker(path):
    with path.open("rb") as disk:
        disk.seek(526336 * 512)
        boot = disk.read(512)
    sentinel = subprocess.check_output(["mtype", "-i", f"{path}@@{526336 * 512}", "::/sentinel.txt"])
    return hashlib.sha256(boot + sentinel).hexdigest()

class TransactionGuest(fixture["FormatterGuest"]):
    def prompt(self, start):
        return self.wait(r"root@[^\s]*:[^\n]*\$ ?$", start, 1100)[0]

def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in ("--capacity-only", "--source-only", "--large-seek", "--metadata", "--discovery")):
        raise SystemExit("usage: run-transaction-qemu.py OUT [--capacity-only|--source-only|--large-seek|--metadata|--discovery]")
    capacity_only = len(sys.argv) == 3 and sys.argv[2] == "--capacity-only"
    discovery = len(sys.argv) == 3 and sys.argv[2] == "--discovery"
    metadata_only = len(sys.argv) == 3 and sys.argv[2] in ("--metadata", "--discovery")
    large_seek = len(sys.argv) == 3 and sys.argv[2] in ("--large-seek", "--metadata", "--discovery")
    source_only = len(sys.argv) == 3 and sys.argv[2] in ("--source-only", "--large-seek", "--metadata", "--discovery")
    success = "PASS managed-file transaction"
    if capacity_only:
        success = "PASS capacity admission"
    if source_only:
        success = "PASS source inspection"
    if large_seek:
        success = "PASS Noct large seek and source inspection"
    if metadata_only:
        success = "PASS retained disk metadata and source inspection"
    if discovery:
        success = "PASS source and destination discovery"
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019/temp")
    out.mkdir(parents=True, exist_ok=False)
    metadata = fixture["prepare_boot"](out, False, False,
        REPO / "build/arch-images/amd64-ws019-transaction.ufs")
    sectors = 10485760 if large_seek else fixture["SECTORS"]
    fixture["create_nvme"](out / "gpt.img", sectors=sectors)
    if discovery:
        add_discovery_payload(out / "gpt.img")
        metadata["payload_before"] = payload_marker(out / "gpt.img")
    image = f"{out / 'gpt.img'}@@1048576"
    for name in ("data.img", "swapfile", "empty", "short"):
        subprocess.run(["mdel", "-i", image, "::/" + name], check=True)
    before = fixture["protected_bytes"](out / "gpt.img")
    guest = TransactionGuest(out, usb_boot=True)
    guest.deadline = time.monotonic() + 1200
    try:
        guest.login()
        guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-admission.noct",
                  "installer admission PASS 576")
        guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-transaction.noct",
                  "installer transaction PASS 102 scenarios")
        if source_only:
            guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-workspace.noct",
                      "installer workspace PASS 10")
            guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-workspace.noct native",
                      "native workspace PASS")
            guest.run("nested-mount", "native nested mount PASS")
            if large_seek:
                guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/noct-large-seek.noct "
                          f"/run/noct-large-seek /dev/nvme0n1 {(sectors - 1) * 512}",
                          "native large GPT seek PASS")
                guest.run("rm /run/noct-large-seek")
            if metadata_only:
                guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-metadata.noct",
                          "installer metadata PASS")
                guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-metadata.noct nvme0n1",
                          "native metadata PASS")
            if discovery:
                guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-destination.noct",
                          "destination contents PASS")
                guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-preflight.noct nvme0n1 nvme0n1p2",
                          "native preflight PASS")
                guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-discovery.noct",
                          "installer discovery PASS")
                guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-discovery.noct nvme0n1 nvme0n1p2",
                          "native discovery PASS")
            guest.run("mkdir /run/q153-esp /run/q153-payload")
            guest.run("mount -t fat -o ro sda1 /run/q153-esp")
            guest.run("mount -t fat -o ro sda2 /run/q153-payload")
            guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-source.noct "
                      "/run/q153-esp/EFI/BOOT/BOOTX64.EFI /run/q153-payload/vmunix "
                      "/run/q153-payload/rootfs.img --capture", "native source capture PASS")
            guest.run("umount /run/q153-payload")
            guest.run("umount /run/q153-esp")
            guest.run("rmdir /run/q153-payload /run/q153-esp")
        else:
            guest.run("mount -t fat nvme0n1p1 /q149")
        if capacity_only:
            guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-admission.noct /q149",
                      "native capacity guard PASS")
        elif not source_only:
            guest.run("noct --path=/usr/lib/zedinst /usr/lib/zedinst/installer-files.noct /q149",
                      "installer files PASS")
            guest.run("swapon /q149/swapfile")
            guest.run("formatter-probe swap /q149/swapfile 1", "slots=16383")
            guest.run("swapoff /q149/swapfile")
        if not source_only:
            guest.run("umount /q149")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        metadata["guest_result"] = success
    except BaseException as error:
        metadata["result"] = "FAIL guest"
        metadata["failure"] = str(error)
        raise
    finally:
        guest.stop()
        metadata["protected_before"] = before
        metadata["protected_after"] = fixture["protected_bytes"](out / "gpt.img")
        metadata["production_sha256_after"] = fixture["digest"](REPO / "build/amd64/hdd-image.img")
        if discovery:
            metadata["payload_after"] = payload_marker(out / "gpt.img")
        (out / "result.json").write_text(json.dumps(metadata, indent=2) + "\n")
    assert before == metadata["protected_after"]
    if discovery:
        assert metadata["payload_before"] == metadata["payload_after"]
    assert metadata["production_sha256"] == metadata["production_sha256_after"]
    metadata["result"] = success
    (out / "result.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(metadata["result"])

if __name__ == "__main__":
    main()
