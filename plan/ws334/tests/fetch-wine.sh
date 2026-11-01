#!/bin/sh
# The network of libbrowser on Windows under Wine (x86_64): plan/ws334/tests/fetch-main.c, linked by the
# Windows build's test-program with every static library, fetches a page from a server on 127.0.0.1 (the
# host's python3 -m http.server over a directory of the test's own) with the blocking fetch and with
# the loader, and each must give 200 and the page's size.  Wine runs under flock /tmp/zedbsd-wine.lock.
#   sh plan/ws334/tests/fetch-wine.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# WINEPREFIX=<dir> the Wine prefix (default build/tmp/ws334-wine); PORT=<n> the server's port (18334).
# Last line: fetch-wine: PASS or fetch-wine: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws334-fetch-wine
fresh_out "$out"
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
WINEPREFIX=${WINEPREFIX:-$(pwd)/build/tmp/ws334-wine}
export WINEPREFIX
port=${PORT:-18334}
build=${BROWSER_WIN32_BUILD:-build/tmp/ws334-fetch-build/browser-win64}
sources=build/tmp/ws334-win32-sources
mkdir -p "$sources"
status=0

# The program.
if ! timeout 1800 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 \
    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" -j8 \
    test-program TEST_PROGRAM=fetch-tool \
    TEST_PROGRAM_SOURCES="plan/ws334/tests/fetch-main.c" \
    > "$out/make.log" 2>&1 || grep -q 'warning:' "$out/make.log"; then
	echo "fetch-wine: the program did not build cleanly (log $out/make.log)"
	tail -10 "$out/make.log"
	echo "fetch-wine: FAIL"
	exit 1
fi
program=$build/tests/fetch-tool.exe

# The page: 100000 bytes of text, so that it comes in several reads.
mkdir -p "$out/site"
/usr/bin/python3 -I -c 'import sys; sys.stdout.write("".join("line %05d of the page\n" % i for i in range(100000 // 22)))' \
    > "$out/site/page.txt"
size=$(wc -c < "$out/site/page.txt" | tr -d ' ')

# The server, for as long as the program runs.
/usr/bin/python3 -I -m http.server "$port" --bind 127.0.0.1 --directory "$out/site" > "$out/server.log" 2>&1 &
server=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
	if /usr/bin/python3 -I -c "import socket,sys; s=socket.socket(); s.settimeout(1); sys.exit(s.connect_ex(('127.0.0.1', $port)))"; then
		break
	fi
	sleep 1
done

# The program under Wine, alone.
WINEDEBUG=-all timeout 300 flock /tmp/zedbsd-wine.lock wine "$program" "http://127.0.0.1:$port/page.txt" \
    > "$out/wine.out" 2> "$out/wine.log"
code=$?
kill "$server" 2> /dev/null
wait "$server" 2> /dev/null
tr -d '\r' < "$out/wine.out" > "$out/wine.txt"
cat "$out/wine.txt"
for way in blocking loader; do
	if ! grep -qx "fetch-tool: $way 200 $size" "$out/wine.txt"; then
		echo "fetch-wine: the $way fetch did not give 200 and $size bytes"
		status=1
	fi
done
if [ "$code" != 0 ]; then
	echo "fetch-wine: exit $code (log $out/wine.log)"
	status=1
fi
sync
if [ "$status" = 0 ]; then echo "fetch-wine: PASS"; else echo "fetch-wine: FAIL"; fi
exit $status
