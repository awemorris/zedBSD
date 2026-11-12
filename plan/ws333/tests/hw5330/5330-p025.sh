#!/bin/sh
# ws333 p025: the 5330's CPU readings again, after p014's were found to be taken the wrong way, end to end (deploy,
# boot, run, collect, summary), under /tmp/i915-hw.lock.  One run; the summary is LOGS/summary.txt.
#   sh plan/ws333/tests/hw5330/5330-p025.sh KDIR NAME
# KDIR holds the kernels of kbuild.sh: KDIR/diag (the tree with limit-diag.patch), KDIR/c6 (the tree as it is, which
# p4 keeps at the end) and, with NOI915=1, KDIR/noi915 (CONFIG_DRIVER_PCI_I915=n with the patch).  THERMAL (default
# KDIR/static/thermal, thermal-static.sh) is this tree's /sbin/thermal, whose "thermal cpu" samples every CPU.
# What is read, and how p014 differs:
#  1. Debian first (msr-debian.py, only reads): E2H on every CPU (the package C-state limit, its lock, the
#     demotion bits), the package power limit, 64FH; 60 s of idle (the package power and the PC2..PC10 and CPU 0's
#     C-state residency from the same registers zedBSD reads); 20 s with every CPU loaded under Debian's own
#     setting and under EPP performance (each CPU's MHz from APERF/MPERF, 64FH, the package power).
#  2. zedBSD with the diag kernel: each strategy with one spinning process, "thermal cpu" (a fresh 100 ms sample of
#     every CPU, unlike p014's last-sample reads) four times 5 s apart, each CPU's MHz set against the ceiling its
#     domain's max gives (max x highest_mhz / highest: HWP's levels are abstract units, a P-core's and an E-core's
#     differ); a direct max of 500 permille of every domain ("cpu set all" takes permille; p014 gave it units);
#     the E-cores of one module with different maxima under load (a shared clock makes them equal); every CPU
#     loaded under performance, balanced and powersave with 64FH (firmware_id of "thermal cpu 0 sample") and the
#     package power; 60 s of idle with E2H as zedBSD sees it (voltage_group).
#  3. NOI915=1 only: zedBSD without i915 (the display left as the firmware set it), 60 s of idle, to tell the
#     display's part in the package C-state from the rest.  Opt-in: the 5330 has no remote power, and this kernel
#     has not booted there before.
# PARTS (default "debian strategies module load idle") picks the sections: debian is 1, the others the parts of 2.
# Each ssh reads no input (-n): a command in a "while read" loop would otherwise eat the loop's list.
# The last line is "5330-p025: done" or "5330-p025: FAIL ...".  Nothing is removed on either machine; p4 keeps
# KDIR/c6 at the end; p014's ~/ws245/vmunix-p014-orig on Debian is the kernel from before p014.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
KDIR=${1:?KDIR}
NAME=${2:?NAME}
cd "$(dirname "$0")/../../../.."
THERMAL=${THERMAL:-$KDIR/static/thermal}
LOGS=${LOGS:-build/hw5330/ws333}/$NAME
HOST=solaris10-man
ZB=10.0.30.3
ZBU=user
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
T=/tmp/p025-thermal
for k in diag c6; do
	[ -f "$KDIR/$k/vmunix" ] || { echo "5330-p025: FAIL: no $KDIR/$k/vmunix"; exit 1; }
done
[ "${NOI915:-0}" = 1 ] && { [ -f "$KDIR/noi915/vmunix" ] || { echo "5330-p025: FAIL: no $KDIR/noi915/vmunix"; exit 1; }; }
[ -x "$THERMAL" ] || { echo "5330-p025: FAIL: no $THERMAL"; exit 1; }
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"

