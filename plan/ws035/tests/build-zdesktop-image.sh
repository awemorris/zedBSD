#!/bin/sh
# Builds the zdesktop guest image for the Venus tests (build/ws035-sq): the guest harness's files
# (SSH keys, net.conf) and, when they are present, the files kept out of git: the fonts
# (build/ws035-fonts/: Inter for zdesktop, JetBrains Mono for zdesktop-terminal, both OFL) and the
# wallpaper (build/ws035-wallpaper/wallpaper.ppm, the user's picture).
#
#   plan/ws035/tests/build-zdesktop-image.sh [BUILD]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
build=${1:-build/ws035-sq}
extra=$(python3 plan/tools/guest/guest.py extra-files | sed -n "s/^ZEDBSD_TEST_EXTRA_FILES='\(.*\)'$/\1/p")
[ -n "$extra" ] || { echo "build-zdesktop-image: no guest files (plan/tools/guest/guest.py keys?)"; exit 1; }
[ -f build/ws035-fonts/Inter.ttf ] && extra="$extra --file /usr/share/fonts/zdesktop.ttf=build/ws035-fonts/Inter.ttf"
[ -f build/ws035-fonts/OFL.txt ] && extra="$extra --file /usr/share/fonts/zdesktop-OFL.txt=build/ws035-fonts/OFL.txt"
[ -f build/ws035-fonts/JetBrainsMono-Regular.ttf ] && extra="$extra --file /usr/share/fonts/zdesktop-mono.ttf=build/ws035-fonts/JetBrainsMono-Regular.ttf"
[ -f build/ws035-fonts/JetBrainsMono-OFL.txt ] && extra="$extra --file /usr/share/fonts/zdesktop-mono-OFL.txt=build/ws035-fonts/JetBrainsMono-OFL.txt"
[ -f build/ws035-wallpaper/wallpaper.ppm ] && extra="$extra --file /usr/share/zdesktop/wallpaper.ppm=build/ws035-wallpaper/wallpaper.ppm"
exec make -j"$(nproc)" ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD="$build" \
    "ZEDBSD_TEST_EXTRA_FILES=$extra" disk-image
