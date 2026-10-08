#!/bin/sh
# BUG-267: builds and runs the host test of the compositor's touch screen with two fingers at an edge (host-touch-group.c)
# against userland/desktop/wayland/touch.c and edge.c, compiled unchanged with the host's compiler as the Linux build does.
# usage: sh plan/bugs/BUG-267/tests/run-host-touch-group.sh   (from the repository's top; a new folder under build/tmp)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
. plan/tools/fresh-out.sh
fresh_out build/tmp/bug267-touch-group
out=$fresh_dir
cc=${CC:-clang}
extra=${EXTRA_CFLAGS:-}
flags="-std=gnu17 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I. -Iuserland/desktop/include"
$cc $flags $extra -c userland/desktop/wayland/touch.c -o "$out/touch.o"
$cc $flags $extra -c userland/desktop/wayland/edge.c -o "$out/edge.o"
$cc $flags $extra -c plan/bugs/BUG-267/tests/host-touch-group.c -o "$out/host-touch-group.o"
$cc $extra "$out/host-touch-group.o" "$out/touch.o" "$out/edge.o" -o "$out/host-touch-group"
"$out/host-touch-group"
