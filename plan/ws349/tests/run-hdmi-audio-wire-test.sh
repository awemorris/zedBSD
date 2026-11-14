#!/bin/sh
# The Raspberry Pi 5 HDMI audio arithmetic (src/drivers/platform/rpi5/rpi5-hdmi-audio-wire.c) on the host:
# subframes, channel status, the mmap copy order, the TMDS rate readback, N and CTS, the sample divider,
# the InfoFrame, the DMA ring and position, the stepping table.  Builds in a new folder under build/tmp
# (old folders are Q1's to remove), normally and with ASan/UBSan.  The last line is "hdmi-audio-wire: all PASS".
# usage: sh plan/ws349/tests/run-hdmi-audio-wire-test.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
mkdir -p "$repo/build/tmp"
work=$(mktemp -d "$repo/build/tmp/hdmi-audio-wire.XXXXXX")
flags="-std=c11 -Wall -Wextra -Werror -Wdeclaration-after-statement -I$repo/src -I$repo/include"
sources="$repo/plan/ws349/tests/hdmi-audio-wire-test.c $repo/src/drivers/platform/rpi5/rpi5-hdmi-audio-wire.c"
# shellcheck disable=SC2086
cc $flags -O2 $sources -o "$work/wire-test"
# shellcheck disable=SC2086
cc $flags -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer $sources -o "$work/wire-sanitized"
echo "== ASan/UBSan"
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$work/wire-sanitized"
echo "== normal"
"$work/wire-test"
