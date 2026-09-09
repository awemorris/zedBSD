#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
umask 022
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-pristine.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
noct=${NOCT:-"$repo/build/NoctLang/build-static/noct"}
${HOSTCC:-cc} -std=c11 -O2 -Wall -Wextra -Werror \
    "$repo/tools/build/zedimage-host.c" -o "$temporary/zedimage-host"
"$noct" --path="$repo/tools/build" "$repo/tools/build/make-data-image.noct" \
    --backend "$temporary/zedimage-host" --output "$temporary/ufs.img" --size-mib 32
"$noct" --path="$repo/tools/build" "$repo/tools/build/make-data-image.noct" \
    --backend "$temporary/zedimage-host" --output "$temporary/feature.img" \
    --size-mib 32 --profile=journal-snapshot
"$noct" --path="$repo/tools/build" "$repo/tools/build/make-swapfile.noct" \
    --output "$temporary/swap.img" --size-mib 64
sha256sum "$temporary/ufs.img" "$temporary/swap.img" "$temporary/feature.img" > "$temporary/before"
for mode in ordinary sanitize; do
    extra=""
    if test "$mode" = sanitize; then
        extra="-fsanitize=address,undefined -fno-omit-frame-pointer"
    fi
    # shellcheck disable=SC2086
    ${HOSTCC:-cc} -std=c89 -D_POSIX_C_SOURCE=200809L -O1 -g \
        -Wall -Wextra -Werror -Wdeclaration-after-statement $extra \
        -I"$repo" -idirafter "$repo/include" \
        -Dpread=pristine_pread -Dpwrite=pristine_pwrite \
        -c "$repo/userland/base/mkswap/swap-format.c" -o "$temporary/swap-$mode.o"
    # shellcheck disable=SC2086
    ${HOSTCC:-cc} -std=c89 -D_POSIX_C_SOURCE=200809L -O1 -g \
        -Wall -Wextra -Werror -Wdeclaration-after-statement $extra \
        -I"$repo" -idirafter "$repo/include" \
        "$repo/plan/ws019-installation/tests/pristine-format-host.c" \
        "$repo/userland/base/mkfs/ufs-super.c" \
        "$repo/userland/base/mkfs/ufs-endian.c" \
        "$repo/userland/base/mkswap/swap-codec.c" \
        "$temporary/swap-$mode.o" -o "$temporary/test-$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$temporary/test-$mode" "$temporary/ufs.img" "$temporary/swap.img" \
        "$temporary/feature.img" "$temporary/scratch-$mode.img"
done
sha256sum "$temporary/ufs.img" "$temporary/swap.img" "$temporary/feature.img" > "$temporary/after"
cmp "$temporary/before" "$temporary/after"
echo 'Pristine independent reference hashes unchanged: PASS'
