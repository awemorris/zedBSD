#!/bin/sh
# ws035-p094〜: builds the lean zdesktop guest image with the graphical login's test greeter
# (plan/ws035/tests/config-amd64-login.mk), with the guest harness's files, the wallpaper and the sample home maker.
# ws136-p001 (2026-10-04): the image is a config.mk build plus files of the tree (plan/tools/guest/test-image.sh).
# The fonts come with the compositor's package (userland/desktop/fonts/); the wallpaper is
# userland/desktop/wallpapers/Birch-Lake.png.
#
#   plan/ws035/tests/build-login-image.sh [BUILD] [graphical] [TARGET...]     (default build/amd64, disk)
# With "graphical" the image boots graphically (config-amd64-graphical.mk: logo, kmsg=quiet, login=graphical);
# "graphical-network" adds the networkd stand-in (config-amd64-graphical-network.mk, ws035-p104).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
build=${1:-build/amd64}
variant=${2:-}
[ $# -ge 2 ] && shift 2 || shift $#
config=plan/ws035/tests/config-amd64-login.mk
[ "$variant" = graphical ] && config=plan/ws035/tests/config-amd64-graphical.mk
[ "$variant" = graphical-network ] && config=plan/ws035/tests/config-amd64-graphical-network.mk
exec plan/tools/guest/test-image.sh "$config" "$build" \
	--file /usr/share/keiland/wallpaper.png=userland/desktop/wallpapers/Birch-Lake.png \
	--file /usr/share/files-tests/make-home.sh=plan/tools/files/make-home.sh \
	"$@"
