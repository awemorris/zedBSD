#!/bin/sh
# The host tests of the Dell thermal provider (src/drivers/acpi/acpi-dell.c, acpi-dell-codec.c):
#   1. test-dell-codec.c: the strategy-to-mode table, the DDV fan decoder, the SMBIOS thermal mode
#      buffer and the SMM fan call buffers with the checks of their answers (ASan, UBSan);
#   2. the AML host harness (plan/tools/aml-host, the boost built in) on the Dell Latitude 5330's
#      DSDT and SSDTs, counting the writes of the SMI command port of every call:
#      attach (0 SMIs: the descriptor, the mode the firmware mirrors, the DDV version and fans),
#      the DDV fan read (0), each strategy (1 SMI per mode change, 0 for the mode in force and for
#      FIRMWARE, a second's wait between two changes), the firmware's answers (completed, a failure
#      status, a call it never ran), the embedded controller not ready (the fans are not read and
#      the poll retries three times, 0 SMIs), the SMM boost (get, set, read back: 3 SMIs; 1 when the
#      fan already runs at the level; the state not taking, the SMM refusing, the firmware putting
#      the fan back, and a boost that holds);
#   3. a machine without the Dell WMI descriptor (one SSDT alone): ENODEV, nothing registered.
# Each check prints "<name>: PASS" or "<name>: FAIL (<file>)"; the last line is
# "dell: PASS" or "dell: FAIL".  Output under build/tmp/ws333-dell (a fresh run directory).
#   sh plan/ws333/tests/dell/run.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws333-dell
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

# Requires the SMI writes of the calls, in order, to be exactly the list given.
expect_smi() {
	name=$1
	file=$2
	want=$3
	got=$(grep '^SMI writes ' "$file" | sed 's/^SMI writes //' | tr '\n' ' ' | sed 's/ $//')
	if [ "$got" = "$want" ]; then
		pass "$name (SMI writes $got)"
	else
		echo "  SMI writes: want '$want', got '$got'"
		fail "$name" "$file"
	fi
}

# 1. The pure parts' unit test.
if timeout 300 "$compiler" -std=gnu11 -g -O1 -Wall -Wextra -Werror -Wdeclaration-after-statement \
    -Wstrict-prototypes -Wmissing-prototypes -Wshadow \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -Iinclude -Isrc -o "$out/test-dell-codec" \
    plan/ws333/tests/dell/test-dell-codec.c src/drivers/acpi/acpi-dell-codec.c > "$out/unit-build.txt" 2>&1 &&
    timeout 60 "$out/test-dell-codec" > "$out/unit.txt" 2>&1 &&
    tail -n 1 "$out/unit.txt" | grep -q '^test-dell-codec: PASS'; then
	echo "unit: PASS ($(tail -n 1 "$out/unit.txt"))"
else
	fail unit "$out/unit.txt"
fi

# 2. The harness on the Latitude 5330's tables.
if ! timeout 600 make -s -C plan/tools/aml-host OUT="$out/host" > "$out/harness-build.txt" 2>&1; then
	fail harness-build "$out/harness-build.txt"
	echo "dell: FAIL"
	exit 1
fi
host="$out/host/aml-host"
dir=plan/tools/aml-host/latitude5330
tables="$dir/dsdt.dat"
for number in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
	tables="$tables $dir/ssdt$number.dat"
done
loaded="--absent-pci 0:10.6 --absent-pci 0:10.7 --events --ec --ec-ports 930,934 --init"
machine="$loaded --reg"
# The EC's fan count and kind (0x6F, read twice for each read of the fan information) and the
# rpm 0x0F39 = 3897 (0x70, 0x71), as the machine gave them under Debian.
fan="--ec-seq 6F=01,00,01,00,01,00,01,00,01,00,01,00,01,00,01,00 --ec-seq 70=39 --ec-seq 71=0f"
# The firmware's answer to an SMI in the shared buffer: SMBA (0x614F3000 in the DSDT) + 4 is
# where the AML puts the call's buffer, and + 0x14 its output0 (the SMBIOS status, the SMM's EAX).
answer=614F3018

# 2a. Attach, the fan read, and the strategies with the firmware completing each call.
timeout 120 "$host" $machine $fan --smi-memory "$answer=00000000" --wmi --dell --dell-fan \
    --dell-apply balanced --dell-apply performance --dell-apply performance \
    --dell-apply powersave --dell-apply manual:50:20:50 --dell-apply manual:80:80:0 \
    --dell-apply manual:50:50:50 --dell-apply firmware \
    --eval-args '\ODV0' $tables > "$out/strategy.txt" 2>&1
