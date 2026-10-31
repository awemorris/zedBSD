#!/bin/sh
# ws333: the CPU driver, the tick and the Dell provider on the bare Latitude 5330, one run per phase, end to end
# (deploy, boot, run, collect, summary), under /tmp/i915-hw.lock.  The kernels carry thermal-diag.patch (test only):
# a root's "hostname td-..." makes the kernel log the thermal core, the meters, the fans, the package's idle residency
# and every CPU ("thermal-diag:" lines), or set a strategy or a boost, until /dev/thermal has its UAPI.
#   sh plan/ws333/tests/hw5330/5330-thermal.sh p011 VMUNIX NAME
#       boot 1: the driver's boot lines, two dumps 10 s apart (meters, residency, levels, feedback), then the strategies
#       performance, powersave and balanced each with a short load on every CPU; boot 2 with cpuperf=off.
#   sh plan/ws333/tests/hw5330/5330-thermal.sh p012 VMUNIX_TSC_DEADLINE VMUNIX_PERIODIC NAME
#       each kernel: boot, settle IDLE_SETTLE s, three idle windows of 20 s (the package meter, the i915 diagnostic's
#       package energy, the package C-state residency).
#   sh plan/ws333/tests/hw5330/5330-thermal.sh p018 VMUNIX VMUNIX_BOOST NAME
#       Debian: power-profiles-daemon masked for the run, platform_profile kept over a reboot (the precheck), set to
#       cool; boot 1 (VMUNIX): the Dell lines, the four modes each with 60 s of load, Quiet last; Debian reads the mode;
#       boot 2 (VMUNIX_BOOST, CONFIG_DELL_SMM_BOOST=y): the mode the firmware shows at boot, boost 0:2 idle and under
#       load (stopped above 90 C), boost 0:0; Debian: the daemon unmasked and started, the profile it restores.
# The zedBSD of the NVMe (an image of plan/tools/hw5330/config-5330.mk, account user) keeps its userland; VMUNIX goes
# to p4 (5330-nvme-write.sh kernel, the one before kept as vmunix.old).  Output: LOGS (default
# build/hw5330/ws333/NAME): each step's kernel log and the summary (summary.txt).  The last line is
# "5330-thermal: done|FAIL ...".  Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
PHASE=${1:?p011|p012|p018}
shift
case "$PHASE" in
p011) K1=${1:?VMUNIX}; NAME=${2:?NAME} ;;
p012 | p018) K1=${1:?VMUNIX}; K2=${2:?VMUNIX}; NAME=${3:?NAME} ;;
*) echo "5330-thermal: FAIL: unknown phase $PHASE"; exit 2 ;;
esac
cd "$(dirname "$0")/../../../.."
HERE=plan/ws333/tests/hw5330
LOGS=${LOGS:-build/hw5330/ws333}/$NAME
HOST=solaris10-man
ZB=10.0.30.3
ZBU=user
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"

