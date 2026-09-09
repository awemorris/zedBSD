#!/usr/bin/env python3
"""Public native mkfs on a 4-GiB partition, metadata copy and remount."""
import json
from pathlib import Path
import re
import runpy
import struct
import sys
import uuid
import zlib

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F, REPO = I["F"], I["REPO"]
outside = runpy.run_path(str(HERE / "run-fat32-command-qemu.py"))["outside"]
START, COUNT = 524288, 8388608
SECTORS = START + COUNT + 2048


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019-installation/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    disk = out / "gpt.img"
    F["create_nvme"](disk, sectors=SECTORS)
    with disk.open("r+b") as f:
        f.seek(1024); entries = bytearray(f.read(16384))
        entries[128:144] = uuid.UUID("516e7cb6-6ecf-11d6-8ff8-00022d09712b").bytes_le
        entries[144:160] = uuid.UUID("78190000-3333-4333-8333-777777777777").bytes_le
        struct.pack_into("<QQ", entries, 160, START, START + COUNT - 1)
        for lba, table in [(1, 2), (SECTORS - 1, SECTORS - 33)]:
            f.seek(lba * 512); header = bytearray(f.read(512))
            struct.pack_into("<I", header, 88, zlib.crc32(entries))
            struct.pack_into("<I", header, 16, 0)
            struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
            f.seek(lba * 512); f.write(header)
            f.seek(table * 512); f.write(entries)
    result["layout_before"] = outside(disk, 2048 * 512, (START + COUNT) * 512)
    result["result"] = "FAIL native UFS command"
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    try:
        guest.login()
        records = guest.run("/sbin/diskpart --machine list")
        match = re.search(r"^device\s+nvme0n1p2\s+(\d+)\s+\d+\s+4\s+512\s+8388608\s+524288$", records, re.M)
        assert match, records
        guest.run("/sbin/mkfs -t ufs --profile=native sda", "no format writes attempted", status=1)
        # Prepare a separate immutable source through the public native formatter.
        source = re.search(r"^device\s+nvme0n1p1\s+(\d+)\s", records, re.M)
        assert source, records
        position = len(guest.text())
        guest.send("/sbin/mkfs -t ufs --profile=native nvme0n1p1; echo storage-result-$?")
        guest.wait("to continue:", position, 120)
        guest.send("FORMAT nvme0n1p1:" + source[1])
        guest.command_status(position, 0)
        guest.run("mkdir /run/source /run/native")
        guest.run("mount -t ufs nvme0n1p1 /run/source")
        guest.run("mkdir /run/source/nested")
        guest.run("echo native-copy > '/run/source/space name'")
        guest.run("touch /run/source/empty")
        guest.run("ln '/run/source/space name' /run/source/nested/hard")
        guest.run("ln -s '../space name' /run/source/nested/symbolic")
        guest.run("chown 17:23 '/run/source/space name'")
        guest.run("chmod 6751 '/run/source/space name'")
        guest.run("umount /run/source")
        guest.run("mount -t ufs -o ro nvme0n1p1 /run/source")
        result["before"] = F["digest"](disk)
        result["outside_before"] = outside(disk, START * 512, (START + COUNT) * 512)
        result["source_fixture"] = "Public native UFS, frozen read-only before target test"
        command = "/sbin/mkfs -t ufs --profile=native nvme0n1p2"
        for answer, status in [("NO", 1), ("FORMAT nvme0n1p2:" + match[1], 0)]:
            position = len(guest.text())
            guest.send(command + "; echo storage-result-$?")
            guest.wait("to continue:", position, 120)
            guest.send(answer)
            text = guest.command_status(position, status)
            if status:
                assert F["digest"](disk) == result["before"]
                result["cancel_unchanged"] = True
            else:
                assert "ufs native initialized and verified (4294967296 bytes)" in text
        guest.run("mount -t ufs nvme0n1p2 /run/native")
        listing = guest.run("/bin/find /run/native -print")
        assert re.findall(r"^/run/native[^\n]*$", listing, re.M) == ["/run/native"], listing
        guest.run(command, "no format writes attempted", status=1)
        result["source_before_copy"] = guest.run("/bin/stat '/run/source/space name'")
        guest.run("cp -a /run/source /run/native/copied")
        result["source_after_copy"] = guest.run("/bin/stat '/run/source/space name'")
        result["copied_attributes"] = guest.run("/bin/stat '/run/native/copied/space name'")
        guest.run("df /run/native")
        guest.run("umount /run/native")
        guest.run("umount /run/source")
        guest.run("mount -t ufs -o ro nvme0n1p1 /run/source")
        guest.run("mount -t ufs -o ro nvme0n1p2 /run/native")
        guest.run("/bin/diff -r -q --metadata /run/source /run/native/copied")
        guest.run(command, "no format writes attempted", status=1)
        guest.run("umount /run/native")
        guest.run("umount /run/source")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["result"] = "PASS native UFS command"
    except BaseException as error:
        result["failure"] = str(error); raise
    finally:
        guest.stop()
        result["outside_after"] = outside(disk, START * 512, (START + COUNT) * 512)
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        result["layout_after"] = outside(disk, 2048 * 512, (START + COUNT) * 512)
        if result.get("outside_before", result["outside_after"]) != result["outside_after"] or result["layout_before"] != result["layout_after"] or result["production_sha256"] != result["production_after"]:
            result["result"] = "FAIL unrelated bytes changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS native UFS command", result
    print(result["result"])


if __name__ == "__main__":
    main()
