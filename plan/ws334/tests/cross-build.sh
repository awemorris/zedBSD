#!/bin/sh
# The Windows cross build, for x86_64 and arm64, through both entries (make browser-win64 and
# browser-winarm64 from the top Makefile, then apps/browser-win32/Makefile alone on the same output):
#  1. the files written for the Windows build (the operating-system layers) compile with warning 0;
#  2. the warnings of libtruetype and the compat libraries (compiled without -Werror) are counted;
#  3. what those libraries leave undefined (nm -u less what they define) is in allowed-imports.txt;
#  4. the Vulkan loader's delayed import: plan/ws334/tests/delay-load.c, linked as the build links a
#     program that calls Vulkan, has vulkan-1.dll in its delay-import table and not in its import table;
#     on x86_64 under Wine it prints the loader's version with vulkan-1.dll, and with the DLL disabled
#     (WINEDLLOVERRIDES=vulkan-1=d) it starts, finds no loader and calls no vk* (arm64: linked only).
#   sh plan/ws334/tests/cross-build.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# WINEPREFIX=<dir> the Wine prefix (default build/tmp/ws334-wine).  The fetched tarballs are kept in
# build/tmp/ws334-win32-sources between runs.
# Last line: cross-build: PASS or cross-build: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws334-cross
fresh_out "$out"
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
WINEPREFIX=${WINEPREFIX:-$(pwd)/build/tmp/ws334-wine}
export WINEPREFIX
sources=build/tmp/ws334-win32-sources
mkdir -p "$sources"
allowed=plan/ws334/tests/allowed-imports.txt
status=0

for name in win64 winarm64; do
	case "$name" in
	win64) arch=x86_64 ;;
	winarm64) arch=aarch64 ;;
	esac
	build=$out/browser-$name
	log=$out/make-$name.log

	# The build, from the top Makefile and then from its own.
	if ! timeout 900 make --no-print-directory browser-$name LLVM_MINGW="$LLVM_MINGW" \
	    BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" > "$log" 2>&1; then
		echo "FAILED make browser-$name (log $log)"
		tail -10 "$log"
		status=1
		continue
	fi
	if ! timeout 900 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=$arch \
	    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" \
	    >> "$log" 2>&1; then
		echo "FAILED make -f apps/browser-win32/Makefile WIN32_ARCH=$arch (log $log)"
		tail -10 "$log"
		status=1
		continue
	fi

	# 1. The layers' warnings (the compat libraries' are in their own logs, not in the make output).
	if grep -n 'warning:' "$log"; then
		echo "FAILED make browser-$name warned (above)"
		status=1
	fi

	# 2. The compat libraries' warnings, as toolchain.txt counts them.
	count=$(sed -n 's/^compat and libtruetype warnings: //p' "$build/obj/toolchain.txt")
	if [ -z "$count" ]; then
		echo "FAILED $name: toolchain.txt has no count of the compat warnings"
		status=1
	else
		echo "$name: compat and libtruetype warnings: $count"
	fi

	# 3. The undefined symbols of the static libraries, less what they define, against the table.
	eval "$(make -s --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=$arch \
	    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" flags |
	    sed "s/'/'\\\\''/g; s/^\([A-Z0-9_]*\)=\(.*\)$/\1='\2'/")"
	archives="$build/obj/libtruetype.a $build/obj/libpng-compat.a $build/obj/libjpeg-compat.a
		$build/obj/libgif-compat.a $build/obj/libz-compat.a"
	"${WIN32_TOOL_PREFIX}nm" -u $archives 2> /dev/null |
	    awk 'NF == 2 { print $2 } NF == 1 && $1 !~ /:$/ { print $1 }' | sed 's/^__imp_//' |
	    sort -u > "$out/undefined-$name.txt"
	"${WIN32_TOOL_PREFIX}nm" --defined-only $archives 2> /dev/null | awk 'NF == 3 { print $3 }' |
	    sort -u > "$out/defined-$name.txt"
	sed 's/#.*//; /^[[:space:]]*$/d; s/[[:space:]]//g' "$allowed" | sort -u > "$out/allowed.txt"
	comm -23 "$out/undefined-$name.txt" "$out/defined-$name.txt" > "$out/imports-$name.txt"
	if [ ! -s "$out/imports-$name.txt" ]; then
		echo "FAILED $name: nm found no undefined symbol (no archives?)"
		status=1
	elif comm -23 "$out/imports-$name.txt" "$out/allowed.txt" | grep .; then
		echo "FAILED $name: the symbols above are not in $allowed"
		status=1
	else
		echo "$name: $(wc -l < "$out/imports-$name.txt") imports, all in the table"
	fi

	# 4. The delayed import of the Vulkan loader.
	exe=$out/delay-load-$name.exe
	if ! $WIN32_CC $WIN32_CFLAGS plan/ws334/tests/delay-load.c -o "$exe" $WIN32_VULKAN_LDFLAGS \
	    > "$out/delay-load-$name.log" 2>&1 || [ -s "$out/delay-load-$name.log" ]; then
		echo "FAILED $name: delay-load.c did not link cleanly"
		head -10 "$out/delay-load-$name.log"
		status=1
		continue
	fi
	"$LLVM_MINGW/bin/llvm-readobj" --coff-imports "$exe" > "$out/delay-load-$name.imports"
	if ! grep -A1 '^DelayImport {' "$out/delay-load-$name.imports" | grep -q 'Name: vulkan-1.dll'; then
		echo "FAILED $name: vulkan-1.dll is not a delayed import"
		status=1
	elif grep -A1 '^Import {' "$out/delay-load-$name.imports" | grep -q 'Name: vulkan-1.dll'; then
		echo "FAILED $name: vulkan-1.dll is also an ordinary import"
		status=1
	else
		echo "$name: vulkan-1.dll is a delayed import"
	fi
	[ "$arch" = x86_64 ] || continue
	if ! command -v wine > /dev/null 2>&1; then
		echo "FAILED wine is not installed (the delayed import is not run)"
		status=1
		continue
	fi
	WINEDEBUG=-all timeout 120 wine "$exe" > "$out/wine-with.out" 2> "$out/wine-with.log"
	code=$?
	with=$(tr -d '\r' < "$out/wine-with.out")
	if [ "$code" != 0 ] || ! echo "$with" | grep -q '^delay-load: Vulkan [0-9]*\.[0-9]*$'; then
		echo "FAILED $name under Wine with vulkan-1.dll: exit $code, '$with'"
		status=1
	else
		echo "$name under Wine with vulkan-1.dll: $with"
	fi
	WINEDEBUG=-all WINEDLLOVERRIDES=vulkan-1=d timeout 120 wine "$exe" > "$out/wine-without.out" \
	    2> "$out/wine-without.log"
	code=$?
	without=$(tr -d '\r' < "$out/wine-without.out")
	if [ "$code" != 0 ] || [ "$without" != 'delay-load: no vulkan-1.dll, no Vulkan call' ]; then
		echo "FAILED $name under Wine without vulkan-1.dll: exit $code, '$without'"
		status=1
	else
		echo "$name under Wine without vulkan-1.dll: $without"
	fi
done

if [ "$status" = 0 ]; then echo "cross-build: PASS"; else echo "cross-build: FAIL"; fi
exit "$status"
