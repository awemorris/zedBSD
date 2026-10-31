#!/bin/sh
# The unit tests of WS334's modules.  Each test plan/ws334/tests/unit/test-<name>.c is built with the module
# sources plan/ws334/tests/modules.sh lists for it and run on each system it applies to:
#   posix  the host's cc with ASan and UBSan (-std=gnu11 -Wall -Wextra -Werror), run on the host;
#   win32  llvm-mingw's x86_64-w64-mingw32-clang with UBSan in trap mode, run under Wine (a prefix of its
#          own under build/tmp/ws334-wine, made on first use, which takes a few minutes).
# A test runs in a fresh empty directory of its own (its current directory) and may make files there.
# It gets one argument, "posix" or "win32", and its last line is "test-<name>: PASS (...)" or "... FAIL (...)".
#   sh plan/ws334/tests/run-unit.sh [--posix|--win32] <name>...
#   sh plan/ws334/tests/run-unit.sh [--posix|--win32] all
# Without --posix or --win32 a test runs on both systems where it applies.
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64).
# Last line: run-unit: PASS or run-unit: FAIL.  Output under build/tmp/ws334-unit (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
. plan/tools/fresh-out.sh
. plan/ws334/tests/modules.sh
out=build/tmp/ws334-unit
fresh_out "$out"
out=$top/$out
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
wine_prefix=$top/build/tmp/ws334-wine

systems="posix win32"
case "${1:-}" in
--posix) systems=posix; shift ;;
--win32) systems=win32; shift ;;
esac

names="$*"
if [ "$names" = "all" ]; then
	names=""
	for file in plan/ws334/tests/unit/test-*.c; do
		[ -f "$file" ] || continue
		name=$(basename "$file" .c)
		names="$names ${name#test-}"
	done
fi
if [ -z "$names" ]; then
	echo "usage: sh plan/ws334/tests/run-unit.sh [--posix|--win32] <name>... | all"
	exit 2
fi

host_flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer"
host_libs="-lpthread -ldl -lm"
win_cc=$LLVM_MINGW/bin/x86_64-w64-mingw32-clang
win_flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE
	-fsanitize=undefined -fsanitize-trap=undefined"
win_libs="-lws2_32 -lbcrypt -lcrypt32 -lshell32 -lole32"

status=0
ran=0
for name in $names; do
	source=plan/ws334/tests/unit/test-$name.c
	if [ ! -f "$source" ]; then
		echo "test-$name: FAIL (no $source)"
		status=1
		continue
	fi
	for system in $systems; do
		modules=$(ws334_test_sources "$name" "$system")
		[ "$modules" = none ] && continue
		if [ "$modules" = unknown ]; then
			echo "test-$name: FAIL (modules.sh does not list it)"
			status=1
			continue
		fi
		run=$out/$name.$system
		mkdir -p "$run"
		if [ "$system" = posix ]; then
			program=$run.bin
			build="cc $host_flags $ws334_includes $source $modules $host_libs -o $program"
		else
			program=$run.exe
			build="$win_cc $win_flags $ws334_includes $source $modules $win_libs -o $program"
		fi
		if ! $build > "$run.build.log" 2>&1 || [ -s "$run.build.log" ]; then
			echo "test-$name ($system): FAIL (build; log $run.build.log)"
			head -20 "$run.build.log"
			status=1
			continue
		fi
		if [ "$system" = posix ]; then
			(cd "$run" && ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			    timeout 600 "$program" posix) > "$run.log" 2>&1
		else
			if [ ! -d "$wine_prefix" ]; then
				WINEPREFIX=$wine_prefix WINEDEBUG=-all timeout 600 wineboot -i > "$out/wineboot.log" 2>&1
			fi
			(cd "$run" && WINEPREFIX=$wine_prefix WINEDEBUG=-all timeout 600 wine "$program" win32) \
			    > "$run.log" 2>&1
		fi
		code=$?
		last=$(tr -d '\r' < "$run.log" | tail -1)
		case "$code:$last" in
		"0:test-$name: PASS"*)
			echo "$last ($system)"
			ran=$((ran + 1)) ;;
		*)
			echo "test-$name ($system): FAIL (exit $code; log $run.log)"
			tr -d '\r' < "$run.log" | grep -v '^ok ' | tail -20
			status=1 ;;
		esac
	done
done

if [ "$ran" = 0 ] && [ "$status" = 0 ]; then
	echo "no test ran"
	status=1
fi
if [ "$status" = 0 ]; then echo "run-unit: PASS"; else echo "run-unit: FAIL"; fi
exit "$status"
