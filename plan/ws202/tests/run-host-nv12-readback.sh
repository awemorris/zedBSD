#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Run production command recording/execution with the WS031 GPU stand-in.
# The test proves emitted plane surfaces; GPU pixels require hardware.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws202-nv12-readback"
work=$fresh_dir
compiler=${CC:-cc}
mkdir "$work/include"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"
driver=$repo/src/drivers/gpu/i915
executor=""
for part in codec object dispatch transport instance vulkan fence objects reply \
    memory image descriptor forget pipeline pipeline-prepare render-pass sync command \
    state batch math video video-mfx video-h264-tables; do
    executor="$executor $driver/render/$part.c"
done
executor="$executor $repo/src/drivers/gpu/compiler/spirv.c"
for part in compile eu; do
    executor="$executor $driver/compiler/$part.c"
done
base="-std=gnu11 -Wall -Wextra -Werror -Wdeclaration-after-statement -DKERN_USER_ABI_LP64 -DVK_REPO=\"$repo\" -I$repo/include -I$repo/src -I$repo -idirafter $repo/include/libc"
for mode in plain sanitize; do
    extra=""
    if test "$mode" = sanitize; then
        extra="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
    fi
    "$compiler" $base -O1 -g $extra "$repo/plan/ws202/tests/host-nv12-readback.c" \
        $executor -o "$work/$mode" -lm
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/$mode"
    library="$repo/userland/desktop/libvulkan"
    "$compiler" -std=gnu89 -Wdeclaration-after-statement -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g \
        -ffunction-sections -fdata-sections $extra \
        -I"$work/include" -I"$repo/include" -I"$library" \
        "$repo/plan/ws202/tests/host-video-readback-query.c" \
        "$library/video.c" "$library/sync2.c" "$library/external-properties.c" \
        "$library/objects.c" "$library/wire.c" "$library/codec.c" \
        -Wl,--gc-sections -pthread -o "$work/query-$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/query-$mode"
done
echo "WS202 NV12 readback host PASS (plain, ASan/UBSan)"