say() {
	echo "5330-thermal: $*" | tee -a "$SUMMARY"
}
lin() {
	timeout "${TO:-120}" ssh -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
zb() {
	timeout "${TO:-60}" sshpass -p "$ZBU" ssh $SO "$ZBU@$ZB" "$1"
}
# As root on zedBSD.
zr() {
	zb "echo $ZBU | sudo -S sh -c '$1' 2> /dev/null"
}
# One diagnostic command; its answer is in the kernel log.
td() {
	TO=${TO:-60} zr "hostname td-$1" > /dev/null 2>&1
}
# The kernel log as it is now, kept as LOGS/STEP.txt.
klog() {
	zb 'dmesg' > "$LOGS/$1.txt" 2>&1
}
debian_up() {
	start=$(date +%s)
	until TO=8 lin hostname 2> /dev/null | grep -q chaos; do
		if [ $(($(date +%s) - start)) -gt 300 ]; then
			say "FAIL: Debian did not come back in 5 minutes"
			exit 2
		fi
		sleep 5
	done
}
zedbsd_up() {
	start=$(date +%s)
	until zb 'uname -s' 2> /dev/null | grep -qi zedbsd; do
		if [ $(($(date +%s) - start)) -gt 300 ]; then
			say "FAIL: zedBSD did not answer in 5 minutes (a power cycle may be needed)"
			exit 2
		fi
		sleep 5
	done
	say "zedBSD up after $(($(date +%s) - start + 20)) s ($(zb 'uname -v' 2> /dev/null))"
}
# Puts KERNEL on the NVMe's p4.
kernel() {
	TO=120 timeout 120 scp -q "$1" "$HOST:ws245/vmunix-ws333-$NAME" || { say "FAIL: copy"; exit 1; }
	lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-ws333-$NAME" > /dev/null || { say "FAIL: kernel install"; exit 1; }
	say "kernel $(sha256sum "$1" | cut -c1-16) on p4 ($1)"
}
# Adds WORDS to p4's zedbsd.cfg for the next start (the file before kept on Debian), or puts the file back.
cfg_add() {
	lin "sudo -n mount $NV_BOOT /mnt/zb && sudo -n cp /mnt/zb/zedbsd.cfg \$HOME/ws245/zedbsd.cfg.ws333-$NAME && for w in $1; do grep -qx \"\$w\" /mnt/zb/zedbsd.cfg || echo \"\$w\" | sudo -n tee -a /mnt/zb/zedbsd.cfg > /dev/null; done && sync && sudo -n umount /mnt/zb" ||
		{ say "FAIL: cfg lines"; exit 1; }
}
cfg_back() {
	lin "sudo -n mount $NV_BOOT /mnt/zb && sudo -n cp \$HOME/ws245/zedbsd.cfg.ws333-$NAME /mnt/zb/zedbsd.cfg && sync && sudo -n umount /mnt/zb" || say "cfg not put back"
}
to_zedbsd() {
	debian_up
	lin 'sudo -n sh $HOME/ws245/5330-nvme-write.sh check' > /dev/null || { say "FAIL: the NVMe's zedBSD partitions are not as expected"; exit 1; }
	entry=$(lin "sudo -n efibootmgr -v" | grep -i 08ab575f-f718-4eff-a3c9-10d80b1217ea | grep -i 'zedbsd.BOOTX64' | sed -n 's/^Boot\([0-9A-F]\{4\}\).*/\1/p' | head -1)
	[ -n "$entry" ] || { say "FAIL: no UEFI entry zedBSD NVMe"; exit 1; }
	lin "sudo -n efibootmgr -n $entry > /dev/null && sudo -n systemctl reboot" > /dev/null 2>&1
	sleep 20
	zedbsd_up
}
to_debian() {
	zr 'reboot' > /dev/null 2>&1
	sleep 20
	debian_up
	say "Debian up ($(TO=15 lin 'uname -r'))"
}
# A busy loop on every CPU for SECONDS on zedBSD, from an SSH session that stays open until the loops end (the
# session's end would take its background jobs with it), itself in the background here.
load() {
	TO=$(($1 + 30)) zb "n=\$(sysctl hw.ncpu | sed 's/.*: *//'); i=0; while [ \$i -lt \$n ]; do timeout $1 sh -c 'while :; do :; done' > /dev/null 2>&1 < /dev/null & i=\$((i + 1)); done; wait" > /dev/null 2>&1 &
}
summary() {
	python3 -I "$HERE/td-summary.py" "$@" | tee -a "$SUMMARY"
}

TO=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh plan/tools/hw5330/ufs-grow.py "$HOST:ws245/" || { echo "5330-thermal: FAIL: copy of the helper"; exit 1; }
exec 9> /tmp/i915-hw.lock
flock 9
say "$PHASE $NAME start $(date '+%F %T')"

case "$PHASE" in
p011)
	kernel "$K1"
	to_zedbsd
	sleep "${SETTLE:-60}"
	klog boot
	td dump; sleep 10; td dump
	klog idle
	td s-performance; sleep 3; td dump
	klog performance
	load 25; sleep 12; td dump; sleep 15
	klog performance-load
	td s-powersave; sleep 3; load 25; sleep 12; td dump; sleep 15
	klog powersave-load
	td s-balanced; sleep 3; td dump
	klog balanced
	to_debian
	summary p011 "$LOGS/boot.txt" "$LOGS/idle.txt" "$LOGS/performance.txt" "$LOGS/performance-load.txt" "$LOGS/powersave-load.txt" "$LOGS/balanced.txt"
	cfg_add cpuperf=off
	to_zedbsd
	sleep 30
	td dump
	klog off
	to_debian
	cfg_back
	summary p011-off "$LOGS/off.txt"
	;;
p012)
	for pair in "tsc-deadline=$K1" "periodic=$K2"; do
		tag=${pair%%=*}
		kernel "${pair#*=}"
		to_zedbsd
		sleep "${IDLE_SETTLE:-120}"
		w=1
		while [ $w -le 3 ]; do
			zb 'sysctl hw.gputelemetry' > "$LOGS/$tag-w$w-gpu0.txt" 2>&1
			td idle
			sleep 20
			zb 'sysctl hw.gputelemetry' > "$LOGS/$tag-w$w-gpu1.txt" 2>&1
			td idle
			klog "$tag-w$w"
			w=$((w + 1))
			sleep 5
		done
		to_debian
		summary p012 "$tag" "$LOGS/$tag-w1.txt" "$LOGS/$tag-w2.txt" "$LOGS/$tag-w3.txt"
	done
	;;
