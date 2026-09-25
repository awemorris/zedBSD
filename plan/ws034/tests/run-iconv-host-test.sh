#!/bin/sh
# Host test of the C library's iconv (ws034-p040), with sanitizers.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../.."
output=${TMPDIR:-/tmp}/zedbsd-iconv-test
${CC:-cc} -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	-Wall -Wextra -Werror -D_DEFAULT_SOURCE \
	plan/ws034/tests/iconv-test.c src/libc/iconv.c -ldl -o "$output"
"$output"
