#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
umask 022
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-swap-format.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
noct=${NOCT:-"$repo/build/NoctLang/build-static/noct"}
if ! test -x "$noct"; then
	printf '%s\n' "Host Noct executable missing: $noct" >&2
	exit 1
fi

# Generate fresh independent format fixtures with the maintained Noct path.
"$noct" --path="$repo/tools/build" "$repo/tools/build/make-swapfile.noct" \
	--output "$temporary/reference.img" --size-mib 64
"$noct" --path="$repo/tools/build" "$repo/tools/build/make-swapfile.noct" \
	--output "$temporary/legacy.img" --format v1 --size-mib 32
"$noct" --path="$repo/tools/build" "$repo/tools/build/make-swapfile.noct" \
	--output "$temporary/labeled.img" --size-mib 1 \
	--uuid 0123456789ABCDEF --label TESTSWAP

for mode in ordinary sanitize; do
	extra=""
	if test "$mode" = sanitize; then
		extra="-fsanitize=address,undefined -fno-omit-frame-pointer"
	fi
	# Rename only production descriptor transfers for controlled failure results.
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c89 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-Wall -Wextra -Werror -Wdeclaration-after-statement $extra \
		-I"$repo" -idirafter "$repo/include" \
		-Dpread=swap_test_pread -Dpwrite=swap_test_pwrite \
		-c "$repo/userland/base/mkswap/swap-format.c" \
		-o "$temporary/formatter-$mode.o"
	# Link the shared production parser used by both kernel and target command.
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c89 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-Wall -Wextra -Werror -Wdeclaration-after-statement $extra \
		-I"$repo" -idirafter "$repo/include" \
		"$repo/plan/ws019-installation/tests/swap-format-test.c" \
		"$repo/src/kern/swap-format.c" "$temporary/formatter-$mode.o" \
		-o "$temporary/test-$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
		"$temporary/test-$mode" "$temporary/output-$mode.img" \
		"$temporary/scratch-$mode.img" "$temporary/legacy.img" \
		"$temporary/labeled.img" "$temporary/reference.img"
	cmp "$temporary/reference.img" "$temporary/output-$mode.img"
	printf '%s\n' "Swap formatter $mode: maintained Noct swapfile byte equality PASS"
done
