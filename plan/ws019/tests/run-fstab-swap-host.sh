#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-fstab.XXXXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
for mode in normal sanitize; do
    flags=''
    if [ "$mode" = sanitize ]; then
        flags='-fsanitize=address,undefined -fno-omit-frame-pointer'
    fi
    ${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror $flags \
        -I"$repo" -I"$repo/include/uapi" \
        "$repo/plan/ws019/tests/fstab-swap-host.c" -o "$out/$mode"
    "$out/$mode"
done
