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
    prefix = {"syscall_regular_buffer": "static uint8_t *\n", "syscall_input_view": "static int\n", "syscall_input_view_release": "static void\n", "syscall_output_view": "static int\n", "syscall_read_transfer": "static ssize_t\n", "syscall_output_release": "static void\n"}.get(name, "static intptr_t\n")
    return prefix + source[start:end] + "\n"
with tempfile.TemporaryDirectory(prefix="q086-syscall-") as work:
    work = Path(work)
    constants = source[source.index("#define SYSCALL_IO_CHUNK"):source.index("#define SYSCALL_SOCKET_BUFFER_MAX")]
    (work / "storage-syscall-config.h").write_text(constants)
    (work / "storage-syscall-extracted.h").write_text("".join(function(name) for name in
        ["syscall_regular_buffer", "syscall_input_view", "syscall_input_view_release", "syscall_output_view", "syscall_read_transfer", "syscall_output_release", "sys_read_call", "sys_write_call",
         "sys_positional_call", "sys_vector_call"]))
    # Keep current I/O counters; the syscall boundary fixture supplies its own
    # deterministic pool. Only those two pre-existing host services override
    # the merged unit. No syscall or counter implementation is replaced.
    io_object = work / "io.o"
    subprocess.run(["cc", "-std=c11", "-O1", "-g",
        "-Dtid_t=int32_t", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "--param=asan-globals=0", "-I", str(REPO / "include"),
        "-I", str(REPO / "include/uapi"), "-c", str(REPO / "src/kern/io.c"),
        "-o", str(io_object)], check=True)
    subprocess.run(["objcopy", "--weaken-symbol=io_pool_borrow",
        "--weaken-symbol=io_pool_release", str(io_object)], check=True)
    for chunk, imod, direct in [(None,4000,0),(None,4000,1)]:
        binary = work / f"io-{chunk}-{imod}-{direct}"
        subprocess.run(["cc","-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
            "-fsanitize=address,undefined","-fno-omit-frame-pointer",
            *([] if chunk is None else [f"-DZEDBSD_SYSCALL_REGULAR_CHUNK={chunk}"]),
            f"-DZEDBSD_XHCI_IMOD={imod}", f"-DZEDBSD_USER_INPUT_VIEW={direct}", f"-DZEDBSD_USER_OUTPUT_VIEW={direct}",
            "-I",str(REPO / "include"), "-I",str(REPO / "include/uapi"),
            str(io_object), "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-I",str(work),str(REPO / "plan/ws018-kernel-architecture/tests/storage-syscall-stories.c"),
            "-o",str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
print("S44 PASS current production 64 KiB pool path and controlled exhaustion; historical q087 count cells remain in retained results, physical IRQ/latency not measured")
