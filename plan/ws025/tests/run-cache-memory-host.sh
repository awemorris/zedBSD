#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-cache-memory.XXXXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
for mode in normal sanitize; do
    flags=''
    if [ "$mode" = sanitize ]; then
        flags='-fsanitize=address,undefined -fno-omit-frame-pointer'
    fi
    ${CC:-cc} -std=c11 -O1 -g -DKERN_USER_ABI_LP64 \
        -I"$repo/include" -I"$repo/include/uapi" -Wall -Wextra -Werror $flags \
        "$repo/src/kern/cache-memory.c" \
        "$repo/plan/ws025/tests/cache-memory-host.c" \
        -pthread -o "$out/$mode"
    timeout 30 "$out/$mode"
done
