#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Test original production parsers with normative vectors and independent encoder output.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws202-aac-input"
work=$fresh_dir
compiler=${CC:-cc}
ffmpeg -nostdin -v error -f lavfi -i 'sine=frequency=997:sample_rate=44100:duration=0.2' \
    -ac 2 -c:a aac -profile:a aac_low -f adts "$work/stereo.aac"
ffmpeg -nostdin -v error -f lavfi -i 'sine=frequency=431:sample_rate=48000:duration=0.2' \
    -ac 1 -c:a aac -profile:a aac_low -f adts "$work/mono.aac"
for mode in plain sanitize; do
    extra=""
    if test "$mode" = sanitize; then
        extra="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
    fi
    "$compiler" -std=c89 -Wall -Wextra -Werror -pedantic -O1 -g $extra -pthread -I"$repo" \
        "$repo/plan/ws202/tests/host-aac-input.c" \
        "$repo/userland/desktop/libmedia/bits.c" \
        "$repo/userland/desktop/libmedia/aac-input.c" \
        "$repo/userland/desktop/libmedia/aac-huffman.c" \
        "$repo/userland/desktop/libmedia/aac-codec.c" -o "$work/$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$work/$mode" "$work/stereo.aac" "$work/mono.aac"
done
echo "WS202 bits/AAC input host PASS (plain, ASan/UBSan; ADTS headers only)"
