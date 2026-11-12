#!/bin/bash
# The QEMU test of the Intel TCO watchdog on q35's ICH9 (KVM when the host has it).
#   plan/ws333/tests/tco-qemu/run.sh IMAGE OUT
# IMAGE is a raw x86_64 disk image of this tree (with intel-tco.c).  Three boots, each from a copy of the image whose
# boot FAT's zedbsd.cfg gets the boot parameters of the case (mtools), watched by tco-watch.py through QMP:
#   off    no parameter: for 100 s no WATCHDOG event; the counter is never loaded (count 0) and its reset stays
#          forbidden (RCBA+3410h bit 5 set)
#   alive  watchdog=8: for 150 s no WATCHDOG event, the counter runs (control bit 11 clear), the reset is allowed
#          (RCBA+3410h bit 5 clear), and the count read every 10 s never falls to 0
#   stop   watchdog=8 watchdog.stop=40: a WATCHDOG event comes, at least 40 s after the start (the driver arms the
#          counter during the boot) and within 150 s
# The summary is OUT/summary.txt, the last line "tco-qemu: PASS" or "tco-qemu: FAIL (...)".  No console or serial
# log is read.  The copies stay under OUT; nothing is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
image=${1:?usage: run.sh IMAGE OUT}
out=${2:?usage: run.sh IMAGE OUT}
mkdir -p "$out"
summary=$out/summary.txt
: >"$summary"
status=0
. "$root/plan/tools/guest/qemu-accel.sh"
read -r -a accel <<<"$(qemu_accel_args max)"

say()
{
	echo "tco-qemu: $*" | tee -a "$summary"
}

# The byte offset of the image's boot FAT (its second partition).
fat=$(/usr/sbin/sfdisk -J "$image" | python3 -I -c 'import json,sys; p=json.load(sys.stdin)["partitiontable"]["partitions"]; print(p[1]["start"] * 512)')
[ -n "$fat" ] || { say "FAIL (no boot FAT)"; exit 1; }

# Boots one case: a copy of the image with LINES added to zedbsd.cfg, watched for SECONDS.
boot()
{
	local name=$1 lines=$2 seconds=$3
	local copy=$out/$name.img
	cp --sparse=always "$image" "$copy" || return 1
	mtype -i "$copy@@$fat" ::zedbsd.cfg >"$out/$name.cfg" || return 1
	for line in $lines; do
		echo "$line" >>"$out/$name.cfg"
	done
	mcopy -o -i "$copy@@$fat" "$out/$name.cfg" ::zedbsd.cfg || return 1
	cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/$name-vars.fd"
	timeout $((seconds + 60)) qemu-system-x86_64 -machine q35 -m 4G -smp 4 "${accel[@]}" \
		-drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
		-drive "if=pflash,format=raw,file=$out/$name-vars.fd" \
		-device qemu-xhci,id=xhci \
		-drive "if=none,id=boot,file=$copy,format=raw" \
		-device usb-storage,bus=xhci.0,port=1,drive=boot,bootindex=1 \
		-vga std -display none -no-reboot \
		-qmp "unix:$out/$name.sock,server,nowait" >"$out/$name-qemu.out" 2>&1 &
	local qemu=$!
	timeout $((seconds + 30)) python3 -I "$here/tco-watch.py" "$out/$name.sock" "$seconds" 10 >"$out/$name.watch"
	python3 -I "$root/plan/tools/qmp.py" "$out/$name.sock" quit >/dev/null 2>&1
	wait "$qemu" 2>/dev/null
	sync
	return 0
}

say "accel ${accel[*]}"

# off: nothing arms the counter.
boot off "" 100 || { say "FAIL (off: copy)"; exit 1; }
end=$(grep '^end' "$out/off.watch")
armed=$(awk '$1 == "probe" { split($4, n, "="); split($6, g, "="); seen++;
	if (n[2] + 0 != 0) bad++; if (and(strtonum(g[2]), 0x20) == 0) bad++ } END { print seen + 0, bad + 0 }' "$out/off.watch")
say "off: $end, probes and those with a count or the reset allowed: $armed"
case $end in *timeout) ;; *) status=1 ;; esac
case $armed in "0 "* | *" "[1-9]*) status=1 ;; esac

# alive: armed and reloaded.
boot alive "watchdog=8" 150 || { say "FAIL (alive: copy)"; exit 1; }
end=$(grep '^end' "$out/alive.watch")
running=$(awk '$1 == "probe" && $2 > 60 { split($5, c, "="); split($6, g, "="); split($4, n, "=");
	if (and(strtonum(c[2]), 0x800) != 0) bad++; if (and(strtonum(g[2]), 0x20) != 0) bad++; if (n[2] + 0 == 0) bad++; seen++ }
	END { print seen + 0, bad + 0 }' "$out/alive.watch")
say "alive: $end, probes after 60 s and their faults: $running"
case $end in *timeout) ;; *) status=1 ;; esac
case $running in "0 "* | *" "[1-9]*) status=1 ;; esac

# stop: every CPU stops 40 s after the arming, and the watchdog resets the guest.
boot stop "watchdog=8 watchdog.stop=40" 150 || { say "FAIL (stop: copy)"; exit 1; }
end=$(grep '^end' "$out/stop.watch")
event=$(grep -m1 '^event .* WATCHDOG' "$out/stop.watch")
say "stop: $end; $event"
at=$(echo "$event" | awk '{ print int($2) }')
if [ -z "$event" ] || [ "${at:-0}" -lt 40 ]; then
	status=1
fi

if [ "$status" = 0 ]; then
	say "PASS"
else
	say "FAIL (see $summary)"
fi
exit "$status"
