#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-nvme-io.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

compiler=${HOSTCC:-cc}
fixture=$repo/plan/ws004-hardware/tests/nvme-io-test.c
common="-std=c11 -O2 -Wall -Wextra -Werror -I$repo/include"

# shellcheck disable=SC2086
$compiler $common "$fixture" -o "$temporary/nvme-io-test"
"$temporary/nvme-io-test"

if $compiler -std=c11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I"$repo/include" "$fixture" \
	-o "$temporary/nvme-io-test-sanitized" 2>/dev/null; then
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$temporary/nvme-io-test-sanitized"
	echo 'HW-T20 NVMe I/O sanitizer: PASS'
else
	echo 'HW-T20 NVMe I/O sanitizer: SKIP (compiler unavailable)'
fi

if $compiler -std=c11 -O0 -Wall -Wextra -Werror -fanalyzer \
	-I"$repo/include" -c "$fixture" \
	-o "$temporary/nvme-io-test-analyzer.o" 2>/dev/null; then
	echo 'HW-T20 NVMe I/O analyzer: PASS'
else
	echo 'HW-T20 NVMe I/O analyzer: SKIP (analyzer unavailable)'
fi
