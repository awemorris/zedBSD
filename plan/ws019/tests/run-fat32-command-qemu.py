#!/usr/bin/env python3
"""Public reserved mkfs, native FAT mount and independent host file reading."""
import hashlib
import json
from pathlib import Path
import re
import runpy
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F, REPO = I["F"], I["REPO"]


def outside(path, first, end):
    h = hashlib.sha256()
    with path.open("rb") as f:
        h.update(f.read(first))
        f.seek(end)
        while data := f.read(1048576):
            h.update(data)
    return h.hexdigest()


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    disk = out / "gpt.img"
    F["create_nvme"](disk, sectors=1048576)
    with disk.open("rb") as f:
        f.seek(1024 + 32)
        start, last = struct.unpack("<QQ", f.read(16))
    result["before"] = F["digest"](disk)
    result["outside_before"] = outside(disk, start * 512, (last + 1) * 512)
    result["result"] = "FAIL FAT32 command"
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    try:
        guest.login()
        records = guest.run("/sbin/diskpart --machine list")
        match = re.search(r"^device\s+nvme0n1p1\s+(\d+)\s", records, re.M)
        assert match, records
        registration = match[1]
        guest.run("ls -l /bin/cp")
        guest.run("umask")
        guest.run("type cp")
        guest.run("mkdir /run/fat32-test")
        guest.run("mount -t fat -o ro nvme0n1p1 /run/fat32-test")
        guest.run("/sbin/mkfs -t fat32 nvme0n1p1", "no format writes attempted", status=1)
        guest.run("umount /run/fat32-test")
        guest.run("/sbin/mkfs -t fat32 sda", "no format writes attempted", status=1)
        for answer, status in [("NO", 1), ("FORMAT nvme0n1p1:" + registration, 0)]:
            position = len(guest.text())
            guest.send("/sbin/mkfs -t fat32 nvme0n1p1; echo storage-result-$?")
            guest.wait("to continue:", position, 120)
            guest.send(answer)
            text = guest.command_status(position, status)
            if status:
                assert "no format writes attempted" in text
                assert F["digest"](disk) == result["before"]
                result["cancel_unchanged"] = True
            else:
                assert "fat32 initialized and verified" in text
        guest.run("mount -t fat nvme0n1p1 /run/fat32-test")
        guest.run("mkdir /run/fat32-test/EFI")
        guest.run("cp /bin/cp /run/fat32-test/EFI/cp")
        guest.run("diff -q /bin/cp /run/fat32-test/EFI/cp")
        guest.run("/run/fat32-test/EFI/cp /bin/cp /run/fat32-test/EFI/copied")
        guest.run("umount /run/fat32-test")
        guest.run("mount -t fat -o ro nvme0n1p1 /run/fat32-test")
        guest.run("diff -q /bin/cp /run/fat32-test/EFI/cp")
        guest.run("umount /run/fat32-test")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["result"] = "PASS FAT32 command"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["outside_after"] = outside(disk, start * 512, (last + 1) * 512)
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        if result["outside_before"] != result["outside_after"] or result["production_sha256"] != result["production_after"]:
            result["result"] = "FAIL unrelated bytes changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS FAT32 command", result
    subprocess.run(["mdir", "-i", f"{disk}@@{start * 512}", "::/EFI"], check=True)
    subprocess.run(["mcopy", "-i", f"{disk}@@{start * 512}", "::/EFI/cp", str(out / "copied-cp")], check=True)
    assert (out / "copied-cp").read_bytes() == (REPO / "build/amd64/rootfs/bin/cp").read_bytes()
    result["mtools_read"] = "PASS"
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["result"])


if __name__ == "__main__":
    main()
