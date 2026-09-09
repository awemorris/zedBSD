#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-format-file.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

for mode in ordinary sanitize; do
	extra=""
	if test "$mode" = sanitize; then
		extra="-fsanitize=address,undefined -fno-omit-frame-pointer"
	fi
	# Compile the unmodified frontend against independently controlled syscalls.
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c89 -D_GNU_SOURCE -O1 -g \
		-Wall -Wextra -Werror -Wdeclaration-after-statement $extra \
		-I"$repo" -idirafter "$repo/include/uapi" \
		-Dlstat=ff_test_lstat -Dfstat=ff_test_fstat -Dopen=ff_test_open \
		-Dioctl=ff_test_ioctl -Dfsync=ff_test_fsync -Dclose=ff_test_close \
		-c "$repo/userland/base/common/format-file.c" \
		-o "$temporary/frontend-$mode.o"
	# Link lifecycle assertions without any production runtime test switch.
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c89 -D_GNU_SOURCE -O1 -g \
		-Wall -Wextra -Werror -Wdeclaration-after-statement $extra \
		-I"$repo" -idirafter "$repo/include/uapi" \
		"$repo/plan/ws019-installation/tests/format-file-test.c" \
		"$temporary/frontend-$mode.o" -o "$temporary/test-$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$temporary/test-$mode"
	# Independently assert the read-only protocol, including the command grammar.
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c89 -D_GNU_SOURCE -O1 -g \
		-Wall -Wextra -Werror -Wdeclaration-after-statement $extra \
		-I"$repo" -idirafter "$repo/include/uapi" \
		"$repo/plan/ws019-installation/tests/pristine-frontend-host.c" \
		"$temporary/frontend-$mode.o" -o "$temporary/pristine-$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$temporary/pristine-$mode"
done
