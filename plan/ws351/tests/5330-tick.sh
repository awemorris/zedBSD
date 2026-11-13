#!/bin/sh
# ws351: the variable tick on the Latitude 5330, end to end (deploy, boot, run, collect, summary), under
# /tmp/i915-hw.lock.  One run; the summary is LOGS/summary.txt, its last line "5330-tick: done" or "5330-tick: FAIL ...".
#   sh plan/ws351/tests/5330-tick.sh VMUNIX DIAG STATIC NAME
# VMUNIX is the tree's kernel (x86_64, CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y); DIAG the same with
# tick-count.patch (each CPU's timer interrupts and its slowdown, logged every 10 s).  STATIC holds the tree's static
# thermal, sysctl, wifi (plan/ws333/tests/hw5330/thermal-static.sh, static-tool.sh) and clocktool
# (plan/ws351/tests/clocktool): the NVMe's userland predates the tree's libc.  The 5330 must be on Debian.  Every
# zedBSD boot is a one-time UEFI boot (~/boot-zedbsd.sh) with the TCO watchdog on (watchdog=600): a zedBSD that
# stops is reset to Debian without a person.  No S0 idle and no sleepctl.
#  1. Keeps p4's vmunix and zedbsd.cfg once as ~/ws245/*-ws351-orig (they must be ORIG_SHA and CFG_SHA).
#  2. tree: VMUNIX on p4, zedBSD as it boots (i915 started), the thermal strategy balanced, after SETTLE seconds two
#     60 s idle windows (the package power, the package C-states, CPU 0's and every CPU's, the GT's RC6 from
#     hw.gputelemetry; as WS333 p029 measured with 5330-p027.sh NOPARK=1 QUIET=1 PARTS=held), a third with the
#     Wi-Fi radios down and audiod stopped, then clocktool res and clocktool sleep 200 1000 idle and with every CPU
#     busy (clocktool spin on each).
#  3. diag: DIAG on p4, after SETTLE seconds the kernel's per-CPU interrupt counts of an idle 10 s and of a 10 s
#     with every CPU busy.
#  4. p4 gets the original vmunix and zedbsd.cfg back; their sha256 are checked.
# PARTS (default "tree diag") picks 2 and 3.  STOPCOMP=1 adds to 2 a window with the compositor stopped (SIGSTOP).  Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
VMUNIX=${1:?VMUNIX}
DIAG=${2:?DIAG}
STATIC=${3:?STATIC}
NAME=${4:?NAME}
cd "$(dirname "$0")/../../.."
LOGS=${LOGS:-build/hw5330/ws351}/$NAME
PARTS=${PARTS:-tree diag}
WATCHDOG=600
SETTLE=${SETTLE:-60}
ORIG_SHA=4c98d617279bf96b
CFG_SHA=1b33a395b0ff4bd7
HOST=solaris10-man
ZB=10.0.30.3
ZBU=user
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
T=/tmp/ws351-thermal
SY=/tmp/ws351-sysctl
CT=/tmp/ws351-clocktool
for f in "$VMUNIX" "$DIAG" "$STATIC/sysctl" "$STATIC/thermal" "$STATIC/wifi" "$STATIC/clocktool"; do
	[ -f "$f" ] || { echo "5330-tick: FAIL: no $f"; exit 1; }
done
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"
exec 9> /tmp/i915-hw.lock
flock -w 3600 9 || { echo "5330-tick: FAIL: the lock"; exit 1; }