p018)
	debian_up
	orig=$(TO=15 lin 'cat /sys/firmware/acpi/platform_profile')
	ppd_enabled=$(TO=15 lin 'systemctl is-enabled power-profiles-daemon')
	ppd_active=$(TO=15 lin 'systemctl is-active power-profiles-daemon')
	ppd_saved=$(TO=15 lin 'sed -n "s/^Profile=//p" /var/lib/power-profiles-daemon/state.ini')
	say "Debian before: platform_profile $orig, power-profiles-daemon enabled=$ppd_enabled active=$ppd_active saved Profile=$ppd_saved, others: $(TO=15 lin 'systemctl is-active thermald tuned tlp 2> /dev/null | tr "\n" " "')"
	# Puts the daemon back as it was, on every way out of the run (Debian must answer for it).
	ppd_restore() {
		trap - EXIT INT TERM
		debian_up
		lin 'sudo -n systemctl unmask power-profiles-daemon' > /dev/null 2>&1 || say "FAIL: unmask"
		[ "$ppd_active" = active ] && { lin 'sudo -n systemctl start power-profiles-daemon' > /dev/null 2>&1 || say "FAIL: start"; }
		sleep 5
		now_profile=$(TO=15 lin 'cat /sys/firmware/acpi/platform_profile')
		say "Debian after: power-profiles-daemon enabled=$(TO=15 lin 'systemctl is-enabled power-profiles-daemon') active=$(TO=15 lin 'systemctl is-active power-profiles-daemon') saved Profile=$(TO=15 lin 'sed -n "s/^Profile=//p" /var/lib/power-profiles-daemon/state.ini'), platform_profile $now_profile (before: $ppd_enabled $ppd_active $ppd_saved $orig)"
		[ "$now_profile" = "$orig" ] || say "FAIL: platform_profile not back to $orig"
	}
	trap 'ppd_restore' EXIT
	trap 'exit 3' INT TERM
	lin 'sudo -n systemctl mask --now power-profiles-daemon' > /dev/null 2>&1 || say "FAIL: mask"
	lin 'echo quiet | sudo -n tee /sys/firmware/acpi/platform_profile > /dev/null'
	say "precheck: set quiet, reads $(TO=15 lin 'cat /sys/firmware/acpi/platform_profile')"
	lin '(sleep 1; sudo -n systemctl reboot) > /dev/null 2>&1 &'
	sleep 30
	debian_up
	say "precheck: after a reboot of Debian (daemon masked) reads $(TO=15 lin 'cat /sys/firmware/acpi/platform_profile')"
	lin 'echo cool | sudo -n tee /sys/firmware/acpi/platform_profile > /dev/null'
	say "Debian set cool before zedBSD, reads $(TO=15 lin 'cat /sys/firmware/acpi/platform_profile')"
	kernel "$K1"
	to_zedbsd
	sleep "${SETTLE:-60}"
	klog boot
	td dump
	klog start
	for mode in s-performance s-powersave s-balanced m-50.20.50; do
		td "$mode"; sleep 3; td dump
		klog "$mode"
		load 60
		t=0
		while [ $t -lt 60 ]; do
			sleep 15
			t=$((t + 15))
			td dump
		done
		klog "$mode-load"
		sleep 30
	done
	to_debian
	say "Debian after zedBSD set Quiet: platform_profile $(TO=15 lin 'cat /sys/firmware/acpi/platform_profile')"
	summary p018 "$LOGS/boot.txt" "$LOGS/start.txt" "$LOGS/s-performance-load.txt" "$LOGS/s-powersave-load.txt" "$LOGS/s-balanced-load.txt" "$LOGS/m-50.20.50-load.txt"
	kernel "$K2"
	to_zedbsd
	sleep "${SETTLE:-60}"
	klog boost-boot
	td dump; sleep 10; td dump
	klog boost-idle
	td b-0.2
	t=0
	while [ $t -lt 30 ]; do
		sleep 10
		t=$((t + 10))
		td dump
	done
	klog boost-on
	load 60
	t=0
	hot=0
	while [ $t -lt 60 ]; do
		sleep 10
		t=$((t + 10))
		td dump
		klog boost-load
		if python3 -I "$HERE/td-summary.py" hot "$LOGS/boost-load.txt" 90000; then
			hot=1
			break
		fi
	done
	[ $hot = 1 ] && say "boost: a sensor passed 90 C under load, boost taken off at once"
	td b-0.0; sleep 15; td dump
	klog boost-off
	td s-balanced; sleep 3; td dump
	klog boost-end
	to_debian
	summary p018-boost "$LOGS/boost-boot.txt" "$LOGS/boost-idle.txt" "$LOGS/boost-on.txt" "$LOGS/boost-load.txt" "$LOGS/boost-off.txt" "$LOGS/boost-end.txt"
	say "Debian with the daemon still masked: platform_profile $(TO=15 lin 'cat /sys/firmware/acpi/platform_profile')"
	ppd_restore
	;;
esac
say "done $(date '+%F %T')"
