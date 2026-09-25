#!/usr/bin/env python3
# ws035-p035: are two kernel builds the same code, apart from __LINE__?
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Compare the objects of two kernel builds of the same configuration.

usage: python3 plan/ws035/tests/kernel-object-compare.py BUILD_A BUILD_B

For every ELF object under BUILD_A (user64/ and dynamic/ are skipped: they
are not the kernel) the contents of its allocated sections (SHF_ALLOC:
.text*, .rodata*, .data*; .bss by size) are compared with the same object
under BUILD_B.  For each object that differs, both are disassembled with
llvm-objdump and every differing instruction is accepted only when the two
lines are the same instruction with one immediate that differs by at most
20: the __LINE__ a fatal-error or assertion macro passes, shifted because
#include lines above it were removed.  Any other difference is reported and
makes the exit status 1.
"""

import hashlib
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
OBJDUMP = str(REPO / "build/llvm/bin/llvm-objdump")
IMMEDIATE = re.compile(r"\$0x([0-9a-f]+)")


def sections(path):
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF":
        return None
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3a)
    headers = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    names = headers[shstrndx][4]
    out = {}
    for header in headers:
        name = data[names + header[0]:data.index(b"\0", names + header[0])].decode()
        kind, flags, offset, size = header[1], header[2], header[4], header[5]
        if not flags & 2:
            continue
        if kind == 8:
            out[name] = "nobits:%d" % size
        else:
            out[name] = hashlib.sha256(data[offset:offset + size]).hexdigest()
    return out


def disassembly(path):
    text = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", path],
                          capture_output=True, text=True, check=True).stdout
    return text.splitlines()[3:]


def line_only(a, b):
    """Returns the number of differing lines, or -1 if one is not a line number."""
    left = disassembly(a)
    right = disassembly(b)
    if len(left) != len(right):
        return -1
    count = 0
    for x, y in zip(left, right):
        if x == y:
            continue
        count += 1
        xs, ys = x.split("#")[0], y.split("#")[0]
        xi, yi = IMMEDIATE.findall(xs), IMMEDIATE.findall(ys)
        if IMMEDIATE.sub("$IMM", xs) != IMMEDIATE.sub("$IMM", ys) or len(xi) != 1 or len(yi) != 1:
            print("  not a line number: %s | %s" % (x.strip(), y.strip()))
            return -1
        if abs(int(xi[0], 16) - int(yi[0], 16)) > 20:
            print("  immediate differs by more than 20: %s | %s" % (x.strip(), y.strip()))
            return -1
    return count


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = sys.argv[1], sys.argv[2]
    same = 0
    lines = 0
    bad = 0
    differing = []
    for root, _, files in os.walk(a):
        for name in sorted(files):
            if not name.endswith(".o"):
                continue
            left = os.path.join(root, name)
            rel = os.path.relpath(left, a)
            if rel.startswith(("user64/", "dynamic/")):
                continue
            right = os.path.join(b, rel)
            if not os.path.exists(right):
                print("only in %s: %s" % (a, rel))
                bad += 1
                continue
            sa, sb = sections(left), sections(right)
            if sa == sb:
                same += 1
                continue
            changed = [n for n in sorted(set(sa) | set(sb)) if sa.get(n) != sb.get(n)]
            count = line_only(left, right)
            print("differs: %s sections=%d (%s) differing_instructions=%s" % (
                rel, len(changed), " ".join(changed[:4]), count if count >= 0 else "NOT-LINE-ONLY"))
            differing.append(rel)
            if count < 0:
                bad += 1
            else:
                lines += count
    print("kernel-object-compare: same=%d differ=%d line_number_only_instructions=%d other=%d"
          % (same, len(differing), lines, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
