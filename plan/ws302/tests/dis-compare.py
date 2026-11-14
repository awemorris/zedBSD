# Compares two disassemblies of a kernel line by line.  It prints "same-text-but-line-numbers N" when they have the
# same lines, the same addresses and the same instructions and differ only in N constants that are each a line
# number of a fatal check: the base commit's line OLD and the new tree's line NEW of one changed C file hold the same
# text, which names a fatal check (adding source lines above a check moves the __LINE__ it passes).  Otherwise it
# prints "DIFFERENT-TEXT" and the first line that differs in another way.  The differing lines go to OUT.diff.
#   python3 -I dis-compare.py BASE.dis NEW.dis OUT.diff BASE-COMMIT
# Run from the top of the tree; the changed C files are the ones git reports between BASE-COMMIT and the work tree.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import re
import subprocess
import sys

NUMBER = re.compile(r"-?0x[0-9a-fA-F]+|-?\b[0-9]+\b")


def code(line):
    """The instruction of a line, without its address and the disassembler's comment."""
    return line.partition(":")[2].split("//")[0].split("# imm")[0]


def shape(line):
    """The line with its address kept and every number of its instruction replaced."""
    return line.partition(":")[0] + ":" + NUMBER.sub("N", code(line))


def value(lines, index):
    """The constant the instruction at index loads, with a RISC-V lui before an addi folded in."""
    text = code(lines[index])
    number = int(NUMBER.findall(text)[-1], 0)
    if "addi" in text and index > 0:
        previous = code(lines[index - 1])
        if "lui" in previous:
            number += int(NUMBER.findall(previous)[-1], 0) << 12
    return number


def changed_files(base):
    """The changed C files: their base lines and their new lines."""
    names = subprocess.run(["git", "diff", "--name-only", base, "--", "*.c"], check=True,
                           capture_output=True, text=True).stdout.split()
    files = []
    for name in names:
        old = subprocess.run(["git", "show", base + ":" + name], capture_output=True, text=True).stdout
        with open(name) as handle:
            new = handle.read()
        files.append((old.splitlines(), new.splitlines()))
    return files


def is_line_number(files, old_value, new_value):
    """Whether the two values are one fatal check's line in the base and the new tree of a changed file."""
    for old_lines, new_lines in files:
        if not (0 < old_value <= len(old_lines) and 0 < new_value <= len(new_lines)):
            continue
        text = new_lines[new_value - 1]
        if text != old_lines[old_value - 1]:
            continue
        if "fatal" in text.lower() or "panic" in text.lower():
            return True
    return False


def main():
    """Compares the two files and prints the verdict."""
    with open(sys.argv[1]) as handle:
        base = handle.read().splitlines()
    with open(sys.argv[2]) as handle:
        new = handle.read().splitlines()
    files = changed_files(sys.argv[4])
    if len(base) != len(new):
        print("DIFFERENT-TEXT (%d lines against %d)" % (len(base), len(new)))
        return
    differing = []
    for index, (old_line, new_line) in enumerate(zip(base, new)):
        if old_line == new_line:
            continue
        if shape(old_line) != shape(new_line):
            print("DIFFERENT-TEXT (%s | %s)" % (old_line.strip(), new_line.strip()))
            return
        if not is_line_number(files, value(base, index), value(new, index)):
            print("DIFFERENT-TEXT (not a fatal check's line: %s | %s)" % (old_line.strip(), new_line.strip()))
            return
        differing.append("< " + old_line + "\n> " + new_line + "\n")
    with open(sys.argv[3], "w") as handle:
        handle.writelines(differing)
    print("same-text-but-line-numbers %d" % len(differing))


main()
