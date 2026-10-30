#!/bin/sh
# The host tests of the ACPI WMI layer (src/drivers/acpi/acpi-wmi.c, acpi-wmi-table.c):
#   1. test-wmi-table.c: the _WDG decoder, GUID text and control-method names (ASan, UBSan);
#   2. the AML host harness (plan/tools/aml-host) on the Dell Latitude 5330's DSDT and SSDTs:
#      the attach lists the blocks of the four mapping devices whose _WDG is data, passes over
#      the five whose _WDG is a method, and writes the SMI command port 0 times; the named
#      attach of \_SB.DIAG, a query, the DDV interface version and fan information (method
#      0x12 and 0x20) write it 0 times; refusals; and two contrasts that show the harness counts
#      SMIs and answers them in system memory (the method _WDG of \_SB.AMW2, an SMBIOS call).
#   3. a machine without a mapping device (one SSDT alone): ENODEV, no block.
# Each check prints "<name>: PASS" or "<name>: FAIL (<file>)"; the last line is
# "wmi: PASS" or "wmi: FAIL".  Output under build/tmp/ws333-wmi (a fresh run directory).
#   sh plan/ws333/tests/wmi/run.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws333-wmi
fresh_out "$out"
out=$(cd "$out" && pwd -P)
compiler=${CC:-cc}
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

# 1. The decoder's unit test.
if timeout 300 "$compiler" -std=gnu11 -g -O1 -Wall -Wextra -Werror -Wdeclaration-after-statement \
    -Wstrict-prototypes -Wmissing-prototypes -Wshadow \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -Iinclude -Isrc -o "$out/test-wmi-table" \
    plan/ws333/tests/wmi/test-wmi-table.c src/drivers/acpi/acpi-wmi-table.c > "$out/unit-build.txt" 2>&1 &&
    timeout 60 "$out/test-wmi-table" > "$out/unit.txt" 2>&1 &&
    tail -n 1 "$out/unit.txt" | grep -q '^test-wmi-table: PASS'; then
	echo "unit: PASS ($(tail -n 1 "$out/unit.txt"))"
else
	fail unit "$out/unit.txt"
fi

# 2. The harness on the Latitude 5330's tables.
if ! timeout 600 make -s -C plan/tools/aml-host OUT="$out/host" > "$out/harness-build.txt" 2>&1; then
	fail harness-build "$out/harness-build.txt"
	echo "wmi: FAIL"
	exit 1
fi
host="$out/host/aml-host"
dir=plan/tools/aml-host/latitude5330
tables="$dir/dsdt.dat"
for number in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
	tables="$tables $dir/ssdt$number.dat"
done
machine="--absent-pci 0:10.6 --absent-pci 0:10.7 --events --ec --ec-ports 930,934 --reg --init"
dell_smbios=A80593CE-A997-11DA-B012-B622A1EF5492
dell_descriptor=8D9DDCBC-A997-11DA-B012-B622A1EF5492
dell_ddv=8A42EA14-4F2A-FD45-6422-0087F7A7E608
dell_bc=A3776CE0-1E88-11DB-A98B-0800200C9A66

# 2a. The attach at boot, then the named attach of DIAG, a query, the DDV version.
timeout 120 "$host" $machine --wmi --wmi-path '\_SB_.DIAG' \
    --wmi-query "$dell_descriptor:0" \
    --wmi-method "$dell_ddv:0:0x12:00000000" \
    $tables > "$out/attach.txt" 2>&1
expect attach "$out/attach.txt" \
    'WMI attach error 0' \
    'acpi: WMI \_SB_.WFDE: 3 blocks' \
    'acpi: WMI \_SB_.WFTE: 2 blocks' \
    'acpi: WMI \_SB_.AMW0: 7 blocks' \
    'acpi: WMI \_SB_.AMWV: 2 blocks' \
    'acpi: WMI \_SB_.AMW2: _WDG is a method, not evaluated' \
    'acpi: WMI \_SB_.AMW5: _WDG is a method, not evaluated' \
    'acpi: WMI \_SB_.AMW4: _WDG is a method, not evaluated' \
    'acpi: WMI \_SB_.DIAG: _WDG is a method, not evaluated' \
    'acpi: WMI \_SB_.ECMO: _WDG is a method, not evaluated' \
    "WMI block \\_SB_.AMW0 $dell_descriptor id AA instances 1 flags 0x00" \
    "WMI block \\_SB_.AMW0 $dell_smbios id BA instances 1 flags 0x02" \
    'WMI block \_SB_.AMW0 9DBB5994-A997-11DA-B012-B622A1EF5492 notify 0xd0 instances 1 flags 0x08' \
    "WMI block \\_SB_.AMW0 $dell_bc id BC instances 1 flags 0x00" \
    "WMI block \\_SB_.AMWV $dell_ddv id DV instances 1 flags 0x02" \
    'WMI blocks 14' \
    'WMI attach-path \_SB_.DIAG error 0' \
    'WMI block \_SB_.DIAG F1DDEE52-063C-4784-A11E-8A06684B9B01 id DM instances 1 flags 0x02' \
    'WMI blocks 16' \
    "WMI query $dell_descriptor:0 = Buffer len=128 { 44 45 4c 4c 20 57 4d 49 01 00 00 00 00 80 00 00 01 00 00 00 " \
    "WMI method $dell_ddv:0:0x12:00000000 = Integer 0x3"
