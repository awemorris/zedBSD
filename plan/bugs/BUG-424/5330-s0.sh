#!/bin/sh
# S0 idle on the 5330 with the root disk behind the VMD, end to end (deploy, boot, idle windows, sleeps, checks,
# summary), under /tmp/i915-hw.lock.  One zedBSD boot; the summary is LOGS/summary.txt, its last line
# "5330-s0: done" or "5330-s0: FAIL ...".
#   sh plan/bugs/BUG-424/5330-s0.sh VMUNIX STATIC NAME
# VMUNIX is a kernel of this tree with plan/ws333/tests/hw5330/s0idle-wake.patch (kbuild.sh): each sleep is woken
# by the CMOS RTC alarm about 40 s after it begins, and the TCO watchdog counts through it.  STATIC holds this tree's
# static sleepctl, sysctl, thermal and wifi (plan/ws333/tests/hw5330/static-tool.sh, thermal-static.sh) and netsleep
# (plan/bugs/BUG-424/netsleep/build.sh).  The 5330
# must be on Debian with p4's original kernel and zedbsd.cfg kept as ~/ws245/*-p027-orig (5330-p027.sh made them).
#  1. VMUNIX on p4, zedbsd.cfg with watchdog=WATCHDOG (default 600), zedBSD started once (UEFI BootNext).
#  2. The usual idle as WS333 p029 measured it: after SETTLE s, 60 s windows (package power, package C-states, the
#     GT's RC6): "idle1", "idle2", then the Wi-Fi radio down and audiod stopped, "quiet".
#  3. The sleeps of SEQ in turn (default "devices devices devices s0idle s0idle s0idle": sleepctl devices suspends
#     and resumes the devices without S0 idle, sleepctl s0idle sleeps), each detached and read back from a file, with
#     networkd's SLEEP_PREPARE before (netsleep, the Wi-Fi radios off as sessiond's sleep has them) and SLEEP_END after.
#     For each, the package energy and C-state counters just before and just after, zedBSD's clock around it, the
#     kernel's new lines; then
#     the root disk (a file written and synced, 64 MiB read raw from the NVMe), the GPU (hw.gputelemetry, i915's
#     lines), the session (sessiond, keiland).  A 5330 back on Debian instead means the watchdog reset it: FAIL.
#  4. "after": a 60 s window after the sleeps, then Debian, p4 as it was, sha256 checked.
# Nothing is removed on either machine (the written file stays in the zedBSD user's home).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
VMUNIX=${1:?VMUNIX}
STATIC=${2:?STATIC}
NAME=${3:?NAME}
cd "$(dirname "$0")/../../.."
LOGS=${LOGS:-build/hw5330/bug424}/$NAME
WATCHDOG=${WATCHDOG:-600}
SETTLE=${SETTLE:-60}
SEQ=${SEQ:-devices devices devices s0idle s0idle s0idle}
ORIG_SHA=4c98d617279bf96b
HOST=solaris10-man
ZB=10.0.30.3
ZBU=user
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
T=/tmp/b424-thermal
SY=/tmp/b424-sysctl
for f in "$VMUNIX" "$STATIC/sleepctl" "$STATIC/sysctl" "$STATIC/thermal" "$STATIC/wifi" "$STATIC/netsleep"; do
	[ -f "$f" ] || { echo "5330-s0: FAIL: no $f"; exit 1; }
done
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"
exec 9> /tmp/i915-hw.lock
flock -w 3600 9 || { echo "5330-s0: FAIL: the lock"; exit 1; }

