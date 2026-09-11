#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-nested-mount.XXXXXX")
trap 'rm -rf -- "$out"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=""
    if test "$mode" = sanitize; then
        extra="-fsanitize=address,undefined -fno-omit-frame-pointer --param asan-globals=0"
    fi
    ${HOSTCC:-cc} -O1 -g -pthread $extra -c \
        "$repo/plan/ws018/tests/mount-thread-host.c" -o "$out/thread.o"
    ${HOSTCC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror \
        -DZEDBSD_USER_ABI_LP64 -DZEDBSD_STORAGE_HOST_TEST -ffunction-sections -fdata-sections \
        $extra -I"$repo/include" -I"$repo/include/uapi" -I"$repo/src" -I"$repo/libc/include" \
        "$repo/plan/ws019/tests/nested-mount-host.c" \
        "$repo/src/kern/mount.c" "$repo/src/kern/inode.c" "$repo/src/kern/namei.c" \
        "$repo/src/kern/namecache.c" "$repo/src/kern/cwdinfo.c" "$out/thread.o" \
        -pthread -Wl,--gc-sections -o "$out/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60 "$out/$mode"
done
