#!/bin/sh
# ws035-p001: userland baseline -- every generated prerequisite of rootfs-bin.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: sh plan/ws035/tests/baseline-userland.sh <name> <config.mk>
# rootfs-bin itself also names build/llvm/share/licenses/llvm/LICENSE.TXT, a
# fixed path outside the private build directory; creating it would plant an
# unmanaged build/llvm tree in this checkout.  The baseline therefore builds
# the generated prerequisites of rootfs-bin (programs, libraries, data under
# BUILD) and not the rootfs assembly.  Use a configuration without firmware
# programs (their rules download from git.kernel.org into build/sources).
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
name=$1; config=$2
cd "$repo"
targets=$(DRY=1 sh plan/ws035/tests/baseline-build.sh "$name" "$config" -p rootfs-bin 2>/dev/null |
	sed -n 's/^rootfs-bin: *//p' | head -1 | tr ' ' '\n' |
	grep "^build/ws035-p001/$name/" || true)
test -n "$targets"
printf '%s\n' "$targets" > "plan/ws035/phase001/baseline/$name.targets"
# shellcheck disable=SC2086
exec sh plan/ws035/tests/baseline-build.sh "$name" "$config" $targets
