#!/bin/sh
# libwebp-compat as the Windows build has it: builds the static library with apps/browser-win32/Makefile (the same
# source list and flags as OmniGrammaton's, WIN32_ARCH=x86_64), counts its compiler warnings, links
# plan/ws344/tests/webp-driver.c with it into a Windows program, runs that under Wine on every fixture and compares
# each report with the one the driver built against libwebp 1.5.0 wrote on the host (FIXTURES/expect/NAME.ref).
# Needs a mingw-w64 UCRT compiler (the Makefile's choice: llvm-mingw or x86_64-w64-mingw32ucrt-gcc) and wine.
#   sh plan/ws344/tests/platform/webp-wine.sh [FIXTURES...]     (default build/tmp/ws344-fixtures)
# Last line: webp-wine: PASS (<n> files, 0 warnings) or webp-wine: FAIL (...).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
fixtures=${*:-build/tmp/ws344-fixtures}
win=build/ws344-win64
obj=$win/obj
. plan/tools/fresh-out.sh
fresh_out build/tmp/ws344-wine
out=build/tmp/ws344-wine

# The static library and the compat headers, as the Windows build makes them.
timeout 1800 make -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 BROWSER_WIN32_BUILD=$win $obj/libwebp-compat.a \
	$obj/include/compat/webp/decode.h $obj/include/compat/webp/types.h > "$out/make.log" 2>&1 || {
	tail -20 "$out/make.log"; echo "webp-wine: FAIL (static library)"; exit 1; }
warnings=$(cat $obj/userland/desktop/libwebp-compat/*.o.log | grep -c 'warning:')
compiler=$(make -s -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 BROWSER_WIN32_BUILD=$win flags 2>/dev/null |
	sed -n 's/^WIN32_CC=//p')
[ -n "$compiler" ] || compiler=x86_64-w64-mingw32ucrt-gcc

# The driver, a static Windows program.
$compiler -std=gnu11 -O2 -Wall -Wextra -Werror -I$obj/include plan/ws344/tests/webp-driver.c $obj/libwebp-compat.a \
	-static -o "$out/webp-driver.exe" > "$out/driver.log" 2>&1 || { cat "$out/driver.log"; echo "webp-wine: FAIL (driver)"; exit 1; }

# Every fixture under Wine, its report against libwebp's.
WINEDEBUG=-all
WINEPREFIX=$(pwd)/build/ws344-wineprefix
export WINEDEBUG WINEPREFIX
total=0
failed=0
for set in $fixtures; do
	for ref in "$set"/expect/*.ref; do
		name=$(basename "$ref" .ref)
		total=$((total + 1))
		timeout 300 wine "$out/webp-driver.exe" "$set/files/$name.webp" 2>/dev/null | tr -d '\r' > "$out/$name.out"
		if ! cmp -s "$out/$name.out" "$ref"; then
			failed=$((failed + 1))
			echo "$set/$name" >> "$out/failed.txt"
		fi
	done
done
if [ "$failed" = 0 ] && [ "$total" -gt 0 ] && [ "$warnings" = 0 ]; then
	echo "webp-wine: PASS ($total files, $warnings warnings)"
	exit 0
fi
head -10 "$out/failed.txt" 2>/dev/null
echo "webp-wine: FAIL ($failed of $total differ, $warnings warnings; $out)"
exit 1
