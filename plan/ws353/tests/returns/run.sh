#!/bin/sh
# Builds and runs the host test of the client buffers' returns (returns-test.c with userland/desktop/wayland/objects.c,
# threads.c and the timer test's stand-ins, plan/ws353/tests/timer/retire-stubs.c) as gnu89, plainly and under
# ASan/UBSan, in a new directory under build/tmp (nothing is removed here).  No Makefile is used.
#   sh plan/ws353/tests/returns/run.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
here="$repo/plan/ws353/tests/returns"
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws353-returns"
work=$fresh_dir
mkdir "$work/include"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"
flags='-std=gnu89 -Wdeclaration-after-statement -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g'
includes="-I$work/include -I$repo -I$repo/include -I$repo/userland/desktop/wayland -I$repo/userland/desktop/include"
for mode in plain sanitize; do
	extra=
	case $mode in
	sanitize) extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' ;;
	esac
	${CC:-cc} $flags -Wno-missing-prototypes $extra $includes "$here/returns-test.c" "$repo/plan/ws353/tests/timer/retire-stubs.c" \
		"$repo/userland/desktop/wayland/objects.c" "$repo/userland/desktop/wayland/threads.c" \
		-pthread -o "$work/returns-$mode"
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 timeout 60 "$work/returns-$mode"
done
echo "WS353 returns host test PASS (plain, ASan/UBSan)"
