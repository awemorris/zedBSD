#!/bin/sh
# Host test H3b of libvulkan's submission hand-off: the whole library (but its Wayland surface) built for the host
# against a fake GPU node (plan/ws291/tests/handoff-lib-host-test.c: its ioctl, mmap and munmap), three threads (draw,
# fences, objects), with the hand-off on and off, plain and with TSan, each run under a 60 s limit.  The binaries go
# in a new directory below build/tmp.
#   usage: sh plan/ws291/tests/handoff-lib-host-test.sh   (from the top of the tree)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$repo/build/tmp"
out=$(mktemp -d "$repo/build/tmp/handoff-lib.XXXXXX")
mkdir -p "$out/hdr"
ln -s "$repo/include/libc/vulkan" "$out/hdr/vulkan"
lv=$repo/userland/desktop/libvulkan
base="-std=gnu11 -D_GNU_SOURCE -DKERN_USER_ABI_LP64 -g -O1 -I$out/hdr -I$repo/include -I$repo -I$lv"
failed=0
for variant in plain tsan; do
	case $variant in
	plain) extra= ;;
	tsan) extra="-fsanitize=thread" ;;
	esac
	mkdir -p "$out/$variant"
	objs=
	for f in "$lv"/*.c; do
		b=$(basename "$f" .c)
		[ "$b" = wsi-wayland ] && continue
		if ! clang $base $extra -w -Dioctl=h3b_ioctl -Dmmap=h3b_mmap -Dmunmap=h3b_munmap -c "$f" -o "$out/$variant/$b.o" \
		    2> "$out/$variant/$b.err"; then
			echo "handoff-lib $variant: FAIL build $b ($out/$variant/$b.err)"
			failed=1
		fi
		objs="$objs $out/$variant/$b.o"
	done
	if ! clang $base $extra -Wall -Wextra -Werror -o "$out/test-$variant" "$repo/plan/ws291/tests/handoff-lib-host-test.c" $objs \
	    -lpthread -lm > "$out/link-$variant.log" 2>&1; then
		echo "handoff-lib $variant: FAIL link ($out/link-$variant.log)"
		failed=1
		continue
	fi
	for handoff in 1 0; do
		VULKAN_ZEDBSD_SUBMIT_HANDOFF=$handoff TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" timeout 60 \
		    "$out/test-$variant" > "$out/run-$variant-$handoff.log" 2>&1
		rc=$?
		reports=$(grep -c 'WARNING: ThreadSanitizer' "$out/run-$variant-$handoff.log")
		echo "handoff-lib $variant handoff=$handoff: rc=$rc tsan-reports=$reports $(grep '^submits' "$out/run-$variant-$handoff.log") | $(tail -1 "$out/run-$variant-$handoff.log")"
		if [ $rc -ne 0 ] || [ "$reports" -ne 0 ]; then
			failed=1
		fi
	done
done
if [ $failed = 0 ]; then
	echo "handoff-lib: PASS"
else
	echo "handoff-lib: FAIL ($out)"
fi
