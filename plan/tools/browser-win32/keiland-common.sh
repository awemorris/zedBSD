#!/bin/sh
# Which files of libkeiland's source list (LIBKEILAND_SOURCES of userland/desktop/libkeiland/sources.mk) the
# Windows build compiles as they are: a file is common when neither it nor any file of the tree it includes
# (followed through every quoted #include) names Wayland or its protocols (wayland, wl_, xdg_, zwp_), the
# POSIX headers <unistd.h>, <poll.h>, <sys/...> or <dlfcn.h>, or the Wayland window's private headers
# (ui/window.h, ui/app.h), comments left out (a type named only as an incomplete struct, "struct wl_surface"
# in a declaration or a pointer, is not counted: it needs nothing of Wayland); and when it compiles with the
# Windows compiler (WIN32_CC=<compiler>, or LLVM_MINGW=<root>) and the Windows build's flags.  The result goes to
# build/tmp/browser-win32-keiland-common/keiland-common.txt ("common <file>" or "excluded <file>: <reason>", in the list's order), and
# the list of common files is compared with LIBKEILAND_WIN32_COMMON_SOURCES of apps/browser-win32/Makefile.
#   sh plan/tools/browser-win32/keiland-common.sh
# Last line: keiland-common: PASS (the Makefile's list is the result's) or keiland-common: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/browser-win32-keiland-common
fresh_out "$out"
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
WIN32_CC=${WIN32_CC:-$LLVM_MINGW/bin/x86_64-w64-mingw32-clang}
result=$out/keiland-common.txt

# The source list, and the Windows build's flags and its list of common files.
printf 'include userland/desktop/libkeiland/sources.mk\n$(info $(LIBKEILAND_SOURCES))\nall: ; @:\n' |
    make -s --no-print-directory -f - all | tr ' ' '\n' | sed '/^$/d' > "$out/sources.txt"
flags=$(make -s --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 LLVM_MINGW="$LLVM_MINGW" \
    BROWSER_WIN32_BUILD="$out/win64" flags | sed -n 's/^WIN32_CFLAGS=//p')
make -s --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 LLVM_MINGW="$LLVM_MINGW" \
    BROWSER_WIN32_BUILD="$out/win64" flags | sed -n 's/^LIBKEILAND_WIN32_COMMON_SOURCES=//p' | tr ' ' '\n' |
    sed '/^$/d' > "$out/makefile-common.txt"
# The headers the Windows build copies (the compat headers, and the Vulkan headers it fetches).
make -s --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 LLVM_MINGW="$LLVM_MINGW" \
    BROWSER_WIN32_BUILD="$out/win64" BROWSER_WIN32_SOURCES=build/tmp/browser-win32-sources headers > "$out/headers.log" 2>&1

# The search, comments left out with the host's preprocessor (-fpreprocessed: no macro and no #include is expanded).
/usr/bin/python3 -I - "$out/sources.txt" "$out/candidates.txt" <<'PYTHON'
import os
import re
import subprocess
import sys

sources = [line.strip() for line in open(sys.argv[1]) if line.strip()]
pattern = re.compile(r'wayland|\bwl_|\bxdg_|\bzwp_|<unistd\.h>|<poll\.h>|<sys/|<dlfcn\.h>')
private = {os.path.normpath("userland/desktop/libkeiland/ui/window.h"), os.path.normpath("userland/desktop/libkeiland/ui/app.h")}
include = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
roots = [".", "userland/desktop/include"]
seen = {}

def stripped(path):
    run = subprocess.run(["cc", "-fpreprocessed", "-dD", "-E", "-P", "-x", "c", path], capture_output=True, text=True)
    if run.returncode != 0:
        return open(path, errors="replace").read()
    return re.sub(r'\bstruct\s+(?:wl|xdg|zwp)_[A-Za-z0-9_]+', 'struct opaque', run.stdout)

def resolve(name, here):
    for base in [os.path.dirname(here)] + roots:
        path = os.path.normpath(os.path.join(base, name))
        if os.path.isfile(path):
            return path
    return None

def reason(path, chain):
    if path in seen:
        return seen[path]
    seen[path] = None
    if path in private:
        seen[path] = "includes " + path
        return seen[path]
    text = stripped(path)
    found = pattern.search(text)
    if found:
        seen[path] = "%s names %s" % (path, found.group(0))
        return seen[path]
    for name in include.findall(open(path, errors="replace").read()):
        target = resolve(name, path)
        if target is None:
            continue
        why = reason(target, chain + [path])
        if why:
            seen[path] = why
            return why
    return None

with open(sys.argv[2], "w") as out:
    for source in sources:
        why = reason(os.path.normpath(source), [])
        if why:
            out.write("excluded %s: %s\n" % (source, why))
        else:
            out.write("candidate %s\n" % source)
PYTHON

# The candidates the Windows compiler takes.
: > "$out/result.txt"
while read -r kind file rest; do
	if [ "$kind" != candidate ]; then
		echo "$kind $file $rest" >> "$out/result.txt"
		continue
	fi
	log=$out/$(echo "$file" | tr '/' '_').log
	if $WIN32_CC $flags -I"$out/win64/obj/include" -c "$file" -o "$out/object.o" > "$log" 2>&1 && [ ! -s "$log" ]; then
		echo "common $file" >> "$out/result.txt"
	else
		echo "excluded $file: the Windows compiler: $(grep -m 1 -E 'error|warning' "$log" | sed 's/^[^:]*:[0-9]*:[0-9]*: //')" >> "$out/result.txt"
	fi
done < "$out/candidates.txt"
{
	echo "# libkeiland's files the Windows build compiles as they are (plan/tools/browser-win32/keiland-common.sh, generated)."
	cat "$out/result.txt"
} > "$result"
sed -n 's/^common //p' "$out/result.txt" > "$out/common.txt"
echo "keiland-common: $(wc -l < "$out/common.txt") common, $(grep -c '^excluded ' "$out/result.txt") excluded (in $result)"
if cmp -s "$out/common.txt" "$out/makefile-common.txt"; then
	echo "keiland-common: PASS"
	exit 0
fi
echo "keiland-common: the Makefile's LIBKEILAND_WIN32_COMMON_SOURCES differs from the result:"
diff "$out/makefile-common.txt" "$out/common.txt" | head -20
echo "keiland-common: FAIL"
exit 1
