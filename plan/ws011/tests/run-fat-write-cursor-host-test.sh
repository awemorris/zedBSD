#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
python3 "$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)/plan/ws025/tests/prepare-driver-fragments.py"
repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repository_root"
mkdir -p plan/ws011/temp
test_root=$(mktemp -d plan/ws011/temp/fat-write-cursor.XXXXXX)
sanitize_flags=
if [ "${NCOM_SANITIZE:-0}" = 1 ]; then
	sanitize_flags='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
# Deliberate compiler-flag word splitting.
# shellcheck disable=SC2086
timeout --kill-after=5s 60s "${CC:-cc}" -std=c11 -O1 -g \
	-DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror -ffunction-sections \
	-fdata-sections -Iinclude -Iinclude/uapi -Isrc -Ilibc/include -I. \
	$sanitize_flags plan/ws011/tests/fat-write-cursor-host-test.c \
	plan/ws025/temp/p031-driver-fragments/src/drivers/fs/fat.c src/kern/io-stats.c -Wl,--gc-sections -o "$test_root/test"
printf 'FAT cursor diagnostic artifacts: %s/%s\n' "$repository_root" "$test_root"
UBSAN_OPTIONS=halt_on_error=1 timeout --kill-after=5s 30s "$test_root/test"
