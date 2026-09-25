#!/usr/bin/env python3
# ws035-p034: the vmunix link no longer contains or needs the C library.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Check a p034 kernel build for C library objects and symbols.

usage: python3 plan/ws035/tests/kcrt-link-check.py --baseline DIR [--baseline DIR]
           [--symbols OUT] [--implicit OUT] <name>=<build.log>=<build dir> ...

For each build (the log written by refactor-build.sh and its BUILD dir):
  1. the vmunix link command lists no object under libc/;
  2. vmunix defines none of the global symbols the baseline C library
     objects (<baseline>/kern64/libc/**/*.o) defined, except memcpy and
     memset, which kcrt defines for the compiler (--symbols writes the list);
  3. llvm-nm -u of kcrt.o is empty, and kcrt.o defines memcpy and memset;
  4. the probe: kcrt.c is recompiled with -DKERN_KCRT_NO_STANDARD_NAMES and
     the link is replayed with it; the undefined symbols the linker reports
     are the standard names the kernel and HAL objects need from kcrt;
  5. --implicit writes, for every linked object that refers to a standard
     name, whether its source has a textual call of that name (a call in
     code, not in a comment or literal).  None is expected in the kernel.
Exit status 1 when a check fails.
"""

import argparse
import importlib.util
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
NM = str(REPO / "build/llvm/bin/llvm-nm")
STANDARD = {"memcpy", "memset", "memmove", "memcmp", "memchr", "strlen",
            "strnlen", "strcmp", "strncmp", "strcpy", "strncpy", "strcat",
            "strchr", "strrchr", "strstr", "snprintf", "vsnprintf",
            "memset_explicit"}
ALLOWED = {"memcpy", "memset"}

spec = importlib.util.spec_from_file_location("kcrt_rewrite", REPO / "plan/ws035/tests/kcrt-rewrite.py")
rewrite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rewrite)


def nm(args, path):
    out = subprocess.run([NM] + args + [str(path)], capture_output=True, text=True, check=True).stdout
    return out


def global_defined(path):
    names = set()
    for line in nm(["--defined-only", "-g"], path).splitlines():
        parts = line.split()
        if len(parts) >= 3:
            names.add(parts[2])
    return names


def undefined(path):
    return {line.split()[-1] for line in nm(["-u"], path).splitlines() if line.strip()}


def log_commands(log):
    text = Path(log).read_text(encoding="utf-8", errors="replace").replace("\\\n", " ")
    compiles = {}
    link = None
    for line in text.splitlines():
        if " -c " in line and " -o " in line:
            argv = shlex.split(line)
            compiles[argv[argv.index("-c") + 1]] = argv
        if "ld.lld" in line and " -o " in line:
            link = shlex.split(line)
    return compiles, link


def textual_call(source, name):
    if not (REPO / source).is_file():
        return None
    text = (REPO / source).read_text(encoding="utf-8", errors="replace")
    mask = rewrite.code_mask(text)
    pattern = re.compile(r"(?<![A-Za-z0-9_.])(?<!->)" + re.escape(name) + r"(?=\s*\()")
    for m in pattern.finditer(text):
        if mask[m.start()]:
            return rewrite.line_of(text, m.start())
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", action="append", required=True)
    parser.add_argument("--symbols")
    parser.add_argument("--implicit")
    parser.add_argument("builds", nargs="+")
    args = parser.parse_args()

    libc_symbols = set()
    for base in args.baseline:
        for obj in sorted(Path(base, "kern64/libc").rglob("*.o")):
            libc_symbols |= global_defined(obj)
    if args.symbols:
        Path(args.symbols).write_text("".join(s + "\n" for s in sorted(libc_symbols)), encoding="utf-8")
    print("baseline libc global symbols: %d" % len(libc_symbols))

    failed = False
    implicit_lines = []
    for item in args.builds:
        name, log, build = item.split("=")
        compiles, link = log_commands(log)
        objects = [w for w in link if w.endswith(".o")]
        out = link[link.index("-o") + 1]

        # 1. no libc object in the link
        libc_objects = [o for o in objects if "/libc/" in o or o.startswith("libc/")]
        print("%s: linked objects %d, libc objects %d" % (name, len(objects), len(libc_objects)))
        failed |= bool(libc_objects)

        # 2. no libc symbol in vmunix except memcpy/memset
        present = sorted((global_defined(REPO / out) & libc_symbols) - ALLOWED)
        print("%s: libc symbols in vmunix (besides memcpy, memset): %d %s" % (name, len(present), present[:20]))
        failed |= bool(present)

        # 3. kcrt.o is self-contained
        kcrt = REPO / build / "kern64/src/kern/kcrt.o"
        kcrt_undefined = sorted(undefined(kcrt))
        kcrt_defined = global_defined(kcrt)
        print("%s: llvm-nm -u kcrt.o: %s; defines memcpy %s memset %s" % (
            name, kcrt_undefined or "(empty)", "memcpy" in kcrt_defined, "memset" in kcrt_defined))
        failed |= bool(kcrt_undefined) or not ALLOWED <= kcrt_defined

        # 4. the probe link without the standard names
        with tempfile.TemporaryDirectory() as tmp:
            probe_obj = os.path.join(tmp, "kcrt-probe.o")
            argv = list(compiles["src/kern/kcrt.c"])
            argv[argv.index("-o") + 1] = probe_obj
            argv = [a for a in argv if a not in ("-MMD", "-MP")]
            argv.insert(1, "-DKERN_KCRT_NO_STANDARD_NAMES")
            subprocess.run(argv, cwd=REPO, check=True)
            kcrt_rel = str(kcrt.relative_to(REPO))
            probe = [probe_obj if w == kcrt_rel else w for w in link]
            probe[probe.index("-o") + 1] = os.path.join(tmp, "vmunix")
            result = subprocess.run(probe + ["--error-limit=0"], cwd=REPO, capture_output=True, text=True)
            demanded = sorted(set(re.findall(r"undefined symbol: (\S+)", result.stderr)))
        print("%s: probe link without memcpy/memset: status %d, undefined %s" % (name, result.returncode, demanded))
        implicit_lines.append("# %s: probe link with kcrt.c -DKERN_KCRT_NO_STANDARD_NAMES: status %d, undefined symbols: %s"
                              % (name, result.returncode, " ".join(demanded) or "(none)"))
        failed |= not set(demanded) <= ALLOWED

        # 5. which objects refer to a standard name, and why
        implicit_lines.append("# %s: object<TAB>standard names referred<TAB>textual call in source (file:line) or implicit" % name)
        for obj in sorted(objects):
            refs = sorted(undefined(REPO / obj) & STANDARD)
            if not refs:
                continue
            rel = obj.split("/", 3)[3] if obj.startswith("build/") else obj
            rel = rel[len("kern64/"):] if rel.startswith("kern64/") else rel
            source = re.sub(r"\.o$", ".c", rel)
            notes = []
            for ref in refs:
                line = textual_call(source, ref)
                notes.append("%s:%s" % (source, line) if line else "implicit")
            implicit_lines.append("%s\t%s\t%s" % (obj, ",".join(refs), ",".join(notes)))
            if any(n != "implicit" for n in notes) and not source.startswith("src/hal/"):
                print("%s: textual standard call remains in %s" % (name, source))
                failed = True

    if args.implicit:
        Path(args.implicit).write_text("\n".join(implicit_lines) + "\n", encoding="utf-8")
    print("kcrt-link-check: %s" % ("FAIL" if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
