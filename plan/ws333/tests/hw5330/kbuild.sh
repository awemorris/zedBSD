#!/bin/sh
# ws333: a 5330 kernel for the 5330 scripts, built from this tree by the build system (PLATFORM=x86_64 with the thermal
# core and the x86 power control on), with test-only patches applied for the build and taken off again after.
#   sh plan/ws333/tests/hw5330/kbuild.sh OUT "VAR=VALUE ..." [PATCH...]
# VAR=VALUE are make variables, such as CONFIG_CPPFLAGS=-DIDLE_WAIT_DEEPEST_CSTATE=1U.  The tree's build/x86_64 is
# built in (its sysroot first, when missing) and the kernel copied to OUT/vmunix; the log is OUT.log, and the last
# line gives the return code, the number of warnings and the kernel's sha256.  Run it while no other build reads
# this tree's sources.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname "$0")/../../../.."
out=${1:?OUT}
vars=${2-}
shift 2
mkdir -p "$out"
applied=
for p in "$@"; do
	git apply "$p" || { echo "kbuild: FAIL apply $p"; for q in $applied; do git apply -R "$q"; done; exit 1; }
	applied="$p $applied"
done
base="PLATFORM=x86_64 CONFIGMK=config/config.x86_64.mk CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y"
: > "$out.log"
if [ ! -d build/x86_64/sysroot/usr/include ]; then
	# shellcheck disable=SC2086
	timeout 3600 make -j4 $base sysroot >> "$out.log" 2>&1 < /dev/null
fi
# shellcheck disable=SC2086
timeout 3600 make -j4 $base $vars build/x86_64/vmunix >> "$out.log" 2>&1 < /dev/null
rc=$?
for q in $applied; do
	git apply -R "$q" || echo "kbuild: FAIL unapply $q"
done
[ "$rc" = 0 ] && cp build/x86_64/vmunix "$out/vmunix"
sync
echo "kbuild: $out rc=$rc warnings=$(grep -c ' warning:' "$out.log") sha256=$(sha256sum "$out/vmunix" 2> /dev/null | cut -c1-16)"