expect strategy "$out/strategy.txt" \
    'dell: thermal mode Optimized' \
    'dell: 1 fans, fan 0 at 3897 rpm' \
    'Dell registered fan dell-fan0 rpm_max 0 boost yes' \
    'Dell registered receiver dell-wmi' \
    'Dell attach error 0' \
    'Dell fan dell-fan0 = error 0 flags 0x1 rpm 3897 boost_count 2' \
    'Dell apply balanced = error 0 flags 0x0' \
    'Dell apply performance = error 0 flags 0x0' \
    'Dell apply powersave = error 0 flags 0x0' \
    'Dell apply manual:50:20:50 = error 0 flags 0x0' \
    'Dell apply manual:80:80:0 = error 0 flags 0x0' \
    'Dell apply manual:50:50:50 = error 0 flags 0x0' \
    'Dell apply firmware = error 0 flags 0x0' \
    'dell: waits 1000 ms before the next firmware call' \
    '\ODV0 = Integer 0x0'
# WMI attach 0, Dell attach 0, fan 0, balanced 0 (in force), performance 1, performance 0,
# powersave 1, quiet 1, performance 1, optimized 1, firmware 0, \ODV0 0.
expect_smi strategy-smi "$out/strategy.txt" '0 0 0 0 1 0 1 1 1 1 0 0'
if grep -q 'the firmware shows' "$out/strategy.txt"; then
	fail strategy-mirror "$out/strategy.txt"
else
	pass "strategy-mirror (the firmware's \\ODV0 followed every mode)"
fi
modes=$(grep '^Dell mode ' "$out/strategy.txt" | sed 's/^Dell mode //' | tr '\n' ' ')
if [ "$modes" = "Optimized Optimized Performance Performance Cool Quiet Performance Optimized Optimized " ]; then
	pass "strategy-modes ($modes)"
else
	fail "strategy-modes ($modes)" "$out/strategy.txt"
fi

# 2b. The firmware's other answers: a call it never ran (the outputs keep the sentinel: the mode
#     call is offered no more), and a failure status (-2: the mode is not known, the next one tries).
timeout 120 "$host" $machine $fan --wmi --dell --dell-apply performance --dell-apply powersave \
    $tables > "$out/untouched.txt" 2>&1
expect untouched "$out/untouched.txt" \
    'dell: the firmware did not run the thermal mode call' \
    'Dell apply performance = error 5 flags 0x0' \
    'Dell apply powersave = error 95 flags 0x0' \
    'Dell mode none (error 2)'
expect_smi untouched-smi "$out/untouched.txt" '0 0 1 0'
timeout 120 "$host" $machine $fan --smi-memory "$answer=FEFFFFFF" --wmi --dell \
    --dell-apply performance --dell-apply performance $tables > "$out/status.txt" 2>&1
expect status "$out/status.txt" \
    'dell: the thermal mode call returned status 0xfffffffe' \
    'Dell apply performance = error 5 flags 0x0' \
    'Dell mode none (error 2)'
expect_smi status-smi "$out/status.txt" '0 0 1 1'

# 2c. The embedded controller not ready for the AML (no _REG: \ECRD is 0, and each EC byte would
#     be an SMI): the fans are not read, the poll probes them again three times, then gives up.
timeout 120 "$host" $loaded $fan --wmi --dell --dell-poll --dell-poll --dell-poll --dell-poll --dell-fan \
    $tables > "$out/ec.txt" 2>&1
expect ec "$out/ec.txt" \
    'Dell attach error 0' \
    'Dell registered receiver dell-wmi' \
    'dell: gave up probing 0x4 after 3 retries' \
    'Dell fans 0'
expect_smi ec-smi "$out/ec.txt" '0 0 0 0 0 0 0'
if grep -q 'Dell registered fan' "$out/ec.txt"; then
	fail ec-no-fan "$out/ec.txt"
else
	pass ec-no-fan
fi

# 2d. The EC lists no fan ({0, Buffer {0}}): no fan is registered.
timeout 120 "$host" $machine --ec-seq 6F=00 --wmi --dell --dell-fan $tables > "$out/nofan.txt" 2>&1
expect nofan "$out/nofan.txt" 'dell: the DDV lists no fan' 'Dell attach error 0' 'Dell fans 0'
expect_smi nofan-smi "$out/nofan.txt" '0 0 0'

# 2e. The boost: get (state 1), set, read back (state 2): 3 SMIs; then put back: 1 SMI.
timeout 120 "$host" $machine $fan --smi-memory "$answer=01000000,00000000,02000000,00000000" \
    --wmi --dell --dell-fan --dell-boost 0:2 --dell-fan --dell-boost 0:0 $tables > "$out/boost.txt" 2>&1
