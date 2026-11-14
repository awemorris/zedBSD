#!/bin/sh
# WS103: builds the Venus guest image of the GPU-boundary tests (plan/tools/gpu-boundary/config-amd64-forge.mk, the
# criteria image's config: the generated wallpapers come from KEILAND_WALLPAPERS there).
# ws136-p001 (2026-10-04): the image is a config.mk build plus files of the tree (plan/tools/guest/test-image.sh).
# The fonts come with the compositor's package (userland/desktop/fonts/); the wallpaper is
# userland/desktop/wallpapers/Birch-Lake.png.
#
#   plan/tools/gpu-boundary/build-forge-image.sh [BUILD]     (default build/amd64)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
build=${1:-build/amd64}
exec plan/tools/guest/test-image.sh plan/tools/gpu-boundary/config-amd64-forge.mk "$build" \
	--file /usr/share/keiland/wallpaper.png=userland/desktop/wallpapers/Birch-Lake.png \
	--file /usr/share/files-tests/make-home.sh=plan/tools/files/make-home.sh
