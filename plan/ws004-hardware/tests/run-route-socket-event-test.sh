#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-route-event.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

warnings="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -ffunction-sections"
includes="-I$repo/include -I$repo/include/uapi -I$repo/src"
sources="$repo/src/kern/net/net-device.c $repo/src/kern/net/route-socket.c"
fixture="$repo/plan/ws004-hardware/tests/route-socket-event-test.c"
layout="$repo/plan/ws004-hardware/tests/route-uapi-layout-test.c"


cc $warnings $includes "$layout" -o "$temporary/route-uapi-layout"
"$temporary/route-uapi-layout"
cc $warnings $includes $sources "$fixture" \
	-Wl,--gc-sections -o "$temporary/route-socket-event-test"
"$temporary/route-socket-event-test"

cc $warnings -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	$includes $sources "$fixture" -Wl,--gc-sections \
	-o "$temporary/route-socket-event-sanitize"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
	"$temporary/route-socket-event-sanitize"

cc $warnings -O0 -fanalyzer $includes -c \
	"$repo/src/kern/net/route-socket.c" \
	-o "$temporary/route-socket-event-analyzer.o"
cc $warnings -O0 -fanalyzer $includes -c \
	"$repo/src/kern/net/net-device.c" \
	-o "$temporary/net-device-event-analyzer.o"

abi_includes="-I$repo/libc/include -I$repo/include/uapi"
cc -m64 -nostdinc $abi_includes -DZEDBSD_USER_ABI_LP64 \
	-std=c11 -Wall -Wextra -Werror -fsyntax-only "$layout"
cc -m32 -nostdinc $abi_includes -std=c11 -Wall -Wextra -Werror \
	-fsyntax-only "$layout"

echo "route socket: lifecycle, overflow, sanitizer, analyzer, ABI PASS"
