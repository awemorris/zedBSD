#!/bin/sh
# The Windows cross build, for x86_64 and arm64, through both entries (make browser-win64 and
# browser-winarm64 from the top Makefile, then apps/browser-win32/Makefile alone on the same output):
#  1. the files written for the Windows build (the operating-system layers) compile with warning 0;
#  2. the warnings of libtruetype and the compat libraries (compiled without -Werror) are counted;
#  3. what those libraries leave undefined (nm -u less what they define) is in allowed-imports.txt, and
#     so is what libbrowser.a (all of libbrowser with its Windows layer) and libmedia.a (all of libmedia
#     and its container reader, with media-os-win32.c) do; every function
#     <browser/browser.h> and <browser/storage.h> declare is defined in libbrowser.a, and every name of
#     libbrowser's exports.map matches a defined function (the API of the one .exe); libkeiland.a and the
#     user interface's libbrowser-ui.a leave undefined only what the table has, and libkeiland.a defines
#     every public function of libkeiland (its own, a common file's, or a stub);
#  4. the Vulkan loader's delayed import: plan/ws334/tests/delay-load.c, linked as the build links a
#     program that calls Vulkan, has vulkan-1.dll in its delay-import table and not in its import table;
#     on x86_64 under Wine it prints the loader's version with vulkan-1.dll, and with the DLL disabled
#     (WINEDLLOVERRIDES=vulkan-1=d) it starts, finds no loader and calls no vk* (arm64: linked only);
#  5. libkeiland's Windows files (libkeiland.a) refer to no POSIX thread name (pthread_*): the Windows
#     port uses the system's threads and locks;
#  6a. OmniGrammaton.exe imports only Windows' own DLLs and the UCRT's, vulkan-1.dll delayed;
#  6. one .exe of static libraries: plan/ws334/tests/script-tool-main.c linked by the build's
#     test-program with every static library imports only Windows' own DLLs and the UCRT's (no DLL of
#     this tree, no libwinpthread-1.dll; vulkan-1.dll only as a delayed import); on x86_64 under Wine
#     it prints 16 random bytes and runs a script that reads the clock (arm64: linked only).
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

	# 3b. libbrowser.a (all of libbrowser, with its Windows layer and its storage): what it leaves
	# undefined, less what it, libmedia.a, the static libraries and the Vulkan loader's import library
	# define, against the table.
	core="$build/obj/libbrowser.a"
	media="$build/obj/libmedia.a"
	"${WIN32_TOOL_PREFIX}nm" -u $core 2> /dev/null |
	    awk 'NF == 2 { print $2 } NF == 1 && $1 !~ /:$/ { print $1 }' | sed 's/^__imp_//' |
	    sort -u > "$out/core-undefined-$name.txt"
	"${WIN32_TOOL_PREFIX}nm" --defined-only $core "$media" $archives "$build/obj/libvulkan-1.a" "$build/openssl/lib/libssl.a" "$build/openssl/lib/libcrypto.a" 2> /dev/null |
	    awk 'NF == 3 { print $3 }' | sed 's/^__imp_//' | sort -u > "$out/core-defined-$name.txt"
	comm -23 "$out/core-undefined-$name.txt" "$out/core-defined-$name.txt" > "$out/core-imports-$name.txt"
	if [ ! -s "$out/core-imports-$name.txt" ]; then
		echo "FAILED $name: nm found no undefined symbol in libbrowser.a (no archive?)"
		status=1
	elif comm -23 "$out/core-imports-$name.txt" "$out/allowed.txt" | grep .; then
		echo "FAILED $name: the libbrowser.a symbols above are not in $allowed"
		status=1
	else
		echo "$name: libbrowser.a $(wc -l < "$out/core-imports-$name.txt") imports, all in the table"
	fi

	# 3c. libmedia.a: what it leaves undefined, less what it defines, against the table.  It loads the
	# Vulkan loader at run time (media-os), so it needs nothing of the loader's import library, and it
	# needs nothing of libbrowser.
	"${WIN32_TOOL_PREFIX}nm" -u "$media" 2> /dev/null |
	    awk 'NF == 2 { print $2 } NF == 1 && $1 !~ /:$/ { print $1 }' | sed 's/^__imp_//' |
	    sort -u > "$out/media-undefined-$name.txt"
	"${WIN32_TOOL_PREFIX}nm" --defined-only "$media" 2> /dev/null | awk 'NF == 3 { print $3 }' |
	    sort -u > "$out/media-defined-$name.txt"
	comm -23 "$out/media-undefined-$name.txt" "$out/media-defined-$name.txt" > "$out/media-imports-$name.txt"
	if [ ! -s "$out/media-imports-$name.txt" ]; then
		echo "FAILED $name: nm found no undefined symbol in libmedia.a (no archive?)"
		status=1
	elif grep -n '^vk[A-Z]' "$out/media-imports-$name.txt"; then
		echo "FAILED $name: libmedia.a refers to the Vulkan functions above by name (it loads the loader)"
		status=1
	elif comm -23 "$out/media-imports-$name.txt" "$out/allowed.txt" | grep .; then
		echo "FAILED $name: the libmedia.a symbols above are not in $allowed"
		status=1
	else
		echo "$name: libmedia.a $(wc -l < "$out/media-imports-$name.txt") imports, all in the table"
	fi

	# 3e. libkeiland.a and the user interface's libbrowser-ui.a: what they leave undefined, less what every
	# static library defines, against the table; and every public function of libkeiland (exports.py
	# --list) is defined in libkeiland.a (its own, a common file's or a stub).
	keiland="$build/obj/libkeiland.a"
	interface="$build/obj/libbrowser-ui.a"
	"${WIN32_TOOL_PREFIX}nm" -u "$keiland" "$interface" 2> /dev/null |
	    awk 'NF == 2 { print $2 } NF == 1 && $1 !~ /:$/ { print $1 }' | sed 's/^__imp_//' |
	    sort -u > "$out/ui-undefined-$name.txt"
	"${WIN32_TOOL_PREFIX}nm" --defined-only $core "$media" "$keiland" "$interface" $archives "$build/obj/libvulkan-1.a" 2> /dev/null |
	    awk 'NF == 3 { print $3 }' | sed 's/^__imp_//' | sort -u > "$out/ui-defined-$name.txt"
	comm -23 "$out/ui-undefined-$name.txt" "$out/ui-defined-$name.txt" > "$out/ui-imports-$name.txt"
	"${WIN32_TOOL_PREFIX}nm" --defined-only -g "$keiland" 2> /dev/null | awk 'NF == 3 && $2 == "T" { print $3 }' |
	    sort -u > "$out/keiland-defined-$name.txt"
	/usr/bin/python3 -I userland/desktop/libkeiland/exports.py --list | sort -u > "$out/keiland-exports-$name.txt"
	absent=$(comm -23 "$out/keiland-exports-$name.txt" "$out/keiland-defined-$name.txt")
	if [ ! -s "$out/ui-imports-$name.txt" ] || [ ! -s "$out/keiland-exports-$name.txt" ]; then
		echo "FAILED $name: nm found nothing in libkeiland.a, or exports.py listed nothing"
		status=1
	elif comm -23 "$out/ui-imports-$name.txt" "$out/allowed.txt" | grep .; then
		echo "FAILED $name: the libkeiland.a and libbrowser-ui.a symbols above are not in $allowed"
		status=1
	elif [ -n "$absent" ]; then
		echo "FAILED $name: libkeiland.a does not define:" $absent
		status=1
	else
		echo "$name: libkeiland.a and libbrowser-ui.a $(wc -l < "$out/ui-imports-$name.txt") imports, all in the table; libkeiland.a defines the $(wc -l < "$out/keiland-exports-$name.txt") public functions"
	fi

	# 3d. The public API: the functions the public headers declare are defined in libbrowser.a (as
	# global text), and each name or glob of exports.map's global list matches one of them.
	"${WIN32_TOOL_PREFIX}nm" --defined-only -g $core 2> /dev/null | awk 'NF == 3 && $2 == "T" { print $3 }' |
	    sort -u > "$out/api-defined-$name.txt"
	sed -n 's/^[a-z].*[ *]\(browser_[a-z_]*\)(.*/\1/p' userland/desktop/include/browser/browser.h \
	    userland/desktop/include/browser/storage.h | sort -u > "$out/api-declared-$name.txt"
	sed -n '/global:/,/local:/s/^[[:space:]]*\(browser_[a-z_*]*\);.*/\1/p' userland/desktop/libbrowser/exports.map \
	    > "$out/api-exports-$name.txt"
	missing=$(comm -23 "$out/api-declared-$name.txt" "$out/api-defined-$name.txt")
	unmatched=
	while read -r pattern; do
		case "$pattern" in
		*'*') matched=$(grep -c "^${pattern%\*}" "$out/api-defined-$name.txt") ;;
		*) matched=$(grep -c -x "$pattern" "$out/api-defined-$name.txt") ;;
		esac
		[ "$matched" -gt 0 ] || unmatched="$unmatched $pattern"
	done < "$out/api-exports-$name.txt"
	if [ ! -s "$out/api-declared-$name.txt" ] || [ ! -s "$out/api-exports-$name.txt" ]; then
		echo "FAILED $name: no function found in the public headers or exports.map"
		status=1
	elif [ -n "$missing" ] || [ -n "$unmatched" ]; then
		echo "FAILED $name: libbrowser.a does not define:" $missing "; exports.map names matching nothing:$unmatched"
		status=1
	else
		echo "$name: libbrowser.a defines the $(wc -l < "$out/api-declared-$name.txt") functions of the public headers, exports.map's $(wc -l < "$out/api-exports-$name.txt") names all match"
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

	# 5. No POSIX thread name in libkeiland's Windows files.
	"${WIN32_TOOL_PREFIX}nm" -u "$build/obj/libkeiland.a" 2> /dev/null |
	    awk 'NF == 2 { print $2 } NF == 1 && $1 !~ /:$/ { print $1 }' | sed 's/^__imp_//' |
	    sort -u > "$out/keiland-undefined-$name.txt"
	if [ ! -s "$out/keiland-undefined-$name.txt" ]; then
		echo "FAILED $name: nm found no undefined symbol in libkeiland.a (no archive?)"
		status=1
	elif grep -n '^_*pthread_' "$out/keiland-undefined-$name.txt"; then
		echo "FAILED $name: libkeiland.a refers to the POSIX thread names above"
		status=1
	else
		echo "$name: libkeiland.a refers to no pthread_ name"
	fi

	# 6a. OmniGrammaton.exe: the system's DLLs and the UCRT's only, vulkan-1.dll delayed.
	"$LLVM_MINGW/bin/llvm-readobj" --coff-imports "$build/OmniGrammaton.exe" > "$out/omnigrammaton-$name.imports" 2>&1
	sed -n '/^Import {/,/^}/s/^ *Name: //p' "$out/omnigrammaton-$name.imports" | sort -u > "$out/omnigrammaton-dlls-$name.txt"
	sed -n '/^DelayImport {/,/^}/s/^ *Name: //p' "$out/omnigrammaton-$name.imports" | sort -u > "$out/omnigrammaton-delayed-$name.txt"
	foreign=$(grep -v -i -x -E 'api-ms-win-crt-[a-z0-9-]+\.dll|(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|crypt32|bcrypt|ws2_32|iphlpapi|imm32|dwmapi|shcore|uiautomationcore|uxtheme)\.dll' \
	    "$out/omnigrammaton-dlls-$name.txt")
	delayed=$(grep -v -i -x 'vulkan-1\.dll' "$out/omnigrammaton-delayed-$name.txt")
	if [ ! -s "$out/omnigrammaton-dlls-$name.txt" ]; then
		echo "FAILED $name: llvm-readobj found no import in OmniGrammaton.exe"
		status=1
	elif [ -n "$foreign" ] || [ -n "$delayed" ]; then
		echo "FAILED $name: OmniGrammaton.exe imports DLLs that are not Windows' own: $foreign $delayed"
		status=1
	else
		echo "$name: OmniGrammaton.exe imports only the system's DLLs ($(wc -l < "$out/omnigrammaton-dlls-$name.txt")), vulkan-1.dll delayed: $(cat "$out/omnigrammaton-delayed-$name.txt")"
	fi

	# 6. One .exe of the static libraries, and what it imports.
	program=$build/tests/script-tool.exe
	if ! timeout 900 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=$arch \
	    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" \
	    test-program TEST_PROGRAM=script-tool TEST_PROGRAM_SOURCES=plan/ws334/tests/script-tool-main.c \
	    > "$out/program-$name.log" 2>&1 || grep -q 'warning:' "$out/program-$name.log"; then
		echo "FAILED $name: the test program did not link cleanly (log $out/program-$name.log)"
		tail -10 "$out/program-$name.log"
		status=1
		continue
	fi
	"$LLVM_MINGW/bin/llvm-readobj" --coff-imports "$program" > "$out/program-$name.imports"
	sed -n '/^Import {/,/^}/s/^ *Name: //p' "$out/program-$name.imports" | sort -u > "$out/program-dlls-$name.txt"
	sed -n '/^DelayImport {/,/^}/s/^ *Name: //p' "$out/program-$name.imports" | sort -u > "$out/program-delayed-$name.txt"
	foreign=$(grep -v -i -x -E 'api-ms-win-crt-[a-z0-9-]+\.dll|(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|crypt32|bcrypt|ws2_32|iphlpapi|imm32|dwmapi|shcore|uiautomationcore|uxtheme)\.dll' \
	    "$out/program-dlls-$name.txt")
	delayed=$(grep -v -i -x 'vulkan-1\.dll' "$out/program-delayed-$name.txt")
	if [ ! -s "$out/program-dlls-$name.txt" ]; then
		echo "FAILED $name: llvm-readobj found no import in $program"
		status=1
	elif [ -n "$foreign" ] || [ -n "$delayed" ]; then
		echo "FAILED $name: $program imports DLLs that are not Windows' own: $foreign $delayed"
		status=1
	else
		echo "$name: one .exe, imports only the system's DLLs ($(wc -l < "$out/program-dlls-$name.txt"))"
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

	# 6 under Wine: the random bytes, and a script that reads the clock.
	WINEDEBUG=-all timeout 120 wine "$program" random > "$out/program-random.out" 2> "$out/program-random.log"
	code=$?
	random=$(tr -d '\r' < "$out/program-random.out")
	if [ "$code" != 0 ] || ! echo "$random" | grep -q -x '[0-9a-f]\{32\}'; then
		echo "FAILED $name: the test program under Wine: exit $code, '$random'"
		status=1
	else
		echo "$name: the test program under Wine: 16 random bytes"
	fi
	printf 'print(typeof Date.now(), String(Date.now()).length);\n' > "$out/program-clock.js"
	WINEDEBUG=-all timeout 120 wine "$program" "$out/program-clock.js" > "$out/program-clock.out" \
	    2> "$out/program-clock.log"
	code=$?
	clock=$(tr -d '\r' < "$out/program-clock.out")
	if [ "$code" != 0 ] || [ "$clock" != 'number 13' ]; then
		echo "FAILED $name: the test program's script under Wine: exit $code, '$clock'"
		status=1
	else
		echo "$name: the test program's script under Wine: $clock"
	fi
done

if [ "$status" = 0 ]; then echo "cross-build: PASS"; else echo "cross-build: FAIL"; fi
exit "$status"
