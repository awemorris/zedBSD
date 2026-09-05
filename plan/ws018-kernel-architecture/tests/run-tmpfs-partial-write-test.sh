#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "$repo/plan/ws018-kernel-architecture/temp/tmpfs-write.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
 extra=""
 if test "$mode" = sanitize; then extra="-fsanitize=address,undefined -fno-omit-frame-pointer --param asan-globals=0"; fi
 ${HOSTCC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  -DZEDBSD_USER_ABI_LP64 $extra -I"$repo/include" -I"$repo/include/uapi" \
  -I"$repo/src" -I"$repo/libc/include" \
  "$repo/plan/ws018-kernel-architecture/tests/tmpfs-partial-write-test.c" \
  -Wl,--gc-sections -o "$temporary/$mode"
 ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$temporary/$mode"
done
