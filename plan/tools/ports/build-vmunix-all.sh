#!/bin/sh
# Builds the kernel of every port and prints one line per port:
#   <port> rc=<make status> warnings=<count> <log>
# Usage: build-vmunix-all.sh <out-dir under build/> [port...]
# The platforms run one at a time, each with make -j4 (the 4 threads of the
# host rule), and each make has a 30-minute limit.
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:?out dir}
shift
cd "$repo" || exit 1
all="x86_64:config/config.x86_64.mk
i386:config/config.i386.mk
pc9821:config/config.pc9821.mk
rpi4:config/config.rpi4.mk
rpi5:config/config.rpi5.mk
vf2:config/config.vf2.mk"
if [ $# -gt 0 ]; then
	sel=""
	for p in "$@"; do
		sel="$sel$(printf '%s\n' "$all" | grep "^$p:")
"
	done
	all=$sel
fi
mkdir -p "$out"
printf '%s\n' "$all" | grep . | xargs -P1 -I{} sh -c '
	port=${1%%:*}; config=${1#*:}; out=$2
	log=$out/$port.log
	timeout 1800 make -j4 BUILD_JOBS=4 CONFIGMK=$config BUILD=$out/$port vmunix >"$log" 2>&1
	rc=$?
	warnings=$(grep -c "warning:" "$log")
	echo "$port rc=$rc warnings=$warnings $log"
' sh {} "$out" | sort
