#!/bin/sh
# BUG-423's image: the x86_64 kernel with a text login and a small userland, built with the ticket's config.
#   plan/bugs/BUG-423/build.sh OUTPUT      (from the repository's top; the tree's sysroot must be built)
# OUTPUT is a new directory under build/; it gets vmunix and hdd-image.img.  Every make has 4 jobs.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
output=${1:?output dir}
cd "$repo" || exit 1
mkdir -p "$output"
output=$(cd "$output" && pwd)
config="CONFIGMK=plan/bugs/BUG-423/config.mk BUILD_JOBS=4 CONFIG_GRAPHICAL_LOGIN=n"
programs="cat chown echo login ls mkdir sh sysctl uname dmesg grep"

# shellcheck disable=SC2086
timeout 1800 make -j4 $config build/x86_64/vmunix >"$output/vmunix.log" 2>&1 </dev/null
echo "vmunix rc=$? warnings=$(grep -c 'warning:' "$output/vmunix.log")"
# shellcheck disable=SC2086
timeout 1800 make -j4 $config CONFIG_USERLAND_PROGRAMS="$programs" programs >"$output/programs.log" 2>&1 </dev/null
echo "programs rc=$? warnings=$(grep -c 'warning:' "$output/programs.log")"
# shellcheck disable=SC2086
flock /tmp/zedbsd-image-build.lock timeout 1800 make -j4 $config CONFIG_USERLAND_PROGRAMS="$programs" \
	build/x86_64/hdd-image.img >"$output/image.log" 2>&1 </dev/null
rc=$?
echo "image rc=$rc warnings=$(grep -c 'warning:' "$output/image.log")"
[ "$rc" = 0 ] || exit 1
cp build/x86_64/hdd-image.img "$output/hdd-image.img"
cp build/x86_64/vmunix "$output/vmunix"
sync
