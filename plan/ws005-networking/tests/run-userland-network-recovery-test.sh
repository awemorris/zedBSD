#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-/tmp}
work=$(mktemp -d "$temporary_root/zedbsd-network-recovery.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc=${CC:-cc}
common="-std=c11 -DZEDBSD_USER_ABI_LP64 -I$root/include/uapi \
	-I$root/libc/include -I$root \
	-Wall -Wextra -Werror -ffunction-sections -fdata-sections"
discard="-Wl,--gc-sections"

# shellcheck disable=SC2086
$cc $common "$root/plan/ws005-networking/tests/dhcpc-state-test.c" \
	"$root/userland/base/net/dhcp.c" \
	$discard -o "$work/dhcpc-state-test"
"$work/dhcpc-state-test"

# shellcheck disable=SC2086
$cc $common "$root/plan/ws005-networking/tests/networkd-status-test.c" \
	$discard -o "$work/networkd-status-test"
"$work/networkd-status-test"

# Compile the production strerror implementation; section GC drops unrelated
# heap-backed string entry points from this focused fixture.
# shellcheck disable=SC2086
$cc $common "$root/plan/ws005-networking/tests/strerror-network-test.c" \
	"$root/libc/string.c" $discard -o "$work/strerror-network-test"
"$work/strerror-network-test"

# Check the stateful transaction fixture and the actual BOOTP/DHCP wire builder
# with the compiler's path analyzer before runtime sanitizer passes.
# shellcheck disable=SC2086
$cc $common -fanalyzer -c \
	"$root/plan/ws005-networking/tests/dhcpc-state-test.c" \
	-o "$work/dhcpc-state-test-analyzer.o"
# shellcheck disable=SC2086
$cc $common -fanalyzer -c "$root/userland/base/net/dhcp.c" \
	-o "$work/dhcp-analyzer.o"

# Repeat the two stateful production-source fixtures under runtime sanitizers.
# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$root/plan/ws005-networking/tests/dhcpc-state-test.c" \
	"$root/userland/base/net/dhcp.c" $discard \
	-o "$work/dhcpc-state-test-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/dhcpc-state-test-sanitize"

# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$root/plan/ws005-networking/tests/networkd-status-test.c" $discard \
	-o "$work/networkd-status-test-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/networkd-status-test-sanitize"

# Keep the transactional error/rollback paths under the compiler's static
# analyzer as well as the runtime sanitizers above.
# shellcheck disable=SC2086
$cc $common -fanalyzer \
	"$root/plan/ws005-networking/tests/dhcpc-state-test.c" \
	"$root/userland/base/net/dhcp.c" $discard \
	-o "$work/dhcpc-state-test-analyzer"
"$work/dhcpc-state-test-analyzer"

# shellcheck disable=SC2086
$cc $common -fanalyzer \
	"$root/plan/ws005-networking/tests/networkd-status-test.c" $discard \
	-o "$work/networkd-status-test-analyzer"
"$work/networkd-status-test-analyzer"

echo 'userland network recovery test: PASS'
