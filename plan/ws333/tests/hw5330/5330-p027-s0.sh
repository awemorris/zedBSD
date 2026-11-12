#!/bin/sh
# ws333 p027: S0 idle on the 5330 and what runs after it, end to end (deploy, boot, sleep, wake, collect, summary),
# under /tmp/i915-hw.lock.  The summary is LOGS/summary.txt, its last line "5330-p027-s0: done" or "... FAIL ...".
#   sh plan/ws333/tests/hw5330/5330-p027-s0.sh VMUNIX STATIC NAME
# VMUNIX is a kernel of this tree with s0idle-wake.patch (kbuild.sh): the sleep is woken by the CMOS RTC alarm about
# 40 s after it begins, and the TCO watchdog counts through the sleep.  STATIC holds this tree's static sleepctl,
# sysctl, wifi and thermal (static-tool.sh, thermal-static.sh).  The 5330 must be on Debian; p4 keeps the WS333 kernel and
# zedbsd.cfg copies of 5330-p027.sh (~/ws245/*-p027-orig) and gets them back at the end.
#  1. VMUNIX on p4 with watchdog=WATCHDOG (default 120), zedBSD started once (UEFI BootNext).
#  2. Before: the package C-state counters, hw.gputelemetry, the kernel log's length.
#  3. sleepctl s0idle as root, detached; zedBSD is polled over SSH.  Back on zedBSD: sleepctl's line (result,
#     resume, wake reason), the kernel's new lines (sleep, i915, acpi, watchdog, pci, faults), the package C-states over
#     the sleep, hw.gputelemetry, the session's processes; then 60 s of idle (the package C-states and power) after it.
#     Back on Debian instead: the watchdog reset the 5330 (the sleep or the wake stopped), recorded as FAIL.
#  4. Debian again, p4 as it was, sha256 checked.
# Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
VMUNIX=${1:?VMUNIX}
STATIC=${2:?STATIC}
NAME=${3:?NAME}
cd "$(dirname "$0")/../../../.."
LOGS=${LOGS:-build/hw5330/ws333}/$NAME
WATCHDOG=${WATCHDOG:-120}
HOST=solaris10-man
ZB=10.0.30.3
ZBU=user
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
for f in "$VMUNIX" "$STATIC/sleepctl" "$STATIC/sysctl" "$STATIC/thermal" "$STATIC/wifi"; do
	[ -f "$f" ] || { echo "5330-p027-s0: FAIL: no $f"; exit 1; }
done
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"
exec 9> /tmp/i915-hw.lock
flock -w 3600 9 || { echo "5330-p027-s0: FAIL: the lock"; exit 1; }

