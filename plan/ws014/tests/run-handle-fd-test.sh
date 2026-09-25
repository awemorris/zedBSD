#!/bin/sh
# Run real generic handle, descriptor, poll and AF_UNIX ownership paths.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=${1:-"$root/build/q309-handle-fd"}
mkdir -p "$work"
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror \
        -DKERN_USER_ABI_LP64 -I"$root/include" -I"$root/include/libc" -DKERN_UAPI_NATIVE \
        -I"$root/include/uapi" -ffunction-sections -fdata-sections $extra \
        "$root/plan/ws014/tests/handle-fd.c" \
        "$root/src/kern/handle.c" "$root/src/kern/fd-object.c" \
        "$root/src/kern/filedesc.c" "$root/src/kern/poll.c" \
        "$root/src/kern/net/socket.c" "$root/src/kern/net/unix-socket.c" \
        "$root/src/kern/net/packet-buf.c" -Wl,--gc-sections \
        -o "$work/$mode" >"$work/$mode-build.log" 2>&1
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
        timeout 20 "$work/$mode" >"$work/$mode.log" 2>&1
    cat "$work/$mode.log"
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
        -DKERN_USER_ABI_LP64 -I"$root/include" -I"$root/include/libc" -DKERN_UAPI_NATIVE \
        -I"$root/include/uapi" -ffunction-sections -fdata-sections $extra \
        "$root/plan/ws014/tests/handle-fd-syscall.c" \
        "$root/src/kern/handle.c" "$root/src/kern/fd-object.c" \
        "$root/src/kern/filedesc.c" "$root/src/kern/poll.c" \
        "$root/src/kern/net/socket.c" "$root/src/kern/net/socket-file.c" \
        "$root/src/kern/net/unix-socket.c" "$root/src/kern/net/packet-buf.c" \
        -Wl,--gc-sections -o "$work/syscall-$mode" >"$work/syscall-$mode-build.log" 2>&1
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
        timeout 20 "$work/syscall-$mode" >"$work/syscall-$mode.log" 2>&1
    cat "$work/syscall-$mode.log"
done
