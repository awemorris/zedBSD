#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Production container/decoder/app integration and standard command-contract checks, without physical GPU claims.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws202-native-playback"
work=$fresh_dir
streams=$(sh plan/ws202/tests/make-streams.sh)
printf '%s\n' "$streams" > "$work/streams-path"
mkdir -p "$work/include"
ln -s "$repo/include/libc/vulkan" "$work/include/vulkan"
compiler=${CC:-cc}
media_sources='userland/desktop/libmedia/decoder.c userland/desktop/libmedia/bitstream.c userland/desktop/libmedia/picture.c userland/desktop/libmedia/h264.c userland/desktop/libmedia/h264-dpb.c userland/desktop/libmedia/vkvideo.c userland/desktop/libmedia/bits.c userland/desktop/libmedia/aac-input.c userland/desktop/libmedia/aac-codec.c userland/desktop/libmedia/aac-huffman.c userland/desktop/libmedia/aac-bands.c userland/desktop/libmedia/aac-frame.c userland/desktop/libmedia/aac-synth.c userland/desktop/libmedia/aac.c userland/desktop/libmedia/sound.c'
container_sources='userland/desktop/mediafile/mediafile.c userland/desktop/mediafile/mp4.c userland/desktop/mediafile/mkv.c userland/desktop/mediafile/ts.c userland/desktop/mediafile/ogg.c userland/desktop/mediafile/avi.c'
for mode in plain sanitize; do
    extra=''
    if [ "$mode" = sanitize ]; then
        extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
    fi
    "$compiler" -std=c89 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-long-long -pedantic -O1 -g $extra \
        -isystem "$work/include" -I. plan/ws202/tests/host-native-playback.c $media_sources $container_sources \
        userland/desktop/media-app/decoder.c -pthread -lm -o "$work/playback-$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/playback-$mode" "$streams/h264-high-b-aac.mp4" "$streams/h264-nocts.mp4"
    "$compiler" -std=c89 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-long-long -pedantic -O1 -g $extra \
        -isystem "$work/include" -I. plan/ws202/tests/host-vkvideo-runtime.c userland/desktop/libmedia/picture.c \
        -pthread -ldl -lm -o "$work/runtime-$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/runtime-$mode"
    "$compiler" -std=c89 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-long-long -pedantic -O1 -g $extra \
        -I. plan/ws202/tests/host-picture.c userland/desktop/libmedia/picture.c -pthread -lm -o "$work/picture-$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/picture-$mode"
    for test in host-h264-order host-h264; do
        "$compiler" -std=c89 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-long-long -pedantic -O1 -g $extra \
            -isystem "$work/include" -I. "plan/ws202/tests/$test.c" userland/desktop/libmedia/h264.c \
            userland/desktop/libmedia/h264-dpb.c userland/desktop/libmedia/bits.c -o "$work/$test-$mode"
    done
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/host-h264-order-$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/host-h264-$mode" "$streams/h264-multislice.h264"

    "$compiler" -std=c89 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wno-long-long -pedantic -O1 -g $extra \
        -isystem "$work/include" -I. plan/ws202/tests/host-media-probe.c $media_sources $container_sources \
        userland/desktop/media-app/decoder.c userland/base/common/sha256.c -pthread -lm -o "$work/probe-$mode"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/probe-$mode" --audio-rms "$streams/h264-high-b-aac.mp4" > "$work/audio-$mode.rms"
    python3 plan/ws202/tests/compare-rms.py "$streams/h264-high-b-aac.rms" "$work/audio-$mode.rms"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/probe-$mode" --video-hash --twice "$streams/h264-high-b-aac.mp4" > "$work/video-$mode.host.sha256"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$work/probe-$mode" --video-hash --seek=1 --expect="$work/video-$mode.host.sha256" "$streams/h264-high-b-aac.mp4" > "$work/seek-$mode.host.sha256"

done
printf '%s\n' 'WS202 native playback/command/picture host PASS (plain, ASan/UBSan; hardware decoding not tested)'
