#!/bin/sh
# Exercise actual Venus sharing and GPU-only display with bounded native peers.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-venus-sharing.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wdeclaration-after-statement \
        -ffunction-sections -fdata-sections $extra \
        -I"$repo/include" -idirafter "$repo/include/libc" \
        "$repo/plan/ws014/tests/venus-sharing.c" -Wl,--gc-sections -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$work/$mode"
done