PARTS=${PARTS:-debian strategies module load idle}
part() {
	case " $PARTS " in
	*" $1 "*) return 0 ;;
	esac
	return 1
}
say() {
	echo "5330-p025: $*" | tee -a "$SUMMARY"
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
	say "zedBSD up after $(($(date +%s) - start + 20)) s"
}
# Puts KERNEL on the NVMe's p4.
kernel() {
	TO=120 timeout 120 scp -q "$1" "$HOST:ws245/vmunix-p025-$NAME" || { say "FAIL: copy"; exit 1; }
	lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-p025-$NAME" > /dev/null || { say "FAIL: kernel install"; exit 1; }
	say "kernel $(sha256sum "$1" | cut -c1-16) on p4 ($1)"
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
# Runs msr-debian.py as root on Debian into LOGS/FILE.
dm() {
	TO=${TO:-120} lin "sudo -n python3 -I \$HOME/ws245/msr-debian.py $1" > "$LOGS/$2" 2>&1
}
# A busy loop on every CPU of zedBSD for 20 s, in the background here.
loadall() {
	TO=50 zb "n=\$(sysctl hw.ncpu | sed 's/.*: *//'); i=0; while [ \$i -lt \$n ]; do timeout 20 sh -c 'while :; do :; done' > /dev/null 2>&1 < /dev/null & i=\$((i + 1)); done; wait" > /dev/null 2>&1 &
}
# One spinning process on zedBSD for 25 s, in the background here.
spin1() {
	TO=45 zb "timeout 25 sh -c 'while :; do :; done' > /dev/null 2>&1 < /dev/null" > /dev/null 2>&1 &
}
# Sets each CPU of a sampled list (FILE2) against the ceiling of its domain's max (FILE1, "thermal cpu domains"):
# the CPU nearest its ceiling and the CPUs above it (5 % and 50 MHz of slack).
comply() {
	awk '
	FNR == NR && $1 == "domain" {
		for (i = 3; i <= NF; i++) {
			split($i, kv, "=")
			if (kv[1] == "levels") { split(kv[2], lv, "\\.\\."); high[$2] = lv[2] }
			if (kv[1] == "max") max[$2] = kv[2]
		}
		next
	}
	$1 == "cpu" {
		for (i = 3; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
		d = f["domain"]; hm = f["highest_mhz"]; m = f["mhz"]
		if (hm == 0 || high[d] == 0 || m == 0) next
		ceil = max[d] * hm / high[d]
		r = m / ceil
		if (r > best) { best = r; who = sprintf("cpu%s(%s) %d/%d MHz", $2, f["core"], m, ceil) }
		if (m > ceil * 1.05 + 50) over = over sprintf(" cpu%s(%s) %d>%d", $2, f["core"], m, ceil)
	}
	END { printf "nearest %s ratio %.2f; above ceiling:%s\n", who, best, (over == "" ? " none" : over) }
	' "$1" "$2"
}
# The CPUs while one process spins: four fresh lists 5 s apart, each set against the domains' ceilings.
spun() {
	th "cpu domains" "$1-domains.txt"
	spin1
	highs=
	for k in 1 2 3 4; do
		sleep 5
		th "cpu" "$1-cpu$k.txt"
		highs="$highs $(sed -n 's/.* mhz=\([0-9]*\).*/\1/p' "$LOGS/$1-cpu$k.txt" | sort -n | tail -1)"
		say "$1 read $k: $(comply "$LOGS/$1-domains.txt" "$LOGS/$1-cpu$k.txt")"
	done
	wait
	say "$1: request $(sed -n 's/^domain \([0-9]*\) .* min=\([0-9]*\) max=\([0-9]*\) desired=\([0-9]*\) epp=\([0-9]*\).*/d\1:\2-\3,e\5/p' "$LOGS/$1-domains.txt" | sort -u -t: -k2 | tr '\n' ' ')"
	say "$1: highest mhz of each read:$highs"
}
# Every CPU loaded under STRATEGY: a fresh list, CPU 0's limit reasons and the package power over 5 s.
loaded() {
	th "strategy $1" "load-$1-set.txt"
	sleep 3
	loadall
	sleep 6
	TO=30 th "power -i 5" "load-$1-power.txt" &
	th "cpu 0 sample" "load-$1-cpu0.txt"
	th "cpu" "load-$1-cpu.txt"
	wait
	say "load $1: $(sed -n 's/^cpu \([0-9]*\) core=\([A-Z]*\) .* mhz=\([0-9]*\).*/\1:\2:\3/p' "$LOGS/load-$1-cpu.txt" | tr '\n' ' ')"
	say "load $1: 64FH=$(sed -n 's/^firmware_id //p' "$LOGS/load-$1-cpu0.txt") flags=$(sed -n 's/^flags //p' "$LOGS/load-$1-cpu0.txt") $(grep -h '^power' "$LOGS/load-$1-power.txt" | sed 's/^power [0-9]* //' | grep package | tr '\n' ' ')"
}
# One idle window of 60 s on zedBSD: the package power, the package's residency and CPU 0's.
window() {
	TO=90 th "power -i 60" "$1-power.txt" &
	TO=90 th "idle package 0 -i 60" "$1-idle-package.txt" &
	TO=90 th "idle cpu 0 -i 60" "$1-idle-cpu0.txt" &
	TO=100 zr "n=\$(sysctl hw.ncpu | sed \"s/.*: *//\"); i=0; while [ \$i -lt \$n ]; do $T idle cpu \$i | sed \"s/^/\$i /\"; i=\$((i + 1)); done; sleep 60; i=0; while [ \$i -lt \$n ]; do $T idle cpu \$i | sed \"s/^/\$i /\"; i=\$((i + 1)); done" > "$LOGS/$1-idle-cpus.txt" 2>&1 &
	wait
	say "$1 power: $(grep -h '^power' "$LOGS/$1-power.txt" | sed 's/^power [0-9]* //' | tr '\n' ';')"
	say "$1 package: $(grep -h '^state' "$LOGS/$1-idle-package.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
	say "$1 cpu0: $(grep -h '^state' "$LOGS/$1-idle-cpu0.txt" | sed 's/residency_ns=[0-9]* //' | tr '\n' ';')"
	# Each CPU's core states over the same 60 s, from two cumulative readings.
	say "$1 cpus (core state % of 60 s): $(awk '$2 == "state" { split($4, kv, "="); k = $1 " " $3; if (k in a) printf "%s:%s=%.1f ", $1, $3, (kv[2] - a[k]) / 6e8; else a[k] = kv[2] }' "$LOGS/$1-idle-cpus.txt")"
}

TO=60 timeout 60 scp -q plan/tools/hw5330/5330-nvme-write.sh plan/tools/hw5330/ufs-grow.py plan/ws333/tests/hw5330/msr-debian.py "$HOST:ws245/" ||
	{ echo "5330-p025: FAIL: copy of the helpers"; exit 1; }
exec 9> /tmp/i915-hw.lock
flock 9
say "$NAME start $(date '+%F %T')"

# 1. Debian: the registers, the idle window and the loaded CPUs.
if part debian; then
	debian_up
	say "p4's kernel kept from before p014: $(TO=15 lin 'cksum < $HOME/ws245/vmunix-p014-orig' 2>&1)"
	lin 'sudo -n modprobe msr' > /dev/null 2>&1 || { say "FAIL: modprobe msr on Debian"; exit 1; }
	dm info debian-info.txt
	say "debian: $(grep -v '^cpu' "$LOGS/debian-info.txt" | tr '\n' ';')"
	say "debian E2H: $(sed -n 's/^cpu \([0-9]*\) cst_config \(0x[0-9a-f]*\) \(.*\)/\1:\3/p' "$LOGS/debian-info.txt" | sort -t: -k2 -u | tr '\n' ';')"
	lin 'for d in /sys/class/powercap/intel-rapl:*; do [ -f $d/name ] || continue; for c in $d/constraint_*_name; do n=${c%_name}; echo "$(cat $d/name) $(cat $c) $(cat ${n}_power_limit_uw 2> /dev/null) uW"; done; done' > "$LOGS/debian-powercap.txt" 2>&1
	say "debian powercap: $(tr '\n' ';' < "$LOGS/debian-powercap.txt")"
	sleep "${IDLE_SETTLE:-60}"
	TO=120 dm "idle 60" debian-idle.txt
	say "debian idle: $(tr '\n' ';' < "$LOGS/debian-idle.txt")"
	lin 'cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null' > "$LOGS/debian-epp.txt" 2>&1
	for mode in default performance; do
		if [ "$mode" = performance ]; then
			lin 'for f in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do echo performance | sudo -n tee $f > /dev/null; done' > /dev/null 2>&1
		fi
		TO=60 lin 'n=$(nproc); i=0; while [ $i -lt $n ]; do timeout 25 sh -c "while :; do :; done" & i=$((i + 1)); done; wait' > /dev/null 2>&1 &
		sleep 8
		TO=60 dm "load 5" "debian-load-$mode.txt"
		wait
		say "debian load ($mode, EPP $(head -1 "$LOGS/debian-epp.txt") before): $(tr '\n' ' ' < "$LOGS/debian-load-$mode.txt")"
	done
	lin "for f in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do echo $(head -1 "$LOGS/debian-epp.txt") | sudo -n tee \$f > /dev/null; done" > /dev/null 2>&1
fi

# 2. zedBSD with the diag kernel.
kernel "$KDIR/diag/vmunix"
to_zedbsd
zb 'dmesg' > "$LOGS/diag-boot.txt" 2>&1
th "" status.txt
th "strategy balanced" strategy-first.txt
sleep 5
th "cpu" cpu-first.txt
th "cpu domains" domains-first.txt
say "after the first strategy: $(sed -n 's/^cpu \([0-9]*\) core=\([A-Z]*\) .* highest_mhz=\([0-9]*\) mhz=\([0-9]*\)/\1:\2:\3:\4/p' "$LOGS/cpu-first.txt" | tr '\n' ' ')"
for n in $(sed -n 's/^cpu \([0-9]*\) .*/\1/p' "$LOGS/cpu-first.txt"); do
	th "cpu $n" "cpu-$n.txt"
done
say "modules: $(for f in "$LOGS"/cpu-[0-9]*.txt; do printf '%s:%s:%s ' "$(sed -n 's/^cpu //p' "$f")" "$(sed -n 's/^core_class //p' "$f")" "$(sed -n 's/^module //p' "$f")"; done)"

# Each strategy with one process spinning.
if part strategies; then
	for s in performance balanced powersave "manual 100 50 50" "manual 0 50 50"; do
		tag=$(echo "$s" | tr ' ' '-')
		th "strategy $s" "strategy-$tag.txt"
		sleep 3
		spun "strategy-$tag"
	done

	# A direct max halfway along every domain's levels (500 permille), then the strategy again.
	th "strategy balanced" strategy-direct-base.txt
	sleep 2
	th "cpu set all max=500" direct-set.txt
	sleep 3
	say "direct all max=500: $(tr '\n' ' ' < "$LOGS/direct-set.txt")"
	spun direct-max500
fi

# The E-cores of one module: the first at its highest, the others at their efficient level, every CPU loaded.
if part module; then
	th "strategy balanced" strategy-module-base.txt
	sleep 2
	emod=
	for f in "$LOGS"/cpu-[0-9]*.txt; do
		[ "$(sed -n 's/^core_class //p' "$f")" = E ] || continue
		emod=$(sed -n 's/^module //p' "$f")
		break
	done
	members=
	for f in "$LOGS"/cpu-[0-9]*.txt; do
		[ "$(sed -n 's/^core_class //p' "$f")" = E ] || continue
		[ "$(sed -n 's/^module //p' "$f")" = "$emod" ] && members="$members $(sed -n 's/^cpu //p' "$f")"
	done
	set -- $members
	if [ $# -ge 2 ]; then
		target=$1
		tdom=$(sed -n 's/^domain //p' "$LOGS/cpu-$target.txt")
		for f in "$LOGS"/cpu-[0-9]*.txt; do
			d=$(sed -n 's/^domain //p' "$f")
			if [ "$d" = "$tdom" ]; then
				v=$(sed -n 's/^highest //p' "$f")
			else
				v=$(sed -n 's/^efficient //p' "$f")
			fi
			echo "$d $v"
		done | sort -u -n > "$LOGS/module-max.txt"
		while read -r d v; do
			th "cpu set $d max=$v" "module-set-$d.txt"
		done < "$LOGS/module-max.txt"
		sleep 3
		th "cpu domains" module-domains.txt
		loadall
		sleep 8
		ecpus=
		for f in "$LOGS"/cpu-[0-9]*.txt; do
			[ "$(sed -n 's/^core_class //p' "$f")" = E ] && ecpus="$ecpus $(sed -n 's/^cpu //p' "$f")"
		done
		for n in $ecpus; do
			th "cpu $n sample" "module-cpu$n.txt"
		done
		wait
		# The other modules' E-cores, all at efficient, are the reference for a CPU held at its own max.
		say "module $emod (cpus$members; cpu $target at its highest, every other E-core at efficient; every CPU loaded): $(for n in $ecpus; do printf 'cpu%s(m%s,max %s):%s ' "$n" "$(sed -n 's/^module //p' "$LOGS/cpu-$n.txt")" "$(sed -n "s/^domain $(sed -n 's/^domain //p' "$LOGS/cpu-$n.txt") .* max=\([0-9]*\) .*/\1/p" "$LOGS/module-domains.txt")" "$(sed -n 's/^delivered_mhz //p' "$LOGS/module-cpu$n.txt")"; done)"
	else
		say "module: no E-core module of two CPUs found"
	fi
fi

# Every CPU loaded under three strategies.
if part load; then
	for s in performance balanced powersave; do
		loaded "$s"
	done
fi

# Idle under balanced, with E2H as zedBSD reads it.
if part idle; then
	th "strategy balanced" strategy-idle.txt
	sleep "${IDLE_SETTLE:-60}"
	window diag-balanced
	th "cpu 0 sample" idle-cpu0.txt
	say "zedBSD E2H on cpu 0: $(sed -n 's/^voltage_group //p' "$LOGS/idle-cpu0.txt") 64FH: $(sed -n 's/^firmware_id //p' "$LOGS/idle-cpu0.txt")"
fi
zb 'dmesg' > "$LOGS/diag-end.txt" 2>&1
to_debian

# 3. Optional: zedBSD without i915.
if [ "${NOI915:-0}" = 1 ]; then
	kernel "$KDIR/noi915/vmunix"
	to_zedbsd
	zb 'dmesg' > "$LOGS/noi915-boot.txt" 2>&1
	th "strategy balanced" noi915-strategy.txt
	sleep "${IDLE_SETTLE:-60}"
	window noi915-balanced
	to_debian
fi

# p4 keeps the tree's kernel.
kernel "$KDIR/c6/vmunix"
say "done $(date '+%F %T')"
