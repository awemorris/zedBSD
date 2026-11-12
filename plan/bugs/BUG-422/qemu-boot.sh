#!/bin/sh
# BUG-422's QEMU check: builds the x86_64 kernel with the thermal core and the x86 power control (the CPU performance
# framework and its measuring threads), a text login and a small userland, boots it on q35 with 4 CPUs through
# plan/tools/boot-test.sh to the login prompt (LOGIN=1: then logs in and shows the kernel's cpuperf lines).  The
# screen is read; no console or serial log is.  Prints one line per step and the boot test's verdict.
#   plan/bugs/BUG-422/qemu-boot.sh OUTPUT        (from the repository's top; the tree's sysroot must be built)
# OUTPUT is a new directory under build/.  Every make has 4 jobs; nothing is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
output=${1:?output dir}
cd "$repo" || exit 1
mkdir -p "$output"
output=$(cd "$output" && pwd)
config="PLATFORM=x86_64 CONFIGMK=config/config.x86_64.mk BUILD_JOBS=4 CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y CONFIG_GRAPHICAL_LOGIN=n"
programs="cat chown echo login ls mkdir sh sysctl uname dmesg grep"

# The kernel, the userland the image carries and the image, one image build at a time across the agents.
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

# The boot to the login prompt on 4 CPUs.  LOGIN=1 then logs in as root on the emulated keyboard and shows the
# kernel's cpuperf lines; it needs a guest that enumerates QEMU's USB keyboard.
then=
if [ "${LOGIN:-0}" = 1 ]; then
	then='keys:root
expect:Password
keys:root
expect:root@[^ ]*:~[$#]
keys:dmesg | grep -c "not measured"; dmesg | grep cpuperf | tail -n 12
expect:root@[^ ]*:~[$#] *$'
fi
BOOT_THEN=$then OUTPUT="$output/boot" BOOT_TIMEOUT=300 BOOT_TEST_WORK="$output/work" \
	timeout 900 plan/tools/boot-test.sh "$output/hdd-image.img" >"$output/boot-test.log" 2>&1
rc=$?
sync
echo "boot-test rc=$rc ($(tail -n 1 "$output/boot-test.log"))"
exit "$rc"
