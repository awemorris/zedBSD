#!/bin/sh
# ws177-p010: builds keiland-preview on Linux as plan/ws168/tests/run-host-preview.sh does (with its seccomp
# confinement; ASAN=1 builds it with ASan and UBSan instead, without the confinement and the memory limit) and runs
# host-preview-fuzz.py: the bombs (pictures and documents that say they are huge, a decompression bomb, an input past
# 64 MiB, a deep PDF) and SAMPLES damaged files made from good ones, each under the limits Files puts on the child
# (linux/spawn.c: 1 GiB of memory, 10 s of processor time, 48 MiB written) and its 10 s; every run must end with a
# status of its own (0 to 4), never a signal.  A new folder each run under build/tmp (Q1's cleaning removes old ones).
#   sh plan/ws177/tests/host-preview-fuzz.sh [SAMPLES [SEED]]   (default 300 samples, seed 1)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
cd "$repo"
fresh_out "$repo/build/tmp/ws177-preview-fuzz"
dir=$fresh_dir
mkdir -p "$dir/inc/truetype"
cp userland/desktop/include/truetype/truetype.h "$dir/inc/truetype/truetype.h"
cp include/libc/pdf.h include/libc/md5.h include/libc/sha1.h include/libc/sha2.h "$dir/inc/"
ln -sfn "$repo/include/libc/compat" "$dir/inc/compat"
P=userland/desktop/preview
B=userland/base
sources="$P/main.c $P/make.c $P/decode.c $P/scale.c $P/fonts.c $P/linux/confine.c userland/desktop/picture/picture.c $B/libpdf/*.c
	userland/desktop/libtruetype/*.c $B/libz-compat/*.c $B/libpng-compat/*.c $B/libjpeg-compat/*.c $B/libgif-compat/*.c"
flags="-std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -I. -I$dir/inc -DPDF_FONT_FILES=0"
limit=1
if [ "${ASAN:-0}" = 1 ]; then
	# The sanitizers make calls the seccomp filter refuses: this build is not confined (a stand-in confine.c).
	flags="$flags -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
	limit=0
	printf '#include "userland/desktop/preview/preview.h"\nint preview_confine(void) { return 0; }\n' > "$dir/confine-none.c"
	sources=$(echo "$sources" | sed "s|$P/linux/confine.c|$dir/confine-none.c|")
fi
cc $flags -w -c src/libc/openbsd-sha2.c -o "$dir/sha2.o"
cc $flags -w -c src/libc/openbsd-digest.c -o "$dir/digest.o"
# shellcheck disable=SC2086
cc $flags -Werror $sources "$dir/sha2.o" "$dir/digest.o" -lm -o "$dir/keiland-preview"
timeout 3600 python3 -I plan/ws177/tests/host-preview-fuzz.py "$dir/keiland-preview" "$dir" "$limit" "${1:-300}" "${2:-1}"
