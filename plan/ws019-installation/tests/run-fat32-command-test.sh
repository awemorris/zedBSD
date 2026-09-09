#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-fat32-command.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=""
    if test "$mode" = sanitize; then extra="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
    ${HOSTCC:-cc} -std=c89 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror $extra \
        -I"$repo" -idirafter "$repo/include/uapi" \
        -Dopen=fc_open -Dfstat=fc_fstat -Dioctl=fc_ioctl -Dclose=fc_close \
        -Dfflush=fc_fflush -Dfgets=fc_fgets \
        -c "$repo/userland/base/mkfs/block-command.c" -o "$temporary/command.o"
    ${HOSTCC:-cc} -std=c89 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror $extra \
        -I"$repo" -idirafter "$repo/include/uapi" \
        "$repo/plan/ws019-installation/tests/fat32-command-test.c" \
        "$temporary/command.o" -o "$temporary/test"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$temporary/test"
done
