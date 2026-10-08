#!/bin/sh
# ws177-p020: builds and runs the host test of Music's collection in its quasi-normal cases (host-music-m.c with
# userland/desktop/music/library.c, tags.c and cover.c, the pictures' decoding and libkeiland's images) under ASan and
# UBSan, on the files host-music-m.py writes into a fresh folder each run (under OUTPUT's folder; Q1's cleaning removes
# the old ones).
#   sh plan/ws177/tests/host-music-m.sh [OUTPUT]   (default build/ws177/host-music-m)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws177/host-music-m}
dir=$(dirname -- "$out")
mkdir -p "$dir/inc/keiland" "$dir/inc/truetype"
cp userland/desktop/include/keiland/keiland.h "$dir/inc/keiland/keiland.h"
cp userland/desktop/include/truetype/truetype.h "$dir/inc/truetype/truetype.h"
ln -sfn "$(pwd)/include/libc/compat" "$dir/inc/compat"
U=userland/desktop
K=$U/libkeiland/ui
B=userland/base
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -I"$dir/inc" -I$K -I$U/music plan/ws177/tests/host-music-m.c $U/music/library.c $U/music/tags.c $U/music/cover.c \
	$U/picture/picture.c $K/canvas.c \
	$B/libz-compat/inflate.c $B/libz-compat/checksum.c $B/libpng-compat/read.c \
	$B/libjpeg-compat/decompress.c $B/libjpeg-compat/error.c $B/libjpeg-compat/huffman.c $B/libjpeg-compat/idct.c \
	$B/libjpeg-compat/marker.c $B/libjpeg-compat/memory.c $B/libjpeg-compat/source.c \
	$B/libgif-compat/decode.c $B/libgif-compat/lzw.c -lm -o "$out"
folder=$(mktemp -d "$dir/music-m.XXXXXX")
python3 plan/ws177/tests/host-music-m.py "$folder"
timeout 60 "$out" "$folder"
