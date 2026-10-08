#!/bin/sh
# ws177-p027〜p030 (案 T): the host test of the media file reader's new containers and damaged files.  Builds
# host-media-t.c with the reader (ASan, UBSan) and runs host-media-t.py, which makes the files with the host's ffmpeg
# and compares the reader's packets with ffprobe's.  GROUP: mp4 (p027) by default.
#   sh plan/ws177/tests/host-media-t.sh [GROUP...]
# Last line: host-media-t: PASS.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
fresh_out build/ws177-media-t
out=build/ws177-media-t
M=userland/desktop/mediafile
cc -std=gnu89 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -I. \
    plan/ws177/tests/host-media-t.c $M/mediafile.c $M/mp4.c $M/mkv.c \
    -o "$out/host-media-t" || { echo "host-media-t: FAIL (build)"; exit 1; }
UBSAN_OPTIONS=halt_on_error=1 python3 -I plan/ws177/tests/host-media-t.py "$out/host-media-t" "$out/media" "$@"
