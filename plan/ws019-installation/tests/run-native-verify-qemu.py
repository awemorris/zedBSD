#!/usr/bin/env python3
"""Read-only verification on a disposable clone of an already copied native root."""
import json
from pathlib import Path
import re
import runpy
import sys
import time

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F, REPO = I["F"], I["REPO"]


def main():
    out, previous = [Path(argument).resolve() for argument in sys.argv[1:]]
    for path in [out, previous]:
        path.relative_to(REPO / "plan/ws019-installation/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    F["copy_image"](previous / "gpt.img", out / "gpt.img")
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    guest.deadline = time.monotonic() + 1800
    result["observations"] = []
    try:
        guest.login()
        identity = re.search(r"^kern.boot.root_image: 1:15:(\d+):", guest.run("sysctl kern.boot.root_image"), re.M)
        inventory = guest.run("diskpart --machine list")
        loop = re.search(r"^device\t(\S+)\t" + identity[1] + r"\t0\t9\t", inventory, re.M)
        assert loop, inventory
        guest.run("mkdir /run/source /run/target")
        guest.run(f"mount -t ufs -r {loop[1]} /run/source")
        guest.run("mount -t ufs -r nvme0n1p2 /run/target")
        for command in ["top -b -n 1", "sysctl vfs.cache_memory.stats", "sysctl vfs.bufcache.stats",
                        "diff -r -q --metadata /run/source /run/target",
                        "cat /run/target/bin/sleep > /dev/null", "top -b -n 1",
                        "sysctl vfs.cache_memory.stats", "sysctl vfs.bufcache.stats"]:
            start = len(guest.text())
            guest.send(command + "; echo verify-status-$?")
            text = guest.prompt(start)
            status = re.search(r"^verify-status-(\d+)$", text, re.M)
            assert status, text
            result["observations"].append(dict(command=command, status=int(status[1]), output=text))
        guest.run("umount /run/target")
        guest.run("umount /run/source")
    finally:
        guest.stop()
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        assert result["production_after"] == result["production_sha256"]
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("read-only native verification diagnostics captured")


if __name__ == "__main__":
    main()
