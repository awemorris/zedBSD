#!/usr/bin/env python3
"""Check target find manifests and capture the current installer confirmation."""
import hashlib
import itertools
import json
from pathlib import Path
import re
import runpy
import sys

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F = I["F"]
REPO = I["REPO"]


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019-installation/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    F["create_nvme"](out / "gpt.img", sectors=10485760)
    I["BASE"]["add_discovery_payload"](out / "gpt.img")
    before = F["digest"](out / "gpt.img")
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    result["result"] = "FAIL find manifest and installer capture"
    try:
        guest.login()
        guest.run("mkdir /run/find-tree")
        guest.run("echo one > /run/find-tree/one")
        guest.run("echo two > '/run/find-tree/space name'")
        ordinary = guest.run("/bin/find /run/find-tree")
        names = ["/run/find-tree", "/run/find-tree/one", "/run/find-tree/space name"]
        for name in names:
            assert ordinary.splitlines().count(name) == 1, ordinary
        guest.run("/bin/find /run/find-tree -fprint0 /run/find-manifest")
        digest = guest.run("cksum -a sha256 /run/find-manifest")
        expected = [hashlib.sha256(("\0".join([names[0], *order]) + "\0").encode()).hexdigest()
                    for order in itertools.permutations(names[1:])]
        assert any(value in digest for value in expected), digest
        guest.run("/bin/find /run/absent-find-tree -print0", status=1)
        guest.run("rm /run/find-tree/one '/run/find-tree/space name' /run/find-manifest")
        guest.run("rmdir /run/find-tree")
        start = len(guest.text())
        guest.send("zedinst nvme0n1 nvme0n1p2; echo storage-result-$?")
        guest.source_continue(start)
        guest.wait("Type exactly: INSTALL nvme0n1 nvme0n1p2", start, 1100)
        result["screenshot"] = guest.capture_screen("installer-confirmation")
        guest.proc.stdin.write("sendkey esc\n")
        guest.proc.stdin.flush()
        text = guest.command_status(start)
        assert "Installation cancelled." in text, text
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["guest_result"] = "PASS default census once, NUL manifest, absent source refusal, installer screenshot/cancel"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["destination_before"] = before
        result["destination_after"] = F["digest"](out / "gpt.img")
        result["production_sha256_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert before == result["destination_after"]
    assert result["production_sha256"] == result["production_sha256_after"]
    result["result"] = "PASS find manifest and installer capture"
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["result"])


if __name__ == "__main__":
    main()
