#!/usr/bin/env python3
"""Extract complete production functions verbatim; never reimplement their loop."""
from pathlib import Path
import subprocess
import tempfile
REPO = Path(__file__).resolve().parents[3]
source = (REPO / "src/kern/syscall.c").read_text()
def function(name):
    start = source.index("\n" + name + "(") + 1
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    prefix = "static uint8_t *\n" if name == "syscall_regular_buffer" else "static intptr_t\n"
    return prefix + source[start:end] + "\n"
with tempfile.TemporaryDirectory(prefix="q086-syscall-") as work:
    work = Path(work)
    (work / "storage-syscall-extracted.h").write_text("".join(function(name) for name in
        ["syscall_regular_buffer", "sys_read_call", "sys_write_call"]))
    for chunk, imod in [(512,4000),(4096,4000),(512,0),(4096,0)]:
        binary = work / f"io-{chunk}-{imod}"
        subprocess.run(["cc","-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
            "-fsanitize=address,undefined","-fno-omit-frame-pointer",
            f"-DZEDBSD_SYSCALL_REGULAR_CHUNK={chunk}",f"-DZEDBSD_XHCI_IMOD={imod}",
            "-I",str(work),str(REPO / "plan/ws018-kernel-architecture/tests/storage-syscall-stories.c"),
            "-o",str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
print("S44 PASS four operation-count cells; physical IRQ/CPU/latency not measured")
