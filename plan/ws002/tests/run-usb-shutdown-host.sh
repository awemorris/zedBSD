#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
for variant in ordinary sanitized; do
    extra=""
    if [ "$variant" = sanitized ]; then
        extra="-fsanitize=address,undefined -fno-omit-frame-pointer"
    fi
    cc -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
        -ffunction-sections -fdata-sections -Wl,--gc-sections $extra \
        -I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" \
        "$repo/src/drivers/usb/usb.c" \
        "$repo/src/kern/io.c" \
        "$repo/plan/ws004/tests/usb-function-model-test.c" \
        -o "$work/$variant"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/$variant"
done
