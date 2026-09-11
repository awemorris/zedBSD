#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
output=${1:?usage: run-static-tls-host.sh OUTPUT}
mkdir -p "$output"
for mode in ordinary sanitize; do
	flags=''
	if test "$mode" = sanitize; then flags='-fsanitize=address,undefined -fno-omit-frame-pointer'; fi
	cc -std=c11 -D_DEFAULT_SOURCE -g -O1 -Wall -Wextra -Werror $flags \
		-Dmmap=tls_test_mmap -Dmunmap=tls_test_munmap \
		-I"$repo" -I"$repo/include/uapi" -idirafter "$repo/libc/include" \
		"$repo/userland/base/libc/static-tls.c" \
		"$repo/plan/ws022/tests/static-tls-host.c" -o "$output/$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$output/$mode"
done
echo 'static TLS owner: PASS (clone, zero-fill, alignment, repeated free, failed alloc/attach)'
