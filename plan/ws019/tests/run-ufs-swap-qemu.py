#!/usr/bin/env python3
"""Native UFS file formatting, swap lifecycle and snapshot exclusion."""
import json
import hashlib
from pathlib import Path
import re
import runpy
import subprocess
import sys

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F, REPO = I["F"], I["REPO"]
outside = runpy.run_path(str(HERE / "run-fat32-command-qemu.py"))["outside"]


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019/temp")
    out.mkdir(parents=True, exist_ok=False)
    sysroot = REPO / "build/amd64/sysroot/usr"
    obj, binary = out / "probe.o", out / "probe"
    subprocess.run([str(REPO / "build/llvm/bin/clang"), "--target=x86_64-unknown-zedbsd",
                    "-nostdinc", "-isystem", str(sysroot / "include"),
                    "-I" + str(REPO / "include/uapi"), "-DKERN_USER_ABI_LP64=1",
                    "-ffreestanding", "-fno-pie", "-O1", "-c",
                    str(HERE / "ufs-snapshot-probe.c"), "-o", str(obj)], check=True)
    subprocess.run(["ld", "-m", "elf_x86_64", "--gc-sections", "-nostdlib", "-static",
                    "-z", "max-page-size=4096", "-z", "stack-size=0x100000",
                    "-T", str(REPO / "platform/amd64/user.ld"), str(sysroot / "lib/crt0.o"),
                    str(sysroot / "lib/libc.o"), str(obj), "-o", str(binary)], check=True)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    disk = out / "gpt.img"
    F["create_nvme"](disk)
    subprocess.run(["mcopy", "-i", f"{disk}@@1048576", str(binary), "::/probe"], check=True)
    result["outside_before"] = outside(disk, 1048576, (2048 + 520192) * 512)
    result["result"] = "FAIL UFS swap admission"
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    try:
        guest.login()
        guest.run("mkdir /run/stage /run/native")
        guest.run("mount -t fat -r nvme0n1p1 /run/stage")
        guest.run("cp /run/stage/probe /run/probe")
        guest.run("umount /run/stage")
        records = guest.run("/sbin/diskpart --machine list")
        match = re.search(r"^device\s+nvme0n1p1\s+(\d+)\s", records, re.M)
        assert match, records
        start = len(guest.text())
        guest.send("mkfs -t ufs --profile=native nvme0n1p1; echo storage-result-$?")
        guest.wait("to continue:", start, 120)
        guest.send("FORMAT nvme0n1p1:" + match[1])
        guest.command_status(start)
        guest.run("mount -t ufs nvme0n1p1 /run/native")
        guest.run("dd if=/dev/zero of=/run/native/swapfile bs=65536 count=32 conv=sync")
        guest.run("sync /run/native/swapfile")
        guest.run("ls -l /run/native/swapfile")
        guest.run("/run/probe diagnose /run/native/swapfile")
        guest.run("mkswap /run/native/swapfile")
        guest.run("ln /run/native/swapfile /run/native/alias")
        guest.run("swapon /run/native/swapfile")
        guest.run("swapon /run/native/alias", status=1)
        guest.run("truncate -s 0 /run/native/swapfile", "Device or resource busy", status=1)
        guest.run("rm /run/native/alias", status=1)
        guest.run("mv /run/native/swapfile /run/native/moved", status=1)
        guest.run("mkswap /run/native/swapfile", status=1)
        guest.run("/run/probe create-busy /run/native", "refusal PASS")
        guest.run("swapoff /run/native/swapfile")
        guest.run("rm /run/native/alias")
        guest.run("/run/probe create /run/native", "operation PASS")
        guest.run("swapon /run/native/swapfile", status=1)
        guest.run("mkswap /run/native/swapfile", status=1)
        guest.run("/run/probe delete /run/native", "operation PASS")
        guest.run("swapon /run/native/swapfile")
        guest.run("swapoff /run/native/swapfile")
        guest.run("truncate -s 0 /run/native/swapfile")
        guest.run("dd if=/dev/zero of=/run/native/hole bs=4096 count=1 seek=512 conv=sync")
        guest.run("sync /run/native/hole")
        guest.run("mkswap /run/native/hole", status=1)
        guest.run("rm /run/native/hole /run/native/swapfile")
        guest.run("umount /run/native")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["result"] = "PASS UFS swap admission"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["outside_after"] = outside(disk, 1048576, (2048 + 520192) * 512)
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        start = F["boot_payload"](out / "boot.img")
        source = subprocess.check_output(["mtype", "-i", f"{out / 'boot.img'}@@{start * 512}", "::/rootfs.img"])
        result["source_after"] = hashlib.sha256(source).hexdigest()
        if (result["outside_after"] != result["outside_before"] or
                result["source_after"] != result["rootfs_sha256"] or
                result["production_after"] != result["production_sha256"]):
            result["result"] = "FAIL unrelated bytes changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS UFS swap admission", result
    print(result["result"])


if __name__ == "__main__":
    main()
