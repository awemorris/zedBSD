#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary=${TMPDIR:-$repo/build/q048-tmp}/usb-hid-driver
ordinary=$temporary/ordinary
sanitized=$temporary/sanitized
hotplug_fixture="$repo/plan/ws006-input/tests/xhci-hid-hot-unplug-test.c"
hotplug_ordinary=$temporary/xhci-hot-unplug-ordinary
hotplug_sanitized=$temporary/xhci-hot-unplug-sanitized

mkdir -p "$temporary"

common="-std=c11 -Wall -Wextra -Werror -Wno-unused-function \
-ffunction-sections -fdata-sections -I$repo/include -I$repo/include/uapi \
-I$repo/src -I$repo"

# The fixture includes the production driver and lets section GC discard paths
# which require a live scheduler/HCD.  Descriptor and event paths are not
# duplicated test implementations.
cc $common -O2 -Wl,--gc-sections \
	"$repo/plan/ws006-input/tests/usb-hid-driver-test.c" \
	"$repo/src/drivers/hid/hid-report.c" -o "$ordinary"
"$ordinary"

cc $common -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	-Wl,--gc-sections \
	"$repo/plan/ws006-input/tests/usb-hid-driver-test.c" \
	"$repo/src/drivers/hid/hid-report.c" -o "$sanitized"
ASAN_OPTIONS=detect_leaks=1 "$sanitized"

cc $common -O2 "$hotplug_fixture" -o "$hotplug_ordinary"
"$hotplug_ordinary"

cc $common -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$hotplug_fixture" -o "$hotplug_sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
	"$hotplug_sanitized"

cc $common -O0 -fanalyzer -c "$hotplug_fixture" \
	-o "$temporary/xhci-hot-unplug-analyzer.o"

cc -std=c11 -O0 -Wall -Wextra -Werror -fanalyzer -nostdinc \
	-ffreestanding -DHAL_ARCH_AMD64 -DHAL_BOARD_PCAT \
	-DZEDBSD_USER_ABI_LP64 -I"$repo/include" -I"$repo/include/uapi" \
	-I"$repo/src" -I"$repo" -I"$repo/libc/include" \
	-c "$repo/src/drivers/usb-hid.c" -o "$temporary/analyzer.o"

xhci_cancel=$(sed -n '/^xhci_cancel_request(/,/^}/p' \
	"$repo/src/drivers/pci-xhci.c")
printf '%s\n' "$xhci_cancel" | grep -Fq \
	'drv_usb_device_is_tearing_down('
printf '%s\n' "$xhci_cancel" | grep -Fq \
	'DRV_XHCI_POST_DEQUEUE_RELEASE_REQUEST'
printf '%s\n' "$xhci_cancel" | grep -Fq \
	'xhci_endpoint_restart_empty'
grep -Fq 'drv_usb_device_is_tearing_down' "$repo/include/drivers/usb.h"
grep -Fq 'USB_DEVICE_LIFECYCLE_DISCONNECTING |' "$repo/src/drivers/usb.c"

echo "usb-hid-driver production source: PASS"
