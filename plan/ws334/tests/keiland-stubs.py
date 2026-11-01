#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Writes userland/desktop/libkeiland/win32/unsupported.c: the stubs of libkeiland on Windows.

The Windows build's libkeiland (apps/browser-win32/Makefile) is its common files and its Windows
files.  Every public function (exports.py --list) that none of them defines, and every function of
libkeiland's own that they call but none defines (the files left to the Wayland backend), gets a stub
here: a function returning int returns ENOTSUP, a pointer NULL, another value 0, and a void function
does nothing.  The prototypes are read from the public headers and libkeiland's private headers
(comments taken out by the host's preprocessor).

    python3 -I plan/ws334/tests/keiland-stubs.py           # writes unsupported.c
    python3 -I plan/ws334/tests/keiland-stubs.py --check   # fails when unsupported.c differs

LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64).
The objects are built under build/tmp/ws334-keiland-stubs.  Last line: keiland-stubs: PASS or FAIL.
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
OUT = os.path.join(ROOT, "userland", "desktop", "libkeiland", "win32", "unsupported.c")
BUILD = "build/tmp/ws334-keiland-stubs"
LLVM_MINGW = os.environ.get("LLVM_MINGW", os.path.expanduser("~/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64"))

# The tree's own names (a call of another library's, libtruetype's, is that library's).
OWN = re.compile(r"^(kl_|keiui_|ka_)")

# The headers the prototypes are read from, the public ones first.
PUBLIC = sorted(glob.glob(os.path.join(ROOT, "userland/desktop/include/keiland/*.h")))
PRIVATE = sorted(set(glob.glob(os.path.join(ROOT, "userland/desktop/libkeiland/**/*.h"), recursive=True)))
NOT_INCLUDED = {"ui/window.h", "ui/app.h"}

HEAD = """/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The functions of libkeiland that its Windows backend does not have yet:
 * a function returning int returns ENOTSUP, a pointer NULL, another value
 * 0, and a void function does nothing.  Generated from the headers by the
 * stubs' generator of the Windows build's tests; do not edit.
 */

#include <vulkan/vulkan.h>

"""


def make(*arguments):
    """Runs the Windows build's Makefile with the build directory of this script."""
    command = ["make", "-s", "--no-print-directory", "-f", "apps/browser-win32/Makefile", "WIN32_ARCH=x86_64",
               "LLVM_MINGW=" + LLVM_MINGW, "BROWSER_WIN32_BUILD=" + BUILD,
               "BROWSER_WIN32_SOURCES=build/tmp/ws334-win32-sources"] + list(arguments)
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True)


def names(arguments):
    """The symbols llvm-nm prints for some objects, one a line."""
    run = subprocess.run([os.path.join(LLVM_MINGW, "bin", "llvm-nm")] + arguments, cwd=ROOT, capture_output=True, text=True)
    found = set()
    for line in run.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "TDBRCV":
            found.add(parts[2])
        elif len(parts) == 2 and parts[0] == "U":
            found.add(parts[1])
    return found


def stripped(path):
    """A header's text without its comments."""
    run = subprocess.run(["cc", "-fpreprocessed", "-dD", "-E", "-P", "-x", "c", path], capture_output=True, text=True)
    return run.stdout


def prototypes():
    """Each function's prototype (return type, parameters) and the header it is in, the first found."""
    found = {}
    call = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    result_shape = re.compile(r"^(?:[A-Za-z_][A-Za-z0-9_]*[\s\*]+)+$")
    for path in PUBLIC + PRIVATE:
        text = stripped(path)
        text = re.sub(r"^\s*#.*$", "", text, flags=re.M)
        for match in call.finditer(text):
            name = match.group(1)
            if name in found:
                continue

            # The parameters, to the matching parenthesis, and a semicolon after them.
            depth = 1
            index = match.end()
            while index < len(text) and depth != 0:
                if text[index] == "(":
                    depth += 1
                elif text[index] == ")":
                    depth -= 1
                index += 1
            if depth != 0:
                continue
            rest = text[index:].lstrip()
            if not rest.startswith(";"):
                continue
            parameters = text[match.end():index - 1]

            # The return type, back to the statement before.
            start = match.start()
            back = start
            while back > 0 and text[back - 1] not in ";{}":
                back -= 1
            result = " ".join(text[back:start].split())
            if not result or not result_shape.match(result + " "):
                continue
            words = result.split()
            if words[0] in ("typedef", "return", "extern", "static") or "(" in result:
                continue
            found[name] = (result, " ".join(parameters.split()), path)
    return found


def split_parameters(text):
    """A parameter list split at its top-level commas."""
    parts = []
    depth = 0
    current = ""
    for character in text:
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        if character == "," and depth == 0:
            parts.append(current.strip())
            current = ""
            continue
        current += character
    if current.strip():
        parts.append(current.strip())
    return parts


