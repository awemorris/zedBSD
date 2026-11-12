#!/bin/sh
# ws333: the i915 RPS contract test (src/drivers/gpu/i915/tests/contracts/rps_contract_test.c, which also checks the
# account of the GT's RC6 time) built and run on the host, plainly and under ASan/UBSan, into OUT instead of the
# contract runner's temporary directory (which that runner removes; this one leaves OUT to its owner).
#   sh plan/ws333/tests/rc6-host/run.sh OUT
# The last line is "rc6-host: PASS" or "rc6-host: FAIL ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname "$0")/../../../.."
out=${1:?OUT}
repo=$(pwd)
here=$repo/src/drivers/gpu/i915/tests/contracts
driver=$repo/src/drivers/gpu/i915
cc=${CC:-cc}
warnings="-std=gnu11 -Wall -Wextra -Werror -Wdeclaration-after-statement"
host_headers="-DKERN_USER_ABI_LP64 -I$repo/include -I$repo/src -I$repo -idirafter $repo/include/libc"
kernel_headers="-DKERN_USER_ABI_LP64 -I$repo/include -I$repo/src -I$repo -I$repo/include/libc -DKERN_UAPI_NATIVE"
sources="$here/rps_contract_test.c $here/mock_mmio.c $here/host_kernel.c $here/host_thread.c $driver/gt-power.c
	$driver/workqueue.c $driver/mmio.c $driver/trace.c $here/contract.c $here/host_unreached.c"
failed=
for variant in ordinary sanitized; do
	case $variant in
	ordinary) flags="-O2" ;;
	*) flags="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" ;;
	esac
	dir=$out/$variant
	mkdir -p "$dir"
	objects=
	for source in $sources; do
		base=$(basename "$source" .c)
		case $base in
		workqueue | host_thread) headers=$kernel_headers ;;
		*) headers=$host_headers ;;
		esac
		# shellcheck disable=SC2086
		timeout 300 "$cc" $warnings $flags $headers -c "$source" -o "$dir/$base.o" || { failed="$failed $variant:$base"; continue 2; }
		objects="$objects $dir/$base.o"
	done
	# shellcheck disable=SC2086
	timeout 300 "$cc" $flags $objects -o "$dir/rps-contract-test" || { failed="$failed $variant:link"; continue; }
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 300 "$dir/rps-contract-test" > "$dir/run.txt" 2>&1 ||
		failed="$failed $variant:run"
	tail -1 "$dir/run.txt"
done
if [ -n "$failed" ]; then
	echo "rc6-host: FAIL$failed"
	exit 1
fi
echo "rc6-host: PASS"
