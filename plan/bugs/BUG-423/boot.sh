#!/bin/sh
# BUG-423's check: boots OUTPUT/hdd-image.img (plan/bugs/BUG-423/build.sh) on q35 with qemu-xhci and usb-kbd
# through plan/tools/boot-test.sh with SMP CPUs, waits for the login prompt, logs in as root on the emulated
# USB keyboard and runs uname -a.  The screen is read; no console or serial log is.
#   plan/bugs/BUG-423/boot.sh OUTPUT SMP [NAME]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
output=${1:?output dir}
smp=${2:?cpus}
name=${3:-boot-smp$smp}
cd "$repo" || exit 1
output=$(cd "$output" && pwd)
then='keys:root
expect:Password
keys:root
expect:root@[^ ]*:~[$#]
keys:uname -a
expect:^zedBSD [^ ]+ .* x86_64'
BUG423_SMP=$smp QEMU="$repo/plan/bugs/BUG-423/qemu-wrap.sh" BOOT_THEN=$then OUTPUT="$output/$name" \
	BOOT_TIMEOUT=300 BOOT_TEST_WORK="$output/work" \
	timeout 900 plan/tools/boot-test.sh "$output/hdd-image.img" >"$output/$name.log" 2>&1
rc=$?
sync
echo "boot-test smp=$smp rc=$rc ($(tail -n 1 "$output/$name.log"))"
exit "$rc"
