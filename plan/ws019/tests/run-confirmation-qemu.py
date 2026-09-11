#!/usr/bin/env python3
"""Run real target Noct confirmation on disposable QEMU USB boot media."""
import json
from pathlib import Path
import re
import runpy
import sys
import time

HERE = Path(__file__).resolve().parent
BASE = runpy.run_path(str(HERE / "run-transaction-qemu.py"))
REPO = BASE["REPO"]
FIXTURE = BASE["fixture"]


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = FIXTURE["prepare_boot"](out, False, False,
        REPO / "build/arch-images/amd64-ws019-transaction.ufs")
    FIXTURE["create_nvme"](out / "gpt.img")
    before = FIXTURE["protected_bytes"](out / "gpt.img")
    guest = BASE["TransactionGuest"](out, usb_boot=True)
    guest.deadline = time.monotonic() + 600
    result["result"] = "FAIL confirmation"
    try:
        guest.login()
        for mode in ("-j", "-j0"):
            for key, expected in (("exact", "ACCEPTED"), ("esc", "CANCELLED"),
                                  ("ctrl-c", "CANCELLED"), ("wrong", "CANCELLED")):
                start = len(guest.text())
                guest.send("storage-exit noct " + mode + " --path=/usr/lib/zedinst "
                           "/usr/lib/zedinst/installer-confirmation.noct")
                guest.wait(r"Type exactly: INSTALL nvme0n1 nvme0n1p2", start)
                if key == "exact":
                    guest.send("INSTALL nvme0n1 nvme0n1p2")
                elif key == "wrong":
                    guest.send("yes")
                else:
                    guest.proc.stdin.write("sendkey " + key + "\n")
                    guest.proc.stdin.flush()
                text, _ = guest.wait("CONFIRM " + expected, start, 30)
                text = guest.prompt(start)
                assert re.search(r"^storage-result-0$", text, re.M), text
        guest.run("id -u", r"^0$")
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
        result["result"] = "PASS 8 native confirmation cases"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["protected_before"] = before
        result["protected_after"] = FIXTURE["protected_bytes"](out / "gpt.img")
        result["production_sha256_after"] = FIXTURE["digest"](REPO / "build/amd64/hdd-image.img")
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert before == result["protected_after"]
    assert result["production_sha256"] == result["production_sha256_after"]
    print(result["result"])


if __name__ == "__main__":
    main()
