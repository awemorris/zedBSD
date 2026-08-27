#!/bin/sh
set -eu

repo=${1:-.}
output=${TMPDIR:-/tmp}/ws016-boot-source-runtime-test

cc -std=c11 -I"$repo/include" -I"$repo/include/uapi" -I"$repo" \
    -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    "$repo/src/kern/boot-source.c" \
    "$repo/src/kern/boot-source-contract.c" \
    "$repo/plan/ws016-swap-control/tests/boot-source-runtime-test.c" \
    -Wl,--gc-sections -o "$output"
"$output"
