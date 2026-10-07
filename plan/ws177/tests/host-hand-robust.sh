#!/bin/sh
# ws177-p009: builds and runs the host test of the handwriting's robustness (host-hand-robust.c with the compositor's
# keyboard-hand.c and hand-cloud.c) under ASan and UBSan: the templates of the package hand-hershey (made by its make
# rule into build/packages/hand-hershey/hershey.txt) put at KEILAND_DATADIR/keiland/hand/hershey.txt of a new directory
# under build/tmp, where the broken files are written too (nothing is removed here; Q1's plan/tools/q1-clean.sh
# removes old runs).
#   sh plan/ws177/tests/host-hand-robust.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
cd "$repo"
make -s ZEDBSD_CONFIG=config/ci/config-amd64.mk hand-hershey >/dev/null
fresh_out "$repo/build/tmp/ws177-hand-robust"
work=$fresh_dir
mkdir -p "$work/share/keiland/hand"
cp build/packages/hand-hershey/hershey.txt "$work/share/keiland/hand/hershey.txt"
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	-pthread -I. -Iuserland/desktop/wayland -DKEILAND_DATADIR="\"$work/share\"" -o "$work/host-hand-robust" \
	plan/ws177/tests/host-hand-robust.c userland/desktop/wayland/keyboard-hand.c userland/desktop/wayland/hand-cloud.c -lm
timeout 300 "$work/host-hand-robust" "$work/share/keiland/hand/hershey.txt" "$work"