say() {
	echo "5330-s0: $*" | tee -a "$SUMMARY"
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
debian_back() {
	start=$(date +%s)
	until is_debian; do
		[ $(($(date +%s) - start)) -gt 600 ] && { say "FAIL: Debian did not come back in 10 minutes; a person is needed"; exit 2; }
		sleep 5
	done
}
to_debian() {
	is_debian && return 0
	zr 'reboot' > /dev/null 2>&1
	sleep 20
	debian_back
}
fail() {
	say "FAIL: $*"
	to_debian
	restore
	say "FAIL: $*"
	exit 1
}
th() {
	TO=${TO:-90} zr "$T $1" > "$LOGS/$2" 2>&1
}
gt() {
	TO=20 zb "$SY hw.gputelemetry" > "$LOGS/$1" 2>&1
}
# The GT's RC6 share between two telemetry readings.
rc6_share() {
	awk '
	$1 == "hw.gputelemetry:" && $3 == "driver=i915" {
		for (i = 2; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
		n++; t[n] = f["time_ns"]; s[n] = f["sleep_ns"]; h[n] = f["awake_holds"]
	}
	END {
		if (n < 2) { print "no i915 telemetry"; exit }
		if (t[2] > t[1]) printf "RC6 %.1f %% of %.1f s, forcewake held %s\n", (s[2] - s[1]) * 100 / (t[2] - t[1]), (t[2] - t[1]) / 1e9, h[2]
		else print "no time between the readings"
	}' "$LOGS/$1" "$LOGS/$2"
}
# One 60 s idle window: the package power, the package C-states, the GT's RC6.
window() {
	gt "$1-gt0.txt"
	TO=90 th "power -i 60" "$1-power.txt" &
	TO=90 th "idle package 0 -i 60" "$1-package.txt" &
	wait
	gt "$1-gt1.txt"
	say "$1: $(grep -h '^power' "$LOGS/$1-power.txt" | grep -i package | sed 's/^power [0-9]* //' | tr '\n' ' ')| $(grep -h '^state' "$LOGS/$1-package.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ' ')| $(rc6_share "$1-gt0.txt" "$1-gt1.txt")"
}

# 1. The kernel and the watchdog on p4, zedBSD once.
TO=15 lin 'uname -sr' | grep -q Linux || { say "FAIL: the 5330 is not on Debian"; exit 1; }
say "$NAME start $(date '+%F %T') on $(TO=15 lin 'uname -sr')"
DH=$(TO=15 lin 'echo $HOME')
case $DH in /home/*) ;; *) say "FAIL: the Debian home ($DH)"; exit 1 ;; esac
orig=$(TO=15 lin 'sha256sum ws245/vmunix-p027-orig' | cut -c1-16)
[ "$orig" = "$ORIG_SHA" ] || { say "FAIL: the kept p4 vmunix is $orig, not $ORIG_SHA"; exit 1; }
TO=15 lin 'test -f ws245/zedbsd.cfg-p027-orig' || { say "FAIL: no kept zedbsd.cfg"; exit 1; }
TO=120 timeout 120 scp -q "$VMUNIX" "$HOST:ws245/vmunix-b424-$NAME" || { say "FAIL: copy"; exit 1; }
lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-b424-$NAME" > /dev/null || { say "FAIL: kernel install"; restore; exit 1; }
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
		debian_back
		restore
		exit 1
	fi
	sleep 5
done
say "zedBSD up after $(($(date +%s) - start + 20)) s"
for t in sleepctl sysctl thermal wifi netsleep; do
	TO=60 timeout 60 sshpass -p "$ZBU" scp $SO -q "$STATIC/$t" "$ZBU@$ZB:/tmp/b424-$t" || fail "copy of $t"
done
zb 'chmod 755 /tmp/b424-sleepctl /tmp/b424-sysctl /tmp/b424-thermal /tmp/b424-wifi /tmp/b424-netsleep'
zb 'dmesg' > "$LOGS/boot.txt" 2>&1
say "boot: $(grep -h 'watchdog: armed' "$LOGS/boot.txt" | head -1)"
say "boot: $(grep -h 'vmd: root port\|vmd: .*resumed' "$LOGS/boot.txt" | head -3 | tr '\n' ';')"
TO=20 zb 'ls /dev' > "$LOGS/dev.txt" 2>&1
disk=$(grep -E '^(nvme[0-9]+n[0-9]+|nvd[0-9]+|nda[0-9]+)$' "$LOGS/dev.txt" | head -1)
say "raw disk: /dev/$disk"
th "strategy balanced" strategy.txt
sleep "$SETTLE"

# 2. The usual idle, as WS333 p029 measured it.
window idle1
window idle2
TO=20 zb 'ifconfig -a' > "$LOGS/ifconfig.txt" 2>&1
: > "$LOGS/quiet.txt"
for w in $(grep -o '^wlan[0-9]*' "$LOGS/ifconfig.txt" | sort -u); do
	TO=30 zr "/tmp/b424-wifi $w down; echo wifi=\$?; ifconfig $w down; echo ifconfig=\$?" >> "$LOGS/quiet.txt" 2>&1
done
TO=30 zr 'service stop audiod; echo audiod=$?' >> "$LOGS/quiet.txt" 2>&1
say "quiet: $(tr '\n' ' ' < "$LOGS/quiet.txt")"
sleep 10
window quiet

# 3. The sleeps.
TO=20 zb 'ps -A' > "$LOGS/ps-before.txt" 2>&1
n=1
for mode in $SEQ; do
	lines=$(TO=20 zb 'dmesg | wc -l' | tr -d ' ')
	say "sleep $n: sleepctl $mode at $(date '+%T')"
	TO=20 zr "rm -f /tmp/b424-s$n.txt; nohup sh -c \"/tmp/b424-netsleep prepare; sync; sleep 2; date +%s; /tmp/b424-thermal meters; /tmp/b424-thermal idle package 0; /tmp/b424-sleepctl $mode; date +%s; /tmp/b424-thermal meters; /tmp/b424-thermal idle package 0; /tmp/b424-netsleep end\" > /tmp/b424-s$n.txt 2>&1 < /dev/null &" > /dev/null 2>&1
	began=$(date +%s)
	sleep 15
	outcome=
	while [ $(($(date +%s) - began)) -lt 420 ]; do
		if TO=8 zb "cat /tmp/b424-s$n.txt" > "$LOGS/s$n.txt" 2> /dev/null && [ "$(grep -c '^idle package' "$LOGS/s$n.txt")" = 2 ]; then
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
		sleep 5
		TO=8 zb "cat /tmp/b424-s$n.txt" > "$LOGS/s$n.txt" 2> /dev/null
		;;
	debian)
		say "FAIL: sleep $n: back on Debian $(($(date +%s) - began)) s after the request (the watchdog reset the 5330)"
		restore
		exit 1
		;;
	*)
		say "FAIL: sleep $n: neither zedBSD nor Debian in 7 minutes"
		debian_back
		restore
		exit 1
		;;
	esac
	TO=30 zb "dmesg | tail -n +$((lines + 1))" > "$LOGS/s$n-dmesg.txt" 2>&1
	say "sleep $n: $(grep -h '^sleep\|^netsleep' "$LOGS/s$n.txt" | tr '\n' ' ')"
	say "sleep $n kernel: $(grep -E 'sleep:|vmd:|pci:|nvme:|intel-ax211|i915: (suspend|resume|unpark|park)|watchdog|fault|panic|trap|acpi: test' "$LOGS/s$n-dmesg.txt" | head -40 | tr '\n' ';')"
	# The package's energy and C-state residency over the sleep, by zedBSD's clock around it.
	say "sleep $n package: $(awk '
		$1 == "meter" && $3 ~ /package/ { for (i = 4; i <= NF; i++) if ($i ~ /^energy=/) { split($i, kv, "="); e[++ne] = kv[2] } }
		$1 == "state" { split($3, kv, "="); k = $2; if (k in r) d[k] = kv[2] - r[k]; else { r[k] = kv[2]; o[++no] = k } }
		/^[0-9]+$/ { c[++nc] = $1 }
		END {
			s = c[2] - c[1]; if (s <= 0) { print "no time"; exit }
			printf "%d s, ", s
			if (ne == 2) printf "%.2f W, ", (e[2] - e[1]) / s
			for (i = 1; i <= no; i++) printf "%s %.1f %% ", o[i], d[o[i]] / (s * 1e7)
		}' "$LOGS/s$n.txt")"
	# The root disk: a file written and synced, then 64 MiB read raw from the NVMe.
	TO=60 zb "dd if=/dev/zero of=\$HOME/b424-$n.bin bs=65536 count=128 2>&1; sync; cksum \$HOME/b424-$n.bin" > "$LOGS/s$n-disk.txt" 2>&1
	if [ -n "$disk" ]; then
		TO=90 zr "dd if=/dev/$disk of=/dev/null bs=1048576 count=64 skip=$((n * 512)) 2>&1; echo dd=\$?" >> "$LOGS/s$n-disk.txt" 2>&1
	fi
	say "sleep $n disk: $(tr '\n' ' ' < "$LOGS/s$n-disk.txt" | cut -c1-300)"
	gt "s$n-gt.txt"
	TO=20 zb 'ps -A' > "$LOGS/s$n-ps.txt" 2>&1
	say "sleep $n gpu: $(grep i915 "$LOGS/s$n-gt.txt" | cut -c1-160); session: $(grep -E -o 'keiland[a-z-]*|sessiond' "$LOGS/s$n-ps.txt" | sort -u | tr '\n' ' ')processes $(grep -c . "$LOGS/s$n-ps.txt") (before $(grep -c . "$LOGS/ps-before.txt"))"
	n=$((n + 1))
	sleep 10
done

# 4. A window after the sleeps, then Debian and p4 as it was.
window after
zb 'dmesg' > "$LOGS/end-dmesg.txt" 2>&1
to_debian
restore
say "done $(date '+%F %T')"
