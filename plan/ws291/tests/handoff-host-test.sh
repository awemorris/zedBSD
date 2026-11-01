#!/bin/sh
# Host test H3a of libvulkan's recording thread (userland/desktop/libvulkan/sender.c): the hand-off FIFO with a faked
# transport (order, drain, back-pressure, wake marks, failure latch, batch waits, fork, abort), built plain, with ASan
# and UBSan, and with TSan, each run under a 60 s limit.  The binaries go in a new directory below build/tmp.
#   usage: sh plan/ws291/tests/handoff-host-test.sh   (from the top of the tree)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$repo/build/tmp"
out=$(mktemp -d "$repo/build/tmp/handoff-host.XXXXXX")
mkdir -p "$out/hdr"
ln -s "$repo/include/libc/vulkan" "$out/hdr/vulkan"
lv=$repo/userland/desktop/libvulkan
cflags="-std=gnu11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DKERN_USER_ABI_LP64 -Wall -Wextra -Werror -g -O1 -I$out/hdr -I$repo/include -I$repo -I$lv"
src="$repo/plan/ws291/tests/handoff-host-test.c $lv/sender.c $lv/wire.c"
failed=0
for variant in plain asan tsan; do
	case $variant in
	plain) extra= ;;
	asan) extra="-fsanitize=address,undefined -fno-sanitize-recover=undefined" ;;
	tsan) extra="-fsanitize=thread" ;;
	esac
	if ! clang $cflags $extra -o "$out/handoff-$variant" $src -lpthread > "$out/build-$variant.log" 2>&1; then
		echo "handoff-host $variant: FAIL build ($out/build-$variant.log)"
		failed=1
		continue
	fi
	TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" timeout 60 "$out/handoff-$variant" > "$out/run-$variant.log" 2>&1
	rc=$?
	warnings=$(grep -c 'WARNING: ThreadSanitizer\|ERROR: AddressSanitizer\|runtime error' "$out/run-$variant.log")
	echo "handoff-host $variant: rc=$rc sanitizer-reports=$warnings $(tail -1 "$out/run-$variant.log")"
	grep '^order:' "$out/run-$variant.log"
	if [ $rc -ne 0 ] || [ "$warnings" -ne 0 ]; then
		failed=1
	fi
done
if [ $failed = 0 ]; then
	echo "handoff-host: PASS"
else
	echo "handoff-host: FAIL ($out)"
fi
