#!/bin/sh
# The host test of S0 idle behind a VMD (host-vmd-power.c): pci-power.c, pci-vmd.c, pci-vmd-plan.c and pci-window.c
# against a simulated VMD, root port and NVMe disk, built twice (-O2, and -O1 with ASan and UBSan) and run.
#   sh plan/bugs/BUG-424/run.sh OUT
# The last line is "host-vmd-power: PASS" or "host-vmd-power: FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname "$0")/../../.."
out=${1:?OUT}
mkdir -p "$out"
cc=${CC:-cc}
warnings="-Wall -Wextra -Werror -Wdeclaration-after-statement -Wstrict-prototypes -Wmissing-prototypes -Wshadow"
sources="plan/bugs/BUG-424/host-vmd-power.c src/drivers/pci/pci-power.c src/drivers/pci/pci-vmd-plan.c src/drivers/pci/pci-window.c"
status=PASS
# shellcheck disable=SC2086
$cc -std=gnu11 -O2 $warnings -Iinclude -Isrc -o "$out/host-vmd-power" $sources || status=FAIL
# shellcheck disable=SC2086
$cc -std=gnu11 -g -O1 $warnings -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-Iinclude -Isrc -o "$out/host-vmd-power-san" $sources || status=FAIL
for t in host-vmd-power host-vmd-power-san; do
	[ -x "$out/$t" ] || continue
	timeout 60 "$out/$t" || status=FAIL
done
echo "host-vmd-power: $status"
