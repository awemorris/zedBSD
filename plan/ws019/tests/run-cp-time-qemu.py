#!/usr/bin/env python3
"""Prove nonzero timestamp nanoseconds survive native cp and UFS persistence."""
import json
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F = I["F"]
REPO = I["REPO"]
UFS = runpy.run_path(str(REPO / "tools/build/check-ufs-image.py"))["UFS"]
ATIME = (1700000001, 123456789)
MTIME = (1700000002, 987654321)


def times(inode):
    return [[struct.unpack_from("<q", inode, 32)[0], struct.unpack_from("<I", inode, 68)[0]],
            [struct.unpack_from("<q", inode, 40)[0], struct.unpack_from("<I", inode, 64)[0]]]


def main():
    nvme = len(sys.argv) == 3 and sys.argv[2] == "nvme"
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    F["create_nvme"](out / "gpt.img")
    before = F["digest"](out / "gpt.img")
    root = out / "native-root"
    (root / "precision").mkdir(parents=True)
    (root / "precision/file").write_bytes(b"nonzero nanosecond copy fixture")
    (root / "precision/link").symlink_to("file")
    shutil.copyfile(HERE / "installer-treeverify.noct", root / "verify.noct")
    disk = out / ("gpt.img" if nvme else "native.img")
    subprocess.run([str(REPO / "build/zedimage-host"), "ufs", "33554432", str(root), str(disk)], check=True)
    data = bytearray(disk.read_bytes())
    image = UFS(data)
    for path in ("/precision", "/precision/file", "/precision/link"):
        inode = image.inode(image.lookup(path))
        struct.pack_into("<q", inode, 32, ATIME[0])
        struct.pack_into("<I", inode, 68, ATIME[1])
        struct.pack_into("<q", inode, 40, MTIME[0])
        struct.pack_into("<I", inode, 64, MTIME[1])
    disk.write_bytes(data)
    if nvme:
        before = F["digest"](disk)
    extra = [] if nvme else [
        "-drive", f"file={disk},format=raw,if=none,id=native",
        "-device", "usb-storage,drive=native,bus=xhci.0,port=2"]
    guest = I["PublicInstallGuest"](out, usb_boot=True, extra_args=extra)
    result["test_storage"] = "raw NVMe UFS" if nvme else "USB UFS plus protected NVMe"
    result["native_before"] = F["digest"](disk)
    result["result"] = "FAIL native copy timestamp precision"
    try:
        try:
            guest.login()
            guest.run("mkdir /run/native")
            guest.run("mount -t ufs " + ("nvme0n1" if nvme else "sdb") + " /run/native")
            guest.run("/bin/cp -a -T /run/native/precision /run/native/copied")
            guest.run("sync /run/native")
            guest.run("umount /run/native")
            device = "nvme0n1" if nvme else "sdb"
            guest.run("mount -t ufs -o ro " + device + " /run/native")
            guest.run("/bin/noct --path=/lib/zedinst /run/native/verify.noct /run/native/precision /run/native/copied",
                      "installer tree verification PASS")
            guest.run("umount /run/native")
            guest.run("mount -t ufs " + device + " /run/native")
            guest.run("chmod 400 /run/native/copied/file")
            guest.run("sync /run/native")
            guest.run("umount /run/native")
            guest.run("mount -t ufs -o ro " + device + " /run/native")
            guest.run("/bin/diff -r -q --metadata /run/native/precision /run/native/copied",
                      "differ: attributes", status=1)
            guest.run("umount /run/native")
            result["comparison"] = "PASS equal read-only trees and deliberate permission difference"
        finally:
            guest.stop()
        image = UFS(disk.read_bytes(), runtime=True)
        result["copied_times"] = {path: times(image.inode(image.lookup(path)))
                                  for path in ("/copied", "/copied/file", "/copied/link")}
        expected = [list(ATIME), list(MTIME)]
        assert all(value == expected for value in result["copied_times"].values()), result["copied_times"]
        result["result"] = "PASS native copy timestamp precision"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        result["destination_before"] = before
        result["destination_after"] = F["digest"](out / "gpt.img")
        result["production_sha256_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        if (not nvme and before != result["destination_after"]) or result["production_sha256"] != result["production_sha256_after"]:
            result["result"] = "FAIL protected image changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS native copy timestamp precision"
    print(result["result"])


if __name__ == "__main__":
    main()
