#!/usr/bin/env python3
"""Continue conflict acceptance on a copy of an already installed QEMU disk."""
import json
from pathlib import Path
import re
import runpy
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
PUBLIC = runpy.run_path(str(HERE / "run-install-qemu.py"))
BASE = PUBLIC["BASE"]
F = PUBLIC["F"]
REPO = PUBLIC["REPO"]


def main():
    accepted = Path(sys.argv[1]).resolve()
    out = Path(sys.argv[2]).resolve()
    for path in (accepted, out):
        path.relative_to(REPO / "plan/ws019-installation/temp")
    previous = json.loads((accepted / "result.json").read_text())
    assert all(case in previous["cases"] for case in ("cancel", "install", "rerun"))
    assert previous["protected_before"] == previous["protected_after"]
    assert previous["payload_before"] == previous["payload_after"]
    assert previous["vars_before"] == previous["vars_after"]
    assert previous["production_sha256"] == previous["production_sha256_after"]
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False,
        REPO / "build/arch-images/amd64.ufs")
    assert result["production_sha256"] == previous["production_sha256"]
    source = accepted / "gpt.img"
    result["accepted_target_before"] = F["digest"](source)
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(source),
                    str(out / "gpt.img")], check=True)
    before = F["protected_bytes"](out / "gpt.img")
    payload = BASE["payload_marker"](out / "gpt.img")
    result["vars_before_boot"] = F["digest"](out / "vars.fd")
    result["accepted_run"] = str(accepted)
    result["result"] = "FAIL installer conflict continuation"
    guest = PUBLIC["PublicInstallGuest"](out, usb_boot=True)
    guest.deadline = time.monotonic() + 1800
    try:
        guest.login()
        # OVMF initializes its variable store during boot. Compare across the
        # installer operation, after firmware initialization, as public6 does.
        result["vars_before"] = F["digest"](out / "vars.fd")
        guest.run("dd if=/dev/zero of=/run/conflict-byte bs=1 count=1")
        guest.run("mkdir /run/verify-installed")
        guest.run("mount -t fat nvme0n1p2 /run/verify-installed")
        guest.run("/bin/cp -T --update=none-fail /run/verify-installed/vmunix /run/kernel-backup")
        guest.run("dd if=/run/conflict-byte of=/run/verify-installed/vmunix bs=1 count=1 conv=notrunc")
        corrupted = guest.run("cksum -a sha256 /run/verify-installed/vmunix")
        digest = re.search(r"^([0-9a-f]{64})  /run/verify-installed/vmunix$", corrupted, re.M)
        assert digest, corrupted
        guest.run("umount /run/verify-installed")
        guest.run_installer("zedinst nvme0n1 nvme0n1p2", "installation incomplete", status=1)
        guest.run("mount -t fat nvme0n1p2 /run/verify-installed")
        unchanged = guest.run("cksum -a sha256 /run/verify-installed/vmunix")
        assert digest.group(1) in unchanged, unchanged
        guest.run("/bin/cp -T /run/kernel-backup /run/verify-installed/vmunix")
        guest.run("cmp /run/kernel-backup /run/verify-installed/vmunix")
        guest.run("sync /run/verify-installed/vmunix")
        guest.run("umount /run/verify-installed")
        guest.run("rmdir /run/verify-installed")
        guest.run("rm /run/kernel-backup")
        guest.run("rm /run/conflict-byte")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["guest_result"] = "PASS conflict refusal preserves content; original restored"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["protected_before"] = before
        result["protected_after"] = F["protected_bytes"](out / "gpt.img")
        result["payload_before"] = payload
        result["payload_after"] = BASE["payload_marker"](out / "gpt.img")
        result["vars_after"] = F["digest"](out / "vars.fd")
        result["production_sha256_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        result["accepted_target_after"] = F["digest"](source)
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert before == result["protected_after"]
    assert payload == result["payload_after"]
    assert result["vars_before"] == result["vars_after"]
    assert result["production_sha256"] == result["production_sha256_after"]
    assert result["accepted_target_before"] == result["accepted_target_after"]
    result["result"] = "PASS installer conflict continuation"
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["result"])


if __name__ == "__main__":
    main()
