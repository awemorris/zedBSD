#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Actual optional software decoding and native audio share the application adapter, not libmedia's table.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws202-app-fallback"
work=$fresh_dir
streams=${1:-}
if [ -z "$streams" ]; then
    streams=$(sh plan/ws202/tests/make-streams.sh)
fi
sources='userland/desktop/libmedia/decoder.c userland/desktop/libmedia/bitstream.c userland/desktop/libmedia/picture.c userland/desktop/libmedia/bits.c userland/desktop/libmedia/aac-input.c userland/desktop/libmedia/aac-codec.c userland/desktop/libmedia/aac-huffman.c userland/desktop/libmedia/aac-bands.c userland/desktop/libmedia/aac-frame.c userland/desktop/libmedia/aac-synth.c userland/desktop/libmedia/aac.c userland/desktop/libmedia/sound.c'
containers='userland/desktop/mediafile/mediafile.c userland/desktop/mediafile/mp4.c userland/desktop/mediafile/mkv.c userland/desktop/mediafile/ts.c userland/desktop/mediafile/ogg.c userland/desktop/mediafile/avi.c'
for mode in plain sanitize; do
    extra=''
    if [ "$mode" = sanitize ]; then
        extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
    fi
    ${CC:-cc} -std=c89 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-long-long -pedantic -O1 -g $extra -I. \
        plan/ws202/tests/host-app-fallback.c $sources $containers \
        userland/desktop/media-app/decoder.c userland/desktop/media-app/avcodec.c userland/desktop/media-app/bitstream.c \
        -pthread -ldl -lm -o "$work/fallback-$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/fallback-$mode" "$streams/h264-high-b-aac.mp4"
done
printf '%s\n' 'WS202 application fallback PASS (plain, ASan/UBSan; native GPU deliberately unsupported)'
