#!/bin/sh
# Builds the x86_64 image the QEMU test of /dev/thermal (run.sh) boots: the
# kernel with the thermal core, the x86 power control and the test
# injection node, a text login, a small userland with /sbin/thermal, its
# boot service, monitor-probe and the tools the test types, and the guest
# tool thermal-inject (built from thermal-inject.c through the package
# rules in OUTPUT/thermal-inject).  The image is copied to
# OUTPUT/hdd-image.img; the logs go to OUTPUT.
#   plan/ws333/tests/thermal-qemu/build-image.sh OUTPUT
# OUTPUT is a new directory under build/.  The tree's sysroot must be built
# (make CONFIGMK=config/config.x86_64.mk sysroot).  Nothing is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
output=${1:?output dir}
cd "$repo"
mkdir -p "$output/thermal-inject"
output=$(cd "$output" && pwd)
config="CONFIGMK=config/config.x86_64.mk CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y CONFIG_THERMAL_TEST_INJECT=y CONFIG_GRAPHICAL_LOGIN=n"
programs="login passwd su useradd account-admin id ls cat grep sed thermal sync service passkey monitor-probe"

# The kernel and the userland the image carries.
timeout 1800 make -j4 BUILD_JOBS=4 $config build/x86_64/vmunix >"$output/vmunix.log" 2>&1 </dev/null
timeout 1800 make -j4 BUILD_JOBS=4 $config CONFIG_USERLAND_PROGRAMS="$programs" programs >"$output/programs.log" 2>&1 </dev/null

# thermal-inject, put in build/x86_64/sbin by the package rules.
cp plan/ws333/tests/thermal-qemu/thermal-inject.c "$output/thermal-inject/main.c"
printf 'PROG := thermal-inject\nSRCS := main.c\nINSTALL_DIR := sbin\ninclude %s/userland/base/package.mk\n' "$repo" >"$output/thermal-inject/Makefile"
timeout 600 make -C "$output/thermal-inject" PLATFORM=x86_64 STANDALONE_CONFIG="$repo/config/config.x86_64.mk" >"$output/thermal-inject.log" 2>&1 </dev/null

# The image, one build at a time across the agents.
flock /tmp/zedbsd-image-build.lock timeout 1800 make -j4 BUILD_JOBS=4 $config CONFIG_USERLAND_PROGRAMS="$programs" build/x86_64/hdd-image.img >"$output/image.log" 2>&1 </dev/null
cp build/x86_64/hdd-image.img "$output/hdd-image.img"
sync
echo "build-image: $output/hdd-image.img warnings=$(cat "$output"/*.log | grep -c 'warning:')"
