#!/bin/sh
# ws134-p009: hw.thermal on QEMU's q35, which has no ACPI thermal zone nor any device with a _TMP, on the System
# Monitor's image (plan/ws134/tests/build-monitor-image.sh BUILD, then plan/tools/files/files-guest.sh start
# BUILD/hdd-image.img).  Over SSH, never the console.
#  1. sysctl hw.thermal reads, with "hw.thermal: sensors=0" (the leaf is there; nothing to list).
#  2. The monitor's backend gives no CPU temperature: the probe's samples have valid without 0x100
#     (KL_MONITOR_HAVE_TEMPERATURE) and cpu_mc=0.
# The values themselves (TCPU, TMEM, TSKN, NGFF and AMBF) are the Latitude 5330's UAT.
#   plan/ws134/tests/thermal-p009.sh [OUTDIR]
# Prints "thermal-p009: PASS" or "thermal-p009: FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws071-run}"
export GUEST_RUNTIME
out=${1:-build/ws134-p009}
. plan/tools/fresh-out.sh
fresh_out "$out"
status=0
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
pass() { echo "ok: $1"; }
fail() { echo "FAILED: $1"; status=1; }

# 1. The sysctl.
guest "sysctl hw.thermal; echo exit=\$?" > "$out/sysctl.txt"
cat "$out/sysctl.txt"
grep -q '^hw.thermal: sensors=0$' "$out/sysctl.txt" && grep -q '^exit=0' "$out/sysctl.txt" &&
    pass "hw.thermal reads, no sensor on q35" || fail "hw.thermal"

# 2. The backend's samples.
guest "/bin/monitor-probe 2 1000; echo exit=\$?" > "$out/probe.txt"
grep '^MPROBE SAMPLE' "$out/probe.txt" | head -2
samples=$(grep -c '^MPROBE SAMPLE' "$out/probe.txt")
bad=$(grep '^MPROBE SAMPLE' "$out/probe.txt" | awk '{
	for (i = 1; i <= NF; i++) {
		if ($i ~ /^valid=/) { v = substr($i, 7) }
		if ($i ~ /^cpu_mc=/) { c = substr($i, 8) }
	}
	if (and(strtonum(v), 256) != 0 || c != "0") print
}' | wc -l)
[ "$samples" -ge 2 ] && [ "$bad" -eq 0 ] && pass "no CPU temperature in the samples" || fail "samples: $samples, with a temperature: $bad"

[ $status -eq 0 ] && echo "thermal-p009: PASS" || echo "thermal-p009: FAIL"
exit $status
