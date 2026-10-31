#!/bin/sh
# ws333: a 5330 kernel for 5330-thermal.sh, built from this tree with test-only patches applied for the build and
# taken off again after (the image's configuration plan/tools/hw5330/config-5330.mk plus the make variables given).
#   sh plan/ws333/tests/hw5330/kbuild.sh OUT "VAR=VALUE ..." [PATCH...]
# The kernel is OUT/vmunix, the log OUT.log; the last line gives the return code and the number of warnings.  Run it
# while no other build reads this tree's sources.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname "$0")/../../../.."
out=${1:?OUT}
vars=${2-}
shift 2
applied=
for p in "$@"; do
	git apply "$p" || { echo "kbuild: FAIL apply $p"; for q in $applied; do git apply -R "$q"; done; exit 1; }
	applied="$p $applied"
done
# shellcheck disable=SC2086
timeout 3600 make -j24 ZEDBSD_CONFIG=plan/tools/hw5330/config-5330.mk BUILD="$out" $vars "$out/vmunix" > "$out.log" 2>&1
rc=$?
for q in $applied; do
	git apply -R "$q" || echo "kbuild: FAIL unapply $q"
done
echo "kbuild: $out rc=$rc warnings=$(grep -c 'warning:' "$out.log") sha256=$(sha256sum "$out/vmunix" 2> /dev/null | cut -c1-16)"
