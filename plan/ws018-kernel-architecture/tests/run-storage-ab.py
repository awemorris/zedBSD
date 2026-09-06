#!/usr/bin/env python3
"""Four real kernel builds + xHCI USB-root guests; restore defaults afterwards."""
from pathlib import Path
import json
import re
import subprocess
import sys
REPO = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.relative_to(REPO / "plan/ws018-kernel-architecture/temp")
out.mkdir(parents=True, exist_ok=False)
base = ["make","-j16","ZEDBSD_CONFIG=config/ci/config-amd64.mk",
        "-f","Makefile","-f","plan/ws018-kernel-architecture/tests/storage-native.mk"]
results = []
try:
    for chunk, imod in [(512,4000),(4096,4000),(512,0),(4096,0)]:
        name = f"chunk{chunk}-imod{imod}"
        print(name, flush=True)
        command = [*base,"-f","plan/ws018-kernel-architecture/tests/storage-ab.mk",
                   f"Q086_CHUNK={chunk}",f"Q086_IMOD={imod}","disk-image","q086-native-fixture"]
        with (out / (name + "-build.log")).open("w") as log:
            subprocess.run(command,cwd=REPO,stdout=log,stderr=subprocess.STDOUT,check=True)
        with (out / (name + "-guest.log")).open("w") as log:
            subprocess.run(["python3","plan/ws018-kernel-architecture/tests/run-storage-native.py",
                str(out / name),"--usb","--bench"],cwd=REPO,stdout=log,stderr=subprocess.STDOUT,check=True)
        text = (out / (name + "-guest.log")).read_text()
        match = re.search(r"BENCH PASS bytes=(\d+) elapsed_ns=(\d+)",text)
        assert match
        results.append({"chunk":chunk,"imod":imod,"bytes":int(match[1]),
                        "elapsed_ns":int(match[2]),"irq_histogram":"unmeasured"})
        (out / "results.json").write_text(json.dumps(results,indent=2) + "\n")
finally:
    with (out / "restore-build.log").open("w") as log:
        subprocess.run([*base,"-W","src/kern/syscall.c","-W","src/drivers/pci-xhci.c",
                        "disk-image","q086-native-fixture"],cwd=REPO,stdout=log,stderr=subprocess.STDOUT,check=True)
print("S44 PASS four real kernel/USB-root cells; production defaults restored", flush=True)
