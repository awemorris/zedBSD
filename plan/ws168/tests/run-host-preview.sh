#!/bin/sh
# ws168-p003: builds keiland-preview on Linux (userland/desktop/preview with linux/confine.c, the decoders and libpdf
# without substitute font files) twice -- as installed, and the test build with PREVIEW_TEST_ESCAPE -- and runs
# host-preview.py on pictures and documents it makes in a fresh folder each run (under OUTPUT's folder; Q1's cleaning
# removes the old ones); ws168-p004: then host-client.c, the callers' side with a child and in the process.
#   sh plan/ws168/tests/run-host-preview.sh [OUTPUT]   (default build/ws168/host-preview)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws168/host-preview}
dir=$(dirname -- "$out")
mkdir -p "$dir/inc/truetype"
cp userland/desktop/include/truetype/truetype.h "$dir/inc/truetype/truetype.h"
cp include/libc/pdf.h include/libc/md5.h include/libc/sha1.h include/libc/sha2.h "$dir/inc/"
ln -sfn "$(pwd)/include/libc/compat" "$dir/inc/compat"
P=userland/desktop/preview
B=userland/base
sources="$P/main.c $P/make.c $P/decode.c $P/scale.c $P/fonts.c $P/linux/confine.c userland/desktop/picture/picture.c $B/libpdf/*.c
	userland/desktop/libtruetype/*.c $B/libz-compat/*.c $B/libpng-compat/*.c $B/libjpeg-compat/*.c $B/libgif-compat/*.c"
flags="-std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -I. -I$dir/inc -DPDF_FONT_FILES=0"
cc $flags -w -c src/libc/openbsd-sha2.c -o "$dir/sha2.o"
cc $flags -w -c src/libc/openbsd-digest.c -o "$dir/digest.o"
# shellcheck disable=SC2086
cc $flags -Werror $sources "$dir/sha2.o" "$dir/digest.o" -lm -o "$out"
# shellcheck disable=SC2086
cc $flags -Werror -DPREVIEW_TEST_ESCAPE=1 $sources "$dir/sha2.o" "$dir/digest.o" -lm -o "$out-escape"
folder=$(mktemp -d "$(pwd)/$dir/preview.XXXXXX")
timeout 300 python3 plan/ws168/tests/host-preview.py "$out" "$out-escape" "$folder"
# ws168-p004: the callers' side, with the program as a child (linux/spawn.c, the program built above as the libexec's)
# and in this process (freebsd/spawn.c, FreeBSD's until Capsicum).
mkdir -p "$folder/libexec"
cp "$out" "$folder/libexec/keiland-preview"
# shellcheck disable=SC2086
cc $flags -Werror -DKEILAND_LIBEXECDIR="\"$folder/libexec\"" plan/ws168/tests/host-client.c $P/client.c $P/linux/spawn.c -o "$out-client"
# shellcheck disable=SC2086
cc $flags -Werror plan/ws168/tests/host-client.c $P/client.c $P/freebsd/spawn.c $P/make.c $P/decode.c $P/scale.c \
	userland/desktop/picture/picture.c $B/libpdf/*.c userland/desktop/libtruetype/*.c $B/libz-compat/*.c $B/libpng-compat/*.c \
	$B/libjpeg-compat/*.c $B/libgif-compat/*.c "$dir/sha2.o" "$dir/digest.o" -lm -o "$out-client-inprocess"
(cd "$folder" && timeout 60 "../$(basename "$out")-client" a.png g.txt && timeout 60 "../$(basename "$out")-client-inprocess" a.png g.txt)
