#!/bin/sh
# Builds and runs the host test of the shown thread and the render thread's present fences
# (wayland/render-thread.c) under ASan+UBSan and TSan.
#   sh plan/ws353/tests/shown/run.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../../.."
out=${1:-build/ws353/host-shown}
mkdir -p "$out"
for sanitizer in address,undefined thread; do
	${CC:-cc} -std=gnu89 -Wall -Wextra -Werror -D_GNU_SOURCE -I. -fsanitize=$sanitizer -g -pthread \
	    userland/desktop/wayland/render-thread.c \
	    plan/ws353/tests/shown/host-shown.c -o "$out/host-shown-${sanitizer%%,*}"
	echo "== sanitizer $sanitizer"
	timeout 120 "$out/host-shown-${sanitizer%%,*}"
done
