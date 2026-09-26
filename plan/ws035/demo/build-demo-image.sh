#!/bin/sh
# Builds the zdesktop demo image for a real amd64 machine with an Intel GPU (i915): zwl --glass starts at
# boot on the machine's own display, and App Home (the launcher at the top left, or a drag from the
# top-left corner) starts the terminal and the model viewer.  The fonts and the wallpaper, kept out of
# git, are put in from build/ws035-fonts/ and build/ws035-wallpaper/ when they are there.
#
#   plan/ws035/demo/build-demo-image.sh [BUILD]     (default build/zdesktop-demo)
#
# The image is BUILD/hdd-image.img; write it to a USB stick and boot the machine from it (UEFI).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
build=${1:-build/zdesktop-demo}
demo=plan/ws035/demo
extra="--file /etc/service.d/zdesktop=$demo/zdesktop --file /etc/zdesktop/run-zdesktop.sh=$demo/run-zdesktop.sh"
[ -f build/ws035-fonts/Inter.ttf ] && extra="$extra --file /usr/share/fonts/zdesktop.ttf=build/ws035-fonts/Inter.ttf"
[ -f build/ws035-fonts/OFL.txt ] && extra="$extra --file /usr/share/fonts/zdesktop-OFL.txt=build/ws035-fonts/OFL.txt"
[ -f build/ws035-fonts/JetBrainsMono-Regular.ttf ] && extra="$extra --file /usr/share/fonts/zdesktop-mono.ttf=build/ws035-fonts/JetBrainsMono-Regular.ttf"
[ -f build/ws035-fonts/JetBrainsMono-OFL.txt ] && extra="$extra --file /usr/share/fonts/zdesktop-mono-OFL.txt=build/ws035-fonts/JetBrainsMono-OFL.txt"
[ -f build/ws035-wallpaper/wallpaper-1080.ppm ] && extra="$extra --file /usr/share/zdesktop/wallpaper.ppm=build/ws035-wallpaper/wallpaper-1080.ppm"
make -j"$(nproc)" ZEDBSD_CONFIG=plan/ws031/tests/config-zdesktop-hw.mk BUILD="$build" \
    "ZEDBSD_TEST_RC_CONF=$demo/rc.conf" "ZEDBSD_TEST_EXTRA_FILES=$extra" ZEDBSD_TEST_IMAGE_TAG=zdesktop-demo disk-image
echo "zdesktop demo image: $build/hdd-image.img"
