#!/usr/bin/env python3
"""Verify an independent immutable mount of the live boot lower image."""
import hashlib
import json
from pathlib import Path
import re
import runpy
import subprocess
import sys

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F, REPO = I["F"], I["REPO"]


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    F["create_nvme"](out / "gpt.img")
    result["target_before"] = F["digest"](out / "gpt.img")
    expected = F["digest"](REPO / "build/amd64/rootfs/bin/noct")
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    result["result"] = "FAIL root image source view"
    try:
        guest.login()
        observed = guest.run("/sbin/sysctl kern.boot.root_image")
        match = re.search(r"^kern.boot.root_image: 1:15:(\d+):(\d+):(\d+):(\d+)$", observed, re.M)
        assert match, observed
        loop, backing, inode, size = map(int, match.groups())
        result["root_image"] = [loop, backing, inode, size]
        records = guest.run("/sbin/diskpart --machine list")
        matches = [m for m in re.finditer(r"^device\t(\S+)\t(\d+)\t(\d+)\t(\d+)\t(\d+)\t(\d+)\t(\d+)$", records, re.M)
                   if int(m[2]) == loop]
        assert len(matches) == 1, records
        device = matches[0]
        assert int(device[3]) == 0 and int(device[4]) & 1, device.groups()
        assert int(device[5]) * int(device[6]) == size and int(device[7]) == 0
        name = device[1]
        assert re.fullmatch(r"[A-Za-z0-9_-]+", name)
        guest.run("mkdir -m 700 /run/root-view")
        guest.run("mkdir -m 700 /run/root-view/source")
        guest.run("echo overlay-only > /zedinst-view-marker")
        for attempt in range(2):
            guest.run(f"mount -t ufs -r {name} /run/root-view/source")
            stat = guest.run("/bin/stat -c %d /run/root-view/source")
            assert re.search(rf"^{loop}$", stat, re.M), stat
            guest.run("/bin/cksum -a sha256 /run/root-view/source/bin/noct", expected)
            guest.run("touch /run/root-view/source/zedinst-write-probe", status=1)
            guest.run("/bin/stat /run/root-view/source/zedinst-view-marker", status=1)
            guest.run("umount /run/root-view/source")
            after = guest.run("/sbin/sysctl kern.boot.root_image")
            assert match[0] in after, after
            guest.run("cat /zedinst-view-marker", "overlay-only")
        guest.run("rmdir /run/root-view/source /run/root-view")
        guest.run("rm /zedinst-view-marker")
        start = len(guest.text())
        guest.send("zedinst nvme0n1 nvme0n1p1; echo storage-result-$?")
        guest.wait("Enter: Continue    Esc: Cancel", start, 120)
        result["screenshot"] = guest.capture_screen("installer-source")
        guest.proc.stdin.write("sendkey esc\n")
        guest.proc.stdin.flush()
        assert "Installation cancelled." in guest.command_status(start)
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["result"] = "PASS root image source view"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        result["target_after"] = F["digest"](out / "gpt.img")
        start = F["boot_payload"](out / "boot.img")
        image = subprocess.check_output(["mtype", "-i", f"{out / 'boot.img'}@@{start * 512}", "::/rootfs.img"])
        result["source_after"] = hashlib.sha256(image).hexdigest()
        if (result["production_after"] != result["production_sha256"] or
                result["source_after"] != result["rootfs_sha256"] or
                result["target_after"] != result["target_before"]):
            result["result"] = "FAIL protected input changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS root image source view", result
    print(result["result"])


if __name__ == "__main__":
    main()
