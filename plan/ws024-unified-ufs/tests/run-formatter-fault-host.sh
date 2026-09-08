#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
python3 "$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)/plan/ws025-io-memory-cache/tests/prepare-driver-fragments.py"
# Fix host-created journal permissions for a deterministic metadata fixture.
umask 022
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ufs-format.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
noct=${NOCT:-"$repo/build/NoctLang/build-static/noct"}
if ! test -x "$noct"; then
	printf '%s\n' "Host Noct executable missing: $noct" >&2
	exit 1
fi

# Compile the maintained backend instead of trusting a stale host executable.
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror \
	"$repo/tools/build/zedimage-host.c" -o "$temporary/zedimage-host"
"$noct" --path="$repo/tools/build" "$repo/tools/build/make-data-image.noct" \
	--backend "$temporary/zedimage-host" --output "$temporary/reference.img" \
	--size-mib 32

for mode in ordinary sanitize; do
	extra=""
	if test "$mode" = sanitize; then
		extra="-fsanitize=address,undefined -fno-omit-frame-pointer"
	fi
	# Wrap only the production formatter's descriptor calls for fault injection.
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c89 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-Wall -Wextra -Werror -Wdeclaration-after-statement $extra \
		-I"$repo" -I"$repo/plan/ws025-io-memory-cache/temp/p031-driver-fragments/src/drivers/fs/ufs" \
		-Dpread=ufs_test_pread -Dpwrite=ufs_test_pwrite \
		-c "$repo/userland/base/mkfs/ufs-format.c" \
		-o "$temporary/formatter-$mode.o"
	# Link the same standalone decoder consumed by the production UFS probe.
	# shellcheck disable=SC2086
	${HOSTCC:-cc} -std=c89 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-Wall -Wextra -Werror -Wdeclaration-after-statement $extra -I"$repo" \
		"$repo/plan/ws024-unified-ufs/tests/formatter-fault-host.c" \
		"$repo/userland/base/mkfs/ufs-super.c" \
		"$repo/userland/base/mkfs/ufs-endian.c" \
		"$temporary/formatter-$mode.o" -o "$temporary/test-$mode"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
		"$temporary/test-$mode" "$temporary/output-$mode.img" \
		"$temporary/scratch-$mode.img" "$temporary/reference.img"
	cmp "$temporary/reference.img" "$temporary/output-$mode.img"
	printf '%s\n' "UFS formatter $mode: maintained Noct data-image byte equality PASS"
done
