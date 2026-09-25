#!/bin/sh
# ws035-p001: the pre-refactor baseline over every platform, one build at a time.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: sh plan/ws035/tests/baseline-all.sh <target> [name-suffix]
#   target  vmunix (kernel) or rootfs-bin (/bin from userland/base, comp, X11)
# Each build has its own BUILD directory and log (baseline-build.sh); a failed
# platform is recorded as failed and the next one still runs.
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
target=$1
suffix=${2:-}
cd "$repo"
summary=plan/ws035/phase001/baseline/summary-$target$suffix.txt
: > "$summary"
while read -r name config; do
	sh plan/ws035/tests/baseline-build.sh "$name$suffix" "$config" "$target" \
		| tail -1 >> "$summary"
done <<EOF
amd64 config/ci/config-amd64.mk
pcat config/ci/config-pcat.mk
pc98 config/ci/config-pc98.mk
rpi4 config/ci/config-rpi4.mk
sun4u plan/ws035/tests/config-sun4u.mk
x68k plan/ws035/tests/config-x68k.mk
i915-amd64 plan/ws029/tests/config-i915-amd64.mk
EOF
cat "$summary"
