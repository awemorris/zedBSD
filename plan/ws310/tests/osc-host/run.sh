#!/bin/sh
# The _OSC of the Dell Latitude 5330's tables (plan/tools/aml-host/latitude5330/) evaluated in the
# AML host harness with --trace and --diff, checking what each evaluation writes, loads and changes
# against the reading of the tables:
#   1. platform-wide (\_SB._OSC, CPPC and CPPC 2), with the simulated memory's zeros: the query
#      and the commit return what was asked, write nothing, and leave \_SB.OSCP 0x60;
#   2. the same with the USB4 values set (U4SE 1, U4CM 0xA1, CMSK 1): the query writes nothing,
#      the commit runs the Thunderbolt connection manager switch (Force Power on, the switch
#      failing against the simulated hardware) and still returns success;
#   3. processor (\_SB.PR00._OSC, nothing asked) after the platform commit, with CFGD bit 22 and
#      the dynamic tables placed where \SSDT lists them: the commit loads Cpu0Psd and Cpu0Hwp,
#      writes HWPV 2, and \_SB.PR00._CPC becomes a 23-element revision 3 package;
#   4. PCI Express host bridge (\_SB.PC00._OSC): the request zedBSD makes (support 0x11,
#      control 0) writes \OSCC 0 and nothing else, query and commit alike; a query asking for
#      control 0x1D already leaves \OSCC 0x18; with NEXP 1 a query asking for hot plug and PME
#      already writes the root ports' configuration.
# The simulated memory reads zero except for what --set stores, so the values are not the
# machine's (that is the next step, which reads them on the machine).
# Each check prints "<name>: PASS" or "<name>: FAIL (<file>)"; the last line is
# "osc-host: PASS" or "osc-host: FAIL".  Output under build/tmp/ws310-osc-host (a fresh run directory).
#   sh plan/ws310/tests/osc-host/run.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws310-osc-host
fresh_out "$out"
out=$(cd "$out" && pwd -P)
status=0

pass() {
	echo "$1: PASS"
}

fail() {
	echo "$1: FAIL ($2)"
	status=1
}

# Requires every pattern in an output file (in any order).
expect() {
	name=$1
	file=$2
	shift 2
	for pattern in "$@"; do
		if ! grep -q -F -- "$pattern" "$file"; then
			echo "  missing: $pattern"
			fail "$name" "$file"
			return
		fi
	done
	pass "$name"
}

# Refuses a pattern in an output file.
refuse() {
	name=$1
	file=$2
	pattern=$3
	if grep -q -F -- "$pattern" "$file"; then
		echo "  present: $pattern"
		fail "$name" "$file"
	else
		pass "$name"
	fi
}

if ! timeout 600 make -s -C plan/tools/aml-host OUT="$out/host" > "$out/build.txt" 2>&1; then
	fail build "$out/build.txt"
	echo "osc-host: FAIL"
	exit 1
fi
host="$out/host/aml-host"
dir=plan/tools/aml-host/latitude5330
tables="$dir/dsdt.dat"
for number in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
	tables="$tables $dir/ssdt$number.dat"
done
loaded="--quiet --absent-pci 0:10.6 --absent-pci 0:10.7 --init --trace --diff"

# The UUIDs in ToUUID order (the codec's bytes, checked against iasl by plan/ws310/tests/osc-codec).
platform=6EB01108274AF9448D603CBBC22E7B48
processor=16A677400C29BE479EBDD87058713953
pcie=5B4DDB33F71F1C4096577441C03DD766
pq="\\_SB._OSC b:$platform i:1 i:2 b:0100000060000000"
pc="\\_SB._OSC b:$platform i:1 i:2 b:0000000060000000"

# 1. Platform-wide, zeros.
timeout 300 "$host" $loaded --eval-args "$pq" --eval-args "$pc" $tables > "$out/platform.txt" 2>&1
expect platform "$out/platform.txt" \
    "$pq = Buffer len=8 { 01 00 00 00 60 00 00 00 }" \
    "$pc = Buffer len=8 { 00 00 00 00 60 00 00 00 }" \
    "TRACE end $pq: 0 writes, 0 loads, 0 unloads, slept 0 ms" \
    "TRACE end $pc: 0 writes, 0 loads, 0 unloads, slept 0 ms" \
    'DIFF ~ \_SB_.OSCP = Integer 0x0 -> \_SB_.OSCP = Integer 0x60' \
    'DIFF 0 added, 0 removed, 2 changed'

# 2. Platform-wide with the USB4 values: the commit alone runs the connection manager switch.
timeout 300 "$host" $loaded --set '\U4SE=i:1' --set '\U4CM=i:0xA1' --set '\CMSK=i:1' \
    --eval-args "$pq" --eval-args "$pc" $tables > "$out/usb4.txt" 2>&1
expect usb4 "$out/usb4.txt" \
    "TRACE end $pq: 0 writes, 0 loads, 0 unloads, slept 0 ms" \
    'TRACE log ACPI Debug: "iTBT CHCM - Switch CM mode to 0000000000000000"' \
    'TRACE log ACPI Debug: "Set Force Power"' \
    'TRACE log ACPI Debug: "DOCM: Fail to apply CM mode to iTBT0, CM mode = 0000000000000000"' \
    "$pc = Buffer len=8 { 00 00 00 00 60 00 00 00 }" \
    'DIFF ~ \_SB_.PC00.TDM0.DFPE = Integer 0x0 -> \_SB_.PC00.TDM0.DFPE = Integer 0x1'

