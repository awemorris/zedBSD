#!/bin/sh
# libbrowser's script core alone (LIBBROWSER_SCRIPT_SOURCES of userland/desktop/libbrowser/sources.mk:
# the base library, the virtual machine, JavaScript and the POSIX operating-system layer) as a library:
#  1. host: each file compiled with the host's cc (-std=gnu11 -Wall -Wextra -Werror) into a static
#     archive, libbrowser-script.a;
#  2. host: what the archive leaves undefined (nm -u less what it defines) is only functions of the
#     C library, libm, libpthread, libdl and the dynamic loader (the table made from nm -D of the
#     host's libraries), so nothing of the engine (dom, bind, page, net, html, css, layout, paint,
#     text, image) is needed;
#  3. host: apps/browser-win32/tools/script-core-main.c linked with the archive and nothing else of libbrowser
#     runs plan/ws074/tests/js (run-js-tests.py --program, compared with Chromium's output) and the
#     tests of plan/tools/browser-win32/script-core (many exceptions, enough garbage to collect many times, the
#     local time's string forms, Math, JSON, regular expressions and a promise), compared with their
#     .expected files;
#  4. Windows x86_64 and aarch64: apps/browser-win32/Makefile's libbrowser-script (the core with its
#     Windows layer) and script-core.exe build with warning 0; what the archive leaves undefined is in
#     allowed-imports.txt; the .exe imports only Windows' own DLLs and the UCRT's (no Vulkan);
#  5. Windows x86_64 under Wine: script-core.exe writes the same bytes as the host's program for every
#     test of 3 (standard output and standard error, the same time zone).  aarch64: built only.
#   sh plan/tools/browser-win32/script-core-link.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# the Wine prefix is build/tmp/browser-win32-wine (run-unit.sh's).  The time zone of the runs is
# America/New_York.  Output under build/tmp/browser-win32-script-core (a fresh run directory).
# Last line: script-core-link: PASS or script-core-link: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
. plan/tools/fresh-out.sh
out=build/tmp/browser-win32-script-core
fresh_out "$out"
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
WINEPREFIX=$top/build/tmp/browser-win32-wine
export WINEPREFIX
TZ=America/New_York
export TZ
sources=build/tmp/browser-win32-sources
mkdir -p "$sources"
allowed=plan/tools/browser-win32/allowed-imports.txt
status=0

# The script core's files, from the list the zedBSD build uses.
script_sources=$(make -s --no-print-directory -f userland/desktop/libbrowser/sources.mk \
    --eval 'script-sources: ; @echo $(LIBBROWSER_SCRIPT_SOURCES)' script-sources)
if [ -z "$script_sources" ]; then
	echo "FAILED no LIBBROWSER_SCRIPT_SOURCES in userland/desktop/libbrowser/sources.mk"
	echo "script-core-link: FAIL"
	exit 1
fi
echo "host: $(echo $script_sources | wc -w) files in LIBBROWSER_SCRIPT_SOURCES"

# 1. The host's archive.  An object is named after its path, since files of the same name are in
# different directories.
host=$out/host
mkdir -p "$host/obj"
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I. -Iuserland/desktop/include -Iuserland/desktop/libbrowser"
printf '%s\n' $script_sources | sed "s#^\\(.*\\)\\.c\$#\\1.c $host/obj/\\1.o#" > "$host/objects.txt"
if ! timeout 900 xargs -P "$(nproc)" -L 1 sh -c 'mkdir -p "$(dirname "$2")" && cc '"$flags"' -c "$1" -o "$2"' sh \
    < "$host/objects.txt" > "$host/compile.log" 2>&1; then
	echo "FAILED host: the script core did not compile (log $host/compile.log)"
	tail -20 "$host/compile.log"
	echo "script-core-link: FAIL"
	exit 1
fi
if grep -n 'warning:' "$host/compile.log"; then
	echo "FAILED host: the script core warned (above)"
	status=1
fi
objects=$(awk '{ print $2 }' "$host/objects.txt")
archive=$host/libbrowser-script.a
ar qcs "$archive" $objects
echo "host: $archive, $(ar t "$archive" | wc -l) members"

# 2. What the archive leaves undefined, against the host's libraries.
nm -u "$archive" | awk 'NF == 2 { print $2 } NF == 1 && $1 !~ /:$/ { print $1 }' | sort -u > "$host/undefined.txt"
nm -g --defined-only "$archive" | awk 'NF == 3 { print $3 }' | sort -u > "$host/defined.txt"
comm -23 "$host/undefined.txt" "$host/defined.txt" > "$host/imports.txt"
: > "$host/system.txt"
for library in libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 ld-linux-x86-64.so.2; do
	path=/lib/x86_64-linux-gnu/$library
	[ -e "$path" ] || continue
	nm -D --defined-only "$path" | awk 'NF == 3 { print $3 }' | sed 's/@.*//' >> "$host/system.txt"
