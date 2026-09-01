#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-intel-ax211-license.XXXXXX")
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

config=$temporary/config.mk
cat >"$config" <<'EOF'
ZEDBSD_PLATFORM := amd64
ZEDBSD_ARCHITECTURE := amd64
ZEDBSD_BOARD := pcat
CONFIG_DRIVER_PCI_INTEL_AX211 := y
EOF

selection=$(make -s -C "$repo" ZEDBSD_CONFIG="$config" \
	--eval='q062-print-driver-license:;@printf "%s\n" "$(filter intel-ax211-driver-license,$(ZEDBSD_USER_PROGRAMS))|$(USERLAND_intel-ax211-driver-license_DATA)"' \
	q062-print-driver-license)
expected='intel-ax211-driver-license|/usr/share/licenses/intel-ax211-driver/LICENSE=userland/base/licenses/intel-ax211-driver/LICENSE'
test "$selection" = "$expected"

disabled=$temporary/disabled.mk
cat >"$disabled" <<'EOF'
ZEDBSD_PLATFORM := amd64
ZEDBSD_ARCHITECTURE := amd64
ZEDBSD_BOARD := pcat
CONFIG_DRIVER_PCI_INTEL_AX211 := n
EOF
selection=$(make -s -C "$repo" ZEDBSD_CONFIG="$disabled" \
	--eval='q062-print-driver-license:;@printf "%s\n" "$(filter intel-ax211-driver-license,$(ZEDBSD_USER_PROGRAMS))"' \
	q062-print-driver-license)
test -z "$selection"

license=$repo/userland/base/licenses/intel-ax211-driver/LICENSE
grep -Fq 'Copyright (c) 2017, 2019, 2020 Stefan Sperling' "$license"
grep -Fq 'Copyright(c) 2018 - 2019 Intel Corporation' "$license"
grep -Fq 'Redistribution and use in source and binary forms' "$license"
grep -Fq 'Redistributions in binary form must reproduce' "$license"
grep -Fq 'Permission to use, copy, modify, and distribute this software' "$license"

printf '%s\n' 'Intel AX211 driver license fixture passed'
