#!/usr/bin/env python3
"""Verify shell status, interpreter placement and pseudo-devices in a normal image."""
import hashlib
import json
from pathlib import Path
import re
import runpy
import sys

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
result["result"] = "FAIL"
try:
    guest.login()
    guest.run("ls -l /bin/noct", "/bin/noct")
    guest.run("ls -l /dev/null", r"^crw-rw-rw-")
    guest.run("ls -l /dev/zero", r"^crw-rw-rw-")
    guest.run("sh -c 'exit 42'", status=42)
    guest.run("sh -c 'exit 7' | cat /dev/null")
    guest.run("cat /dev/null | sh -c 'exit 7'", status=7)
    guest.run("echo discarded > /dev/null")
    guest.run("cat /dev/null")
    guest.run("dd if=/dev/zero of=/run/zeros bs=512 count=128")
    zero_digest = hashlib.sha256(bytes(65536)).hexdigest()
    guest.run("cksum -a sha256 /run/zeros", zero_digest)
    guest.run("echo false > /run/status.sh")
    guest.run("echo 'echo $?; exit 42' >> /run/status.sh")
    guest.run("sh /run/status.sh", r"^1$", status=42)
    guest.run("zedinst", "usage: zedinst", status=1)
    guest.run("zedinst nvme0n1 nvme0n1p1 < /dev/null",
              "interactive terminal", status=1)
    guest.run("rm /run/status.sh /run/zeros")
    assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
    result["result"] = "PASS"
    result["cases"] = 16
except BaseException as error:
    result["failure"] = str(error)
    raise
finally:
    guest.stop()
    result["production_sha256_after"] = F["digest"](ROOT / "build/amd64/hdd-image.img")
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
assert result["production_sha256_after"] == result["production_sha256"]
print(json.dumps(result, indent=2))
