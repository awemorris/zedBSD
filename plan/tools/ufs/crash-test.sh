#!/bin/bash
# ws054: stops the guest in the middle of a journaled directory's growth,
# restarts it, and checks what the journal's replay left.
#   plan/tools/ufs/crash-test.sh IMAGE SECONDS...
# For each SECONDS: a fresh journaled volume, names created in one directory
# for that long, the machine cut off, restarted, the volume mounted (the
# journal replays) and counted, unmounted, and checked on the host.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
image=$1
shift
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root" || exit 1
export GUEST_RUNTIME=build/ws054-crash-run
volume=build/ws054-crash.img
guest=plan/tools/guest/guest.py
empty=$(mktemp -d)
drive="-drive if=none,id=jour,file=$root/$volume,format=raw -device nvme,serial=ws054crash,drive=jour"

for seconds in "$@"; do
	echo "== cut off after $seconds s"
	rm -f "$volume"
	build/zedimage-host ufs $((256 * 1024 * 1024)) "$empty" "$root/$volume" \
		--profile=journal-snapshot --inodes=8192 || exit 1

	# Creates names until the machine is cut off.
	python3 $guest stop > /dev/null 2>&1
	python3 $guest start --qemu-extra "$drive" "$image" > /dev/null || exit 1
	python3 $guest wait > /dev/null || { echo "no SSH"; continue; }
	python3 $guest put plan/tools/ufs/crash-grow.sh /root/crash-grow.sh
	python3 $guest run 'mkdir -p /jour && mount -t ufs /dev/nvme0n1 /jour &&
		(nohup sh /root/crash-grow.sh /jour run > /root/made.txt 2>&1 < /dev/null &)'
	sleep "$seconds"
	python3 $guest stop > /dev/null

	# Restarts, replays the journal by mounting, and counts the names.
	python3 $guest start --qemu-extra "$drive" "$image" > /dev/null || exit 1
	python3 $guest wait > /dev/null || { echo "no SSH after restart"; continue; }
	python3 $guest put plan/tools/ufs/crash-grow.sh /root/crash-grow.sh
	python3 $guest run 'mkdir -p /jour && mount -t ufs /dev/nvme0n1 /jour &&
		sh /root/crash-grow.sh /jour check; ls -ld /jour/c; umount /jour; sync'
	python3 $guest stop > /dev/null

	# Checks the volume the replay and the unmount left.
	python3 plan/tools/ufs/check-volume.py "$volume"
done
rm -rf "$empty"