TYPE_WORDS = {"void", "char", "short", "int", "long", "unsigned", "signed", "float", "double", "const", "struct",
              "enum", "union", "volatile", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t",
              "int16_t", "int32_t", "int64_t", "kl_color", "VkInstance", "VkSurfaceKHR"}


def parameter_name(parameter):
    """A parameter's name, or None when it has none."""
    pointer = re.search(r"\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", parameter)
    if pointer:
        return pointer.group(1)
    bare = re.sub(r"\[[^\]]*\]\s*$", "", parameter).strip()
    word = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", bare)
    if not word:
        return None
    words = bare.replace("*", " ").split()
    if len(words) < 2 or word.group(1) in TYPE_WORDS or bare.endswith("*"):
        return None
    if words[-2] in ("struct", "enum", "union"):
        return None
    return word.group(1)


def stub(name, result, parameters):
    """The stub's definition."""
    parts = split_parameters(parameters)
    named = []
    lines = []
    if parts == ["void"] or not parts:
        parts = ["void"]
    else:
        for index, part in enumerate(parts):
            if part == "...":
                continue
            given = parameter_name(part)
            if given is None:
                given = "unused_%d" % index
                part = part + " " + given
                parts[index] = part
            named.append(given)
    body = []
    for given in named:
        body.append("\t(void)%s;" % given)
    if body:
        body.append("")
    if result == "void":
        body.append("\t/* Nothing is done on Windows. */")
    elif "*" in result:
        body.append("\t/* Not on Windows. */")
        body.append("\treturn NULL;")
    elif result == "int":
        body.append("\t/* Not on Windows. */")
        body.append("\treturn ENOTSUP;")
    elif result.startswith("struct ") or result.startswith("union "):
        body.insert(0, "\t%s value;" % result)
        body.insert(1, "")
        body.append("\t/* Not on Windows: a value of zeros. */")
        body.append("\tmemset(&value, 0, sizeof(value));")
        body.append("\treturn value;")
    else:
        body.append("\t/* Not on Windows. */")
        body.append("\treturn 0;")
    if parts == ["void"]:
        signature = "%s(void)" % name
    else:
        signature = "%s(\n\t%s)" % (name, ",\n\t".join(parts))
    lines.append("/* Not on Windows. */")
    lines.append(result)
    lines.append(signature)
    lines.append("{")
    lines.extend(body)
    lines.append("}")
    return "\n".join(lines) + "\n"


def main():
    check = "--check" in sys.argv[1:]

    # The objects, built, and their names.
    built = make("keiland-objects", "-j8")
    if built.returncode != 0:
        sys.stdout.write(built.stdout + built.stderr)
        print("keiland-stubs: FAIL (the objects did not build)")
        return 1
    flags = make("flags").stdout
    objects = []
    for line in flags.splitlines():
        if line.startswith("LIBKEILAND_WIN32_OBJECTS="):
            objects = line.split("=", 1)[1].split()
    defined = names(["--defined-only"] + objects)
    used = names(["-u"] + objects)
    exported = subprocess.run([sys.executable, "-I", "userland/desktop/libkeiland/exports.py", "--list"], cwd=ROOT,
                              capture_output=True, text=True).stdout.split()

    # What needs a stub: the public functions not defined, and the own functions called but not defined.
    wanted = set(name for name in exported if name not in defined)
    wanted |= set(name for name in used if OWN.match(name) and name not in defined)
    table = prototypes()
    missing = sorted(name for name in wanted if name not in table)
    if missing:
        print("keiland-stubs: no prototype for: " + " ".join(missing))
        print("keiland-stubs: FAIL")
        return 1

    # The headers to include, and the stubs in the order of the names.
    headers = []
    for name in sorted(wanted):
        path = os.path.relpath(table[name][2], ROOT)
        if path.startswith("userland/desktop/include/"):
            continue
        short = os.path.relpath(path, "userland/desktop/libkeiland")
        if short in NOT_INCLUDED:
            print("keiland-stubs: %s is declared only in %s" % (name, path))
            print("keiland-stubs: FAIL")
            return 1
        if path not in headers:
            headers.append(path)
    text = HEAD + "#include <keiland/keiland.h>\n"
    for path in sorted(headers):
        text += '#include "%s"\n' % path
    text += "\n#include <errno.h>\n#include <stddef.h>\n#include <string.h>\n"
    for name in sorted(wanted):
        result, parameters, _ = table[name]
        text += "\n" + stub(name, result, parameters)

    # Written, or compared.
    if check:
        try:
            now = open(OUT).read()
        except OSError:
            now = ""
        if now != text:
            print("keiland-stubs: %s differs from what the generator writes" % os.path.relpath(OUT, ROOT))
            print("keiland-stubs: FAIL")
            return 1
    else:
        with open(OUT, "w") as out:
            out.write(text)
    print("keiland-stubs: %d stubs (%d public)" % (len(wanted), len([n for n in wanted if n in exported])))
    print("keiland-stubs: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
