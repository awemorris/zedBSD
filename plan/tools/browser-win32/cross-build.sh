#!/bin/sh
# The Windows cross build, for x86_64 (the CI's; aarch64 when asked, below), through both entries (make browser-win32
# WIN32_ARCH=<arch> from the top Makefile, then apps/browser-win32/Makefile alone on the same output):
#  1. the files written for the Windows build (the operating-system layers) compile with warning 0;
#  2. the warnings of libtruetype and the compat libraries (compiled without -Werror) are counted;
#  3. what those libraries leave undefined (nm -u less what they define) is in allowed-imports.txt, and
#     so is what libbrowser.a (all of libbrowser with its Windows layer) and libmedia.a (all of libmedia
#     and its container reader, with media-os-win32.c) do; every function
#     <browser/browser.h> and <browser/storage.h> declare is defined in libbrowser.a, and every name of
#     libbrowser's exports.map matches a defined function (the API of the one .exe); libkeiland.a and the
#     user interface's libbrowser-ui.a leave undefined only what the table has, and libkeiland.a defines
#     every public function of libkeiland (its own, a common file's, or a stub);
#  4. the Vulkan loader is an ordinary import: OmniGrammaton.exe has vulkan-1.dll in its import table and
#     no delay-import table (the browser needs Vulkan to start);
#  5. no POSIX thread layer: the static libraries (libbrowser, libmedia, libkeiland, the user interface, the
#     compat and TLS libraries) refer to no POSIX thread name (pthread_*, nor winpthreads' clock_gettime
#     and nanosleep), and OmniGrammaton.exe (its copy with symbols) holds no winpthreads name: every
#     library uses the system's threads and locks;
#  6a. OmniGrammaton.exe imports only Windows' own DLLs, the UCRT's and vulkan-1.dll;
#  7. the zip (make -f apps/browser-win32/Makefile WIN32_ARCH=<arch> zip): it holds what
#     plan/tools/browser-win32/zip-contents.txt lists, is the same bytes when made again, and its icon has 8 images
#     (ImageMagick's identify);
#  6. one .exe of static libraries: plan/tools/browser-win32/script-tool-main.c linked by the build's
#     test-program with every static library imports only Windows' own DLLs, the UCRT's and at most
#     vulkan-1.dll, none delayed (no DLL of this tree, no libwinpthread-1.dll), and holds no winpthreads
#     name either; on x86_64 under Wine it
#     prints 16 random bytes and runs a script that reads the clock (arm64: linked only).
#   sh plan/tools/browser-win32/cross-build.sh                               x86_64
#   WIN32_ARCH=aarch64 sh plan/tools/browser-win32/cross-build.sh            arm64 only
#   WIN32_ARCH="x86_64 aarch64" sh plan/tools/browser-win32/cross-build.sh   both
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# WINEPREFIX=<dir> the Wine prefix (default build/tmp/browser-win32-wine).  The fetched tarballs are kept in
# build/tmp/browser-win32-sources between runs.
# Last line: cross-build: PASS or cross-build: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/browser-win32-cross
fresh_out "$out"
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
WINEPREFIX=${WINEPREFIX:-$(pwd)/build/tmp/browser-win32-wine}
export WINEPREFIX
sources=build/tmp/browser-win32-sources
mkdir -p "$sources"
allowed=plan/tools/browser-win32/allowed-imports.txt
status=0

