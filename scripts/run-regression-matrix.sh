#!/usr/bin/env bash
# Run and record the selected zedBSD regression matrix.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -uo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/test-common.sh
source "$repo/scripts/test-common.sh"

profile="${1:-build}"
case "$profile" in
build|runtime|full) ;;
*) echo "usage: $0 [build|runtime|full]" >&2; exit 2 ;;
esac

result_root="${ZEDBSD_TEST_RESULTS:-$repo/build/test-results/regression}"
mkdir -p "$result_root"
"$repo/scripts/collect-toolchain-info.sh" "$result_root/toolchain.json" || exit 1

overall=0
run_case()
{
	local suite="$1" arch="$2"
	shift 2
	local directory="$result_root/$suite/$arch"
	local log="$directory/output.log" result="$directory/result.json"
	local started finished status code
	mkdir -p "$directory"
	started="$(zedbsd_test_utc_now)"
	(
		cd "$repo"
		"$@"
	) >"$log" 2>&1
	code=$?
	finished="$(zedbsd_test_utc_now)"
	if test "$code" -eq 0; then status=PASS; else status=FAIL; overall=1; fi
	zedbsd_test_write_result "$result" "$suite" "$arch" "$status" \
		"$started" "$finished" "exit_code=$code" "log=output.log"
	printf '%-28s %-8s %s\n' "$suite" "$arch" "$status"
}

for arch in pc98 pcat amd64 arm64 sparcv9; do
	run_case build-check "$arch" "$repo/build.sh" check "$arch"
done

if test "$profile" = runtime || test "$profile" = full; then
	for arch in pc98 pcat amd64 arm64 sparcv9; do
		run_case posix-r1 "$arch" "$repo/scripts/test-posix-r1.sh" "$arch"
	done
	run_case amd64-smp amd64 "$repo/scripts/test-amd64-smp.sh"
	run_case amd64-smp-stress amd64 "$repo/scripts/test-amd64-smp-stress.sh"
fi

if test "$profile" = full; then
	# The all-architecture invocations above remain individual records; full
	# adds storage-specific runtime gates that are not part of POSIX R1.
	run_case sparcv9-ufs sparcv9 "$repo/scripts/test-sparcv9-ufs-qemu.sh"
fi

exit "$overall"
