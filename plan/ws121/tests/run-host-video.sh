#!/bin/sh
# ws121-p004: builds the browser's engine on the host (plan/ws074/tests/host-build.sh, which links libmedia's sources)
# and runs host-video.c over <browser/browser.h> against the host's FFmpeg by dlopen: a page with WS122's sample.mp4
# as a muted autoplay <video>, drawn by the CPU before and while it plays (the drawings become PNG files), and
# (ws121-p005) a page whose script uses HTMLMediaElement and its events, and (p006) a video with controls clicked.
#   sh plan/ws121/tests/run-host-video.sh [OUTPUT]   (default build/ws121/host-video; the page and the video are put
#   beside it)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws121/host-video}
dir=$(dirname -- "$out")
mkdir -p "$dir"
sh plan/ws074/tests/host-build.sh plain > "$dir/host-build.log" 2>&1 || { tail -20 "$dir/host-build.log"; exit 1; }
base=build/ws074-host
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I"$base/include" -o "$out" plan/ws121/tests/host-video.c \
	-L"$base/plain" -Wl,-rpath,"$(pwd)/$base/plain" -l:libbrowser.so
cp plan/ws121/tests/video.html "$dir/video.html"
cp plan/ws121/tests/script.html "$dir/script.html"
cp plan/ws121/tests/controls.html "$dir/controls.html"
cp plan/tools/media/sample.mp4 "$dir/sample.mp4"
timeout 60 "$out" "$(pwd)/$dir/video.html" "$(pwd)/$dir/script.html" "$(pwd)/$dir/controls.html" "$out"
for p in "$out"-*.ppm; do
	python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$p" "${p%.ppm}.png"
done
