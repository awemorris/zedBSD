#!/bin/sh
# The host tests of libkeiland's threads and network (thread-test.c, socket-test.c).  Run from the worktree's root.
#
#   sh plan/ws327/tests/host-test.sh [LINUX_BUILD [TEST...]]      # TEST: thread, socket (default both)
#
# builds each test with gcc and clang: linked with a Linux build of libkeiland (LINUX_BUILD, default
# build/ws327-linux: make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=DIR DIR/lib/libkeiland.so), and
# with the layers' sources (platform/thread-posix.c, platform/socket-posix.c) compiled in under ThreadSanitizer and
# under AddressSanitizer with UndefinedBehaviorSanitizer (which also report a leaked record).  Each run prints
# PASS/FAIL/NOT-DONE lines; this prints one line per build and a SUMMARY.  Output in build/tmp/ws327-host-test (a new
# directory each run, plan/tools/fresh-out.sh).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u

. plan/tools/fresh-out.sh

build=${1:-build/ws327-linux}
[ $# -gt 0 ] && shift
tests=${*:-thread socket}
fresh_out build/tmp/ws327-host-test
out=build/tmp/$(basename "$fresh_dir")
flags="-D_GNU_SOURCE -std=gnu17 -Wall -Wextra -Werror -O1 -g -Iuserland/desktop/include"
pass=0
fail=0

run() {
	name=$1
	shift
	if ! "$@" > "$out/$name.build" 2>&1; then
		echo "FAIL $name (build: $out/$name.build)"
		fail=$((fail + 1))
		return
	fi
	if timeout 300 env TSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=1 \
		LD_LIBRARY_PATH="$PWD/$build/lib" "$out/$name" > "$out/$name.log" 2>&1; then
		echo "PASS $name ($(tail -n 1 "$out/$name.log"))"
		pass=$((pass + 1))
	else
		echo "FAIL $name ($(grep -m 1 -E '^FAIL|WARNING|ERROR' "$out/$name.log") log: $out/$name.log)"
		fail=$((fail + 1))
	fi
}

layers="userland/desktop/libkeiland/platform/thread-posix.c userland/desktop/libkeiland/platform/socket-posix.c"
for test in $tests; do
	source=plan/ws327/tests/$test-test.c
	for cc in gcc clang; do
		if [ -f "$build/lib/libkeiland.so" ]; then
			run "$test-$cc-lib" $cc $flags "$source" -o "$out/$test-$cc-lib" -L"$build/lib" -lkeiland -pthread \
				-Wl,-rpath-link,"$build/lib" -Wl,-rpath,"$PWD/$build/lib"
		else
			echo "NOT-DONE $test-$cc-lib (no $build/lib/libkeiland.so)"
		fi
		run "$test-$cc-tsan" $cc $flags -fsanitize=thread "$source" $layers -o "$out/$test-$cc-tsan" -pthread
		run "$test-$cc-asan" $cc $flags -fsanitize=address,undefined "$source" $layers -o "$out/$test-$cc-asan" -pthread
	done
done

printf 'SUMMARY pass=%d fail=%d out=%s\n' "$pass" "$fail" "$(readlink -f "$out")"
[ "$fail" -eq 0 ]
