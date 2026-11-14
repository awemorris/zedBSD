#!/bin/sh
# Builds and runs the host test of the cursor thread's waits (wayland/cursor-thread.c) under ASan+UBSan and TSan.
#   sh plan/ws353/tests/cursor-step/run.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../../.."
out=${1:-build/ws353/host-cursor-step}
mkdir -p "$out"
for sanitizer in address,undefined thread; do
	${CC:-cc} -std=gnu89 -Wall -Wextra -Werror -D_GNU_SOURCE -I. -fsanitize=$sanitizer -g -pthread \
	    userland/desktop/wayland/cursor-thread.c \
	    plan/ws353/tests/cursor-step/host-cursor-step.c -o "$out/host-cursor-step-${sanitizer%%,*}"
	echo "== sanitizer $sanitizer"
	timeout 120 "$out/host-cursor-step-${sanitizer%%,*}"
done
