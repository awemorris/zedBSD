#!/bin/sh
# Boots one kernel on the VisionFive 2 board COUNT times through the SwitchBot plug and keeps, for each boot, how long
# SSH took to come back, the kernel's lines of interest (SMP, the SD card's errors and clock, the root's mount, the
# PowerVR firmware's file) and the whole dmesg; a boot that does not come back within LIMIT seconds is noted and the
# board cycled again.  The live serial line is kept beside each boot (OUT/bootN/serial.txt) as a clue for the boots
# that did not come back; the verdict comes from SSH and dmesg.  Run from the top of the tree while the board is
# assigned:
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=14C19FA80F86 [GPU=1] [LOAD='command'] [KEEP=1] [LIMIT=180]
#       [SERIAL=/dev/ttyUSB0]
#       [ORIG=vmunix.q1772orig] sh plan/tools/vf2/boot-cycle.sh KERNEL OUT COUNT
# The board's kernel is first kept as /boot/boot0/ORIG (when not kept already) and read back to OUT/board-vmunix.
# KERNEL is written beside it as vmunix.new, read back and compared, then renamed over vmunix.  GPU=1 runs the board's
# gpu-selftest and vkcompute after the first boot and keeps the kernel lines they add.  LOAD is a command run as root
# after every boot (OUT/bootN/load.txt), the dmesg after it kept (OUT/bootN/load-dmesg.txt).  Unless KEEP=1 the board's own
# kernel is put back the same way at the end, read back and compared, and the board power-cycled onto it.  The
# password comes from the environment, never from this file.  OUT/summary.txt holds what is printed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
kernel=${1:?usage: boot-cycle.sh KERNEL OUT COUNT}
out=${2:?the output directory}
count=${3:?the number of boots}
: "${VF2_HOST:?VF2_HOST=user@host}"
: "${VF2_PASSWORD:?VF2_PASSWORD}"
: "${VF2_PLUG:?VF2_PLUG (the board's SwitchBot plug)}"
limit=${LIMIT:-180}
serial=${SERIAL:-/dev/ttyUSB0}
orig=/boot/boot0/${ORIG:-vmunix.q1772orig}
export SSHPASS="$VF2_PASSWORD"
SSHOPT="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5"
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"

say() {
	echo "$*" | tee -a "$summary"
}

b() {
	timeout "${BT:-60}" sshpass -e ssh $SSHOPT "$VF2_HOST" "$@"
}

bsudo() {
	printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c '$1'" 2> /dev/null
}

bcp() {
	timeout "${BT:-300}" sshpass -e scp $SSHOPT "$@"
}

# Puts FILE in as the board's kernel: written as vmunix.new, read back and compared, renamed over vmunix.
put_kernel() {
	bcp "$1" "$VF2_HOST:vmunix.new" || return 1
	bsudo "cp vmunix.new /boot/boot0/vmunix.new && sync" || return 1
	bcp "$VF2_HOST:/boot/boot0/vmunix.new" "$out/readback" || return 1
	cmp -s "$1" "$out/readback" || { say "FAIL vmunix.new does not read back as $1"; return 1; }
	bsudo "mv /boot/boot0/vmunix.new /boot/boot0/vmunix && sync" || return 1
	bcp "$VF2_HOST:/boot/boot0/vmunix" "$out/readback" || return 1
	cmp -s "$1" "$out/readback" || { say "FAIL vmunix does not read back as $1"; return 1; }
	return 0
}

# The newest of the board's boot logs (bootlog writes one each boot), or "none" while the board does not answer.
newest_log() {
	b 'ls /boot/boot0/log | tail -1' 2> /dev/null || echo none
}

# Power-cycles the board and waits for SSH from a new boot (a newer boot log than before); prints the seconds it
# took, "none" when SSH did not come back, or "same" when the board answered from the boot before the cycle (the
# plug did not switch).  The plug's command is tried up to three times.
cycle() {
	before=$(newest_log)
	b sync > /dev/null 2>&1
	for try in 1 2 3; do
		start=$(date +%s)
		python3 -I plan/tools/switchbot.py cycle "$VF2_PLUG" 10 > /dev/null 2>&1 && break
		sleep 10
	done
	while [ $(($(date +%s) - start)) -lt "$limit" ]; do
		sleep 3
		if b true > /dev/null 2>&1; then
			sleep 5
			if [ "$before" != none ] && [ "$(newest_log)" = "$before" ]; then
				echo same
				return 1
			fi
			echo $(($(date +%s) - start))
			return 0
		fi
	done
	echo none
	return 1
}

serial_start() {
	stty -F "$serial" 115200 raw -echo 2> /dev/null
	timeout $((limit + 30)) cat "$serial" > "$1" 2> /dev/null &
	serial_pid=$!
}

serial_stop() {
	kill "$serial_pid" 2> /dev/null
	wait "$serial_pid" 2> /dev/null
}

