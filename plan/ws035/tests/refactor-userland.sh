#!/bin/sh
# ws035 refactor Phases: the userland check of ws035-p001 (baseline-userland.sh)
# in the Phase's private build directory.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: sh plan/ws035/tests/refactor-userland.sh <phase> <name> <config.mk>
# Builds every generated prerequisite of rootfs-bin (programs, libraries, data
# under BUILD), as baseline-userland.sh did for the p001 baseline, with
# refactor-build.sh (build/ws035-<phase>/<name>/, log in the Phase directory).
# The target list, relative to BUILD, goes to
# plan/ws035/phase<NNN>/build/<name>.targets.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
phase=$1; name=$2; config=$3
cd "$repo"
logdir=plan/ws035/phase${phase#p}/build
mkdir -p "$logdir"
targets=$(DRY=1 sh plan/ws035/tests/refactor-build.sh "$phase" "$name" "$config" -p rootfs-bin 2>/dev/null |
	sed -n 's/^rootfs-bin: *//p' | head -1 | tr ' ' '\n' |
	grep "^build/ws035-$phase/$name/" || true)
test -n "$targets"
printf '%s\n' "$targets" | sed "s|^build/ws035-$phase/$name/||" > "$logdir/$name.targets"
# shellcheck disable=SC2086
exec sh plan/ws035/tests/refactor-build.sh "$phase" "$name" "$config" $targets
