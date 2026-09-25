#!/bin/sh
# ws035-p034: host tests of the kernel C runtime and the kernel heap.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: sh plan/ws035/tests/run-kcrt-host-tests.sh
# Environment: CC (default clang).
#
#   kcrt      src/kern/kcrt.c compiled apart with -ffreestanding -fno-builtin
#             -DKERN_KCRT_NATIVE -DKERN_KCRT_NO_STANDARD_NAMES, linked with
#             kcrt-test.c; normal and ASan/UBSan.  nm -u of the kcrt object
#             must name none of memcpy/memset/memmove/memcmp.
#   explicit  kern_memset_explicit() keeps its stores at -O2 when the cleared
#             buffer is dead (the asm of the probe still clears it).
#   heap      kern-heap-test.c (includes src/kern/heap.c); normal, ASan/UBSan,
#             and both again with -DKERN_KERNEL_HEAP_TRACE.
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
cc=${CC:-clang}
work=$(mktemp -d "${TMPDIR:-/tmp}/ws035-kcrt.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

warn="-std=c11 -Wall -Wextra -Werror"
sanitize="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
kcrt_flags="-ffreestanding -fno-builtin -DKERN_KCRT_NATIVE -DKERN_KCRT_NO_STANDARD_NAMES -I$repo/include"

run_kcrt()
{
	name=$1
	flags=$2
	# shellcheck disable=SC2086
	"$cc" $warn $flags $kcrt_flags -c "$repo/src/kern/kcrt.c" -o "$work/kcrt-$name.o"
	if nm -u "$work/kcrt-$name.o" | grep -E -w 'memcpy|memset|memmove|memcmp'; then
		echo "kcrt ($name): FAIL: the kcrt object calls a standard memory function" >&2
		exit 1
	fi
	echo "kcrt ($name): nm -u names no memcpy/memset/memmove/memcmp"
	# shellcheck disable=SC2086
	"$cc" $warn -Wno-format-truncation $flags -I"$repo/include" -c "$test_dir/kcrt-test.c" -o "$work/kcrt-test-$name.o"
	# shellcheck disable=SC2086
	"$cc" $flags "$work/kcrt-test-$name.o" "$work/kcrt-$name.o" -o "$work/kcrt-test-$name"
	"$work/kcrt-test-$name"
}

run_heap()
{
	name=$1
	flags=$2
	# shellcheck disable=SC2086
	"$cc" $warn $flags -I"$repo/include" "$test_dir/kern-heap-test.c" -o "$work/heap-$name"
	printf 'heap (%s): ' "$name"
	"$work/heap-$name"
}

run_kcrt normal "-O2"
run_kcrt sanitize "$sanitize"

# kern_memset_explicit(): the clear of a dead buffer survives -O2.
cat > "$work/explicit.c" <<'EOF'
#include "src/kern/kcrt.c"
void consume(char *key);
void probe_explicit(void);
void probe_plain(void);
void probe_explicit(void)
{
	char key[64];
	consume(key);
	kern_memset_explicit(key, 0, sizeof(key));
}
void probe_plain(void)
{
	char key[64];
	consume(key);
	kern_memset(key, 0, sizeof(key));
}
EOF
# shellcheck disable=SC2086
"$cc" -O2 $kcrt_flags -I"$repo" -S "$work/explicit.c" -o "$work/explicit.s"
clears_after_use()
{
	awk -v name="$1" '$0 ~ "^" name ":" {on=1} on {print} on && /^\.Lfunc_end/ {exit}' "$work/explicit.s" |
		awk '/call.*consume/ {on=1; next} on {print}' |
		grep -E -c 'call.*kern_memset|mov[a-z]*[[:space:]].*\(%r[a-z0-9]+\)' || true
}
explicit_stores=$(clears_after_use probe_explicit)
plain_stores=$(clears_after_use probe_plain)
if [ "$explicit_stores" -eq 0 ]; then
	echo "explicit: FAIL: the clear after the last use was removed" >&2
	cat "$work/explicit.s" >&2
	exit 1
fi
echo "explicit: kern_memset_explicit keeps $explicit_stores clearing instruction(s) after the last use at -O2 (kern_memset control: $plain_stores)"

run_heap normal "-O2"
run_heap sanitize "$sanitize"
run_heap trace "-O2 -DKERN_KERNEL_HEAP_TRACE"
run_heap trace-sanitize "$sanitize -DKERN_KERNEL_HEAP_TRACE"

echo "run-kcrt-host-tests: PASS"