part() {
	case " $PARTS " in
	*" $1 "*) return 0 ;;
	esac
	return 1
}
say() {
	echo "5330-tick: $*" | tee -a "$SUMMARY"
}
lin() {
	timeout "${TO:-120}" ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
zb() {
	timeout "${TO:-60}" sshpass -p "$ZBU" ssh -n $SO "$ZBU@$ZB" "$1"
}
# As root on zedBSD.
zr() {
	zb "echo $ZBU | sudo -S sh -c '$1' 2> /dev/null"
}
debian_up() {
	start=$(date +%s)
	until TO=8 lin hostname 2> /dev/null | grep -q chaos; do
		if [ $(($(date +%s) - start)) -gt "${1:-420}" ]; then
			return 1
		fi
		sleep 5
	done
	echo $(($(date +%s) - start))
}
zedbsd_up() {
	start=$(date +%s)
	until zb 'uname -s' 2> /dev/null | grep -qi zedbsd; do
		if [ $(($(date +%s) - start)) -gt 300 ]; then
			return 1
		fi
		sleep 5
	done
	say "zedBSD up after $(($(date +%s) - start + 20)) s"
}
restore() {
	lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/vmunix-ws351-orig /mnt/zb/vmunix && cp $DH/ws245/zedbsd.cfg-ws351-orig /mnt/zb/zedbsd.cfg && sync && sha256sum /mnt/zb/vmunix /mnt/zb/zedbsd.cfg && umount /mnt/zb'" > "$LOGS/restore.txt" 2>&1
	v=$(awk 'NR == 1 { print substr($1, 1, 16) }' "$LOGS/restore.txt")
	c=$(awk 'NR == 2 { print substr($1, 1, 16) }' "$LOGS/restore.txt")
	if [ "$v" = "$ORIG_SHA" ] && [ "$c" = "$CFG_SHA" ]; then
		say "p4 restored: vmunix $v zedbsd.cfg $c"
	else
		say "FAIL: p4 not restored: vmunix $v zedbsd.cfg $c ($LOGS/restore.txt)"
	fi
}
# Gives up: Debian back if it can be, p4 restored, the reason last.
fail() {
	say "FAIL: $*"
	if ! TO=8 lin hostname 2> /dev/null | grep -q chaos; then
		zr 'reboot' > /dev/null 2>&1
		sleep 20
		debian_up 600 > /dev/null || { say "FAIL: Debian did not come back; the watchdog (${WATCHDOG} s) should reset zedBSD, else a person"; exit 2; }
	fi
	restore
	say "FAIL: $*"
	exit 1
}
# Puts a kernel on p4 and the original zedbsd.cfg with the watchdog.
install() {
	TO=120 timeout 120 scp -q "$1" "$HOST:ws245/vmunix-ws351-$NAME-$2" || fail "copy of $1"
	lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-ws351-$NAME-$2" > /dev/null || fail "kernel install ($2)"
	lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp $DH/ws245/zedbsd.cfg-ws351-orig /mnt/zb/zedbsd.cfg && echo watchdog=$WATCHDOG >> /mnt/zb/zedbsd.cfg && cat /mnt/zb/zedbsd.cfg && sha256sum /mnt/zb/vmunix && sync && umount /mnt/zb'" > "$LOGS/install-$2.txt" 2>&1 ||
		fail "zedbsd.cfg ($2)"
	say "$2: kernel $(sha256sum "$1" | cut -c1-16) on p4 ($(awk '$2 == "/mnt/zb/vmunix" { print substr($1, 1, 16) }' "$LOGS/install-$2.txt"))"
}
to_zedbsd() {
	lin 'sudo -n sh $HOME/ws245/5330-nvme-write.sh check' > /dev/null || fail "the NVMe's zedBSD partitions are not as expected"
	lin '$HOME/boot-zedbsd.sh' > "$LOGS/boot-$1.txt" 2>&1
	sleep 20
	zedbsd_up || fail "zedBSD did not answer in 5 minutes"
	for f in thermal:$T sysctl:$SY clocktool:$CT wifi:/tmp/ws351-wifi; do
		TO=60 timeout 60 sshpass -p "$ZBU" scp $SO -q "$STATIC/${f%%:*}" "$ZBU@$ZB:${f#*:}" || fail "copy of ${f%%:*}"
	done
	zb "chmod 755 $T $SY $CT /tmp/ws351-wifi"
	zb 'dmesg' > "$LOGS/$1-boot.txt" 2>&1
	say "$1: $(grep -h 'watchdog: armed' "$LOGS/$1-boot.txt" | head -1)"
	say "$1: $(grep -h 'A64 TIMER TICK source\|HAL initialized' "$LOGS/$1-boot.txt" | tr '\n' ';')"
}
to_debian() {
	zr 'reboot' > /dev/null 2>&1
	sleep 20
	took=$(debian_up 600) || { say "FAIL: Debian did not come back in 10 minutes"; exit 2; }
	say "Debian up after $took s ($(TO=15 lin 'uname -r'))"
}
th() {
	TO=${TO:-60} zr "$T $1" > "$LOGS/$2" 2>&1
}
gt() {
	TO=20 zb "$SY hw.gputelemetry" > "$LOGS/$1" 2>&1
}
rc6_share() {
	awk '
	$1 == "hw.gputelemetry:" && $3 == "driver=i915" {
		for (i = 2; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
		n++; t[n] = f["time_ns"]; s[n] = f["sleep_ns"]; v[n] = f["valid"]; h[n] = f["awake_holds"]
	}
	END {
		if (n < 2) { print "no i915 telemetry"; exit }
		if (t[2] > t[1]) printf "RC6 %.1f %% of %.1f s (valid %s/%s), forcewake domains held %s\n", (s[2] - s[1]) * 100 / (t[2] - t[1]), (t[2] - t[1]) / 1e9, v[1], v[2], h[2]
		else print "no time between the readings"
	}' "$LOGS/$1" "$LOGS/$2"
}
# One idle window of 60 s: the package power, the package's residency, CPU 0's, every CPU's, the GT's RC6.
window() {
	gt "$1-gt0.txt"
	TO=90 th "power -i 60" "$1-power.txt" &
	TO=90 th "idle package 0 -i 60" "$1-idle-package.txt" &
	TO=90 th "idle cpu 0 -i 60" "$1-idle-cpu0.txt" &
	TO=100 zr "n=\$($SY hw.ncpu | sed \"s/.*: *//\"); i=0; while [ \$i -lt \$n ]; do $T idle cpu \$i | sed \"s/^/\$i /\"; i=\$((i + 1)); done; sleep 60; i=0; while [ \$i -lt \$n ]; do $T idle cpu \$i | sed \"s/^/\$i /\"; i=\$((i + 1)); done" > "$LOGS/$1-idle-cpus.txt" 2>&1 &
	wait
	gt "$1-gt1.txt"
	say "$1 power: $(grep -h '^power' "$LOGS/$1-power.txt" | sed 's/^power [0-9]* //' | tr '\n' ';')"
	say "$1 package: $(grep -h '^state' "$LOGS/$1-idle-package.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
	say "$1 cpu0: $(grep -h '^state' "$LOGS/$1-idle-cpu0.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
	say "$1 cpus (core state % of 60 s): $(awk '$2 == "state" { split($4, kv, "="); k = $1 " " $3; if (k in a) printf "%s:%s=%.1f ", $1, $3, (kv[2] - a[k]) / 6e8; else a[k] = kv[2] }' "$LOGS/$1-idle-cpus.txt")"
	say "$1 gt: $(rc6_share "$1-gt0.txt" "$1-gt1.txt")"
}
# Every CPU busy for SECONDS while COMMAND runs as the user; its output into LOGS/FILE.
busy() {
	TO=$(($1 + 30)) zb "n=\$($SY hw.ncpu | sed \"s/.*: *//\"); i=0; while [ \$i -lt \$n ]; do $CT spin $1 > /dev/null & i=\$((i + 1)); done; sleep 2; $2; wait" > "$LOGS/$3" 2>&1
}
# The per-CPU interrupt rates of the last tickdiag report in a dmesg (10 s each).
rates() {
	awk '{ for (i = 1; i <= NF; i++) if ($i == "tickdiag:") break }
	i <= NF { t = $(i + 1); if (t != last) { last = t; line = "" }
		line = line sprintf("%s=%d/s%s ", $(i + 3), $(i + 5) / 10, $(i + 7) == "1" ? "(slow)" : ""); out = t " " line }
	END { print out }' "$LOGS/$1"
}

# 1. The original kernel and configuration, kept once.
TO=15 lin 'uname -sr' | grep -q Linux || { say "FAIL: the 5330 is not on Debian"; exit 1; }
say "$NAME start $(date '+%F %T') on $(TO=15 lin 'uname -sr')"
DH=$(TO=15 lin 'echo $HOME')
case $DH in /home/*) ;; *) say "FAIL: the Debian home ($DH)"; exit 1 ;; esac
TO=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh "$HOST:ws245/" || { say "FAIL: copy of the writer"; exit 1; }
lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && { [ -f $DH/ws245/vmunix-ws351-orig ] || cp /mnt/zb/vmunix $DH/ws245/vmunix-ws351-orig; } && { [ -f $DH/ws245/zedbsd.cfg-ws351-orig ] || cp /mnt/zb/zedbsd.cfg $DH/ws245/zedbsd.cfg-ws351-orig; } && chown $(id -un) $DH/ws245/*-ws351-orig && sync && umount /mnt/zb'" > /dev/null 2>&1 ||
	{ say "FAIL: keeping the original"; exit 1; }
orig=$(TO=15 lin 'sha256sum ws245/vmunix-ws351-orig' | cut -c1-16)
cfg=$(TO=15 lin 'sha256sum ws245/zedbsd.cfg-ws351-orig' | cut -c1-16)
[ "$orig" = "$ORIG_SHA" ] && [ "$cfg" = "$CFG_SHA" ] || { say "FAIL: p4 is vmunix $orig zedbsd.cfg $cfg, not $ORIG_SHA $CFG_SHA"; exit 1; }
say "original kept: vmunix $orig zedbsd.cfg $cfg"

# 2. The tree's kernel: idle power, the clock, sleeps idle and busy.
if part tree; then
	install "$VMUNIX" tree
	to_zedbsd tree
	th "strategy balanced" tree-strategy.txt
	sleep "$SETTLE"
	window held
	window held2
	TO=30 zr "/tmp/ws351-wifi wlan0 down; ifconfig wlan0 down; service stop audiod" > "$LOGS/quiet.txt" 2>&1
	sleep 10
	window quiet
	# STOPCOMP=1: one more window with the compositor (/bin/wayland) stopped by SIGSTOP, continued after it.
	if [ "${STOPCOMP:-0}" = 1 ]; then
		printf '%s\n' 'kill -"$1" $(ps -ef | awk '"'"'$8 == "/bin/wayland" { print $2 }'"'"')' > "$LOGS/comp.sh"
		TO=60 timeout 60 sshpass -p "$ZBU" scp $SO -q "$LOGS/comp.sh" "$ZBU@$ZB:/tmp/ws351-comp.sh" || fail "copy of comp.sh"
		TO=30 zr "sh /tmp/ws351-comp.sh STOP" > "$LOGS/stopcomp.txt" 2>&1
		zb 'ps -ef' > "$LOGS/stopcomp-ps.txt" 2>&1
		sleep 5
		window nocomp
		TO=30 zr "sh /tmp/ws351-comp.sh CONT" >> "$LOGS/stopcomp.txt" 2>&1
	fi
	TO=60 zb "$CT res" > "$LOGS/res.txt" 2>&1
	say "tree $(cat "$LOGS/res.txt")"
	TO=60 zb "$CT sleep 200 1000" > "$LOGS/sleep-idle.txt" 2>&1
	say "tree idle $(cat "$LOGS/sleep-idle.txt")"
	busy 20 "$CT sleep 200 1000" sleep-busy.txt
	say "tree busy $(grep '^sleep' "$LOGS/sleep-busy.txt")"
	zb 'dmesg' > "$LOGS/tree-end.txt" 2>&1
	to_debian
fi

# 3. The diagnostic kernel: the interrupt rates idle and busy.
if part diag; then
	install "$DIAG" diag
	to_zedbsd diag
	th "strategy balanced" diag-strategy.txt
	sleep "$SETTLE"
	zb 'dmesg' > "$LOGS/diag-idle.txt" 2>&1
	zb 'ps -ef' > "$LOGS/diag-ps.txt" 2>&1
	# With wake-reason.patch: the sleepers that kept an idle CPU fast most often in the last 10 s, by process.
	say "diag near sleepers: $(awk '{ for (i = 1; i <= NF; i++) if ($i == "neardiag:") break } i <= NF { c[$(i + 4)] += $(i + 2) } END { for (p in c) print c[p], "pid", p }' "$LOGS/diag-idle.txt" | sort -rn | head -6 | tr '\n' ';')"
	say "diag idle (t cpu=interrupts/s): $(rates diag-idle.txt)"
	busy 25 "sleep 21; dmesg > /tmp/ws351-busy.txt" diag-busy-run.txt
	zb 'cat /tmp/ws351-busy.txt' > "$LOGS/diag-busy.txt" 2>&1
	say "diag busy (t cpu=interrupts/s): $(rates diag-busy.txt)"
	sleep 15
	zb 'dmesg' > "$LOGS/diag-after.txt" 2>&1
	say "diag idle again (t cpu=interrupts/s): $(rates diag-after.txt)"
	to_debian
fi

# 4. p4 as it was.
restore
say "done $(date '+%F %T')"
sync