say "start $(date '+%F %H:%M')"
say "board $(b 'uname -sm; cksum /boot/boot0/vmunix' 2>&1 | tr '\n' ' ')"
bsudo "test -f $orig || cp -p /boot/boot0/vmunix $orig; sync"
bcp "$VF2_HOST:$orig" "$out/board-vmunix" || { say "FAIL the board's kernel was not read back"; exit 1; }
say "kept $(b "cksum $orig" 2>&1)"
put_kernel "$kernel" || { say "FAIL the kernel was not put in"; exit 1; }
say "kernel $(cksum < "$kernel" | cut -d' ' -f1) put in"

up=0
down=0
same=0
i=1
while [ "$i" -le "$count" ]; do
	d=$out/boot$i
	mkdir -p "$d"
	serial_start "$d/serial.txt"
	t=$(cycle)
	serial_stop
	if [ "$t" = same ]; then
		same=$((same + 1))
		say "boot$i not counted: the board answered from the boot before the cycle ($same)"
		[ "$same" -ge 3 ] && { say "FAIL the plug did not power-cycle the board"; break; }
		continue
	fi
	if [ "$t" = none ]; then
		down=$((down + 1))
		say "boot$i no ssh within $limit s (serial $(wc -c < "$d/serial.txt") bytes)"
		i=$((i + 1))
		continue
	fi
	up=$((up + 1))
	# The boot's own lines settle for a while after SSH comes up (the firmware's file, the GPU).
	sleep 20
	b dmesg > "$d/dmesg.txt" 2>&1
	b 'f=/boot/boot0/log/$(ls /boot/boot0/log | tail -1); echo $f; cat $f' > "$d/bootlog.txt" 2>&1
	smp=$(grep -m1 'CPUs ready' "$d/dmesg.txt")
	mmc=$(grep -c 'dw-mmc: .*failed' "$d/dmesg.txt")
	slow=$(grep -c 'card clock down' "$d/dmesg.txt")
	fw=$(grep -m1 -E 'powervr: firmware: file (done|failed)' "$d/dmesg.txt")
	vfs=$(grep -m1 'the VFS is initialized' "$d/dmesg.txt")
	say "boot$i ssh ${t}s; $smp; mmc failures $mmc, slowed $slow; $fw; $vfs"
	if [ -n "${LOAD:-}" ]; then
		printf '%s\n' "$VF2_PASSWORD" | BT=900 b "sudo -S sh -c '$LOAD'" > "$d/load.txt" 2>&1
		say "boot$i load exit=$? $(grep -v '^\[sudo\]' "$d/load.txt" | tail -1)"
		b dmesg > "$d/load-dmesg.txt" 2>&1
	fi
	if [ "$i" = 1 ] && [ "${GPU:-0}" = 1 ]; then
		b dmesg > "$d/gpu.before" 2>&1
		printf '%s\n' "$VF2_PASSWORD" | BT=600 b 'sudo -S gpu-selftest' > "$d/gpu-selftest.txt" 2>&1
		say "boot$i gpu-selftest exit=$? $(grep -E '[0-9]+ ok, [0-9]+ fail' "$d/gpu-selftest.txt" | tail -1)"
		printf '%s\n' "$VF2_PASSWORD" | BT=300 b 'sudo -S vkcompute' > "$d/vkcompute.txt" 2>&1
		say "boot$i vkcompute exit=$? $(grep -E 'PASS|FAIL' "$d/vkcompute.txt" | tail -1)"
		b dmesg > "$d/gpu.after" 2>&1
		grep -F -x -v -f "$d/gpu.before" "$d/gpu.after" > "$d/gpu.kernel"
		say "boot$i gpu kernel lines $(wc -l < "$d/gpu.kernel"), faults $(grep -ciE 'fault|lost|hang|reset|fatal' "$d/gpu.kernel")"
		grep -iE 'fault|lost|hang|reset|fatal' "$d/gpu.kernel" | head -5 | sed "s/^/boot$i gpu kernel /" | tee -a "$summary"
	fi
	i=$((i + 1))
done
say "boots $count: ssh $up, none $down"

if [ "${KEEP:-0}" = 1 ]; then
	say "kept KERNEL on the board (KEEP=1); board $(b "cksum /boot/boot0/vmunix $orig" 2>&1 | tr '\n' ' ')"
	say "end $(date '+%F %H:%M')"
	exit 0
fi
# The board's own kernel again; a board that is down is cycled until SSH answers (up to three times).
for try in 1 2 3; do
	b true > /dev/null 2>&1 && break
	say "back: board down, cycling ($try)"
	cycle > /dev/null
done
put_kernel "$out/board-vmunix" || { say "FAIL the board's kernel was not put back"; exit 1; }
t=$(cycle)
say "back ssh ${t}s; board now $(b "cksum /boot/boot0/vmunix" 2>&1 | tr '\n' ' ') expected $(cksum < "$out/board-vmunix" | cut -d' ' -f1)"
say "end $(date '+%F %H:%M')"
