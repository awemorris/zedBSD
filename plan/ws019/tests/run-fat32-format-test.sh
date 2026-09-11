#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-fat32.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
for mode in ordinary sanitize; do
    extra=""
    if test "$mode" = sanitize; then
        extra="-fsanitize=address,undefined -fno-omit-frame-pointer"
    fi
    ${HOSTCC:-cc} -std=c89 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
        -Wdeclaration-after-statement $extra -I"$repo" \
        -Dpread=fat_test_pread -Dpwrite=fat_test_pwrite -Dfsync=fat_test_fsync \
        -c "$repo/userland/base/mkfs/fat32-format.c" -o "$temporary/codec.o"
    ${HOSTCC:-cc} -std=c89 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
        -Wdeclaration-after-statement $extra -I"$repo" \
        "$repo/plan/ws019/tests/fat32-format-test.c" \
        "$temporary/codec.o" -o "$temporary/test"
    for sector in 512 1024 2048 4096; do
        image="$temporary/$mode-$sector.img"
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
            "$temporary/test" "$image" "$sector"
        python3 "$repo/plan/ws019/tests/fat32-inspect.py" "$image"
        mdir -i "$image" ::/ > "$temporary/mdir.log"
        printf 'independent mtools round trip\n' > "$temporary/payload"
        mcopy -i "$image" "$temporary/payload" ::/PROOF.TXT
        mcopy -i "$image" ::/PROOF.TXT "$temporary/readback"
        cmp "$temporary/payload" "$temporary/readback"
        rm "$temporary/readback"
        echo "mtools $mode/$sector list/create/read PASS"
    done
done
