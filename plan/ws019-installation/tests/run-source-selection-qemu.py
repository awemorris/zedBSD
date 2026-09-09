#!/usr/bin/env python3
"""Prove live private-root admission or native-root refusal through the text UI."""
import json
from pathlib import Path
import re
import runpy
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F = I["F"]
REPO = I["REPO"]


def main():
    out = Path(sys.argv[1]).resolve()
    native = len(sys.argv) == 3 and sys.argv[2] == "native"
    out.relative_to(REPO / "plan/ws019-installation/temp")
    out.mkdir(parents=True, exist_ok=False)
    rootfs = REPO / "build/arch-images/amd64.ufs"
    result = F["prepare_boot"](out, False, native, rootfs)
    F["create_nvme"](out / "gpt.img", sectors=10485760)
    if native:
        F["copy_image"](out / "mbr.img", out / "gpt.img")
        config = out / "native.cfg"
        config.write_text("kernel=vmunix\nrootpart=nvme0n1p1\n")
        start = F["boot_payload"](out / "boot.img")
        subprocess.run(["mcopy", "-o", "-i", f"{out / 'boot.img'}@@{start * 512}", str(config), "::/zedbsd.cfg"], check=True)
    else:
        I["BASE"]["add_discovery_payload"](out / "gpt.img")
    protected = out / ("boot.img" if native else "gpt.img")
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    guest.deadline = time.monotonic() + 1800
    result["result"] = "FAIL source selection"
    try:
        guest.login()
        before = F["digest"](protected)
        result["protected_before"] = before
        observed = guest.run("/sbin/sysctl kern.boot.root_image")
        match = re.search(r"^kern.boot.root_image: (\d+):(\d+):(\d+):(\d+):(\d+):(\d+)$", observed, re.M)
        assert match, observed
        fields = list(map(int, match.groups()))
        assert fields[0] == 1 and fields[1] == (0 if native else 15), fields
        result["root_image"] = fields
        guest.run("/sbin/sysctl kern.boot.root_image=0", status=1)
        if not native:
            guest.run("mkdir /run/source-inspect")
            guest.run("mount -t fat -o ro sda2 /run/source-inspect")
            identity = guest.run("/bin/stat -c %d:%i:%s /run/source-inspect/rootfs.img")
            assert re.search(rf"^{fields[3]}:{fields[4]}:{fields[5]}$", identity, re.M), identity
            guest.run("umount /run/source-inspect")
            guest.run("rmdir /run/source-inspect")
        command = "zedinst sda sda2" if native else "zedinst nvme0n1 nvme0n1p2"
        start = len(guest.text())
        guest.send(command + "; echo storage-result-$?")
        guest.wait("Enter: Continue    Esc: Cancel", start, 120)
        result["source_screen"] = guest.capture_screen("installer-source")
        guest.proc.stdin.write("sendkey esc\n")
        guest.proc.stdin.flush()
        cancelled = guest.command_status(start)
        assert "Installation cancelled." in cancelled, cancelled
        start = len(guest.text())
        guest.send(command + "; echo storage-result-$?")
        guest.source_continue(start)
        if native:
            refused = guest.command_status(start, 1)
            assert "Not booted from the installer disk" in refused, refused
            result["refusal_screen"] = guest.capture_screen("installer-source-refusal")
        else:
            guest.wait("Type exactly: INSTALL nvme0n1 nvme0n1p2", start, 1100)
            result["review_screen"] = guest.capture_screen("installer-review")
            guest.proc.stdin.write("sendkey esc\n")
            guest.proc.stdin.flush()
            assert "Installation cancelled." in guest.command_status(start)
            guest.run("/bin/stat /run/zedinst", status=1)
            after = guest.run("/sbin/sysctl kern.boot.root_image")
            assert match[0] in after, after
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["guest_result"] = "PASS native-root refusal" if native else "PASS private root identity and review/cancel"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["protected_after"] = F["digest"](protected)
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["protected_before"] == result["protected_after"]
    assert result["production_sha256"] == result["production_after"]
    result["result"] = "PASS source selection " + ("native refusal" if native else "private image")
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["result"])


if __name__ == "__main__":
    main()
