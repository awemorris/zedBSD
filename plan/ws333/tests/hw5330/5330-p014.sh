#!/bin/sh
# ws333 p014: the CPU's acceptance on the bare Latitude 5330, end to end (deploy, boot, run, collect, summary), under
# /tmp/i915-hw.lock, read through /dev/thermal with the tree's /sbin/thermal (copied to /tmp on zedBSD).
#   sh plan/ws333/tests/hw5330/5330-p014.sh KDIR NAME [cpu]
# With cpu, only one boot of the c6 kernel and the CPU part (each strategy and the direct maximum, the request the
# domains show and the CPUs' MHz read four times while one process spins, then while every CPU is loaded).
# KDIR holds the kernels of kbuild.sh: KDIR/hlt, c6, c10 and periodic (each DIR/vmunix); THERMAL (default
# KDIR/static/thermal, made by thermal-static.sh: the NVMe's userland predates the tree's shared libc) is the program.  Each kernel goes to the NVMe's p4 (5330-nvme-write.sh kernel, the one
# before kept as vmunix.old) and boots the NVMe's zedBSD (its userland is kept); after IDLE_SETTLE seconds (default
# 120) one window of 60 s reads at once the package power (thermal power), the package's and two CPUs' C-state
# residency (thermal idle).  The c6 kernel's boot then measures the same window under strategy balanced, and runs the
# CPU part: the CPUs, domains and relative performance; each strategy and a direct maximum with one spinning process,
# the CPUs' MHz read while it spins; every CPU loaded, to see the E-core domains share a clock.  A last boot of the
# c6 kernel with cpuperf.hwp=off.  Debian measures the same idle window (intel-rapl:0 and cpuidle) on the same day.
# Not done here: the i915 frame rate (it needs the mview image) and the sleep to idle (no remote power to wake it).
# Output: LOGS (default build/hw5330/ws333/NAME): each step's output and summary.txt.  The last line is
# "5330-p014: done|FAIL ...".  Nothing is removed on either machine.  p4's kernel from before the first run is kept
# on Debian as ~/ws245/vmunix-p014-orig (5330-nvme-write.sh kernel puts it back); p4 keeps the c6 kernel.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
KDIR=${1:?KDIR}
NAME=${2:?NAME}
MODE=${3:-all}
cd "$(dirname "$0")/../../../.."
THERMAL=${THERMAL:-$KDIR/static/thermal}
LOGS=${LOGS:-build/hw5330/ws333}/$NAME
HOST=solaris10-man
ZB=10.0.30.3
ZBU=user
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
T=/tmp/p014-thermal
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"

