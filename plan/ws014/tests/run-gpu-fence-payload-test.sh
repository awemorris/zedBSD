#!/bin/sh
# Link actual handle/fence/poll/AF_UNIX cores with the existing bounded scheduler peer.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
work=$(mktemp -d /tmp/zedbsd-gpu-fence-payload.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=
    if test "$mode" = sanitize; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie'
    fi
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections $extra \
        -DKERN_USER_ABI_LP64 -I"$repo/include" -I"$repo/include/libc" -DKERN_UAPI_NATIVE -I"$repo/include/uapi" \
        "$repo/plan/ws014/tests/gpu-fence-payload.c" \
        "$repo/src/kern/handle.c" "$repo/src/drivers/gpu/gpu-fence.c" "$repo/src/kern/fd-object.c" \
        "$repo/src/kern/filedesc.c" "$repo/src/kern/poll.c" \
        "$repo/src/kern/net/socket.c" "$repo/src/kern/net/unix-socket.c" \
        "$repo/src/kern/net/packet-buf.c" -Wl,--gc-sections -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$work/$mode"
done
