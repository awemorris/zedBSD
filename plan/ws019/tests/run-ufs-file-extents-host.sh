#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-ufs-extents.XXXXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
cd "$repo"
for mode in normal sanitize; do
    flags=''
    if [ "$mode" = sanitize ]; then
        flags='-fsanitize=address,undefined -fno-omit-frame-pointer --param=asan-globals=0'
    fi
    ${CC:-cc} -std=c11 -O1 -g $flags -pthread -c \
        plan/ws018/tests/mount-thread-host.c -o "$out/thread.o"
    ${CC:-cc} -std=c11 -O1 -g $flags -DZEDBSD_USER_ABI_LP64 \
        -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
        -I. -Iinclude -Iinclude/uapi -Isrc -Ilibc/include \
        -Iplan/ws018/tests \
        plan/ws019/tests/ufs-file-extents-host.c \
        src/kern/quota.c src/kern/io.c "$out/thread.o" -pthread \
        -Wl,--gc-sections -o "$out/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60 "$out/$mode"
done