done
# The linker's own symbol, which each object that reaches a global through the GOT names.
echo _GLOBAL_OFFSET_TABLE_ >> "$host/system.txt"
sort -u -o "$host/system.txt" "$host/system.txt"
comm -23 "$host/imports.txt" "$host/system.txt" > "$host/foreign.txt"
if [ ! -s "$host/imports.txt" ]; then
	echo "FAILED host: nm found no undefined symbol in $archive"
	status=1
elif [ -s "$host/foreign.txt" ]; then
	echo "FAILED host: $archive needs symbols that are not the system's:"
	cat "$host/foreign.txt"
	status=1
else
	echo "host: $(wc -l < "$host/imports.txt") undefined symbols, all of the C library, libm, libpthread, libdl or the loader ($host/imports.txt)"
fi

# 3. The program over the archive alone, and the tests.
program=$host/script-core
if ! timeout 300 cc $flags apps/browser-win32/tools/script-core-main.c "$archive" -lm -lpthread -ldl -o "$program" \
    > "$host/link.log" 2>&1 || [ -s "$host/link.log" ]; then
	echo "FAILED host: script-core-main.c did not link cleanly with the archive alone (log $host/link.log)"
	cat "$host/link.log"
	echo "script-core-link: FAIL"
	exit 1
fi
echo "host: $program linked with $archive alone"
timeout 1800 /usr/bin/python3 -I plan/ws074/tests/run-js-tests.py --program "$top/$program" > "$host/js-tests.log" 2>&1
code=$?
result=$(tail -1 "$host/js-tests.log")
if [ "$code" != 0 ]; then
	echo "FAILED host: plan/ws074/tests/js: $result (log $host/js-tests.log)"
	grep -v '^pass ' "$host/js-tests.log" | head -20
	status=1
else
	echo "host: plan/ws074/tests/js: $result"
fi

