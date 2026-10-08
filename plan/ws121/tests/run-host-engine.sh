#!/bin/sh
# ws121-p002: builds and runs the host test of libmedia's engine (host-engine.c with userland/desktop/libmedia/engine.c,
# the container reader and Video Player's decoding add-in; no sound since WS191) under ASan and UBSan, against the host's
# FFmpeg by dlopen: WS122's sample.mp4 through a source, and a 2 s AAC song the host's ffmpeg makes.
#   sh plan/ws121/tests/run-host-engine.sh [OUTPUT]   (default build/ws121/host-engine)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws121/host-engine}
dir=$(dirname -- "$out")
mkdir -p "$dir"
U=userland/desktop
cc -std=gnu99 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -I. plan/ws121/tests/host-engine.c $U/libmedia/engine.c $U/libmedia/decoder.c $U/libmedia/avcodec.c \
	$U/libmedia/bitstream.c $U/mediafile/mediafile.c $U/mediafile/mp4.c $U/mediafile/mkv.c $U/mediafile/ts.c $U/mediafile/ogg.c $U/mediafile/avi.c \
	-ldl -lpthread -o "$out"
ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=440:duration=2" -c:a aac -b:a 64k "$dir/song.m4a"
timeout 60 "$out" plan/ws122/tests/sample.mp4 "$dir/song.m4a"
