#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-vm-discard.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
cd "$repo"
cc -std=c11 -O1 -g -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -Iinclude -Iinclude/uapi -Isrc -I. \
    plan/ws025/tests/vm-discard-host.c \
    -Wl,--gc-sections -o "$out/test"
"$out/test"
cc -std=c11 -O1 -g -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined --param asan-globals=0 \
    -fno-omit-frame-pointer -fno-pie -no-pie \
    -Iinclude -Iinclude/uapi -Isrc -I. \
    plan/ws025/tests/vm-discard-host.c \
    -Wl,--gc-sections -o "$out/sanitized"
ASAN_OPTIONS=detect_leaks=1 "$out/sanitized"
