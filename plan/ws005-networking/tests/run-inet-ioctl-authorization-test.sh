#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-inet-auth.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -ffunction-sections \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src/kern/net" \
	-I"$repo/libc/include" \
	"$repo/src/kern/net/net-device.c" \
	"$repo/src/kern/net/inet-socket.c" \
	"$repo/plan/ws005-networking/tests/inet-ioctl-authorization-test.c" \
	-Wl,--gc-sections -o "$temporary/inet-ioctl-authorization-test"
"$temporary/inet-ioctl-authorization-test"

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -ffunction-sections \
	-g -fno-omit-frame-pointer -fsanitize=address,undefined \
	-I"$repo/include" -I"$repo/include/uapi" -I"$repo/src/kern/net" \
	-I"$repo/libc/include" \
	"$repo/src/kern/net/net-device.c" \
	"$repo/src/kern/net/inet-socket.c" \
	"$repo/plan/ws005-networking/tests/inet-ioctl-authorization-test.c" \
	-Wl,--gc-sections -o "$temporary/inet-ioctl-authorization-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/inet-ioctl-authorization-sanitize"
