#!/bin/sh
# The boot check of the balancer's statistics on x86_64 QEMU: an image of the text login
# (plan/tools/boot-images/config-amd64-boot.mk) with the kernel without the balancer (n) and with it (y,
# CONFIG_KERN_DESKTOP_BALANCER=y), each booted by plan/tools/boot-test.sh, logged in as root on the
# emulated keyboard (input events and system calls on the new paths) and asked for uname.
#
#   plan/ws302/tests/p002-boot.sh OUT [VARIANT...]
#
# VARIANT is n or y (default both).  The images are made in build/x86_64, whose sysroot must already be made.  Each
# line of the summary is "<variant> image rc=<n>" and "<variant> boot rc=<n>"; the last line is "p002-boot: PASS"
# when every image was made and every boot reached the shell and showed uname's answer.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
[ $# -ge 1 ] || { echo "usage: p002-boot.sh OUT [VARIANT...]" >&2; exit 2; }
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
variants=${*:-n y}
summary=$out/summary.txt
: > "$summary"
status=PASS
config=plan/tools/boot-images/config-amd64-boot.mk

# Makes and boots one image per variant.
for variant in $variants; do
	extra=
	if [ "$variant" = y ]; then
		extra="CONFIG_KERN_DESKTOP_BALANCER=y"
	fi

	# Makes the image; the kernel is rebuilt when the variant's flags differ from the last build's.
	# shellcheck disable=SC2086
	timeout 5400 make -j4 CONFIGMK=$config $extra disk > "$out/image-$variant.log" 2>&1 < /dev/null
	rc=$?
	echo "$variant image rc=$rc warnings=$(grep -c 'warning:' "$out/image-$variant.log")" >> "$summary"
	if [ $rc -ne 0 ]; then
		status=FAIL
		continue
	fi
	cp build/x86_64/hdd-image.img "$out/hdd-$variant.img"
	cp build/x86_64/vmunix "$out/vmunix-$variant"
	sync

	# Boots it, logs in and asks for the system's name.
	OUTPUT=$out/boot-$variant BOOT_TIMEOUT=300 \
	BOOT_THEN="$(printf 'keys:root\nexpect:Password\nkeys:root\nexpect:root@\nkeys:uname -s\nexpect:zedBSD')" \
		timeout 900 plan/tools/boot-test.sh "$out/hdd-$variant.img" > "$out/boot-$variant.log" 2>&1 < /dev/null
	rc=$?
	echo "$variant boot rc=$rc" >> "$summary"
	if [ $rc -ne 0 ]; then
		status=FAIL
	fi
	sync
done

# Reports.
cat "$summary"
echo "p002-boot: $status"