say() {
	echo "5330-p014: $*" | tee -a "$SUMMARY"
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
			say "FAIL: zedBSD did not answer in 5 minutes (no remote power: ask Q1)"
			exit 2
		fi
		sleep 5
	done
	say "zedBSD up after $(($(date +%s) - start + 20)) s ($(zb 'uname -v' 2> /dev/null))"
}
# Puts KERNEL on the NVMe's p4.
kernel() {
	TO=120 timeout 120 scp -q "$1" "$HOST:ws245/vmunix-p014-$NAME" || { say "FAIL: copy"; exit 1; }
	lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-p014-$NAME" > /dev/null || { say "FAIL: kernel install"; exit 1; }
	say "kernel $(sha256sum "$1" | cut -c1-16) on p4 ($1)"
}
# Adds WORDS to p4's zedbsd.cfg for the next start (the file before kept on Debian), or puts the file back.
cfg_add() {
	lin "sudo -n mount $NV_BOOT /mnt/zb && sudo -n cp /mnt/zb/zedbsd.cfg \$HOME/ws245/zedbsd.cfg.p014-$NAME && for w in $1; do grep -qx \"\$w\" /mnt/zb/zedbsd.cfg || echo \"\$w\" | sudo -n tee -a /mnt/zb/zedbsd.cfg > /dev/null; done && sync && sudo -n umount /mnt/zb" ||
		{ say "FAIL: cfg lines"; exit 1; }
}
cfg_back() {
	lin "sudo -n mount $NV_BOOT /mnt/zb && sudo -n cp \$HOME/ws245/zedbsd.cfg.p014-$NAME /mnt/zb/zedbsd.cfg && sync && sudo -n umount /mnt/zb" || say "cfg not put back"
}
to_zedbsd() {
	debian_up
	lin 'sudo -n sh $HOME/ws245/5330-nvme-write.sh check' > /dev/null || { say "FAIL: the NVMe's zedBSD partitions are not as expected"; exit 1; }
	entry=$(lin "sudo -n efibootmgr -v" | grep -i 08ab575f-f718-4eff-a3c9-10d80b1217ea | grep -i 'zedbsd.BOOTX64' | sed -n 's/^Boot\([0-9A-F]\{4\}\).*/\1/p' | head -1)
	[ -n "$entry" ] || { say "FAIL: no UEFI entry zedBSD NVMe"; exit 1; }
	lin "sudo -n efibootmgr -n $entry > /dev/null && sudo -n systemctl reboot" > /dev/null 2>&1
	sleep 20
	zedbsd_up
	TO=60 timeout 60 sshpass -p "$ZBU" scp $SO -q "$THERMAL" "$ZBU@$ZB:$T" || { say "FAIL: copy of thermal"; exit 1; }
	zb "chmod 755 $T"
}
to_debian() {
	zr 'reboot' > /dev/null 2>&1
	sleep 20
	debian_up
	say "Debian up ($(TO=15 lin 'uname -r'))"
}
# Runs a thermal command as root on zedBSD into LOGS/FILE.
th() {
	TO=${TO:-60} zr "$T $1" > "$LOGS/$2" 2>&1
}
# One idle window of 60 s on zedBSD: the package power, the package's residency and two CPUs' at once.
window() {
	last=$(($(zb 'sysctl hw.ncpu' | sed 's/.*: *//') - 1))
	TO=90 th "power -i 60" "$1-power.txt" &
	TO=90 th "idle package 0 -i 60" "$1-idle-package.txt" &
	TO=90 th "idle cpu 0 -i 60" "$1-idle-cpu0.txt" &
	TO=90 th "idle cpu $last -i 60" "$1-idle-cpu$last.txt" &
	wait
	say "$1 power: $(grep -h '^power' "$LOGS/$1-power.txt" | sed 's/^power [0-9]* //' | tr '\n' ';')"
	say "$1 package: $(grep -h '^state' "$LOGS/$1-idle-package.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
	say "$1 cpu0: $(grep -h '^state' "$LOGS/$1-idle-cpu0.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
	say "$1 cpu$last: $(grep -h '^state' "$LOGS/$1-idle-cpu$last.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
}
# One spinning process for 20 s from an SSH session that stays open until it ends, in the background here.
spin1() {
	TO=45 zb "timeout 25 sh -c 'while :; do :; done' > /dev/null 2>&1 < /dev/null" > /dev/null 2>&1 &
}
# A busy loop on every CPU for 20 s.
loadall() {
	TO=50 zb "n=\$(sysctl hw.ncpu | sed 's/.*: *//'); i=0; while [ \$i -lt \$n ]; do timeout 20 sh -c 'while :; do :; done' > /dev/null 2>&1 < /dev/null & i=\$((i + 1)); done; wait" > /dev/null 2>&1 &
}
# The CPUs while one process spins (25 s): each CPU's MHz read four times, 5 s apart, and the highest of each read.
spun() {
	spin1
	highs=
	for k in 1 2 3 4; do
		sleep 5
		th "cpu" "$1-cpu$k.txt"
		highs="$highs $(sed -n 's/.* mhz=\([0-9]*\).*/\1/p' "$LOGS/$1-cpu$k.txt" | sort -n | tail -1)"
	done
	wait
	say "$1: highest mhz of each read:$highs; last read $(sed -n 's/^cpu \([0-9]*\) .* mhz=\([0-9]*\).*/\1:\2/p' "$LOGS/$1-cpu4.txt" | tr '\n' ' ')"
}

TO=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh plan/tools/hw5330/ufs-grow.py "$HOST:ws245/" || { echo "5330-p014: FAIL: copy of the helper"; exit 1; }
exec 9> /tmp/i915-hw.lock
flock 9
say "$NAME start $(date '+%F %T')"

# Keeps p4's kernel as it was on Debian once (each install keeps only the one before as vmunix.old).
debian_up
lin "test -f \$HOME/ws245/vmunix-p014-orig || { sudo -n mount $NV_BOOT /mnt/zb && cp /mnt/zb/vmunix \$HOME/ws245/vmunix-p014-orig; sudo -n umount /mnt/zb; }" ||
	{ say "FAIL: p4's kernel not kept"; exit 1; }
say "p4's kernel before the run: $(TO=15 lin 'cksum < $HOME/ws245/vmunix-p014-orig') (Debian ~/ws245/vmunix-p014-orig; put back: 5330-nvme-write.sh kernel it)"

