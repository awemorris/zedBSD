#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$root/build/q027-tmp"}
mkdir -p "$temporary_root"
work=$(mktemp -d "$temporary_root/xhci-concurrent.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc=${CC:-cc}
make_command=${MAKE:-make}
common="-std=c11 -I$root/include -I$root/include/uapi -Wall -Wextra -Werror"
fixture="$root/plan/ws004-hardware/tests/xhci-concurrent-urbs-test.c"
function_fixture="$root/plan/ws004-hardware/tests/usb-function-model-test.c"

# shellcheck disable=SC2086
$cc $common "$fixture" -o "$work/xhci-concurrent"
"$work/xhci-concurrent"

# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$fixture" -o "$work/xhci-concurrent-sanitize"
# LeakSanitizer cannot run under the PTY/ptrace harness used by Codex; this
# fixture owns no heap allocation, while ASan bounds and UBSan remain active.
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/xhci-concurrent-sanitize"

# GCC's analyzer is a compile-only ownership check; it complements the two
# runtime sanitizers without relying on the repository-wide make check target.
# shellcheck disable=SC2086
$cc $common -fanalyzer -c "$fixture" -o "$work/xhci-concurrent-analyzer.o"

xhci="$root/src/drivers/pci-xhci.c"
storage="$root/src/drivers/usb-storage.c"
usb="$root/src/drivers/usb.c"

# The production USB-core fixture supplies a real callback/HCD ownership graph.
# A callback deliberately blocks after terminal publication so drain cannot
# return until drv_usb_hcd_complete() drops HCD ownership after callback return.
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

grep -q 'drv_xhci_transfer_event_matches' "$xhci"
grep -q 'transfer_claim(c, &event)' "$xhci"
grep -q 'xhci_completion_drain(c)' "$xhci"
grep -q 'endpoint->active' "$xhci"
grep -q 'DRV_USB_URB_RECLAIM_SAFE' "$xhci"
grep -q 'DRV_USB_URB_RECLAIM_SAFE' "$storage"
grep -q 'drv_usb_urb_setup_control_flags' "$usb"
grep -q 'drv_usb_urb_drain' "$usb"
grep -q 'drv_usb_device_hcd_capabilities' "$usb"
grep -q 'hcd.capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS' "$xhci"
if grep -q 'DRV_USB_HCD_CAP_CONCURRENT_URBS' \
	"$root/src/drivers/pci-ehci.c" "$root/src/drivers/pci-uhci.c"; then
	echo 'xHCI source audit: EHCI/UHCI advertises concurrent URBs' >&2
	exit 1
fi

# The legacy single-active HCDs share their request slot between submit,
# interrupt completion, cancellation, and quiesce.  p016 replaced the former
# unconditional EBUSY dequeue with checked controller-specific retirement.
# Keep this xHCI regression aware of that contract without duplicating the
# detailed FRNUM/IAA fixture owned by run-legacy-hcd-retirement-test.sh.
for legacy_hcd in ehci uhci; do
	legacy_source="$root/src/drivers/pci-$legacy_hcd.c"
	grep -q 'struct spinlock active_lock;' "$legacy_source"
	grep -q 'submitting = 1;' "$legacy_source"
	grep -q 'quiescing = 1;' "$legacy_source"
	dequeue_body=$(sed -n "/${legacy_hcd}_urb_dequeue(/,/^}/p" \
		"$legacy_source")
	irq_body=$(sed -n "/${legacy_hcd}_irq(/,/^}/p" "$legacy_source")
	printf '%s\n' "$dequeue_body" | grep -q 'REQUEST_RETIRED_CANCEL'
	printf '%s\n' "$dequeue_body" | grep -q 'return 0;'
	if printf '%s\n' "$irq_body" | grep -q 'request_free'; then
		echo "xHCI source audit: $legacy_hcd IRQ bypasses retirement worker" >&2
		exit 1
	fi
done
grep -q 'uhci_retirement_begin_locked' "$root/src/drivers/pci-uhci.c"
grep -q 'uhci_wait_frame_advance' "$root/src/drivers/pci-uhci.c"
grep -q 'ehci_retirement_begin_iaa_locked' "$root/src/drivers/pci-ehci.c"
grep -q 'ehci_retirement_observe_iaa_locked' "$root/src/drivers/pci-ehci.c"
if sed -n '/struct xhci_controller {/,/^};/p' "$xhci" |
	grep -q 'struct xhci_request[[:space:]]*\*active'; then
	echo 'xHCI source audit: controller-global active request remains' >&2
	exit 1
fi

# Both event consumers must claim a Transfer Event before releasing event_lock.
# Check the local lexical window rather than accepting a file-wide name match.
awk '
	/transfer_claim\(c, &event\)/ { claimed = NR }
	/event_unlock\(c\)/ {
		if (claimed != 0 && NR - claimed <= 4) pairs++
		claimed = 0
	}
	END { exit pairs == 2 ? 0 : 1 }
' "$xhci"

echo 'xHCI concurrent URB source audit: PASS'

# Compile the actual production translation units as part of the fixture gate;
# the model cannot substitute for the kernel headers and warning policy.
TMPDIR="$temporary_root" "$make_command" -C "$root" -j16 \
	build/amd64/kern64/src/drivers/pci-xhci.o \
	build/amd64/kern64/src/drivers/pci-ehci.o \
	build/amd64/kern64/src/drivers/pci-uhci.o \
	build/amd64/kern64/src/drivers/usb.o \
	build/amd64/kern64/src/drivers/usb-storage.o
TMPDIR="$temporary_root" "$make_command" -C "$root" -j16 \
	ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-pcat-xhci.mk \
	build/pcat/drivers/pci-xhci.o build/pcat/drivers/pci-ehci.o \
	build/pcat/drivers/pci-uhci.o build/pcat/drivers/usb.o \
	build/pcat/drivers/usb-storage.o
echo 'xHCI concurrent URB production object gate: PASS'
