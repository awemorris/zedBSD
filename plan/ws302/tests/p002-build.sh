#!/bin/sh
# The kernel builds of the balancer's statistics: for each platform, the kernel without the balancer (CONFIG_KERN_DESKTOP_BALANCER n,
# the default) and with it (CONFIG_KERN_DESKTOP_BALANCER=y through desktop-balancer-overlay.mk until mk/kernel.mk has the rule), each with its
# warnings counted; and, when REF holds a kernel of the same platform built from the base commit with the same
# configuration (REF/<platform>.vmunix, and REF/<platform>.elf for the boards that link one first) and REF/commit
# names that commit, whether the kernel without the balancer is the same file, and if not, whether its disassembly
# is the same but for the line numbers its fatal checks pass (dis-compare.py).
#
#   plan/ws302/tests/p002-build.sh OUT [REF] [PLATFORM...]
#
# PLATFORM is x86_64, rpi4, rpi5 or vf2 (default x86_64 rpi5 vf2), built in build/<platform> with
# config/config.<platform>.mk, whose sysroot must already be made (make CONFIGMK=... sysroot).  x86_64 is also built
# with the balancer and the thermal core and the x86 power control, the configuration of the 5330's measurements.
# Each line of the summary is "<platform> <variant> rc=<n> warnings=<n>" or "<platform> n-vs-base <same|...>"; the
# last line is "p002-build: PASS" when every build succeeded without a warning and every comparison found the same.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
[ $# -ge 1 ] || { echo "usage: p002-build.sh OUT [REF] [PLATFORM...]" >&2; exit 2; }
out=$1
shift
ref=
if [ $# -ge 1 ] && [ -d "$1" ]; then
	ref=$1
	shift
fi
platforms=${*:-x86_64 rpi5 vf2}
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"
status=PASS
objdump=build/llvm/bin/llvm-objdump

# Builds one kernel and keeps it and its log; $1 platform, $2 variant name, the rest make arguments.
build_one() {
	platform=$1
	variant=$2
	shift 2
	log=$out/$platform-$variant.log
	timeout 3600 make -j4 CONFIGMK=config/config.$platform.mk "$@" build/$platform/vmunix > "$log" 2>&1 < /dev/null
	rc=$?
	warnings=$(grep -c 'warning:' "$log")
	echo "$platform $variant rc=$rc warnings=$warnings" >> "$summary"
	if [ $rc -ne 0 ] || [ "$warnings" -ne 0 ]; then
		status=FAIL
		return
	fi
	cp build/$platform/vmunix "$out/$platform-$variant.vmunix"
	if [ -f build/$platform/kernel.elf ]; then
		cp build/$platform/kernel.elf "$out/$platform-$variant.elf"
	fi
}

# Compares the kernel without the balancer with the base commit's; $1 platform.
compare_base() {
	platform=$1
	[ -n "$ref" ] && [ -f "$ref/$platform.vmunix" ] || return
	if cmp -s "$ref/$platform.vmunix" "$out/$platform-n.vmunix"; then
		echo "$platform n-vs-base same-file" >> "$summary"
		return
	fi
	elf=$out/$platform-n.vmunix
	base=$ref/$platform.vmunix
	if [ -f "$out/$platform-n.elf" ] && [ -f "$ref/$platform.elf" ]; then
		elf=$out/$platform-n.elf
		base=$ref/$platform.elf
	fi
	"$objdump" -d --no-show-raw-insn "$base" | tail -n +3 > "$out/$platform-base.dis"
	"$objdump" -d --no-show-raw-insn "$elf" | tail -n +3 > "$out/$platform-n.dis"
	if cmp -s "$out/$platform-base.dis" "$out/$platform-n.dis"; then
		echo "$platform n-vs-base same-text (the files differ outside the code)" >> "$summary"
		return
	fi

	# The same instructions at the same addresses, differing only in the line numbers the fatal checks pass (a
	# source line added above a check moves its __LINE__), are the same kernel.  The lines that differ are kept.
	verdict=$(python3 -I plan/ws302/tests/dis-compare.py "$out/$platform-base.dis" "$out/$platform-n.dis" \
		"$out/$platform-constants.diff" "$(cat "$ref/commit")")
	echo "$platform n-vs-base $verdict" >> "$summary"
	case $verdict in
	same-text*) ;;
	*) status=FAIL ;;
	esac
}

# Builds every platform without and with the balancer.
for platform in $platforms; do
	build_one "$platform" n
	compare_base "$platform"
	build_one "$platform" y -f Makefile -f plan/ws302/tests/desktop-balancer-overlay.mk CONFIG_KERN_DESKTOP_BALANCER=y
	if [ "$platform" = x86_64 ]; then
		build_one "$platform" y-thermal -f Makefile -f plan/ws302/tests/desktop-balancer-overlay.mk CONFIG_KERN_DESKTOP_BALANCER=y \
			CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y
	fi
done
sync

# Reports.
cat "$summary"
echo "p002-build: $status"
