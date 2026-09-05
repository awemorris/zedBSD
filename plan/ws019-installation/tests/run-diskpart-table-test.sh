#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-diskpart-test.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
	extra=""
	if test "$mode" = sanitize; then extra="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror $extra -I"$repo" \
		"$repo/plan/ws019-installation/tests/diskpart-table-test.c" \
		"$repo/userland/base/diskpart/table.c" -o "$temporary/$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$temporary/$mode"
	# Use host POSIX headers first and only zedBSD-specific UAPI afterwards.
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror $extra -I"$repo" \
		-idirafter "$repo/include/uapi" \
		"$repo/plan/ws019-installation/tests/diskpart-cli-test.c" \
		"$repo/userland/base/diskpart/table.c" -o "$temporary/cli-$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$temporary/cli-$mode"
done