# The tests of both lists, each run by the host's program; the outputs are kept for 5.
tests=$(ls plan/ws074/tests/js/*.js plan/tools/browser-win32/script-core/*.js)
mkdir -p "$host/out"
for test in $tests; do
	name=$(basename "$(dirname "$test")")-$(basename "$test" .js)
	timeout 300 "$program" --js "$test" > "$host/out/$name.out" 2> "$host/out/$name.err"
	echo $? > "$host/out/$name.code"
done
passed=0
count=0
for test in plan/tools/browser-win32/script-core/*.js; do
	name=script-core-$(basename "$test" .js)
	count=$((count + 1))
	if cmp -s "$host/out/$name.out" "${test%.js}.expected" && [ ! -s "$host/out/$name.err" ] &&
	    [ "$(cat "$host/out/$name.code")" = 0 ]; then
		passed=$((passed + 1))
	else
		echo "FAILED host: $test (exit $(cat "$host/out/$name.code")):"
		diff "${test%.js}.expected" "$host/out/$name.out" | head -10
		head -5 "$host/out/$name.err"
		status=1
	fi
done
echo "host: plan/tools/browser-win32/script-core: $passed/$count as expected"

# 4 and 5 on Windows.
for name in win64 winarm64; do
	case "$name" in
	win64) arch=x86_64 ;;
	winarm64) arch=aarch64 ;;
	esac
	build=$out/browser-$name
	log=$out/make-$name.log
	if ! timeout 900 make --no-print-directory -j"$(nproc)" -f apps/browser-win32/Makefile WIN32_ARCH=$arch \
	    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" \
	    libbrowser-script script-core > "$log" 2>&1; then
		echo "FAILED $name: make libbrowser-script script-core (log $log)"
		tail -10 "$log"
		status=1
		continue
	fi
	if grep -n 'warning:' "$log"; then
		echo "FAILED $name: the build warned (above)"
		status=1
	fi
	tool_prefix=$LLVM_MINGW/bin/$arch-w64-mingw32-
	win_archive=$build/obj/libbrowser-script.a
	echo "$name: $win_archive, $("${tool_prefix}ar" t "$win_archive" | wc -l) members"

	# What the Windows archive leaves undefined, against the table.
	"${tool_prefix}nm" -u "$win_archive" 2> /dev/null |
	    awk 'NF == 2 { print $2 } NF == 1 && $1 !~ /:$/ { print $1 }' | sed 's/^__imp_//' |
	    sort -u > "$out/undefined-$name.txt"
	"${tool_prefix}nm" --defined-only "$win_archive" 2> /dev/null | awk 'NF == 3 { print $3 }' |
	    sort -u > "$out/defined-$name.txt"
	sed 's/#.*//; /^[[:space:]]*$/d; s/[[:space:]]//g' "$allowed" | sort -u > "$out/allowed.txt"
	comm -23 "$out/undefined-$name.txt" "$out/defined-$name.txt" > "$out/imports-$name.txt"
	if [ ! -s "$out/imports-$name.txt" ]; then
		echo "FAILED $name: nm found no undefined symbol in $win_archive"
		status=1
	elif comm -23 "$out/imports-$name.txt" "$out/allowed.txt" | grep .; then
		echo "FAILED $name: the symbols above are not in $allowed"
		status=1
	else
		echo "$name: $(wc -l < "$out/imports-$name.txt") undefined symbols, all in $allowed ($out/imports-$name.txt)"
	fi

	# What the .exe imports.
	exe=$build/tests/script-core.exe
	"$LLVM_MINGW/bin/llvm-readobj" --coff-imports "$exe" > "$out/imports-$name.readobj"
	sed -n '/^Import {/,/^}/s/^ *Name: //p' "$out/imports-$name.readobj" | sort -u > "$out/dlls-$name.txt"
	sed -n '/^DelayImport {/,/^}/s/^ *Name: //p' "$out/imports-$name.readobj" | sort -u > "$out/delayed-$name.txt"
	foreign=$(grep -v -i -x -E 'api-ms-win-crt-[a-z0-9-]+\.dll|(kernel32|user32|advapi32|shell32|ole32|oleaut32|crypt32|bcrypt|ws2_32)\.dll' \
	    "$out/dlls-$name.txt")
	if [ ! -s "$out/dlls-$name.txt" ]; then
		echo "FAILED $name: llvm-readobj found no import in $exe"
		status=1
	elif [ -n "$foreign" ] || [ -s "$out/delayed-$name.txt" ]; then
		echo "FAILED $name: $exe imports DLLs that are not Windows' own: $foreign $(cat "$out/delayed-$name.txt")"
		status=1
	else
		echo "$name: $exe imports only the system's DLLs ($(tr '\n' ' ' < "$out/dlls-$name.txt"))"
	fi
	[ "$arch" = x86_64 ] || continue

	# 5. Under Wine, the same bytes as the host's program.
	if ! command -v wine > /dev/null 2>&1; then
		echo "FAILED wine is not installed"
		status=1
		continue
	fi
	if [ ! -f "$WINEPREFIX/.browser-win32-ready" ]; then
		if WINEDEBUG=-all timeout 600 wineboot -i > "$out/wineboot.log" 2>&1; then
			: > "$WINEPREFIX/.browser-win32-ready"
		else
			echo "FAILED wineboot (log $out/wineboot.log)"
			status=1
			continue
		fi
	fi
	# One Wine server for all the runs, kept a minute after the last (a server that is exiting when the
	# next run starts refuses it).
	WINEDEBUG=-all timeout 60 wineserver -p 60
	mkdir -p "$out/wine"
	same=0
	count=0
	for test in $tests; do
		test_name=$(basename "$(dirname "$test")")-$(basename "$test" .js)
		count=$((count + 1))
		WINEDEBUG=-all WINEDLLOVERRIDES=winedbg.exe=d timeout 600 wine "$exe" --js "$test" \
		    > "$out/wine/$test_name.out" 2> "$out/wine/$test_name.err"
		echo $? > "$out/wine/$test_name.code"
		if cmp -s "$out/wine/$test_name.out" "$host/out/$test_name.out" &&
		    cmp -s "$out/wine/$test_name.err" "$host/out/$test_name.err" &&
		    cmp -s "$out/wine/$test_name.code" "$host/out/$test_name.code"; then
			same=$((same + 1))
		else
			echo "FAILED $name under Wine: $test differs from the host (exit $(cat "$out/wine/$test_name.code"), host $(cat "$host/out/$test_name.code")):"
			cmp "$out/wine/$test_name.out" "$host/out/$test_name.out"
			cmp "$out/wine/$test_name.err" "$host/out/$test_name.err"
			status=1
		fi
	done
	echo "$name under Wine: $same/$count tests write the host's bytes"
done
sync

if [ "$status" = 0 ]; then echo "script-core-link: PASS"; else echo "script-core-link: FAIL"; fi
exit "$status"
