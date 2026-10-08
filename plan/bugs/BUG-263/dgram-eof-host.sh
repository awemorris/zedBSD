#!/bin/sh
# BUG-263: the real AF_UNIX code on the host (dgram-eof-host.c, with plan/ws014/tests/handle-fd.c's collaborators):
# a datagram socket whose pair's other end closed, or that is shut for reading, is readable to poll and its receive
# returns 0 instead of waiting.  Plain and ASan/UBSan.  The last line is "dgram-eof-host: PASS" or "... FAIL".
#   sh plan/bugs/BUG-263/dgram-eof-host.sh [WORK]     (default build/bug263-host)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=${1:-"$root/build/bug263-host"}
mkdir -p "$work"
status=0
for mode in plain sanitize; do
	extra=
	[ "$mode" = sanitize ] && extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
	# shellcheck disable=SC2086
	if ! cc -std=gnu11 -O1 -g -Wall -Wextra -Werror \
		-DKERN_USER_ABI_LP64 -I"$root/include" -I"$root/include/libc" -DKERN_UAPI_NATIVE \
		-I"$root/include/uapi" -ffunction-sections -fdata-sections $extra \
		"$root/plan/bugs/BUG-263/dgram-eof-host.c" \
		"$root/src/kern/handle.c" "$root/src/kern/fd-object.c" \
		"$root/src/kern/filedesc.c" "$root/src/kern/poll.c" \
		"$root/src/kern/net/socket.c" "$root/src/kern/net/unix-socket.c" \
		"$root/src/kern/net/packet-buf.c" -Wl,--gc-sections \
		-o "$work/$mode" >"$work/$mode-build.log" 2>&1; then
		echo "$mode: build FAILED ($work/$mode-build.log)"
		status=1
		continue
	fi
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 20 "$work/$mode" >"$work/$mode.log" 2>&1; then
		echo "$mode: $(tail -1 "$work/$mode.log")"
	else
		echo "$mode: FAILED"
		cat "$work/$mode.log"
		status=1
	fi
done
[ $status = 0 ] && echo "dgram-eof-host: PASS" || echo "dgram-eof-host: FAIL"
exit $status
