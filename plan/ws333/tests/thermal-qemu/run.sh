#!/bin/bash
# The QEMU amd64 test of /dev/thermal and /sbin/thermal: boots an image that
# carries /sbin/thermal and the guest tool thermal-inject (a kernel built with
# the test injection node) on q35 with its PS/2 keyboard, logs in as root on
# the console, and reads every answer from the screen with the reader of
# plan/tools/boot-test.py (no serial or console log is read).
#   plan/ws333/tests/thermal-qemu/run.sh IMAGE OUTPUT [emergency|direct|group|readers]
# Without a mode: the status (balanced, which the boot's "thermal restore"
# gave with nothing kept), a made-up sensor and receiver, the strategy
# chosen, kept in /etc/thermal.conf and restored, manual, the CPUs, the power
# and a refused boost.  "emergency": a made-up zone at its critical level on
# the init path: QEMU must end by itself (checked from the host), and a
# second boot of the same disk must show the status the guest kept once the
# emergency left SIGNALED, "init" (init acknowledged), and init's record
# /var/log/thermal-emergency.  "direct": the test path that skips init; the
# kept status reads "cutting".  The machine may end before the screen shows
# the emergency's line, so that line's screen is not required.
# "group": who may change the strategy.  /dev/thermal and /etc/thermal.conf
# (made by the boot's "thermal restore") are root:desktop 0664; the desktop
# group's member "user" (not root) chooses powersave through su and it is
# kept in the file; "bob", added by useradd without the group, reads the
# strategy but is refused a change (EPERM on the descriptor opened for
# reading) and leaves the file as it was; bob reads a CPU (its rate, which
# any reader gets).
# "readers" (the image also carries grep, sed and monitor-probe): the
# readers of the temperatures.  A made-up sensor that follows
# the processor at 45 degrees shows in "thermal sensors" and in the System
# Monitor's backend (the image's monitor-probe, its sample cut to the valid
# bits and the temperature by sed: the temperature's bit 0x100 and
# cpu_mc=45000); the sysctl name of the old interface is gone (sysctl
# fails on it, "sysctl -a" still succeeds).
# The emulator's files go to a new directory under OUTPUT; nothing is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
image=${1:?image}
output=${2:?output dir}
mode=${3:-get-set}
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
. "$repo/plan/tools/guest/qemu-accel.sh"
mkdir -p "$output"
work=$(mktemp -d "$output/run.XXXXXX")
qemu-img create -q -f qcow2 -F raw -b "$(readlink -f -- "$image")" "$work/disk.qcow2"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$work/vars.fd"

login=(--then-keys=root --then-expect=Password --then-keys=root '--then-expect=root@[^ ]*:~[$#]')
if [ "$mode" = emergency ] || [ "$mode" = direct ]; then
	path=init
	state=init
	if [ "$mode" = direct ]; then
		path=direct
		state=cutting
	fi
	steps=("${login[@]}"
		"--then-keys=thermal-inject path $path; thermal-inject sensor hot zone 0 60000 0 90000"
		'--then-expect=thermal-inject: ok'
		'--then-keys=thermal-inject sensor hot zone 0 91000 0 90000; thermal-inject sensor hot zone 0 92000 0 90000; while :; do thermal > /root/st; case "$(cat /root/st)" in *"emergency none"*|*"emergency signaled"*) ;; *) sync; break;; esac; done')
	record=("${login[@]}"
		'--then-keys=cat /root/st'
		"--then-expect=emergency $state sensor=0 temperature=92.000")
	if [ "$mode" = emergency ]; then
		record=("${record[@]}"
			'--then-keys=cat /var/log/thermal-emergency'
			'--then-expect=thermal emergency at [0-9]+, [0-9]+ ms after init started')
	fi
elif [ "$mode" = group ]; then
	steps=("${login[@]}"
		'--then-keys=thermal-inject receiver test 0 0'
		'--then-expect=thermal-inject: ok'
		'--then-keys=ls -l /dev/thermal /etc/thermal.conf'
		'--then-expect=crw-rw-r-- +[0-9]+ +root +desktop .*/dev/thermal'
		'--then-expect=-rw-rw-r-- +[0-9]+ +root +desktop .*/etc/thermal.conf'
		'--then-keys=id user'
		'--then-expect=45\(desktop\)'
		"--then-keys=su user -c 'thermal strategy powersave'; echo user-status=\$?"
		'--then-expect=strategy powersave generation=[0-9]+ flags='
		'--then-expect=user-status=0'
		'--then-keys=cat /etc/thermal.conf'
		'--then-expect=^strategy powersave'
		'--then-keys=useradd bob; id bob'
		'--then-expect=uid=[0-9]+\(bob\)'
		"--then-keys=su bob -c 'thermal strategy balanced'; echo bob-status=\$?"
		'--then-expect=thermal: strategy: Operation not permitted'
		'--then-expect=bob-status=1'
		"--then-keys=su bob -c 'thermal strategy'; cat /etc/thermal.conf; echo bob-read-done"
		'--then-expect=strategy powersave generation=[0-9]+ result=ok'
		'--then-expect=strategy powersave'
		'--then-expect=bob-read-done'
		"--then-keys=su bob -c 'thermal cpu 0'; echo bob-cpu-status=\$?"
		'--then-expect=^delivered_mhz [0-9]+'
		'--then-expect=bob-cpu-status=0')
