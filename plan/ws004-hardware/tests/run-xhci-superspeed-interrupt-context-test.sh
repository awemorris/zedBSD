#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$root/build/q045-tmp"}
mkdir -p "$temporary_root"
work=$(mktemp -d "$temporary_root/xhci-ss-interrupt.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc=${CC:-cc}
make_command=${MAKE:-make}
common="-std=c11 -I$root/include -I$root/include/uapi -Wall -Wextra -Werror"
fixture="$root/plan/ws004-hardware/tests/xhci-superspeed-interrupt-context-test.c"
xhci_model="$root/plan/ws004-hardware/tests/xhci-model-test.c"
function_fixture="$root/plan/ws004-hardware/tests/usb-function-model-test.c"
usb="$root/src/drivers/usb.c"
xhci="$root/src/drivers/pci-xhci.c"

# Exact endpoint-context and compatibility corpus.
# shellcheck disable=SC2086
$cc $common "$fixture" -o "$work/xhci-ss-interrupt"
"$work/xhci-ss-interrupt"
# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$fixture" -o "$work/xhci-ss-interrupt-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/xhci-ss-interrupt-sanitize"
# shellcheck disable=SC2086
$cc $common -fanalyzer -c "$fixture" \
	-o "$work/xhci-ss-interrupt-analyzer.o"

# Existing pure xHCI arithmetic remains unchanged.
# shellcheck disable=SC2086
$cc $common "$xhci_model" -o "$work/xhci-model"
"$work/xhci-model"
# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$xhci_model" -o "$work/xhci-model-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/xhci-model-sanitize"
# shellcheck disable=SC2086
$cc $common -fanalyzer -c "$xhci_model" \
	-o "$work/xhci-model-analyzer.o"

# Production USB parsing proves host-endian retention and the typed accessor.
# shellcheck disable=SC2086
$cc $common -pthread "$usb" "$function_fixture" \
	-o "$work/usb-function-model"
"$work/usb-function-model"
# shellcheck disable=SC2086
$cc $common -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$usb" "$function_fixture" -o "$work/usb-function-model-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/usb-function-model-sanitize"
# shellcheck disable=SC2086
$cc $common -pthread -fanalyzer -c "$function_fixture" \
	-o "$work/usb-function-model-analyzer.o"

# A malformed SS interrupt descriptor must fail before transfer-ring DMA is
# allocated.  Keep this ordering check tied to the production function body.
endpoint_enable=$(sed -n '/^xhci_endpoint_enable(/,/^}/p' "$xhci")
encode_line=$(printf '%s\n' "$endpoint_enable" |
	grep -n 'drv_xhci_endpoint_context_encode' | head -n 1 | cut -d: -f1)
ring_line=$(printf '%s\n' "$endpoint_enable" |
	grep -n 'ring_alloc(c, &ep->ring)' | head -n 1 | cut -d: -f1)
test -n "$encode_line" && test -n "$ring_line"
test "$encode_line" -lt "$ring_line"
printf '%s\n' "$endpoint_enable" |
	grep -q 'drv_usb_endpoint_superspeed_companion'
grep -q 'w\[4\] = encoded->word4;' "$xhci"
echo 'xHCI SuperSpeed interrupt source ordering: PASS'

# Compile the production translation units under both configured x86 builds.
TMPDIR="$temporary_root" "$make_command" -C "$root" -j16 \
	ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-amd64-xhci.mk \
	build/amd64/kern64/src/drivers/pci-xhci.o \
	build/amd64/kern64/src/drivers/usb.o
TMPDIR="$temporary_root" "$make_command" -C "$root" -j16 \
	ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-pcat-xhci.mk \
	build/pcat/drivers/pci-xhci.o build/pcat/drivers/usb.o
echo 'xHCI SuperSpeed interrupt production object gate: PASS'
