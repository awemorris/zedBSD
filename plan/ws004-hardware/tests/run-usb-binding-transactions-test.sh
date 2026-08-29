#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$root/build/q027-tmp"}
mkdir -p "$temporary_root"
work=$(mktemp -d "$temporary_root/usb-binding-transactions.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc=${CC:-cc}
common="-std=c11 -I$root/include -I$root/include/uapi -Wall -Wextra -Werror"
fixture="$root/plan/ws004-hardware/tests/usb-binding-transactions-test.c"

# The fixture directly includes the production USB core so private binding
# states remain testable without adding a public transaction/query API.
# shellcheck disable=SC2086
$cc $common -pthread "$fixture" -o "$work/usb-binding-transactions"
"$work/usb-binding-transactions"

# shellcheck disable=SC2086
$cc $common -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$fixture" -o "$work/usb-binding-transactions-sanitize"
# LeakSanitizer cannot run under the PTY/ptrace harness.  The fixture's own
# allocation accounting remains the exact end-of-run leak gate.
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/usb-binding-transactions-sanitize"

# Keep the analyzer compile-only; runtime behavior is covered above.
# shellcheck disable=SC2086
$cc $common -pthread -fanalyzer -c "$fixture" \
	-o "$work/usb-binding-transactions-analyzer.o"

echo 'USB binding transaction production-source gate: PASS'
