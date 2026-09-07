#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-page-index.XXXXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
for mode in normal sanitize; do
    flags=''
    if [ "$mode" = sanitize ]; then
        flags='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    ${CC:-cc} -std=c11 -O1 -g -I"$repo/include" -I"$repo/include/uapi" \
        -I"$repo" -Wall -Wextra -Werror $flags \
        "$repo/plan/ws025-io-memory-cache/tests/page-index-host.c" -o "$out/$mode"
    timeout 30 "$out/$mode"
done
