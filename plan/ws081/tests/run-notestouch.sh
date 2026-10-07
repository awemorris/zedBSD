#!/bin/sh
# ws081-p013: builds Notes' touch screen (touch.c) with libkeiland's motion, scroller, scroll and gestures for the host
# (plain, and with EXTRA_CFLAGS such as the sanitizers) and runs host-notestouch.
#   plan/ws081/tests/run-notestouch.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:-$root/build/ws081-p013-host}
cc=${CC:-clang}
extra=${EXTRA_CFLAGS:-}
mkdir -p "$out/include"

# Only <keiland/keiland.h> is taken from include/libc: the rest of that directory is zedBSD's C library.
mkdir -p "$out/include/keiland"
ln -sf "$root/userland/desktop/include/keiland/keiland.h" "$out/include/keiland/keiland.h"

flags="-std=gnu11 -O2 -g -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion $extra -I$out/include"
for name in motion scroll gesture; do
	"$cc" $flags -c "$root/userland/desktop/libkeiland/$name.c" -o "$out/keiland-$name.o"
done
# The scroll the touch screen holds (kl_scroll, ws090-p015) and what it draws its bars with (libc and libm only).
for name in scroll scroll-bar canvas; do
	"$cc" $flags -c "$root/userland/desktop/libkeiland/ui/$name.c" -o "$out/keiland-ui-$name.o"
done
"$cc" $flags -c "$root/userland/desktop/notes/touch.c" -o "$out/touch.o"
"$cc" $flags -Wno-conversion -c "$root/plan/ws081/tests/host-notestouch.c" -o "$out/host-notestouch.o"
"$cc" $extra "$out/host-notestouch.o" "$out/touch.o" "$out/keiland-motion.o" "$out/keiland-scroll.o" "$out/keiland-gesture.o" \
	"$out/keiland-ui-scroll.o" "$out/keiland-ui-scroll-bar.o" "$out/keiland-ui-canvas.o" -lm \
	-o "$out/host-notestouch"
"$out/host-notestouch" > "$out/host-notestouch.log" 2>&1 || { grep -E '^(FAIL|host-notestouch)' "$out/host-notestouch.log"; exit 1; }
grep -E '^(FAIL|host-notestouch)' "$out/host-notestouch.log"
