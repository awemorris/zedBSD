#!/bin/sh
# Cross-compiles libkeiland's Windows layers (platform/thread-win32.c, platform/socket-win32.c) with mingw-w64 and
# links win32-link.c, which calls every function of keiland_thread.h and keiland_socket.h, into a Windows program:
# every function is defined and the layers build without a warning.  Nothing is run (no Windows, no wine).
# Run from the worktree's root.
#   sh plan/ws327/tests/build-win32.sh [OUT]     (OUT default build/tmp/ws327-win32, a new directory each run)
# Prints one line per step and "ws327-win32: PASS" or "FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u

. plan/tools/fresh-out.sh

cc=${MINGW_CC:-x86_64-w64-mingw32-gcc}
fresh_out "${1:-build/tmp/ws327-win32}"
out=$fresh_dir
# Windows 10 (0x0A00): SetThreadDescription and the rest of what the layers call are there.
flags="-std=gnu17 -O2 -g -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Iuserland/desktop/include"
failed=0
command -v "$cc" > /dev/null 2>&1 || { echo "ws327-win32: NOT-DONE (no $cc)"; exit 2; }
for layer in thread socket; do
	if timeout 300 "$cc" $flags -c "userland/desktop/libkeiland/platform/$layer-win32.c" -o "$out/$layer-win32.o" \
	    > "$out/$layer-win32.log" 2>&1; then
		echo "compile $layer-win32.c: PASS (warnings $(grep -c 'warning:' "$out/$layer-win32.log"))"
	else
		echo "compile $layer-win32.c: FAIL ($out/$layer-win32.log)"
		failed=1
	fi
done
if [ $failed = 0 ]; then
	if timeout 300 "$cc" $flags plan/ws327/tests/win32-link.c "$out/thread-win32.o" "$out/socket-win32.o" \
	    -o "$out/win32-link.exe" -lws2_32 -liphlpapi > "$out/link.log" 2>&1; then
		echo "link win32-link.exe: PASS"
	else
		echo "link win32-link.exe: FAIL ($out/link.log)"
		failed=1
	fi
fi
[ $failed = 0 ] && echo "ws327-win32: PASS" || echo "ws327-win32: FAIL"
[ $failed = 0 ]
