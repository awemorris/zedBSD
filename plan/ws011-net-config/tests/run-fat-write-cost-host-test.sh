#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repository_root"
mkdir -p plan/ws011-net-config/temp
test_root=$(mktemp -d plan/ws011-net-config/temp/fat-write-cost.XXXXXX)
sanitize_flags=
if [ "${NCOM_SANITIZE:-0}" = 1 ]; then
	sanitize_flags='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
# Deliberate compiler-flag word splitting.
# shellcheck disable=SC2086
timeout --kill-after=5s 60s "${CC:-cc}" -std=c11 -O1 -g \
	-DZEDBSD_USER_ABI_LP64 -Wall -Wextra -Werror -ffunction-sections \
	-fdata-sections -Iinclude -Iinclude/uapi -Isrc -Ilibc/include -I. \
	$sanitize_flags plan/ws011-net-config/tests/fat-write-cost-host-test.c \
	src/drivers/fs/fat.c -Wl,--gc-sections -o "$test_root/test"
printf 'FAT cost diagnostic artifacts: %s/%s\n' "$repository_root" "$test_root"
status=0
timeout --kill-after=5s 30s "$test_root/test" > "$test_root/result.log" 2>&1 || status=$?
cat "$test_root/result.log"
exit "$status"
