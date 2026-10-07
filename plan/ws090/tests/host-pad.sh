#!/bin/sh
# ws090-p019: builds the terminal's touch screen (touch.c, with its touch pad inputs) with libkeiland's motion,
# scroller and gestures for the host and runs host-pad-terminal.
#   plan/ws090/tests/host-pad.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:-$root/build/ws090-pad-host}
cc=${CC:-cc}
mkdir -p "$out/include"
mkdir -p "$out/include/keiland"
ln -sf "$root/userland/desktop/include/keiland/keiland.h" "$out/include/keiland/keiland.h"
flags="-std=gnu11 -O2 -g -Wall -Wextra -Werror -I$out/include"
for name in motion scroll gesture; do
	"$cc" $flags -c "$root/userland/desktop/libkeiland/$name.c" -o "$out/keiland-$name.o"
done
# The scroll the touch screen holds (kl_scroll, ws090-p015) and what it draws its bars with (libc and libm only).
for name in scroll scroll-bar canvas; do
	"$cc" $flags -c "$root/userland/desktop/libkeiland/ui/$name.c" -o "$out/keiland-ui-$name.o"
done
"$cc" $flags -c "$root/userland/desktop/terminal/touch.c" -o "$out/terminal-touch.o"
"$cc" $flags -c "$root/plan/ws090/tests/host-pad-terminal.c" -o "$out/host-pad-terminal.o"
"$cc" "$out/host-pad-terminal.o" "$out/terminal-touch.o" "$out/keiland-motion.o" "$out/keiland-scroll.o" "$out/keiland-gesture.o" \
	"$out/keiland-ui-scroll.o" "$out/keiland-ui-scroll-bar.o" "$out/keiland-ui-canvas.o" -lm \
	-o "$out/host-pad-terminal"
"$out/host-pad-terminal" > "$out/host-pad-terminal.log" 2>&1 || { grep -E '^(FAIL|host-pad)' "$out/host-pad-terminal.log"; exit 1; }
grep -E '^(FAIL|host-pad)' "$out/host-pad-terminal.log"
grep -E 'KINETIC|caught' "$out/host-pad-terminal.log" | head -5
