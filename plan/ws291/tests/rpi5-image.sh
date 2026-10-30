#!/bin/sh
# ws291-p021: builds the Raspberry Pi 5 measurement image (plan/ws291/tests/config-rpi5.mk) into BUILD/hdd-image.img and
# prints its size, sha256 and the tree's commit.  The firmware licenses must have been accepted already (the shared
# cache); the question is never answered here (make reads no input).  Image builds take the host's image lock.
#   sh plan/ws291/tests/rpi5-image.sh BUILD
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
build=${1:?usage: rpi5-image.sh BUILD}
for key in id_ed25519.pub ssh_host_rsa_key ssh_host_ecdsa_key ssh_host_ed25519_key; do
	[ -s "plan/tmp/guest/$key" ] || { echo "rpi5-image: no plan/tmp/guest/$key (python3 plan/tools/guest/guest.py keys)" >&2; exit 1; }
done
flock /tmp/zedbsd-image-build.lock timeout 7200 make -j"${ZEDBSD_JOBS:-16}" ZEDBSD_CONFIG=plan/ws291/tests/config-rpi5.mk \
    BUILD="$build" disk-image < /dev/null
img=$build/hdd-image.img
echo "rpi5-image: $(readlink -f "$img") $(stat -c %s "$img") bytes sha256 $(sha256sum "$img" | cut -d' ' -f1) commit $(git rev-parse --short HEAD)$(git diff --quiet HEAD || echo '+changes')"
