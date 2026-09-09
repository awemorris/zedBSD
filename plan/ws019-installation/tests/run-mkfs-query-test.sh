#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-mkfs-query.XXXXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
cd "$repo"
for mode in normal sanitize; do
    flags=''
    if [ "$mode" = sanitize ]; then
        flags='-fsanitize=address,undefined -fno-omit-frame-pointer'
    fi
    ${CC:-cc} -std=c89 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror $flags \
        -I. -idirafter include/uapi userland/base/mkfs/main.c \
        userland/base/mkfs/block-command.c userland/base/mkfs/ufs-format.c \
        userland/base/mkfs/ufs-super.c userland/base/mkfs/ufs-endian.c \
        userland/base/mkfs/fat32-format.c userland/base/common/format-file.c \
        plan/ws019-installation/tests/mkfs-query-no-io.c \
        -Wl,--wrap=open,--wrap=pread,--wrap=pwrite,--wrap=fsync,--wrap=ioctl \
        -o "$out/$mode"
    python3 plan/ws019-installation/tests/mkfs-query-test.py "$out/$mode"
done
