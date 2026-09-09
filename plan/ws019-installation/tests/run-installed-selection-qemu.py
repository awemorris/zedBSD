#!/usr/bin/env python3
"""Exercise installed loader selection using disposable disk mutations."""
import json
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys
import uuid
import zlib

HERE = Path(__file__).resolve().parent
BOOT = runpy.run_path(str(HERE / "run-installed-boot-qemu.py"))
F = BOOT["F"]
REPO = BOOT["REPO"]


def distinct_gpt(path):
    with path.open("r+b") as disk:
        disk.seek(512)
        primary = disk.read(512)
        backup = struct.unpack_from("<Q", primary, 32)[0]
        disk.seek(1024)
        entries = bytearray(disk.read(16384))
        for index in range(128):
            offset = index * 128
            if entries[offset:offset + 16] != bytes(16):
                entries[offset + 16:offset + 32] = uuid.UUID(int=0x99190000000040008000000000000000 + index).bytes_le
        for lba in (1, backup):
            disk.seek(lba * 512)
            header = bytearray(disk.read(512))
            table = struct.unpack_from("<Q", header, 72)[0]
            header[56:72] = uuid.UUID("99190000-aaaa-4aaa-8aaa-aaaaaaaaaaaa").bytes_le
            struct.pack_into("<I", header, 88, zlib.crc32(entries))
            struct.pack_into("<I", header, 16, 0)
            struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
            disk.seek(lba * 512)
            disk.write(header)
            disk.seek(table * 512)
            disk.write(entries)


def main():
    accepted = Path(sys.argv[1]).resolve()
    boot_result = Path(sys.argv[2]).resolve()
    out = Path(sys.argv[3]).resolve()
    for path in (accepted, boot_result, out):
        path.relative_to(REPO / "plan/ws019-installation/temp")
    previous = json.loads((boot_result / "result.json").read_text())
    assert previous["result"].startswith("PASS installed boot/persistence;")
    source = accepted / "gpt.img"
    original = F["digest"](source)
    assert original == previous["source_sha256"]
    out.mkdir(parents=True, exist_ok=False)
    result = {"result": "FAIL installed selection", "source_sha256": original, "cases": []}
    try:
        cases = ("absent", "duplicate", "auxiliary")
        if len(sys.argv) == 5:
            assert sys.argv[4] == "--auxiliary-only"
            cases = ("auxiliary",)
        for case in cases:
            directory = out / case
            directory.mkdir()
            disk = directory / "installed.img"
            variables = directory / "vars.fd"
            F["copy_image"](source, disk)
            shutil.copyfile(accepted / "vars.fd", variables)
            extra = []
            if case == "absent":
                subprocess.run(["mdel", "-i", f"{disk}@@{526336 * 512}", "::/zedbsd.cfg"], check=True)
            elif case == "duplicate":
                marker = directory / "zedbsd.cfg"
                marker.write_text("kernel=missing-duplicate-kernel\n")
                subprocess.run(["mcopy", "-i", f"{disk}@@1048576", str(marker), "::/zedbsd.cfg"], check=True)
            else:
                auxiliary = directory / "auxiliary.img"
                F["copy_image"](source, auxiliary)
                distinct_gpt(auxiliary)
                marker = directory / "zedbsd.cfg"
                marker.write_text("kernel=missing-auxiliary-kernel\n")
                subprocess.run(["mcopy", "-o", "-i", f"{auxiliary}@@{526336 * 512}", str(marker), "::/zedbsd.cfg"], check=True)
                extra = ["-device", "qemu-xhci,id=auxusb",
                         "-drive", f"file={auxiliary},format=raw,if=none,id=auxiliary",
                         "-device", "usb-storage,drive=auxiliary,bus=auxusb.0,bootindex=2"]
            guest = BOOT["InstalledGuest"](directory / "guest", disk, variables, extra)
            try:
                if case == "absent":
                    guest.wait("zedbsd.cfg was not found on the boot disk", timeout=120)
                    assert "A64 UEFI ELF" not in guest.text()
                elif case == "duplicate":
                    guest.wait("A64 KERNEL missing-duplicate-kernel", timeout=120)
                    assert "WARNING: multiple zedbsd.cfg files on the boot disk; using the first" in guest.text()
                    assert "A64 CFG SELECTED 0x0000000000000000" in guest.text()
                else:
                    guest.login()
                    guest.check_root()
                    assert "A64 CFG OTHER DISK" in guest.text()
                    assert "A64 KERNEL missing-auxiliary-kernel" not in guest.text()
                    guest.halt_checked()
                result["cases"].append(case)
            finally:
                guest.stop()
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        result["source_sha256_after"] = F["digest"](source)
        result["production_sha256_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert original == result["source_sha256_after"]
    assert previous["production_sha256"] == result["production_sha256_after"]
    result["result"] = "PASS installed selection"
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["result"])


if __name__ == "__main__":
    main()