expect boost "$out/boost.txt" \
    'Dell boost 0:2 = error 0' \
    'Dell boost 0:0 = error 0' \
    'Dell fan dell-fan0 = error 0 flags 0x1 rpm 3897 boost_count 2'
expect_smi boost-smi "$out/boost.txt" '0 0 0 3 0 1'

#     A fan that already runs at the level: the get alone.
timeout 120 "$host" $machine $fan --smi-memory "$answer=02000000" --wmi --dell \
    --dell-boost 0:2 --dell-boost 0:0 $tables > "$out/boost-high.txt" 2>&1
expect boost-high "$out/boost-high.txt" 'Dell boost 0:2 = error 0' 'Dell boost 0:0 = error 0'
expect_smi boost-high-smi "$out/boost-high.txt" '0 0 1 0'

#     The state does not take (reads back 1): no boost from then on.
timeout 120 "$host" $machine $fan --smi-memory "$answer=01000000,00000000,01000000" --wmi --dell \
    --dell-boost 0:2 --dell-fan --dell-boost 0:1 $tables > "$out/boost-take.txt" 2>&1
expect boost-take "$out/boost-take.txt" \
    'dell: fan 0: the SMM fan state did not take (error 5), no boost' \
    'Dell boost 0:2 = error 5' \
    'Dell fan dell-fan0 = error 0 flags 0x1 rpm 3897 boost_count 0' \
    'Dell boost 0:1 = error 95'
expect_smi boost-take-smi "$out/boost-take.txt" '0 0 3 0 0'

#     The SMM refuses the call (EAX 0xFFFF).
timeout 120 "$host" $machine $fan --smi-memory "$answer=FFFF0000" --wmi --dell --dell-boost 0:1 \
    $tables > "$out/boost-refuse.txt" 2>&1
expect boost-refuse "$out/boost-refuse.txt" \
    'dell: fan 0: the SMM fan state could not be read (error 5), no boost' \
    'Dell boost 0:1 = error 5'
expect_smi boost-refuse-smi "$out/boost-refuse.txt" '0 0 1'

#     The firmware puts the fan back: three reads no faster than before the raise end the boost.
timeout 120 "$host" $machine $fan --smi-memory "$answer=00000000,00000000,02000000" --wmi --dell \
    --dell-fan --dell-boost 0:2 --dell-fan --dell-fan --dell-fan --dell-boost 0:0 \
    $tables > "$out/boost-back.txt" 2>&1
expect boost-back "$out/boost-back.txt" \
    'Dell boost 0:2 = error 0' \
    'dell: fan 0 runs at 3897 rpm, not above 3897 before the boost: the firmware put it back, no boost' \
    'Dell fan dell-fan0 = error 0 flags 0x1 rpm 3897 boost_count 0' \
    'Dell boost 0:0 = error 0'
expect_smi boost-back-smi "$out/boost-back.txt" '0 0 0 3 0 0 0 0'

#     A boost that holds: the fan runs faster after the raise (0x0F80 = 3968 rpm).
timeout 120 "$host" $machine --ec-seq 6F=01,00,01,00,01,00,01,00,01,00,01,00 --ec-seq 70=39,39,80 \
    --ec-seq 71=0f --smi-memory "$answer=00000000,00000000,02000000" --wmi --dell \
    --dell-fan --dell-boost 0:2 --dell-fan --dell-fan --dell-fan --dell-fan \
    $tables > "$out/boost-hold.txt" 2>&1
expect boost-hold "$out/boost-hold.txt" 'Dell boost 0:2 = error 0' \
    'Dell fan dell-fan0 = error 0 flags 0x1 rpm 3968 boost_count 2'
if grep -q 'the firmware put it back' "$out/boost-hold.txt"; then
	fail boost-hold-kept "$out/boost-hold.txt"
else
	pass boost-hold-kept
fi

# 3. No Dell WMI descriptor: ENODEV, nothing registered, no SMI.
timeout 120 "$host" --wmi --dell "$dir/ssdt1.dat" > "$out/none.txt" 2>&1
expect none "$out/none.txt" 'Dell attach error 19'
expect_smi none-smi "$out/none.txt" '0 0'
if grep -q 'Dell registered' "$out/none.txt"; then
	fail none-nothing "$out/none.txt"
else
	pass none-nothing
fi

if [ "$status" -eq 0 ]; then
	echo "dell: PASS"
else
	echo "dell: FAIL"
fi
exit "$status"
