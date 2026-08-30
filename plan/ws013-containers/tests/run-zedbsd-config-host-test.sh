#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-config-test.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

compiler=${HOSTCC:-cc}
production=$repo/bootloader/uefi/zedbsd-config.c
fixture=$repo/plan/ws013-containers/tests/zedbsd-config-host-test.c
common="-std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -I$repo"

# Compile the production helper as freestanding code before linking the host
# fixture, so an accidental libc dependency is rejected independently.
# shellcheck disable=SC2086
$compiler $common -ffreestanding -fno-builtin -c "$production" \
	-o "$temporary/zedbsd-config.o"
# shellcheck disable=SC2086
$compiler $common "$fixture" "$temporary/zedbsd-config.o" \
	-o "$temporary/zedbsd-config-test"
"$temporary/zedbsd-config-test"

# Check the production parser's transactional and bounded paths with the host
# compiler's static analyzer when that compiler provides one.
# shellcheck disable=SC2086
if $compiler -std=c11 -fanalyzer -x c -c /dev/null \
	-o "$temporary/analyzer-probe.o" 2>/dev/null; then
	# Analyzer findings in the production source are test failures, not skips.
	# shellcheck disable=SC2086
	$compiler $common -ffreestanding -fno-builtin -fanalyzer \
		-c "$production" -o "$temporary/zedbsd-config-analyzer.o"
	echo 'zedbsd.cfg parser static analyzer: PASS'
else
	echo 'zedbsd.cfg parser static analyzer: SKIP (analyzer unavailable)'
fi

# Repeat every parser and boundary case with runtime memory/UB checks.
# shellcheck disable=SC2086
$compiler $common -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined "$production" "$fixture" \
	-o "$temporary/zedbsd-config-test-sanitize"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	"$temporary/zedbsd-config-test-sanitize"

echo 'zedbsd.cfg parser ordinary + sanitizer fixtures: PASS'
