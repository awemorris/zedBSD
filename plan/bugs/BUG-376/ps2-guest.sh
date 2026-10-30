#!/bin/sh
# BUG-376: the PS/2 auxiliary port on QEMU, with its mouse (q35's i8042 has one) and without the controller
# (-machine i8042=off: nothing answers on its ports).  For each: boots the image, waits for SSH, lists the input nodes
# with plan's evdev probe (evdev-probe -l opens each node), times 20 such lists from the host, and stops the guest.
# The judgement is what the probe prints over SSH and the host's clock; the QEMU console is not read.
#   sh plan/bugs/BUG-376/ps2-guest.sh IMGBUILD OUT      (from the top of the tree; OUT a new directory under build/)
# IMGBUILD: AAT_CONFIG=plan/bugs/BUG-376/config-amd64-ps2.mk plan/tools/aat/build-image.sh IMGBUILD.
# GUEST_RUNTIME (default build/bug376-run) keeps the guest apart from others.
# Lines "bug376: MODE CHECK pass|fail DETAIL", then "bug376: PASS" or "bug376: FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
img=${1:?usage: ps2-guest.sh IMGBUILD OUT}
out=${2:?usage: ps2-guest.sh IMGBUILD OUT}
top=$(pwd)
mkdir -p "$out"
GUEST_RUNTIME=${GUEST_RUNTIME:-$top/build/bug376-run}
export GUEST_RUNTIME
failed=0

note() {
	echo "bug376: $*" | tee -a "$out/summary.txt"
}

check() {
	# check MODE NAME STATUS DETAIL
	if [ "$3" -eq 0 ]; then
		note "$1 $2 pass $4"
	else
		note "$1 $2 fail $4"
		failed=1
	fi
}

one() {
	mode=$1
	extra=$2
	dir=$out/$mode
	mkdir -p "$dir"
	timeout 120 python3 -I plan/tools/guest/guest.py start "$img/hdd-image.img" --symbols "$img/vmunix" \
		${extra:+--qemu-extra "$extra"} > "$dir/start.log" 2>&1
	check "$mode" start $? "$dir/start.log"
	timeout 400 python3 -I plan/tools/guest/guest.py wait --timeout 360 > "$dir/wait.log" 2>&1
	status=$?
	check "$mode" ssh $status "$dir/wait.log"
	if [ $status -eq 0 ]; then
		# The nodes and their places.
		timeout 60 python3 -I plan/tools/guest/guest.py run '/bin/evdev-probe -l -t 0' > "$dir/list.txt" 2>&1
		aux=$(grep -c 'phys=pcat/i8042/aux0' "$dir/list.txt")
		kbd=$(grep -c 'phys=pcat/i8042/kbd0' "$dir/list.txt")
		if [ "$mode" = mouse ]; then
			[ "$aux" -eq 1 ]
			check "$mode" aux-node $? "aux=$aux"
		else
			[ "$aux" -eq 0 ]
			check "$mode" no-aux-node $? "aux=$aux"
		fi
		[ "$kbd" -eq 1 ] || [ "$mode" = nomouse ]
		check "$mode" keyboard-node $? "kbd=$kbd"

		# Twenty lists, each opening every node, timed from the host (one SSH command).
		start=$(date +%s%N)
		timeout 120 python3 -I plan/tools/guest/guest.py run \
			'i=0; while [ $i -lt 20 ]; do /bin/evdev-probe -l -t 0 > /dev/null; i=$((i + 1)); done' > "$dir/loop.txt" 2>&1
		end=$(date +%s%N)
		note "$mode lists20 ms=$(( (end - start) / 1000000 ))"
		start=$(date +%s%N)
		timeout 60 python3 -I plan/tools/guest/guest.py run 'true' > /dev/null 2>&1
		end=$(date +%s%N)
		note "$mode ssh-true ms=$(( (end - start) / 1000000 ))"
	fi
	timeout 120 python3 -I plan/tools/guest/guest.py stop > "$dir/stop.log" 2>&1
}

one mouse ""
one nomouse "-machine i8042=off"

if [ $failed -eq 0 ]; then
	note PASS
else
	note FAIL
fi
exit $failed
