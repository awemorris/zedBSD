#!/usr/bin/env python3
"""Exercise installed library/application ELF contracts without a guest build.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CHECK = ROOT / "tools/build/check-dynamic-elf.py"


def execute(arguments, good=True, diagnostic=None):
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=20)
    if (result.returncode == 0) != good:
        raise RuntimeError(f"unexpected status: {arguments}: {result.stdout}{result.stderr}")
    if diagnostic and diagnostic not in result.stdout + result.stderr:
        raise RuntimeError(f"wrong rejection: {result.stdout}{result.stderr}")


def main():
    with tempfile.TemporaryDirectory(prefix="zedbsd-vulkan-elf-") as directory:
        work = Path(directory)
        (work / "dependency.c").write_text("int dependency(void) { return 7; }\n")
        (work / "library.c").write_text(
            "extern int dependency(void);\n"
            "int vkFixture(void) { return dependency(); }\n"
            "int private_fixture(void) { return 9; }\n"
        )
        (work / "entry.c").write_text(
            "extern int vkFixture(void);\n"
            "void _start(void) { (void)vkFixture(); }\n"
        )
        (work / "exports.map").write_text("{ global: vkFixture; local: *; };\n")
        (work / "exports.tsv").write_text("name\tscope\nvkFixture\tglobal\n")
        for source in ("dependency", "library", "entry"):
            execute(["cc", "-ffreestanding", "-fPIC", "-fno-stack-protector",
                     "-c", str(work / (source + ".c")), "-o", str(work / (source + ".o"))])
        common = ["ld", "-m", "elf_x86_64", "--hash-style=both",
                  "-z", "now", "-z", "relro", "-z", "separate-code"]
        execute(common + ["-shared", "-soname", "libfixturedep.so",
                          str(work / "dependency.o"), "-o", str(work / "libfixturedep.so")])
        library = common + ["-shared", "-z", "defs", "-soname", "libfixture.so",
                            str(work / "library.o"), "-L" + str(work), "-l:libfixturedep.so"]
        execute(library + ["--version-script=" + str(work / "exports.map"),
                           "-o", str(work / "library.so")])
        execute(library + ["-o", str(work / "private-export.so")])
        executable = common + ["-pie", "-e", "_start", str(work / "entry.o"),
                               "-L" + str(work), "-l:library.so",
                               "-rpath-link", str(work)]
        execute(executable + ["--dynamic-linker=/lib/ld.so", "-o", str(work / "app")])
        execute(executable + ["--dynamic-linker=/foreign/ld.so", "-o", str(work / "bad-interp")])
        check = ["python3", str(CHECK), "--machine", "amd64"]
        shared = check + ["--role", "shared-library", "--needed", "libfixturedep.so",
                          "--soname", "libfixture.so", "--exports-tsv", str(work / "exports.tsv")]
        application = check + ["--role", "application", "--needed", "libfixture.so"]
        execute(shared + [str(work / "library.so")])
        execute(application + [str(work / "app")])
        execute(shared + [str(work / "private-export.so")], False, "public exports differ")
        execute(application + [str(work / "bad-interp")], False, "must use /lib/ld.so")
        execute(check + ["--role", "application", str(work / "app")], False, "DT_NEEDED differs")
        execute(check + ["--role", "shared-library", "--soname", "wrong.so",
                         "--needed", "libfixturedep.so", str(work / "library.so")],
                False, "SONAME differs")
        execute(check + ["--role", "program", str(work / "app")],
                False, "test program must depend only on libc.so")
        execute(shared + [str(work / "app")], False, "must not contain PT_INTERP")
        (work / "exports.tsv").write_text("name\tscope\nvkMissing\tglobal\n")
        execute(shared + [str(work / "library.so")], False, "public exports differ")
    print("Vulkan ELF contracts: PASS (two valid artifacts, seven rejected contracts)")


if __name__ == "__main__":
    main()
