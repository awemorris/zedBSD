#!/usr/bin/env python3
"""Exercise shell return and cancellation without preparing any destination."""
import json
from pathlib import Path
import runpy
import sys
import time

HERE = Path(__file__).resolve().parent
N = runpy.run_path(str(HERE / "run-native-installer-qemu.py"))
I, F, REPO, key = N["I"], N["F"], N["REPO"], N["key"]


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    disk = out / "gpt.img"
    with disk.open("xb") as stream:
        stream.truncate(1024 * 1024 * 1024)
    before = F["digest"](disk)
    result.update(result="FAIL installer navigation", cases=[])
    guest = I["PublicInstallGuest"](out, usb_boot=True)
    try:
        guest.login()
        start = len(guest.text())
        guest.send("zedinst; echo storage-result-$?")
        guest.source_continue(start)
        guest.wait("Coexist with an existing FAT filesystem", start, 120)
        key(guest, "down")
        key(guest, "down")
        time.sleep(0.5)
        shell = len(guest.text())
        key(guest, "ret")
        guest.prompt(shell)
        guest.run("echo installer-shell-active", "installer-shell-active")
        restart = len(guest.text())
        guest.send("exit")
        # Returning from a shell must admit the source again, not resume old selection.
        guest.source_continue(restart)
        guest.wait("Coexist with an existing FAT filesystem", restart, 120)
        key(guest, "down")
        key(guest, "ret")
        guest.wait("The installation source is excluded.", restart, 120)
        time.sleep(1)
        result["screenshot"] = guest.capture_screen("shell-return-disk")
        key(guest, "esc")
        text = guest.command_status(restart)
        assert "Installation cancelled." in text, text
        result["cases"].append("shell return restarts source and mode admission; disk Escape cancels")
        guest.run("ls /run")
        assert F["digest"](disk) == before, "navigation changed destination"
        result["result"] = "PASS installer navigation"
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["production_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        if result["production_after"] != result["production_sha256"]:
            result["result"] = "FAIL production image changed"
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["result"] == "PASS installer navigation", result
    print(result["result"])


if __name__ == "__main__":
    main()