# 1. Idle: each kernel, the window; c6 also under balanced, and the CPU part.
tags="hlt periodic c10 c6"
[ "$MODE" = cpu ] && tags=c6
for tag in $tags; do
	kernel "$KDIR/$tag/vmunix"
	to_zedbsd
	[ "$MODE" = cpu ] || sleep "${IDLE_SETTLE:-120}"
	zb 'dmesg' > "$LOGS/$tag-boot.txt" 2>&1
	th "" "$tag-status.txt"
	[ "$MODE" = cpu ] || window "$tag"
	if [ "$tag" = c6 ]; then
		if [ "$MODE" != cpu ]; then
			th "strategy balanced" c6-balanced-set.txt
			sleep 30
			window c6-balanced
		fi

		# 3. The CPUs, the domains, the relative performance; each strategy with one process spinning.
		th "cpu" cpu-idle.txt
		th "cpu domains" cpu-domains.txt
		th "cpu relative" cpu-relative.txt
		th "cpu relative frequency" cpu-relative-frequency.txt
		th "cpu relative feedback" cpu-relative-feedback.txt
		th "cpu relative provider" cpu-relative-provider.txt
		say "domains: $(tr '\n' ';' < "$LOGS/cpu-domains.txt")"
		for s in performance balanced powersave "manual 100 50 50" "manual 0 50 50"; do
			tag2=$(echo "$s" | tr ' ' '-')
			th "strategy $s" "strategy-$tag2.txt"
			sleep 3
			th "cpu domains" "strategy-$tag2-domains.txt"
			say "strategy $s request: $(sed -n 's/^domain \([0-9]*\) .* min=\([0-9]*\) max=\([0-9]*\) desired=\([0-9]*\) epp=\([0-9]*\).*/d\1:\2-\3,d\4,e\5/p' "$LOGS/strategy-$tag2-domains.txt" | sort -u -t: -k2 | tr '\n' ' ')"
			spun "strategy-$tag2"
		done

		# A direct maximum halfway along the first domain's levels, then the strategy again.
		range=$(sed -n 's/.* levels=\([0-9]*\)\.\.\([0-9]*\).*/\1 \2/p' "$LOGS/cpu-domains.txt" | head -1)
		low=${range% *}
		high=${range#* }
		if [ -n "$range" ]; then
			mid=$(((low + high) / 2))
			th "cpu set all max=$mid" direct-set.txt
			sleep 3
			spun "direct-max$mid"
			say "direct max=$mid (levels $low..$high): $(tr '\n' ' ' < "$LOGS/direct-set.txt")"
		else
			say "direct: no levels in the domains"
		fi
		th "strategy balanced" strategy-back.txt

		# Every CPU loaded: the CPUs of one E-core domain should run at one clock.
		th "strategy performance" strategy-all-performance.txt
		loadall
		sleep 12
		th "cpu" cpu-loaded.txt
		wait
		say "loaded: $(sed -n 's/^cpu \([0-9]*\) core=\([A-Za-z0-9-]*\) domain=\([0-9]*\) .* mhz=\([0-9]*\).*/\1:\2:d\3:\4/p' "$LOGS/cpu-loaded.txt" | tr '\n' ' ')"
		th "strategy balanced" strategy-end.txt
		zb 'dmesg' > "$LOGS/c6-end.txt" 2>&1
	fi
	to_debian
done

if [ "$MODE" = cpu ]; then
	say "done $(date '+%F %T')"
	exit 0
fi

# 4. cpuperf.hwp=off on the c6 kernel.
cfg_add cpuperf.hwp=off
to_zedbsd
sleep 30
zb 'dmesg' > "$LOGS/hwp-off-boot.txt" 2>&1
th "cpu" hwp-off-cpu.txt
th "cpu domains" hwp-off-domains.txt
th "" hwp-off-status.txt
say "hwp=off: $(grep -h 'cpu-x86' "$LOGS/hwp-off-boot.txt" | head -4 | tr '\n' ';')"
say "hwp=off cpu: $(head -3 "$LOGS/hwp-off-cpu.txt" | tr '\n' ';')"
to_debian
cfg_back

# 1. Debian's idle on the same day: the package's energy over 60 s and cpu0's cpuidle.
sleep "${IDLE_SETTLE:-120}"
lin 'sudo -n cat /sys/class/powercap/intel-rapl:0/energy_uj; for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do echo "$(cat $s/name) $(cat $s/time)"; done; sleep 60; sudo -n cat /sys/class/powercap/intel-rapl:0/energy_uj; for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do echo "$(cat $s/name) $(cat $s/time)"; done' > "$LOGS/debian-idle.txt" 2>&1
e0=$(sed -n 1p "$LOGS/debian-idle.txt")
e1=$(awk 'NR>1 && /^[0-9]+$/ {print; exit}' "$LOGS/debian-idle.txt")
if [ -n "$e0" ] && [ -n "$e1" ]; then
	say "debian package: $(awk -v a="$e0" -v b="$e1" 'BEGIN { printf "%.3f W", (b - a) / 60e6 }')"
else
	say "debian package: not read"
fi
say "debian cpu0 cpuidle (name time_us at 0 s, then at 60 s): $(grep -v '^[0-9]*$' "$LOGS/debian-idle.txt" | tr '\n' ';')"
say "done $(date '+%F %T')"
