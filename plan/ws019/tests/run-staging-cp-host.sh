#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
output=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-staging-cp.XXXXXX")
trap 'rm -rf -- "$output"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=""
    if test "$mode" = sanitize; then extra="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
    cc -std=c89 -O1 -g -Wall -Wextra -Werror $extra -I"$repo" \
        "$repo/plan/ws019/tests/staging-cp-host.c" -o "$output/$mode"
    mkdir "$output/$mode-files"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$output/$mode" "$output/$mode-files"
done
