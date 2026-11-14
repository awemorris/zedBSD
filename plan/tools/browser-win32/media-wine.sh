#!/bin/sh
# libmedia on Windows under Wine (x86_64) against the same program on the host: plan/tools/browser-win32/media-main.c
# reads an H.264 MP4 through the decoder layer and through the engine and prints the pictures' sizes and
# hashes; the host's build (libmedia's zedBSD source list, its POSIX media-os) and the Windows build's
# (test-program, linked with every static library) must print the same lines.  The MP4 is
# plan/ws083/tests/streams/pb-main-352.h264 (Main, I, P and B pictures, 352x288, 15 pictures) put in
# an MP4 by the host's ffmpeg without coding it again.  Neither the host's Vulkan (llvmpipe) nor Wine's
# decodes video, so both use the software H.264; the Windows run must still find the Vulkan loader
# (vulkan-1.dll, through media-os) and ask it for the devices, and the .exe imports only Windows' own
# DLLs and the UCRT's and holds no winpthreads name.  Wine runs under flock /tmp/zedbsd-wine.lock.
#   sh plan/tools/browser-win32/media-wine.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# WINEPREFIX=<dir> the Wine prefix (default build/tmp/browser-win32-wine).
# Last line: media-wine: PASS or media-wine: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/browser-win32-media-wine
fresh_out "$out"
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
WINEPREFIX=${WINEPREFIX:-$(pwd)/build/tmp/browser-win32-wine}
export WINEPREFIX
build=${BROWSER_WIN32_BUILD:-build/tmp/browser-win32-media-build/browser-win64}
sources=build/tmp/browser-win32-sources
mkdir -p "$sources"
status=0

# The MP4.
clip=$out/pb-main-352.mp4
if ! timeout 120 ffmpeg -loglevel error -y -framerate 25 -i plan/ws083/tests/streams/pb-main-352.h264 -c copy \
    "$clip" > "$out/ffmpeg.log" 2>&1; then
	echo "media-wine: ffmpeg could not make the MP4 (log $out/ffmpeg.log)"
	echo "media-wine: FAIL"
	exit 1
fi

# The host's program, from libmedia's source list.
list=$(printf 'include userland/desktop/libmedia/sources.mk\n$(info $(LIBMEDIA_SOURCES))\nall: ; @:\n' |
    make -s --no-print-directory -f - all)
if ! timeout 900 cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I. plan/tools/browser-win32/media-main.c $list \
    -o "$out/media-tool" -lm -ldl -lpthread > "$out/host-build.log" 2>&1; then
	echo "media-wine: the host's program did not build (log $out/host-build.log)"
	head -10 "$out/host-build.log"
	echo "media-wine: FAIL"
	exit 1
fi

# The Windows program.
if ! timeout 1800 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 \
    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" -j4 \
    test-program TEST_PROGRAM=media-tool TEST_PROGRAM_SOURCES="plan/tools/browser-win32/media-main.c" \
    > "$out/make.log" 2>&1 || grep -q 'warning:' "$out/make.log"; then
	echo "media-wine: the Windows program did not build cleanly (log $out/make.log)"
	tail -10 "$out/make.log"
	echo "media-wine: FAIL"
	exit 1
fi
program=$build/tests/media-tool.exe

# What the .exe imports: Windows' own DLLs and the UCRT's only.
"$LLVM_MINGW/bin/llvm-readobj" --coff-imports "$program" > "$out/imports.txt"
sed -n '/^Import {/,/^}/s/^ *Name: //p' "$out/imports.txt" | sort -u > "$out/dlls.txt"
foreign=$(grep -v -i -x -E 'api-ms-win-crt-[a-z0-9-]+\.dll|(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|crypt32|bcrypt|ws2_32)\.dll' \
    "$out/dlls.txt")
if [ ! -s "$out/dlls.txt" ] || [ -n "$foreign" ]; then
	echo "media-wine: media-tool.exe imports DLLs that are not Windows' own: $foreign"
	status=1
else
	echo "media-wine: media-tool.exe imports only the system's DLLs ($(wc -l < "$out/dlls.txt"))"
fi

# No POSIX thread layer linked into the .exe (winpthreads' names; libmedia uses the system's threads).
"$LLVM_MINGW/bin/llvm-nm" "$program" > "$out/symbols.txt" 2> /dev/null
if [ ! -s "$out/symbols.txt" ]; then
	echo "media-wine: llvm-nm found no symbol in media-tool.exe"
	status=1
elif grep -i -E 'pthread|winpthread' "$out/symbols.txt" | head -5 | grep .; then
	echo "media-wine: media-tool.exe holds the winpthreads names above"
	status=1
else
	echo "media-wine: media-tool.exe holds no winpthreads name ($(wc -l < "$out/symbols.txt") symbols)"
fi

# The two runs.
timeout 300 "$out/media-tool" "$clip" > "$out/host.txt" 2> "$out/host.log"
host_code=$?
WINEDEBUG=-all timeout 300 flock /tmp/zedbsd-wine.lock wine "$program" "$clip" > "$out/wine.out" 2> "$out/wine.log"
wine_code=$?
tr -d '\r' < "$out/wine.out" > "$out/wine.txt"
tr -d '\r' < "$out/wine.log" > "$out/wine-log.txt"
cat "$out/wine.txt"
if [ "$host_code" != 0 ] || [ "$wine_code" != 0 ]; then
	echo "media-wine: exit host $host_code, Wine $wine_code (logs $out/host.log, $out/wine.log)"
	status=1
fi
if ! cmp -s "$out/host.txt" "$out/wine.txt"; then
	echo "media-wine: the host and Wine printed different lines"
	diff "$out/host.txt" "$out/wine.txt" | head -20
	status=1
fi
pictures=$(grep -c '^media-tool: picture ' "$out/wine.txt")
if [ "$pictures" -lt 8 ] || ! grep -qx 'media-tool: pictures 15' "$out/wine.txt" ||
    ! grep -q '^media-tool: engine 352x288 ' "$out/wine.txt"; then
	echo "media-wine: not every picture came ($pictures hashed)"
	status=1
fi

# The Vulkan loader was found and asked for its devices (the software H.264 then decodes).
if grep -q 'no Vulkan loader' "$out/wine-log.txt" || ! grep -q '^Vulkan Video: ' "$out/wine-log.txt"; then
	echo "media-wine: under Wine the Vulkan loader was not used (log $out/wine.log)"
	status=1
else
	echo "media-wine: under Wine: $(grep '^Vulkan Video: ' "$out/wine-log.txt" | head -1)"
fi
sync
if [ "$status" = 0 ]; then echo "media-wine: PASS"; else echo "media-wine: FAIL"; fi
exit $status
