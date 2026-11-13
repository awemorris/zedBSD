#!/bin/sh
# Builds and runs the host test of the compositor's timers (timer-test.c with userland/desktop/wayland/threads.c)
# and of the pace of the gone clients' releases (retire-test.c with objects.c and threads.c), as gnu89, plainly, under
# ASan/UBSan and under TSan, in a new directory under build/tmp (nothing is removed here; Q1's plan/tools/q1-clean.sh
# removes old runs).  No Makefile is used.
#   sh plan/ws353/tests/timer/run.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
here="$repo/plan/ws353/tests/timer"
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws353-timer"
work=$fresh_dir
mkdir "$work/include"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"
flags='-std=gnu89 -Wdeclaration-after-statement -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g'
includes="-I$work/include -I$repo -I$repo/include -I$repo/userland/desktop/wayland -I$repo/userland/desktop/include"
for mode in plain sanitize thread; do
	extra=
	case $mode in
	sanitize) extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' ;;
	thread) extra='-fsanitize=thread -fno-omit-frame-pointer' ;;
	esac
	${CC:-cc} $flags $extra $includes "$here/timer-test.c" "$repo/userland/desktop/wayland/threads.c" \
		-pthread -o "$work/timer-$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 \
		timeout 60 "$work/timer-$mode"
	# The stubs stand in for the rest of the compositor; they are declared apart from kwl.h on purpose.
	${CC:-cc} $flags -Wno-missing-prototypes $extra $includes "$here/retire-test.c" "$here/retire-stubs.c" \
		"$repo/userland/desktop/wayland/objects.c" "$repo/userland/desktop/wayland/threads.c" \
		-pthread -o "$work/retire-$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 \
		timeout 60 "$work/retire-$mode"
done
echo "WS353 timer and retire pace host test PASS (plain, ASan/UBSan, TSan)"
