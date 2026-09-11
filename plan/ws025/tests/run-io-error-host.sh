#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-io-error.XXXXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
for mode in normal sanitize; do
    flags=''
    if [ "$mode" = sanitize ]; then
        flags='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    ${CC:-cc} -std=c11 -O1 -g -I"$repo/include" -I"$repo/include/uapi" \
        -Wall -Wextra -Werror $flags "$repo/src/kern/io-error.c" \
        "$repo/plan/ws025/tests/io-error-host.c" -pthread -o "$out/$mode"
    timeout 30 "$out/$mode"
    ${CC:-cc} -std=c11 -O1 -g -I"$repo/include" -I"$repo/include/uapi" \
        -Wall -Wextra -Werror $flags \
        "$repo/plan/ws025/tests/io-context-host.c" -o "$out/context-$mode"
    timeout 30 "$out/context-$mode"
done