say() {
	echo "5330-p027-s0: $*" | tee -a "$SUMMARY"
}
lin() {
	timeout "${TO:-120}" ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
zb() {
	timeout "${TO:-60}" sshpass -p "$ZBU" ssh -n $SO "$ZBU@$ZB" "$1"
}
zr() {
	zb "echo $ZBU | sudo -S sh -c '$1' 2> /dev/null"
}
is_debian() {
	TO=8 lin hostname 2> /dev/null | grep -q chaos
}
is_zedbsd() {
	TO=8 zb 'uname -s' 2> /dev/null | grep -qi zedbsd
}
restore() {
	TO=300 lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/vmunix-p027-orig /mnt/zb/vmunix && cp $DH/ws245/zedbsd.cfg-p027-orig /mnt/zb/zedbsd.cfg && sync && sha256sum /mnt/zb/vmunix /mnt/zb/zedbsd.cfg && umount /mnt/zb'" > "$LOGS/restore.txt" 2>&1
	say "p4: $(awk '{ printf "%s %s ", substr($1, 1, 16), $2 }' "$LOGS/restore.txt")"
}
to_debian() {
	is_debian && return 0
	zr 'reboot' > /dev/null 2>&1
	start=$(date +%s)
	until is_debian; do
		[ $(($(date +%s) - start)) -gt 600 ] && { say "FAIL: Debian did not come back in 10 minutes"; exit 2; }
		sleep 5
	done
}
# The package C-state counters (cumulative) into LOGS/FILE.
pkg() {
	TO=30 zr "/tmp/p027-thermal idle package 0" > "$LOGS/$1" 2>&1
}

# 1. The kernel and the watchdog on p4, zedBSD once.
TO=15 lin 'uname -sr' | grep -q Linux || { say "FAIL: the 5330 is not on Debian"; exit 1; }
say "$NAME start $(date '+%F %T') on $(TO=15 lin 'uname -sr')"
# The Debian user's home, written out: root's shell under sudo would read its own.
DH=$(TO=15 lin 'echo $HOME')
case $DH in /home/*) ;; *) say "FAIL: the Debian home ($DH)"; exit 1 ;; esac
TO=15 lin 'test -f ws245/vmunix-p027-orig && test -f ws245/zedbsd.cfg-p027-orig' || { say "FAIL: no p027 copies of p4"; exit 1; }
TO=120 timeout 120 scp -q "$VMUNIX" "$HOST:ws245/vmunix-p027-$NAME" || { say "FAIL: copy"; exit 1; }
lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-p027-$NAME" > /dev/null || { say "FAIL: kernel install"; exit 1; }
lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/zedbsd.cfg-p027-orig /mnt/zb/zedbsd.cfg && echo watchdog=$WATCHDOG >> /mnt/zb/zedbsd.cfg && sync && umount /mnt/zb'" > /dev/null 2>&1 ||
	{ say "FAIL: zedbsd.cfg"; restore; exit 1; }
say "kernel $(sha256sum "$VMUNIX" | cut -c1-16) on p4, watchdog=$WATCHDOG"
lin 'sudo -n sh $HOME/ws245/5330-nvme-write.sh check' > /dev/null || { say "FAIL: the NVMe's partitions"; restore; exit 1; }
lin "sudo -n efibootmgr -n \$(sudo -n efibootmgr -v | grep -i 08ab575f-f718-4eff-a3c9-10d80b1217ea | grep -i 'zedbsd.BOOTX64' | sed -n 's/^Boot\([0-9A-F]\{4\}\).*/\1/p' | head -1) > /dev/null && sudo -n systemctl reboot" > /dev/null 2>&1
sleep 20
start=$(date +%s)
until is_zedbsd; do
	if [ $(($(date +%s) - start)) -gt 300 ]; then
		say "FAIL: zedBSD did not answer in 5 minutes"
		start=$(date +%s)
		until is_debian; do [ $(($(date +%s) - start)) -gt 600 ] && exit 2; sleep 5; done
		restore
		exit 1
	fi
	sleep 5
done
for t in sleepctl sysctl thermal wifi; do
	TO=60 timeout 60 sshpass -p "$ZBU" scp $SO -q "$STATIC/$t" "$ZBU@$ZB:/tmp/p027-$t" || { say "FAIL: copy of $t"; to_debian; restore; exit 1; }
done
zb 'chmod 755 /tmp/p027-sleepctl /tmp/p027-sysctl /tmp/p027-thermal /tmp/p027-wifi'
zb 'dmesg' > "$LOGS/boot.txt" 2>&1
say "zedBSD up: $(grep -h 'watchdog: armed' "$LOGS/boot.txt" | head -1)"
sleep 60

# 2. Before.
pkg pkg-before.txt
TO=20 zb '/tmp/p027-sysctl hw.gputelemetry' > "$LOGS/gt-before.txt" 2>&1
lines=$(TO=20 zb 'dmesg | wc -l' | tr -d ' ')
TO=20 zb 'ps ax' > "$LOGS/ps-before.txt" 2>&1

# 3. The sleep, detached from the SSH session.  A Wi-Fi radio that is on refuses the suspend by design (sessiond's
# sleep asks networkd to turn it off first); sleepctl asks the kernel directly, so the radios go down first.
TO=20 zb 'ifconfig -a' > "$LOGS/ifconfig.txt" 2>&1
for w in $(grep -o '^wlan[0-9]*' "$LOGS/ifconfig.txt" | sort -u); do
	TO=20 zr "/tmp/p027-wifi $w down; echo wifi=\$?; ifconfig $w down; echo ifconfig=\$?" >> "$LOGS/wifi-down.txt" 2>&1
done
say "wifi: $(tr '\n' ' ' < "$LOGS/wifi-down.txt" 2> /dev/null)"
TO=20 zr 'rm -f /tmp/p027-s0.txt; nohup /tmp/p027-sleepctl s0idle > /tmp/p027-s0.txt 2>&1 < /dev/null &' > /dev/null 2>&1
began=$(date +%s)
say "sleep asked at $(date '+%T')"
sleep 15
outcome=
while [ $(($(date +%s) - began)) -lt 420 ]; do
	if TO=8 zb 'cat /tmp/p027-s0.txt' > "$LOGS/s0.txt" 2> /dev/null && grep -q 'sleep \(result\|refused\)' "$LOGS/s0.txt"; then
		outcome=zedbsd
		break
	fi
	if is_debian; then
		outcome=debian
		break
	fi
	sleep 5
done
case $outcome in
zedbsd)
	say "back on zedBSD $(($(date +%s) - began)) s after the request: $(cat "$LOGS/s0.txt" | tr '\n' ' ')"
	TO=30 zb "dmesg | tail -n +$((lines + 1))" > "$LOGS/sleep-dmesg.txt" 2>&1
	say "kernel: $(grep -E 'sleep:|acpi: test|watchdog|i915: (suspend|resume|unpark|park)|fault|panic|trap' "$LOGS/sleep-dmesg.txt" | head -30 | tr '\n' ';')"
	pkg pkg-after.txt
	say "package C-states over the sleep: $(awk '$1 == "state" { split($3, kv, "="); if (FILENAME ~ /before/) b[$2] = kv[2]; else printf "%s +%.1f s ", $2, (kv[2] - b[$2]) / 1e9 }' "$LOGS/pkg-before.txt" "$LOGS/pkg-after.txt")"
	TO=20 zb '/tmp/p027-sysctl hw.gputelemetry' > "$LOGS/gt-after.txt" 2>&1
	say "gt after: $(grep i915 "$LOGS/gt-after.txt" | sed 's/.*valid=/valid=/' | cut -c1-200)"
	TO=20 zb 'ps ax' > "$LOGS/ps-after.txt" 2>&1
	say "processes: before $(grep -c . "$LOGS/ps-before.txt"), after $(grep -c . "$LOGS/ps-after.txt"); session: $(grep -E -o 'keiland[a-z-]*|sessiond' "$LOGS/ps-after.txt" | sort -u | tr '\n' ' ')"
	TO=90 zr "/tmp/p027-thermal power -i 60" > "$LOGS/after-power.txt" 2>&1 &
	TO=90 zr "/tmp/p027-thermal idle package 0 -i 60" > "$LOGS/after-package.txt" 2>&1 &
	wait
	say "60 s after: $(grep -h '^power' "$LOGS/after-power.txt" | grep package | sed 's/^power [0-9]* //') $(grep -h '^state' "$LOGS/after-package.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
	to_debian
	;;
debian)
	say "FAIL: the 5330 came back on Debian $(($(date +%s) - began)) s after the request (the watchdog reset it: the sleep or the wake stopped)"
	;;
*)
	say "FAIL: neither zedBSD's answer nor Debian in 7 minutes"
	start=$(date +%s)
	until is_debian; do [ $(($(date +%s) - start)) -gt 600 ] && { say "FAIL: no Debian; a person is needed"; exit 2; }; sleep 5; done
	;;
esac

# 4. p4 as it was.
restore
say "done $(date '+%F %T')"
