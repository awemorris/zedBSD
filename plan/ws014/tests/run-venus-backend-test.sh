#!/bin/sh
# Exercise production Venus ownership with a strict synthetic control peer.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ws014-venus-backend.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

cc -std=gnu11 -O2 -Wall -Wextra -Werror -Wdeclaration-after-statement \
    -ffunction-sections -fdata-sections -I"$repo/include" \
    -idirafter "$repo/libc/include" \
    "$repo/plan/ws014/tests/venus-backend.c" -Wl,--gc-sections \
    -o "$work/ordinary"
"$work/ordinary"

cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wdeclaration-after-statement \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections -I"$repo/include" \
    -idirafter "$repo/libc/include" \
    "$repo/plan/ws014/tests/venus-backend.c" -Wl,--gc-sections \
    -o "$work/sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/sanitized"
