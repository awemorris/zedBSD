#!/bin/sh
# WS052 (ws052-p003): the host test of the ACPI side of S0 idle.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
#   plan/ws052/tests/run-host-sleep.sh
#
# Builds build/ws052/host/aml-host-sleep (the WS049 harness with
# acpi-sleep.c and host-sleep.c, ASan/UBSan) and plays two scripts:
#   1. sleep.asl (compiled with iasl): the LPS0 _DSM order of both
#      families, the power resources' reference counts across two devices
#      sharing one, _PS0/_PS3 before or after the resources, D3hot and
#      D3cold, the refusals (D1 without _PS1/_PR1, D1 from D3, no _PRW, a
#      GPE block device), _DSW/_PSW, and the GPEs during a sleep (only the
#      armed ones enabled, a runtime GPE held back and handled after the
#      sleep, the GPE that woke the system).  The printed lines must equal
#      sleep.expected.
#   2. the Latitude 5330's DSDT and SSDTs (plan/tools/aml-host/latitude5330/):
#      the LPS0 device and its functions (Intel 0x7f, Microsoft 0x1ff), the
#      entry and the exit, _PS3/_PS0 of the two xHCI controllers, the wake
#      of the lid and the power button (PPRW, _PSW), and the power button
#      present with zedBSD's _OSI (OSYS 0x7df, PBTN._STA 0xf).  The printed
#      lines must equal latitude5330.expected.  The simulated memory reads
#      zero, so the values are not the machine's, and the simulated GPE
#      block has GPEs 0 to 63 only: XHCI's GPE 0x6d is refused (22).
# Exits 0 on success.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
tests="$repo/plan/ws052/tests"
out="$repo/build/ws052"
host="$out/host/aml-host-sleep"
mkdir -p "$out/asl"
make -s -C "$tests" >/dev/null
failed=0

# Keeps the lines the steps print.
steps='^(LPS0|EVAL|POWER|WAKE|SLEEP|ENABLED|RAISE|SCI|STEP|PATH)'

# 1. The test table.
iasl -oa -vi -p "$out/asl/sleep" "$tests/sleep.asl" > "$out/asl/sleep.iasl.log" 2>&1
script='lps0-attach;lps0-enter;eval \TAKS;lps0-exit;eval \TAKS'
script="$script"';power \_SB_.DEV0 3;eval \TAKE;power \_SB_.DEV1 0;eval \TAKE'
script="$script"';power \_SB_.DEV0 4;eval \TAKE;power \_SB_.DEV1 4;eval \TAKE'
script="$script"';power \_SB_.DEV0 0;eval \TAKE;power \_SB_.DEV2 1'
script="$script"';power \_SB_.DEV0 3;power \_SB_.DEV0 1;eval \TAKE;power \_SB_.DEV0 0;eval \TAKE'
script="$script"';wake-on \_SB_.DEV0 3;eval \TAKE;wake-on \_SB_.DEV0 3'
script="$script"';wake-on \_SB_.DEV1 3;eval \TAKE;wake-on \_SB_.DEV2 3;wake-on \_SB_.DEV3 3'
script="$script"';enabled 0x12;enabled 0x13;enabled 0x14;sleep-begin;sleep-begin'
script="$script"';enabled 0x12;enabled 0x13;enabled 0x14'
script="$script"';raise 0x14;raise 0x12;eval \TAKE;enabled 0x12;raise 0x13;enabled 0x13'
script="$script"';sleep-end;enabled 0x12;enabled 0x13;enabled 0x14;sci;eval \TAKE'
script="$script"';wake-off \_SB_.DEV0;eval \TAKE;wake-off \_SB_.DEV0;wake-off \_SB_.DEV1;eval \TAKE'
script="$script"';sleep-end;sleep-begin;sleep-end;wake-state \_SB_.DEV0;wake-state \_SB_.DEV2'
if WS052_SLEEP="$script" "$host" --events "$out/asl/sleep.aml" > "$out/sleep.txt" 2>&1 &&
    grep -E "$steps" "$out/sleep.txt" | diff -u "$tests/sleep.expected" - > "$out/sleep.diff" &&
    grep -q '^ACPI: LPS0 \\_SB_.PEPD, Intel functions 0x7f, Microsoft functions 0x1f9$' "$out/sleep.txt"; then
	echo "sleep: the steps printed sleep.expected"
else
	echo "sleep: FAILED, see $out/sleep.txt and $out/sleep.diff"
	failed=1
fi

# 2. The Latitude 5330's tables.
dir="$repo/plan/tools/aml-host/latitude5330"
tables="$dir/dsdt.dat"
for number in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
	tables="$tables $dir/ssdt$number.dat"
done
script='eval \OSYS;eval \_SB_.PEPD._STA;eval \_SB_.PBTN._STA'
script="$script"';lps0-attach;lps0-enter;lps0-exit'
script="$script"';power \_SB_.PC00.TXHC 3;power \_SB_.PC00.TXHC 0'
script="$script"';power \_SB_.PC00.XHCI 3;power \_SB_.PC00.XHCI 0;wake-state \_SB_.PC00.TXHC'
script="$script"';wake-on \_SB_.LID0 3;wake-on \_SB_.PBTN 3;wake-on \_SB_.PC00.XHCI 3'
script="$script"';sleep-begin;sleep-end;wake-off \_SB_.LID0;wake-off \_SB_.PBTN'
# shellcheck disable=SC2086
if WS052_SLEEP="$script" "$host" --absent-pci 0:10.6 --absent-pci 0:10.7 \
    --events --ec --ec-ports 930,934 --reg --init $tables > "$out/latitude5330.txt" 2>&1 &&
    grep -E "$steps" "$out/latitude5330.txt" | diff -u "$tests/latitude5330.expected" - > "$out/latitude5330.diff" &&
    grep -q '^ACPI: LPS0 \\_SB_.PEPD, Intel functions 0x7f, Microsoft functions 0x1ff$' "$out/latitude5330.txt" &&
    ! grep -q "LPS0 function .* failed\|power of .* failed" "$out/latitude5330.txt"; then
	echo "latitude5330: the steps printed latitude5330.expected"
else
	echo "latitude5330: FAILED, see $out/latitude5330.txt and $out/latitude5330.diff"
	failed=1
fi

exit $failed
