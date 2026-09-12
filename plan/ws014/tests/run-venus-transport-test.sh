#!/bin/sh
# Exercise actual modern PCI and split-queue code with a bounded host peer.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ws014-venus-transport.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

cc -std=gnu11 -O2 -Wall -Wextra -Werror -Wdeclaration-after-statement \
    -I"$repo/include" -idirafter "$repo/libc/include" \
    "$repo/plan/ws014/tests/venus-transport.c" -o "$work/ordinary"
"$work/ordinary"

cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wdeclaration-after-statement \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo/include" -idirafter "$repo/libc/include" \
    "$repo/plan/ws014/tests/venus-transport.c" -o "$work/sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/sanitized"