for arch in ${WIN32_ARCH:-x86_64}; do
	case "$arch" in
	x86_64) name=win64 ;;
	aarch64) name=winarm64 ;;
	*) echo "FAILED WIN32_ARCH: $arch is not x86_64 or aarch64"; status=1; continue ;;
	esac
	build=$out/browser-$name
	log=$out/make-$name.log

	# The build, from the top Makefile and then from its own.
	if ! timeout 900 make --no-print-directory browser-win32 WIN32_ARCH=$arch \
	    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" > "$log" 2>&1; then
		echo "FAILED make browser-win32 WIN32_ARCH=$arch (log $log)"
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
		echo "FAILED make browser-win32 WIN32_ARCH=$arch warned (above)"
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
		$build/obj/libgif-compat.a $build/obj/libz-compat.a $build/obj/libwebp-compat.a"
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
	"${WIN32_TOOL_PREFIX}nm" --defined-only $core "$media" $archives "$build/obj/libvulkan-1.a" "$build/obj/libssl-compat.a" "$build/obj/libcrypto-compat.a" 2> /dev/null |
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

	# 4. The Vulkan loader, an ordinary import of OmniGrammaton.exe (no delay-import table).
	"$LLVM_MINGW/bin/llvm-readobj" --coff-imports "$build/OmniGrammaton.exe" > "$out/omnigrammaton-$name.imports" 2>&1
	if ! grep -A1 '^Import {' "$out/omnigrammaton-$name.imports" | grep -q 'Name: vulkan-1.dll'; then
		echo "FAILED $name: vulkan-1.dll is not an import of OmniGrammaton.exe"
		status=1
	elif grep -q '^DelayImport {' "$out/omnigrammaton-$name.imports"; then
		echo "FAILED $name: OmniGrammaton.exe has delayed imports"
		status=1
	else
		echo "$name: vulkan-1.dll is an ordinary import of OmniGrammaton.exe"
	fi

	# 5. No POSIX thread layer: no POSIX thread name in the static libraries, no winpthreads in the program.
	"${WIN32_TOOL_PREFIX}nm" -u $core "$media" "$build/obj/libkeiland.a" "$interface" $archives \
	    "$build/obj/libssl-compat.a" "$build/obj/libcrypto-compat.a" 2> /dev/null |
	    awk 'NF == 2 { print $2 } NF == 1 && $1 !~ /:$/ { print $1 }' | sed 's/^__imp_//' |
	    sort -u > "$out/threads-undefined-$name.txt"
	"${WIN32_TOOL_PREFIX}nm" "$build/obj/OmniGrammaton-full.exe" 2> /dev/null | awk '{ print $NF }' |
	    sort -u > "$out/omnigrammaton-symbols-$name.txt"
	if [ ! -s "$out/threads-undefined-$name.txt" ] || [ ! -s "$out/omnigrammaton-symbols-$name.txt" ]; then
		echo "FAILED $name: nm found no undefined symbol in the static libraries or no symbol in OmniGrammaton-full.exe"
		status=1
	elif grep -n -E '^_*(pthread_|clock_gettime|nanosleep)' "$out/threads-undefined-$name.txt"; then
		echo "FAILED $name: the static libraries refer to the POSIX thread names above"
		status=1
	elif grep -n -i -E 'pthread|winpthread' "$out/omnigrammaton-symbols-$name.txt"; then
		echo "FAILED $name: OmniGrammaton.exe holds the winpthreads names above"
		status=1
	else
		echo "$name: the static libraries refer to no pthread_ name; OmniGrammaton.exe holds no winpthreads name ($(wc -l < "$out/omnigrammaton-symbols-$name.txt") symbols)"
	fi

	# 6a. OmniGrammaton.exe: the system's DLLs, the UCRT's and vulkan-1.dll only.
	sed -n '/^Import {/,/^}/s/^ *Name: //p' "$out/omnigrammaton-$name.imports" | sort -u > "$out/omnigrammaton-dlls-$name.txt"
	foreign=$(grep -v -i -x -E 'api-ms-win-crt-[a-z0-9-]+\.dll|(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|crypt32|bcrypt|ws2_32|iphlpapi|imm32|dwmapi|shcore|uiautomationcore|uxtheme|vulkan-1)\.dll' \
	    "$out/omnigrammaton-dlls-$name.txt")
	if [ ! -s "$out/omnigrammaton-dlls-$name.txt" ]; then
		echo "FAILED $name: llvm-readobj found no import in OmniGrammaton.exe"
		status=1
	elif [ -n "$foreign" ]; then
		echo "FAILED $name: OmniGrammaton.exe imports DLLs that are not Windows' own: $foreign"
		status=1
	else
		echo "$name: OmniGrammaton.exe imports only the system's DLLs and vulkan-1.dll ($(wc -l < "$out/omnigrammaton-dlls-$name.txt"))"
	fi

	# 7. The zip (apps/browser-win32/Makefile's zip target): what it holds is plan/tools/browser-win32/zip-contents.txt,
	# the same bytes when it is made again from the same directory, and the icon of 8 images (ImageMagick's
	# identify).
	zip=$build/OmniGrammaton-$(sed -n 's/^OMNIGRAMMATON_VERSION := //p' apps/browser-win32/version.mk)-$name.zip
	if ! timeout 900 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=$arch zip \
	    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" \
	    > "$out/zip-$name.log" 2>&1 || [ ! -f "$zip" ]; then
		echo "FAILED $name: make -f apps/browser-win32/Makefile WIN32_ARCH=$arch zip (log $out/zip-$name.log)"
		tail -5 "$out/zip-$name.log"
		status=1
	else
		unzip -Z1 "$zip" | sed 's|^OmniGrammaton/||' > "$out/zip-$name.txt"
		sed '/^#/d' plan/tools/browser-win32/zip-contents.txt > "$out/zip-expected.txt"
		/usr/bin/python3 -I apps/browser-win32/tools/make-zip.py "$build/OmniGrammaton" "$out/zip-again-$name.zip" > /dev/null
		icons=$(identify "$build/obj/OmniGrammaton.ico" 2> /dev/null | wc -l)
		if ! cmp -s "$out/zip-$name.txt" "$out/zip-expected.txt"; then
			echo "FAILED $name: the zip holds other files than plan/tools/browser-win32/zip-contents.txt:"
			diff "$out/zip-expected.txt" "$out/zip-$name.txt" | head -10
			status=1
		elif ! cmp -s "$zip" "$out/zip-again-$name.zip"; then
			echo "FAILED $name: the zip made again differs"
			status=1
		elif [ "$icons" != 8 ]; then
			echo "FAILED $name: identify reads $icons images in OmniGrammaton.ico, not 8"
			status=1
		else
			echo "$name: $(basename "$zip"): $(wc -l < "$out/zip-$name.txt") files as listed, the same bytes made again ($(sha256sum "$zip" | cut -c1-16)); the icon has 8 images"
		fi
	fi

	# 6. One .exe of the static libraries, and what it imports.
	program=$build/tests/script-tool.exe
	if ! timeout 900 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=$arch \
	    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" \
	    test-program TEST_PROGRAM=script-tool TEST_PROGRAM_SOURCES=plan/tools/browser-win32/script-tool-main.c \
	    > "$out/program-$name.log" 2>&1 || grep -q 'warning:' "$out/program-$name.log"; then
		echo "FAILED $name: the test program did not link cleanly (log $out/program-$name.log)"
		tail -10 "$out/program-$name.log"
		status=1
		continue
	fi
	"$LLVM_MINGW/bin/llvm-readobj" --coff-imports "$program" > "$out/program-$name.imports"
	sed -n '/^Import {/,/^}/s/^ *Name: //p' "$out/program-$name.imports" | sort -u > "$out/program-dlls-$name.txt"
	foreign=$(grep -v -i -x -E 'api-ms-win-crt-[a-z0-9-]+\.dll|(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|crypt32|bcrypt|ws2_32|iphlpapi|imm32|dwmapi|shcore|uiautomationcore|uxtheme|vulkan-1)\.dll' \
	    "$out/program-dlls-$name.txt")
	if [ ! -s "$out/program-dlls-$name.txt" ]; then
		echo "FAILED $name: llvm-readobj found no import in $program"
		status=1
	elif [ -n "$foreign" ] || grep -q '^DelayImport {' "$out/program-$name.imports"; then
		echo "FAILED $name: $program imports DLLs that are not Windows' own, or delays one: $foreign"
		status=1
	elif "${WIN32_TOOL_PREFIX}nm" "$program" 2> /dev/null | grep -i -E 'pthread|winpthread'; then
		echo "FAILED $name: $program holds the winpthreads names above"
		status=1
	else
		echo "$name: one .exe, imports only the system's DLLs ($(wc -l < "$out/program-dlls-$name.txt"))"
	fi
	[ "$arch" = x86_64 ] || continue
	if ! command -v wine > /dev/null 2>&1; then
		echo "FAILED wine is not installed (the test program is not run)"
		status=1
		continue
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
