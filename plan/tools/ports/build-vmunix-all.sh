#!/bin/sh
# Builds the kernel of every port and prints one line per port:
#   <port> rc=<make status> warnings=<count> <log>
# Usage: build-vmunix-all.sh <out-dir under build/> [port...]
# The ports run four at a time, each with make -j16, and each make has a
# 30-minute limit.
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:?out dir}
shift
cd "$repo" || exit 1
all="amd64:config/ci/config-amd64.mk
intelmac:config/ci/config-intelmac.mk
pcat:config/ci/config-pcat.mk
pc98:config/ci/config-pc98.mk
rpi4:config/ci/config-rpi4.mk
rpi5:config/ci/config-rpi5.mk
odroid-c5:config/ci/config-odroid-c5.mk
visionfive2:config/ci/config-visionfive2.mk
zybo:config/ci/config-zybo.mk
armv7-virt:config/ci/config-armv7-virt.mk
riscv64-virt:config/ci/config-riscv64-virt.mk
riscv32-virt:config/ci/config-riscv32-virt.mk
m68k-virt:config/ci/config-m68k-virt.mk
ppc32-mac99:config/ci/config-ppc32-mac99.mk
ppc64-pseries:config/ci/config-ppc64-pseries.mk
mips32-malta:plan/tools/ports/config-mips32-malta.mk
mips64-malta:plan/tools/ports/config-mips64-malta.mk
mips32-rg350:plan/ws273/tests/config-mips32-rg350.mk
sun4u:plan/tools/ports/config-sun4u.mk
x68k:plan/tools/ports/config-x68k.mk"
if [ $# -gt 0 ]; then
	sel=""
	for p in "$@"; do
		sel="$sel$(printf '%s\n' "$all" | grep "^$p:")
"
	done
	all=$sel
fi
mkdir -p "$out"
printf '%s\n' "$all" | grep . | xargs -P4 -I{} sh -c '
	port=${1%%:*}; config=${1#*:}; out=$2
	log=$out/$port.log
	timeout 1800 make -j16 ZEDBSD_CONFIG=$config BUILD=$out/$port $out/$port/vmunix >"$log" 2>&1
	rc=$?
	warnings=$(grep -c "warning:" "$log")
	echo "$port rc=$rc warnings=$warnings $log"
' sh {} "$out" | sort
