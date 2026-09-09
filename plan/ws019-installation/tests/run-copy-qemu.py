#!/usr/bin/env python3
"""Isolate installer-sized FAT USB-to-NVMe copies, keeping the command bound."""
import json
from pathlib import Path
import runpy
import sys
import time

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F = I["F"]
ROOT = I["REPO"]
out = Path(sys.argv[1]).resolve()
out.relative_to(ROOT / "plan/ws019-installation/temp")
out.mkdir(exist_ok=False)
result = F["prepare_boot"](out, False, False,
                          ROOT / "build/arch-images/amd64.ufs")
F["create_nvme"](out / "gpt.img")
guest = I["PublicInstallGuest"](out, usb_boot=True)
guest.deadline = time.monotonic() + 1200
result["result"] = "FAIL"
result["copies"] = []
try:
    guest.login()
    guest.run("mkdir /run/from")
    guest.run("mkdir /run/to")
    guest.run("mount -t fat -o ro sda2 /run/from")
    guest.run("mount -t fat nvme0n1p1 /run/to")
    guest.run("cp -T --attributes-only --preserve=mode /bin/sh /run/seed")
    guest.run("chmod 755 /run/seed")
    for name in ("vmunix", "rootfs.img"):
        source = "/run/from/" + name
        target = "/run/to/" + name
        guest.run("cp -T --attributes-only --preserve=mode /run/seed " + target)
        guest.run("stat -c %s " + source)
        started = time.monotonic()
        guest.run("timeout -k 2 180 cp -T " + source + " " + target)
        result["copies"].append({"file": name, "seconds": time.monotonic() - started})
        guest.run("cmp " + source + " " + target)
        guest.run("sync " + target)
    guest.run("umount /run/to")
    guest.run("umount /run/from")
    result["result"] = "PASS"
except BaseException as error:
    result["failure"] = str(error)
    raise
finally:
    guest.stop()
    result["production_sha256_after"] = F["digest"](ROOT / "build/amd64/hdd-image.img")
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
