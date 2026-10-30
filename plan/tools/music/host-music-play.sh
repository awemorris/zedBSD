#!/bin/sh
# ws177-p021: builds and runs the host test of Music's player when a song cannot go on (host-music-play.c with
# userland/desktop/music/play.c and stand-ins for the container, the decoder and the sound stream) under ASan and
# UBSan.
#   sh plan/tools/music/host-music-play.sh [OUTPUT]   (default build/ws177/host-music-play)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws177/host-music-play}
dir=$(dirname -- "$out")
mkdir -p "$dir/inc/keiland"
cp userland/desktop/include/keiland/keiland*.h "$dir/inc/keiland/"
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -I"$dir/inc" -Iuserland/desktop/music plan/tools/music/host-music-play.c userland/desktop/music/play.c \
	userland/desktop/libkeiland/platform/thread-posix.c \
	-lpthread -o "$out"
timeout 60 "$out"
