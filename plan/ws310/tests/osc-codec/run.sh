#!/bin/sh
# The host tests of the _OSC negotiation's pure parts (src/drivers/acpi/acpi-osc-codec.c):
#   1. test-osc-codec.c with ASan and UBSan (the UUIDs, the buffers passed, the buffers returned,
#      the commit after a query);
#   2. the three UUIDs against iasl's ToUUID, read back through the AML host harness;
#   3. the codec compiled with the kernel's freestanding flags (-Wall -Wextra -Werror);
#   4. four broken copies of the codec (big-endian DWORDs, no cut to the bits asked, the failure
#      bit not a refusal, a wrong UUID byte), each of which the unit test must catch.
# Each check prints "<name>: PASS" or "<name>: FAIL (<file>)"; the last line is
# "osc-codec: PASS" or "osc-codec: FAIL".  Output under build/tmp/ws310-osc-codec (a fresh run directory).
#   sh plan/ws310/tests/osc-codec/run.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws310-osc-codec
fresh_out "$out"
out=$(cd "$out" && pwd -P)
compiler=${CC:-cc}
here=plan/ws310/tests/osc-codec
codec=src/drivers/acpi/acpi-osc-codec.c
status=0
flags="-std=gnu11 -g -O1 -Wall -Wextra -Werror -Wdeclaration-after-statement -Wstrict-prototypes
 -Wmissing-prototypes -Wshadow -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"

pass() {
	echo "$1: PASS"
}

fail() {
	echo "$1: FAIL ($2)"
	status=1
}

# Builds the unit test against a codec source and runs it; $1 names the run, $2 is the source.
unit() {
	# shellcheck disable=SC2086
	timeout 300 "$compiler" $flags -Iinclude -Isrc -Isrc/drivers/acpi -o "$out/$1" \
	    "$here/test-osc-codec.c" "$2" > "$out/$1-build.txt" 2>&1 &&
	    timeout 60 "$out/$1" > "$out/$1.txt" 2>&1
}

# 1. The unit test.
if unit unit "$codec" && tail -n 1 "$out/unit.txt" | grep -q '^test-osc-codec: PASS'; then
	echo "unit: PASS ($(tail -n 1 "$out/unit.txt"))"
else
	fail unit "$out/unit.txt"
fi

# 2. The UUIDs against iasl's ToUUID, evaluated by the harness.
cat > "$out/uuids.asl" <<'EOF'
DefinitionBlock ("", "SSDT", 2, "ZEDBSD", "OSCUUID", 1)
{
    Name (UPLT, ToUUID ("0811B06E-4A27-44F9-8D60-3CBBC22E7B48"))
    Name (UPRC, ToUUID ("4077A616-290C-47BE-9EBD-D87058713953"))
    Name (UPCI, ToUUID ("33DB4D5B-1FF7-401C-9657-7441C03DD766"))
}
EOF
if timeout 120 iasl -oa -p "$out/uuids" "$out/uuids.asl" > "$out/iasl.txt" 2>&1 &&
    timeout 600 make -s -C plan/tools/aml-host OUT="$out/host" > "$out/host-build.txt" 2>&1 &&
    timeout 60 "$out/host/aml-host" --quiet --eval '\UPLT' --eval '\UPRC' --eval '\UPCI' "$out/uuids.aml" \
    > "$out/uuids-host.txt" 2>&1 &&
    timeout 60 "$out/unit" --uuids > "$out/uuids-codec.txt" 2>&1; then
	mismatch=0
	for pair in UPLT:platform UPRC:processor UPCI:pcie; do
		name=${pair%%:*}
		kind=${pair#*:}
		# The harness prints "\NAME = Buffer [16] 6E B0 ..."; keep the bytes in upper case.
		host=$(grep "^\\\\$name = Buffer \\[16\\] " "$out/uuids-host.txt" | sed 's/^.*\] //' | tr 'a-f' 'A-F')
		mine=$(grep "^$kind " "$out/uuids-codec.txt" | cut -d' ' -f2-)
		if [ -z "$host" ] || [ "$host" != "$mine" ]; then
			echo "  $kind: iasl '$host', codec '$mine'"
			mismatch=1
		fi
	done
	if [ "$mismatch" -eq 0 ]; then
		pass "uuids (iasl ToUUID = codec for platform, processor, pcie)"
	else
		fail uuids "$out/uuids-host.txt"
	fi
else
	fail uuids "$out"
fi

# 3. The kernel's freestanding flags.
if timeout 120 clang --target=x86_64-unknown-none -std=gnu11 -ffreestanding -nostdlibinc -fno-builtin \
    -mno-red-zone -mgeneral-regs-only -Os -Wall -Wextra -Werror -Wdeclaration-after-statement \
    -Wstrict-prototypes -Wmissing-prototypes -Wshadow -DKERN_UAPI_NATIVE -Iinclude -Isrc -c "$codec" -o "$out/kernel.o" \
    > "$out/kernel.txt" 2>&1 && [ ! -s "$out/kernel.txt" ]; then
	pass "kernel-flags (warning 0)"
else
	fail kernel-flags "$out/kernel.txt"
fi

# 4. Broken copies, each of which the unit test must fail.
mutant() {
	name=$1
	expression=$2
	sed "$expression" "$codec" > "$out/$name.c"
	if cmp -s "$codec" "$out/$name.c"; then
		fail "mutant-$name (the edit did not apply)" "$out/$name.c"
		return
	fi
	cp src/drivers/acpi/acpi-osc-codec.h "$out/acpi-osc-codec.h"
	if unit "$name" "$out/$name.c"; then
		fail "mutant-$name (not caught)" "$out/$name.txt"
	elif tail -n 1 "$out/$name.txt" | grep -q '^test-osc-codec: FAIL'; then
		pass "mutant-$name ($(tail -n 1 "$out/$name.txt"))"
	else
		fail "mutant-$name (did not run)" "$out/$name-build.txt"
	fi
}
mutant big-endian 's/dword\[0\] = (uint8_t)value;/dword[0] = (uint8_t)(value >> 24);/'
mutant no-cut 's/result->support = returned & request->support;/result->support = returned;/'
mutant failure-accepted 's/#define STATUS_REFUSALS\t(DRV_ACPI_OSC_STATUS_FAILURE | \\/#define STATUS_REFUSALS\t(0 | \\/'
mutant uuid-byte 's/0x6E, 0xB0, 0x11, 0x08/0x08, 0x11, 0xB0, 0x6E/'

if [ "$status" -eq 0 ]; then
	echo "osc-codec: PASS"
else
	echo "osc-codec: FAIL"
fi
exit "$status"