# 3. Processor after the platform commit, CFGD bit 22, the dynamic tables in memory.
memory="--memory-file 60D0D098=$dir/dynamic/ssdt17.dat --memory-file 608FC018=$dir/dynamic/ssdt20.dat
 --memory-file 60D0DB18=$dir/dynamic/ssdt16.dat --memory-file 608F9018=$dir/dynamic/ssdt23.dat
 --memory-file 608FE018=$dir/dynamic/ssdt19.dat --memory-file 608F7018=$dir/dynamic/ssdt21.dat
 --memory-file 60D0D718=$dir/dynamic/ssdt18.dat --memory-file 608FA018=$dir/dynamic/ssdt22.dat"
rq="\\_SB.PR00._OSC b:$processor i:1 i:2 b:0100000000000000"
rc="\\_SB.PR00._OSC b:$processor i:1 i:2 b:0000000000000000"
# shellcheck disable=SC2086
timeout 300 "$host" $loaded $memory --set '\_SB.CFGD=i:0x400000' --eval-args "$pq" --eval-args "$pc" \
    --eval-args "$rq" --eval-args "$rc" --eval-args '\_SB.PR00._CPC' $tables > "$out/processor.txt" 2>&1
expect processor "$out/processor.txt" \
    "$rq = Buffer len=8 { 01 00 00 00 00 00 00 00 }" \
    "TRACE end $rq: 0 writes, 0 loads, 0 unloads, slept 0 ms" \
    "$rc = Buffer len=8 { 00 00 00 00 00 00 00 00 }" \
    'TRACE write SystemMemory 0x61511039 8 0x2' \
    'TRACE load SSDT Cpu0Psd 427 bytes' \
    'TRACE load SSDT Cpu0Hwp 1210 bytes' \
    "TRACE end $rc: 1 writes, 2 loads, 0 unloads, slept 0 ms" \
    '\_SB.PR00._CPC = Package [23] { Integer 0x17, Integer 0x3, Buffer len=17 { 82 0c 00 7f 08 00 04 71 07' \
    'DIFF ~ \PF00 = Integer 0x80000000 -> \PF00 = Integer 0x0' \
    'DIFF ~ \_SB_.HWPV = Integer 0x0 -> \_SB_.HWPV = Integer 0x2' \
    'DIFF + \_SB_.PR00._CPC (Method)'

# 4. PCI Express: zedBSD's request, the 0x1D query, and hot plug and PME asked with NEXP 1.
xq="\\_SB.PC00._OSC b:$pcie i:1 i:3 b:010000001100000000000000"
xc="\\_SB.PC00._OSC b:$pcie i:1 i:3 b:000000001100000000000000"
x1d="\\_SB.PC00._OSC b:$pcie i:1 i:3 b:010000001F0000001D000000"
timeout 300 "$host" $loaded --eval-args "$xq" --eval-args "$xc" $tables > "$out/pcie.txt" 2>&1
expect pcie "$out/pcie.txt" \
    "$xq = Buffer len=12 { 01 00 00 00 11 00 00 00 00 00 00 00 }" \
    "$xc = Buffer len=12 { 00 00 00 00 11 00 00 00 00 00 00 00 }" \
    "TRACE end $xq: 2 writes, 0 loads, 0 unloads, slept 0 ms" \
    "TRACE end $xc: 2 writes, 0 loads, 0 unloads, slept 0 ms" \
    'TRACE write SystemMemory 0x6150c03c 8 0x0'
refuse pcie-only-oscc "$out/pcie.txt" 'TRACE write SystemMemory 0xe'
timeout 300 "$host" $loaded --eval-args "$x1d" $tables > "$out/pcie-1d.txt" 2>&1
expect pcie-query-writes "$out/pcie-1d.txt" \
    "$x1d = Buffer len=12 { 11 00 00 00 1f 00 00 00 18 00 00 00 }" \
    'TRACE write SystemMemory 0x6150c03c 8 0x18' \
    'DIFF ~ \OSCC = Integer 0x0 -> \OSCC = Integer 0x18'
x05="\\_SB.PC00._OSC b:$pcie i:1 i:3 b:010000001F00000005000000"
timeout 300 "$host" $loaded --set '\NEXP=i:1' --eval-args "$xq" --eval-args "$x05" $tables > "$out/pcie-nexp.txt" 2>&1
expect pcie-nexp "$out/pcie-nexp.txt" \
    "TRACE end $xq: 2 writes, 0 loads, 0 unloads, slept 0 ms" \
    'TRACE write SystemMemory 0x6150c03c 8 0x5' \
    'TRACE write SystemMemory 0xe00db 8 0x0' \
    'TRACE write SystemMemory 0xe00df 8 0x40'

if [ "$status" -eq 0 ]; then
	echo "osc-host: PASS"
else
	echo "osc-host: FAIL"
fi
exit "$status"
