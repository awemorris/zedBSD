#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-inode-revoked.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
cd "$repo"
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g \
    -ffunction-sections -fdata-sections \
    -Iinclude -Iinclude/uapi -Isrc -I. \
    plan/ws025-io-memory-cache/tests/inode-revoked-host.c \
    -Wl,--gc-sections -o "$out/test"
"$out/test"
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g \
    -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined --param asan-globals=0 \
    -fno-omit-frame-pointer -fno-pie -no-pie \
    -Iinclude -Iinclude/uapi -Isrc -I. \
    plan/ws025-io-memory-cache/tests/inode-revoked-host.c \
    -Wl,--gc-sections -o "$out/sanitized"
ASAN_OPTIONS=detect_leaks=1 "$out/sanitized"