elif [ "$mode" = readers ]; then
	old=hw.thermal
	steps=("${login[@]}"
		"--then-keys=monitor-probe 1 100 | grep SAMPLE | sed 's/.* valid=\\([^ ]*\\) .* cpu_mc=/probe valid=\\1 cpu_mc=/'"
		'--then-expect=probe valid=0x[0-9a-f]+ cpu_mc=0([^0-9]|$)'
		'--then-keys=thermal-inject sensor cpu0 zone 1 45000 80000 95000'
		'--then-expect=thermal-inject: ok'
		'--then-keys=thermal sensors'
		'--then-expect=sensor 0 cpu0 kind=zone cpu=yes temperature=45.000 passive=80.000 critical=95.000'
		"--then-keys=monitor-probe 1 100 | grep SAMPLE | sed 's/.* valid=\\([^ ]*\\) .* cpu_mc=/probe valid=\\1 cpu_mc=/'"
		'--then-expect=probe valid=0x1[0-9a-f][0-9a-f] cpu_mc=45000([^0-9]|$)'
		"--then-keys=sysctl $old; echo old-status=\$?"
		"--then-expect=sysctl: $old: "
		'--then-expect=old-status=1'
		'--then-keys=sysctl -a > /dev/null; echo all-status=$?'
		'--then-expect=all-status=0')
else
	steps=("${login[@]}"
		'--then-keys=thermal'
		'--then-expect=strategy balanced generation=1 result=unsupported'
		'--then-expect=emergency none'
		'--then-keys=thermal-inject receiver test 0 0; thermal-inject sensor zone0 zone 1 45000 80000 95000'
		'--then-expect=thermal-inject: ok'
		'--then-keys=thermal sensors'
		'--then-expect=sensor 0 zone0 kind=zone cpu=yes temperature=45.000 passive=80.000 critical=95.000 flags=valid,armed'
		'--then-keys=thermal strategy powersave'
		'--then-expect=strategy powersave generation=2 flags=-'
		'--then-expect=receiver 1 test result=ok generation=2'
		'--then-keys=cat /etc/thermal.conf; thermal restore'
		'--then-expect=strategy powersave generation=3 flags=-'
		'--then-keys=thermal strategy manual 30 20 10; thermal strategy'
		'--then-expect=strategy manual speed=30 noise=20 gating=10 generation=4 result=ok'
		'--then-keys=cat /etc/thermal.conf'
		'--then-expect=strategy manual 30 20 10'
		'--then-keys=thermal cpu; thermal power; thermal fans'
		'--then-expect=cpu 0 core='
		'--then-expect=power: no energy meter'
		'--then-keys=thermal boost all 1'
		'--then-expect=thermal: boost: ')
fi

read -r -a accel <<<"$(qemu_accel_args max)"

# Boots the disk once, runs the steps, and for an emergency waits for the machine to end itself.
run_once()
{
	local status=0
	local waited=0
	qemu-system-x86_64 -name p1-ws333f-thermal -machine q35 -m 8G -smp 4 "${accel[@]}" \
		-drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
		-drive "if=pflash,format=raw,file=$work/vars.fd" \
		-drive "if=none,id=boot,file=$work/disk.qcow2,format=qcow2" \
		-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
		-vga std -display none -no-reboot \
		-qmp "unix:$work/qmp.sock,server,nowait" >>"$work/qemu.log" 2>&1 &
	pid=$!
	timeout 900 python3 "$repo/plan/tools/boot-test.py" --monitor "$work/qmp.sock" \
		--screenshot "$output/$1.png" --frame "$work/screen.ppm" --timeout 240 --interval 0.5 \
		"${@:2}" || status=$?
	if { [ $status -eq 0 ] || ! kill -0 $pid 2>/dev/null; } && { [ "$1" = emergency ] || [ "$1" = direct ]; }; then
		status=0
		while kill -0 $pid 2>/dev/null && [ $waited -lt 150 ]; do
			sleep 1
			waited=$((waited + 1))
		done
		if kill -0 $pid 2>/dev/null; then
			echo "thermal-qemu: the machine is still on ${waited} s after the emergency"
			status=1
		else
			echo "thermal-qemu: the machine turned itself off ${waited} s after the status was read"
		fi
	fi
	kill $pid 2>/dev/null || true
	wait $pid 2>/dev/null || true
	sync
	return $status
}

status=0
run_once "$mode" "${steps[@]}" || status=$?
if [ $status -eq 0 ] && { [ "$mode" = emergency ] || [ "$mode" = direct ]; }; then
	run_once record "${record[@]}" || status=$?
fi
if [ $status -eq 0 ]; then
	echo "thermal-qemu: PASS ($mode)"
else
	echo "thermal-qemu: FAIL ($mode)"
fi
exit $status
