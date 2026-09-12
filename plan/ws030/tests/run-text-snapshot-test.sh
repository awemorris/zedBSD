#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Exercise complete generic dispatch and PC/AT renderer sources with only hardware/font collaborators mocked.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-text-snapshot.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=
    if [ "$mode" = sanitize ]; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -g'
    fi
    timeout 30 cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
        -Wdeclaration-after-statement $extra -I"$repo/include" -I"$repo/src" \
        "$repo/plan/ws030/tests/text-snapshot.c" \
        "$repo/src/kern/text-display.c" \
        "$repo/src/drivers/platform/pcat/graphics/text.c" \
        -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        timeout 20 "$work/$mode"
    printf '%s: PASS\n' "$mode"
done
