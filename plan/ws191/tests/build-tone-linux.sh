#!/bin/sh
# ws191-p004: builds the two tone clients for the Linux guest (T1-447 found
# the bare cc lines short of these flags).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   sh plan/ws191/tests/build-tone-linux.sh OUT [KEILAND_LINUX_BUILD]
#
# After `make keiland-linux` (KEILAND_LINUX_BUILD, build/keiland-linux by
# default).  OUT/tone is linked to that build's libkeiland, whose own
# libwayland-client the linker finds by -rpath-link (the system's lacks the
# Keiland extensions' symbols), and finds both in /opt/keiland/lib on the
# guest; OUT/tone-backend-linux is the
# backend's stream alone (it dlopens libasound, and pump.c needs the GNU
# memfd seals).  Copy both to the guest with guest.sh put.
set -eu

out=${1:?usage: build-tone-linux.sh OUT [KEILAND_LINUX_BUILD]}
build=${2:-build/keiland-linux}
mkdir -p "$out"

cc -std=gnu17 -Wall -Wextra -Werror -I userland/desktop/include plan/ws191/tests/tone.c \
	-L "$build/lib" -lkeiland -Wl,-rpath-link,"$build/lib" \
	-Wl,-rpath,/opt/keiland/lib -lm -o "$out/tone"
cc -std=gnu17 -Wall -Wextra -Werror -D_GNU_SOURCE -I. plan/ws191/tests/tone-backend.c \
	userland/desktop/libkeiland-backend-linux/audio-stream-linux.c \
	userland/desktop/libkeiland-backend/audio/pump.c -lpthread -ldl -lm -o "$out/tone-backend-linux"
echo "build-tone-linux: $out/tone $out/tone-backend-linux"
