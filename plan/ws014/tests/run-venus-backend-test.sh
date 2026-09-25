#!/bin/sh
# Exercise production Venus ownership with a strict control peer and complete production display linkage.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ws014-venus-backend.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

cc -std=gnu11 -O2 -Wall -Wextra -Werror -Wdeclaration-after-statement \
    -I"$repo/include" \
    -idirafter "$repo/include/libc" \
    "$repo/plan/ws014/tests/venus-backend-linked.c" \
    -o "$work/ordinary"
timeout 30 "$work/ordinary"

cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wdeclaration-after-statement \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo/include" \
    -idirafter "$repo/include/libc" \
    "$repo/plan/ws014/tests/venus-backend-linked.c" \
    -o "$work/sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$work/sanitized"
