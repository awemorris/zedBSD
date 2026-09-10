#!/usr/bin/env python3
"""Exercise real paging on a clone made by the public native installer."""
import json
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
B = runpy.run_path(str(HERE / "run-installed-boot-qemu.py"))
F, REPO = B["F"], B["REPO"]


def main():
    out, accepted = [Path(value).resolve() for value in sys.argv[1:]]
    out.relative_to(REPO / "plan/ws019-installation/temp")
    accepted.relative_to(REPO / "plan/ws019-installation/temp")
    assert json.loads((accepted / "result.json").read_text())["result"] == "PASS public native installer"
    out.mkdir(parents=True, exist_ok=False)
    reference = accepted / "gpt.img"
    result = {"result": "FAIL native UFS paging", "reference_before": F["digest"](reference), "cases": []}
    disk = out / "installed.img"
    shutil.copyfile(reference, disk)
    variables = out / "vars.fd"
    shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)
    sysroot = REPO / "build/amd64/sysroot/usr"
    obj, binary = out / "probe.o", out / "probe"
    subprocess.run([str(REPO / "build/llvm/bin/clang"), "--target=x86_64-unknown-zedbsd",
        "-nostdinc", "-isystem", str(sysroot / "include"), "-I" + str(REPO / "include/uapi"),
        "-DZEDBSD_USER_ABI_LP64=1", "-ffreestanding", "-fno-pie", "-O1",
        "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror", "-c",
        str(HERE / "native-swap-pressure.c"), "-o", str(obj)], check=True)
    subprocess.run(["ld", "-m", "elf_x86_64", "--gc-sections", "-nostdlib", "-static",
        "-z", "max-page-size=4096", "-z", "stack-size=0x100000", "-T",
        str(REPO / "platform/amd64/user.ld"), str(sysroot / "lib/crt0.o"),
        str(sysroot / "lib/libc.o"), str(obj), "-o", str(binary)], check=True)
    # Instrument only the disposable ESP; leave the installer-created root untouched.
    subprocess.run(["mcopy", "-i", f"{disk}@@1048576", str(binary), "::/q183-probe"], check=True)
    guest = B["InstalledGuest"](out / "guest", disk, variables, memory_mib=128)
    guest.deadline = time.monotonic() + 3600
    try:
        guest.login()
        guest.run("mount", r" on / type ufs ")
        identity = guest.run("/bin/stat -c %d /root /swapfile")
        devices = re.findall(r"^([0-9]+)$", identity, re.M)
        assert len(devices) == 2 and devices[0] == devices[1], "fragment destination is not on native root"
        guest.run("mkdir /run/probe-esp")
        guest.run("mount -t fat -r nvme0n1p1 /run/probe-esp")
        guest.run("cp /run/probe-esp/q183-probe /run/probe")
        guest.run("chmod 700 /run/probe")
        guest.run("umount /run/probe-esp")
        guest.run("rmdir /run/probe-esp")

        def probe(command, expected):
            start = len(guest.text())
            guest.send(command + "; echo storage-result-$?")
            text, _ = guest.wait(r"root@[^\s]*:[^\n]*\$ ?$", start, 1200)
            assert expected in text and re.search(r"^storage-result-0$", text, re.M), text

        probe("/run/probe /swapfile", "NATIVE SWAP PRESSURE PASS source=/swapfile")
        result["cases"].append("installed UFS swap: three paging/readback/release/reactivation rounds")
        probe("/run/probe prepare /root/native-fragmented.swap /root/native-fragment-gap",
              "NATIVE SWAP fragmented files prepared")
        guest.run("swapoff /swapfile")
        guest.run("mkswap /root/native-fragmented.swap")
        guest.run("swapon /root/native-fragmented.swap")
        probe("/run/probe /root/native-fragmented.swap fragmented",
              "NATIVE SWAP PRESSURE PASS source=/root/native-fragmented.swap")
        result["cases"].append("fragmented UFS file: at least 32 extents and three paging rounds")
        guest.run("swapoff /root/native-fragmented.swap")
        guest.run("rm /root/native-fragmented.swap /root/native-fragment-gap")
        guest.run("swapon /swapfile")
        guest.run("cat /etc/installer-check", "native-installer-persistence")
        guest.halt_checked()
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["result"] = "PASS native UFS paging"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["reference_after"] = F["digest"](reference)
        if result["reference_after"] != result["reference_before"]:
            result["result"] = "FAIL accepted installation changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS native UFS paging", result
    print(result["result"])


if __name__ == "__main__":
    main()