blocks=$(grep -c '^WMI block ' "$out/attach.txt")
smi=$(grep '^SMI writes ' "$out/attach.txt" | tr '\n' ' ')
if [ "$blocks" -eq 16 ] && [ "$smi" = "SMI writes 0 SMI writes 0 SMI writes 0 SMI writes 0 " ]; then
	pass "attach-no-smi (16 blocks; $smi)"
else
	fail "attach-no-smi ($blocks blocks; $smi)" "$out/attach.txt"
fi

# 2b. The DDV fan information, with the EC's fan count and rpm as the machine gave them (3897 rpm).
timeout 120 "$host" --quiet $machine --ec-seq 6F=01,00 --ec-seq 70=39 --ec-seq 71=0f --wmi \
    --wmi-method "$dell_ddv:0:0x20:00000000" $tables > "$out/fan.txt" 2>&1
expect fan "$out/fan.txt" \
    "WMI method $dell_ddv:0:0x20:00000000 = Package [2] { Integer 0x4, Buffer len=4 { 00 39 0f ff } }"
smi=$(grep '^SMI writes ' "$out/fan.txt" | tr '\n' ' ')
if [ "$smi" = "SMI writes 0 SMI writes 0 " ]; then
	pass "fan-no-smi"
else
	fail "fan-no-smi ($smi)" "$out/fan.txt"
fi

# 2c. Refusals: a query of a method block, an instance past the count, a block without WQxx,
#     a GUID nobody lists, a path that is not a mapping device.
timeout 120 "$host" --quiet $machine --wmi \
    --wmi-query "$dell_smbios:0" \
    --wmi-method "$dell_ddv:1:0x12:00000000" \
    --wmi-method "$dell_descriptor:0:1:00000000" \
    --wmi-query "$dell_bc:0" \
    --wmi-query 00000000-0000-0000-0000-000000000000:0 \
    --wmi-path '\_SB_.PC00' \
    $tables > "$out/refuse.txt" 2>&1
expect refuse "$out/refuse.txt" \
    "WMI query $dell_smbios:0 = error 22" \
    "WMI method $dell_ddv:1:0x12:00000000 = error 22" \
    "WMI method $dell_descriptor:0:1:00000000 = error 22" \
    "WMI query $dell_bc:0 = error 2" \
    'WMI query 00000000-0000-0000-0000-000000000000:0 = error 2' \
    'WMI attach-path \_SB_.PC00 error 19'

# 2d. Contrasts: evaluating AMW2's method _WDG writes the SMI port (the attach passes it over),
#     and an SMBIOS call (class 0x11, select 0x13, input 1, 2) through WMBA gets the bytes the
#     simulated firmware stores at SMBA + 4 + 0x14 (SMBA 0x614F3000 in the DSDT).
input=1100130001000000020000000000000000000000000000000000000000000000000000000000000000000000
timeout 120 "$host" --quiet $machine --smi-memory 614F3018=AABBCCDD \
    --wmi-path '\_SB_.AMW2' --wmi --wmi-method "$dell_smbios:0:0:$input" \
    $tables > "$out/contrast.txt" 2>&1
expect contrast "$out/contrast.txt" \
    'WMI attach-path \_SB_.AMW2 error 0' \
    "WMI method $dell_smbios:0:0:$input = Buffer len=44 { 11 00 13 00 01 00 00 00 02 00 00 00 00 00 00 00 00 00 00 00 aa bb cc dd "
smi=$(grep '^SMI writes ' "$out/contrast.txt" | tr '\n' ' ')
if [ "$smi" = "SMI writes 1 SMI writes 0 SMI writes 1 " ]; then
	pass "contrast-smi ($smi)"
else
	fail "contrast-smi ($smi)" "$out/contrast.txt"
fi

# 3. No mapping device: ENODEV and no block.
timeout 120 "$host" --quiet --wmi "$dir/ssdt1.dat" > "$out/none.txt" 2>&1
expect none "$out/none.txt" 'WMI attach error 19' 'WMI blocks 0' 'SMI writes 0'

if [ "$status" -eq 0 ]; then
	echo "wmi: PASS"
else
	echo "wmi: FAIL"
fi
exit "$status"
