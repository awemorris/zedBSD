#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-df-capacity.XXXXXX")
trap 'rm -rf -- "$out"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=""
    if test "$mode" = sanitize; then
        extra="-fsanitize=address,undefined -fno-omit-frame-pointer"
    fi
    ${HOSTCC:-cc} -std=c89 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
        -Wdeclaration-after-statement $extra -I"$repo" \
        "$repo/plan/ws019/tests/df-capacity-host.c" \
        "$repo/userland/base/common/command.c" -o "$out/test-$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/test-$mode"
done
