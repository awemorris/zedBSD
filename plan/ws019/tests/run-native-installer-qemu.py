#!/usr/bin/env python3
"""Run the public text installer on a blank disposable disk, then boot that disk."""
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
B = runpy.run_path(str(HERE / "run-installed-boot-qemu.py"))
F, REPO = I["F"], I["REPO"]


def key(guest, name):
    guest.proc.stdin.write("sendkey " + name + " 50\n")
    guest.proc.stdin.flush()
    time.sleep(0.15)


def review(guest, result, case):
    start = len(guest.text())
    guest.send("zedinst; echo storage-result-$?")
    guest.source_continue(start)
    guest.wait("Coexist with an existing FAT filesystem", start, 120)
    key(guest, "down")
    time.sleep(0.2)
    key(guest, "ret")
    guest.wait("The installation source is excluded.", start, 180)
    time.sleep(1)
    result["screenshots"].append(guest.capture_screen(case + "-disk"))
    key(guest, "ret")
    text, _ = guest.wait(r"NO - Cancel|root@[^\s]*:[^\n]*\$ ?$", start, 1200)
    assert "NO - Cancel" in text, text
    time.sleep(1)
    result["screenshots"].append(guest.capture_screen(case + "-review"))
    return start


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    disk = out / "gpt.img"
    with disk.open("xb") as stream:
        stream.truncate(1024 * 1024 * 1024)
    result.update(result="FAIL public native installer", cases=[], screenshots=[])
    before = F["digest"](disk)
    guest = None
    try:
        guest = I["PublicInstallGuest"](out, usb_boot=True)
        guest.deadline = time.monotonic() + 5400
        guest.login()
        guest.run("zedinst < /dev/null", "interactive terminal", status=1)
        if len(sys.argv) == 2:
            start = review(guest, result, "cancel")
            key(guest, "ret")
            text = guest.command_status(start)
            assert "Installation cancelled." in text, text
            assert F["digest"](disk) == before, "default NO modified the blank target"
            result["cases"].append("default NO preserves the blank destination")
        else:
            assert sys.argv[2:] == ["--install-only"], "unknown test option"
            result["cases"].append("diagnostic run: cancellation coverage omitted explicitly")
        start = review(guest, result, "install")
        key(guest, "down")
        time.sleep(0.2)
        key(guest, "ret")
        text, _ = guest.wait(r"Files copied:|root@[^\s]*:[^\n]*\$ ?$", start, 1200)
        assert "Files copied:" in text, text
        result["screenshots"].append(guest.capture_screen("install-copy"))
        text, _ = guest.wait(r"Installation complete\.|root@[^\s]*:[^\n]*\$ ?$", start, 3600)
        assert "Installation complete." in text, text
        guest.command_status(start)
        result["screenshots"].append(guest.capture_screen("install-complete"))
        guest.run("mount")
        guest.run("ls /run")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["cases"].append("public dedicated installer completes")
        guest.stop()
        guest = None

        # No host-side provisioning: the exact installer-created disk must boot by itself.
        variables = out / "native-vars.fd"
        shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)
        for attempt in range(2):
            guest = B["InstalledGuest"](out / f"native{attempt}", disk, variables)
            guest.login()
            guest.run("mount", r" on / type ufs ")
            assert "init: swapon -a failed" not in guest.text()
            guest.run("truncate -s 0 /swapfile", "Device or resource busy", status=1)
            if attempt == 0:
                guest.run("echo native-installer-persistence > /etc/installer-check")
                guest.run("sync /etc/installer-check")
            else:
                guest.run("cat /etc/installer-check", "native-installer-persistence")
            guest.halt_checked()
            guest.stop()
            guest = None
            result["cases"].append(f"source-free native boot {attempt}, active swap, persistence and halt")
        result["result"] = "PASS public native installer"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        if guest is not None:
            guest.stop()
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        start = F["boot_payload"](out / "boot.img")
        source = subprocess.check_output(["mtype", "-i", f"{out / 'boot.img'}@@{start * 512}", "::/rootfs.img"])
        result["source_after"] = hashlib.sha256(source).hexdigest()
        if result["source_after"] != result["rootfs_sha256"]:
            result["result"] = "FAIL installation source changed"
        if result["production_after"] != result["production_sha256"]:
            result["result"] = "FAIL production image changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS public native installer", result
    print(result["result"])


if __name__ == "__main__":
    main()
