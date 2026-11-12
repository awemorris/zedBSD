#!/bin/sh
# The Raspberry Pi 5's fan as the thermal core's fan, receiver and boost, on the board, over SSH:
# run from the top of the tree.
#   PI5=ADDRESS sh plan/ws333/tests/pi5/pi5-thermal.sh check     what the board has, no change
#   PI5=ADDRESS KERNEL=build/rpi5/vmunix sh plan/ws333/tests/pi5/pi5-thermal.sh deploy
#       keeps /boot/boot0/vmunix once as vmunix.p020old, puts KERNEL in its place, reboots, and
#       checks the running kernel's file is KERNEL's
#   PI5=ADDRESS THERMAL=build/rpi5/sbin/thermal sh plan/ws333/tests/pi5/pi5-thermal.sh run
#       copies THERMAL to /tmp/p020-thermal and runs it (as root through sudo): the status, the
#       sensors, the fans and the receivers; each strategy and the trips it chose; the boost at
#       3, 4 and 0 with the fan read after it settles; then puts /etc/thermal.conf back as it was
#   PI5=ADDRESS sh plan/ws333/tests/pi5/pi5-thermal.sh restore
#       vmunix.p020old back as vmunix, a reboot, and the cksum compared with the one check saw
# Everything the board said goes to OUT (default build/tmp/pi5-thermal/<step>.txt); the last
# lines are the summary.  The board's user is user/user (sudo).  No step power-cycles the board:
# one that does not come back says so and stops.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
step=${1:?usage: pi5-thermal.sh check|deploy|run|restore}
: "${PI5:?PI5=the board's address}"
PW=${PI5_PASSWORD:-user}
OUT=${OUT:-build/tmp/pi5-thermal}
SO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o PubkeyAuthentication=no"
mkdir -p "$OUT"
log=$OUT/$step.txt
: > "$log"

b() {
	timeout "${BT:-60}" sshpass -p "$PW" ssh $SO "user@$PI5" "$@"
}

sb() {
	b "echo $PW | sudo -S sh -c '$1' 2> /dev/null"
}

put() {
	timeout 300 sshpass -p "$PW" scp $SO -q "$1" "user@$PI5:$2"
}

say() {
	echo "$*" | tee -a "$log"
}

# Runs one command on the board as root and keeps what it said under a header.
run() {
	echo "### $1" >> "$log"
	sb "$1" >> "$log" 2>&1
	echo "rc=$?" >> "$log"
}

wait_down_up() {
	n=0
	while [ $n -lt 24 ]; do
		sleep 5
		b 'uname -s' > /dev/null 2>&1 || break
		n=$((n + 1))
	done
	n=0
	while [ $n -lt 60 ]; do
		sleep 5
		if b 'uname -s' 2> /dev/null | grep -q zedBSD; then
			return 0
		fi
		n=$((n + 1))
	done
	return 1
}

reboot_board() {
	sb 'sync; (sleep 1; /sbin/reboot) > /dev/null 2>&1 &' > /dev/null 2>&1
	wait_down_up || { say "FAIL the board did not come back (no remote power: ask Q1)"; exit 1; }
	say "up $(b 'uname -v')"
}

case $(b 'uname -m' 2>&1) in
arm64|aarch64) ;;
*) say "FAIL $PI5 is not the arm64 zedBSD board"; exit 1 ;;
esac
case $(b 'uname -s' 2>&1) in
zedBSD*) ;;
*) say "FAIL $PI5 is not zedBSD"; exit 1 ;;
esac

case $step in
check)
	say "uname $(b 'uname -a')"
	say "vmunix $(b 'cksum < /boot/boot0/vmunix')"
	say "kept $(b 'ls /boot/boot0/ | grep p020old')"
	say "thermal.conf $(b 'cat /etc/thermal.conf 2>&1')"
	say "node $(b 'ls -l /dev/thermal 2>&1')"
	say "groups $(b 'id')"
	say "sbin $(b 'ls -l /sbin/thermal 2>&1')"
	;;
deploy)
	: "${KERNEL:?KERNEL=the kernel}"
	want=$(cksum < "$KERNEL" | cut -d' ' -f1)
	put "$KERNEL" /tmp/p020-vmunix || { say "FAIL copy"; exit 1; }
	sb 'test -f /boot/boot0/vmunix.p020old || cp /boot/boot0/vmunix /boot/boot0/vmunix.p020old'
	sb 'cp /tmp/p020-vmunix /boot/boot0/vmunix && sync'
	have=$(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1)
	say "kept $(b 'cksum < /boot/boot0/vmunix.p020old')"
	[ "$have" = "$want" ] || { say "FAIL the kernel on the board is $have, not $want"; exit 1; }
	reboot_board
	have=$(b 'cksum < /boot/boot0/vmunix' | cut -d' ' -f1)
	say "DEPLOYED vmunix=$have (want $want)"
	;;
run)
	: "${THERMAL:?THERMAL=the thermal program}"
	put "$THERMAL" /tmp/p020-thermal || { say "FAIL copy"; exit 1; }
	b 'chmod 755 /tmp/p020-thermal'
	T=/tmp/p020-thermal
	sb 'if test -f /etc/thermal.conf; then cp -p /etc/thermal.conf /tmp/p020-thermal.conf.orig; else : > /tmp/p020-thermal.conf.none; fi'
	run "uname -v"
	run "$T"
	run "$T sensors"
	run "$T fans"
	run "$T receivers"
	for s in balanced "manual 50 100 50" performance "manual 50 66 50" "manual 50 67 50" powersave; do
		run "$T strategy $s"
		run "$T"
		run "$T fans"
	done
	for level in 3 4 0; do
		run "$T boost all $level"
		sleep 12
		run "$T fans"
		run "$T sensors"
	done
	sb 'if test -f /tmp/p020-thermal.conf.orig; then cp -p /tmp/p020-thermal.conf.orig /etc/thermal.conf; fi'
	run "cat /etc/thermal.conf"
	run "$T restore"
	run "$T"
	echo "### summary" >> "$log"
	grep -E '^(### |firmware |fan |sensor |receiver |strategy )' "$log" | tee "$OUT/run-summary.txt"
	;;
restore)
	sb 'test -f /boot/boot0/vmunix.p020old && cp /boot/boot0/vmunix.p020old /boot/boot0/vmunix && sync'
	reboot_board
	say "RESTORED vmunix $(b 'cksum < /boot/boot0/vmunix')"
	;;
*)
	echo "usage: pi5-thermal.sh check|deploy|run|restore" >&2
	exit 2
	;;
esac
