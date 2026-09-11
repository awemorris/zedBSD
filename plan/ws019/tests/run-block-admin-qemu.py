#!/usr/bin/env python3
"""Exercise fd-owned whole-disk admission on disposable QEMU media."""
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
    sysroot = REPO / "build/amd64/sysroot/usr"
    obj, binary = out / "probe.o", out / "probe"
    subprocess.run([str(REPO / "build/llvm/bin/clang"),
                    "--target=x86_64-unknown-zedbsd", "-nostdinc", "-isystem", str(sysroot / "include"),
                    "-I" + str(REPO / "include/uapi"), "-DKERN_USER_ABI_LP64=1", "-ffreestanding", "-fno-pie", "-O1",
                    "-c", str(HERE / "block-admin-probe.c"), "-o", str(obj)], check=True)
    subprocess.run(["ld", "-m", "elf_x86_64", "--gc-sections", "-nostdlib", "-static",
                    "-z", "max-page-size=4096", "-z", "stack-size=0x100000",
                    "-T", str(REPO / "platform/amd64/user.ld"), str(sysroot / "lib/crt0.o"),
                    str(sysroot / "lib/libc.o"), str(obj), "-o", str(binary)], check=True)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    F["create_nvme"](out / "gpt.img")
    subprocess.run(["mcopy", "-i", f"{out / 'gpt.img'}@@1048576", str(binary), "::/probe"], check=True)
    result["target_before"] = F["digest"](out / "gpt.img")
    result["result"] = "FAIL block administration"
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    try:
        guest.login()
        guest.run("mkdir /run/admin-source")
        guest.run("mount -t fat -o ro nvme0n1p1 /run/admin-source")
        guest.run("cp /run/admin-source/probe /run/block-admin-probe")
        guest.run("chmod 755 /run/block-admin-probe")
        guest.run("/run/block-admin-probe busy /dev/nvme0n1 /dev/nvme0n1p1", "block-admin busy PASS")
        guest.run("/run/block-admin-probe busy /dev/sda /dev/sda2", "block-admin busy PASS")
        guest.run("umount /run/admin-source")
        guest.run("/run/block-admin-probe exercise /dev/nvme0n1 /dev/nvme0n1p1", "block-admin exercise PASS")
        guest.run("mount -t fat -o ro nvme0n1p1 /run/admin-source")
        guest.run("cat /run/admin-source/sentinel.txt", "WS019")
        guest.run("umount /run/admin-source")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["result"] = "PASS block administration"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["target_after"] = F["digest"](out / "gpt.img")
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        if result["target_before"] != result["target_after"] or result["production_sha256"] != result["production_after"]:
            result["result"] = "FAIL protected image changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS block administration", result
    print(result["result"])


if __name__ == "__main__":
    main()
