#!/usr/bin/env python3
"""Partition reservation with a mounted FAT sibling and preserved target."""
import hashlib
import json
from pathlib import Path
import re
import runpy
import struct
import subprocess
import sys
import uuid
import zlib

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F, REPO = I["F"], I["REPO"]
SECTORS, START = 1048576, 524288
COUNT = SECTORS - 33 - START


def selected_hash(path, offset, count):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        stream.seek(offset)
        while count:
            data = stream.read(min(count, 1048576)); assert data
            digest.update(data); count -= len(data)
    return digest.hexdigest()


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019-installation/temp")
    out.mkdir(parents=True, exist_ok=False)
    sysroot = REPO / "build/amd64/sysroot/usr"
    obj, binary = out / "probe.o", out / "probe"
    subprocess.run([str(REPO / "build/llvm/bin/clang"), "--target=x86_64-unknown-zedbsd",
                    "-nostdinc", "-isystem", str(sysroot / "include"), "-I" + str(REPO / "include/uapi"),
                    "-DZEDBSD_USER_ABI_LP64=1", "-ffreestanding", "-fno-pie", "-O1", "-c", str(HERE / "partition-admin-probe.c"),
                    "-o", str(obj)], check=True)
    subprocess.run(["ld", "-m", "elf_x86_64", "--gc-sections", "-nostdlib", "-static",
                    "-z", "max-page-size=4096", "-z", "stack-size=0x100000", "-T", str(REPO / "platform/amd64/user.ld"),
                    str(sysroot / "lib/crt0.o"), str(sysroot / "lib/libc.o"), str(obj), "-o", str(binary)], check=True)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    disk = out / "gpt.img"
    F["create_nvme"](disk, sectors=SECTORS)
    with disk.open("r+b") as stream:
        stream.seek(1024); entries = bytearray(stream.read(16384))
        entries[128:144] = uuid.UUID("516e7cb6-6ecf-11d6-8ff8-00022d09712b").bytes_le
        entries[144:160] = uuid.UUID("78190000-3333-4333-8333-555555555555").bytes_le
        struct.pack_into("<QQ", entries, 160, START, START + COUNT - 1)
        for lba, table in [(1, 2), (SECTORS - 1, SECTORS - 33)]:
            stream.seek(lba * 512); header = bytearray(stream.read(512))
            struct.pack_into("<I", header, 88, zlib.crc32(entries))
            struct.pack_into("<I", header, 16, 0)
            struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
            stream.seek(lba * 512); stream.write(header)
            stream.seek(table * 512); stream.write(entries)
    subprocess.run(["mcopy", "-i", f"{disk}@@1048576", str(binary), "::/probe"], check=True)
    result["partition_before"] = selected_hash(disk, START * 512, COUNT * 512)
    result["head_before"] = selected_hash(disk, 0, 34 * 512)
    result["tail_before"] = selected_hash(disk, (SECTORS - 33) * 512, 33 * 512)
    result["result"] = "FAIL partition administration"
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    try:
        guest.login()
        guest.run("mkdir /run/sibling")
        guest.run("mount -t fat -o ro nvme0n1p1 /run/sibling")
        guest.run("cp /run/sibling/probe /run/partition-admin-probe")
        guest.run("chmod 755 /run/partition-admin-probe")
        guest.run("/run/partition-admin-probe /dev/nvme0n1p2 /dev/nvme0n1 /run/sibling ro", "partition-admin ro PASS")
        guest.run("umount /run/sibling")
        guest.run("mount -t fat nvme0n1p1 /run/sibling")
        guest.run("/run/partition-admin-probe /dev/nvme0n1p2 /dev/nvme0n1 /run/sibling rw", "partition-admin rw PASS")
        guest.run("cat /run/sibling/admin-proof", "disjoint sibling write PASS")
        guest.run("umount /run/sibling")
        guest.run("/sbin/diskpart reload nvme0n1", "Kernel partition devices reloaded")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["result"] = "PASS partition administration"
    except BaseException as error:
        result["failure"] = str(error); raise
    finally:
        guest.stop()
        result["partition_after"] = selected_hash(disk, START * 512, COUNT * 512)
        result["head_after"] = selected_hash(disk, 0, 34 * 512)
        result["tail_after"] = selected_hash(disk, (SECTORS - 33) * 512, 33 * 512)
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        if any(result[name + "_before"] != result[name + "_after"] for name in ("partition", "head", "tail")) or result["production_sha256"] != result["production_after"]:
            result["result"] = "FAIL protected regions changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS partition administration", result
    print(result["result"])


if __name__ == "__main__":
    main()
