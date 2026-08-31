#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$root/build/q047-tmp"}
mkdir -p "$temporary_root"
TMPDIR=$temporary_root
export TMPDIR
work=$(mktemp -d "$temporary_root/usb-recovery-contract.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc=${CC:-cc}
make_command=${MAKE:-make}
common="-std=c11 -I$root/include -I$root/include/uapi -Wall -Wextra -Werror"
fixture="$root/plan/ws004-hardware/tests/usb-recovery-contract-test.c"
function_fixture="$root/plan/ws004-hardware/tests/usb-function-model-test.c"
unregister_fixture="$root/plan/ws003-bringup/tests/usb-hcd-unregister-test.c"
binding_runner="$root/plan/ws004-hardware/tests/run-usb-binding-transactions-test.sh"
usb="$root/src/drivers/usb.c"
xhci="$root/src/drivers/pci-xhci.c"
uhci="$root/src/drivers/pci-uhci.c"
ehci="$root/src/drivers/pci-ehci.c"
storage="$root/src/drivers/usb-storage.c"
header="$root/include/drivers/usb.h"

# The focused fixture includes the production USB core.  Its private-state
# assertions cover exact latch/gate/generation/quarantine and recovery-URB
# ownership without adding test-only public accessors.
# shellcheck disable=SC2086
$cc $common -pthread "$fixture" -o "$work/usb-recovery-contract"
"$work/usb-recovery-contract"

# shellcheck disable=SC2086
$cc $common -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$fixture" -o "$work/usb-recovery-contract-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/usb-recovery-contract-sanitize"

# GCC's analyzer is compile-only; the ordinary and sanitizer binaries own the
# execution evidence and the fixture's allocation ledger is the leak gate.  It
# cannot model the core's atomic status load or the caller/HCD reference split,
# so keep every other analyzer diagnostic fatal while suppressing those two
# known interprocedural false positives.
# shellcheck disable=SC2086
$cc $common -pthread -fanalyzer \
	-Wno-analyzer-use-of-uninitialized-value \
	-Wno-analyzer-use-after-free -c "$fixture" \
	-o "$work/usb-recovery-contract-analyzer.o"

# Keep the two pre-existing fake-HCD contracts link-compatible with the now
# mandatory endpoint_reset operation and the core's IRQ-gate dependency.
# shellcheck disable=SC2086
$cc $common -pthread "$usb" "$function_fixture" \
	-o "$work/usb-function-model"
"$work/usb-function-model"
# shellcheck disable=SC2086
$cc $common -I"$root" "$usb" "$unregister_fixture" \
	-o "$work/usb-hcd-unregister"
"$work/usb-hcd-unregister"
TMPDIR="$temporary_root" "$binding_runner"

# Public/internal registration contract and core ordering.
grep -Fq 'drv_usb_endpoint_clear_halt(' "$header"
grep -Fq '*endpoint_reset)(' "$header"
register_body=$(sed -n '/^int drv_usb_hcd_register(/,/^}/p' "$usb")
printf '%s\n' "$register_body" | grep -Fq 'hcd->ops->endpoint_reset == NULL'

terminal_body=$(sed -n '/^urb_publish_terminal(/,/^}/p' "$usb")
terminal_latch_line=$(printf '%s\n' "$terminal_body" |
	grep -nF 'endpoint_publish_halted(urb->device, urb->endpoint, 1U)' |
	cut -d: -f1)
terminal_status_line=$(printf '%s\n' "$terminal_body" |
	grep -nF 'hal_atomic_store_release(&urb->status, status)' |
	cut -d: -f1)
terminal_callback_line=$(printf '%s\n' "$terminal_body" |
	grep -nF 'urb->callback(urb, urb->callback_argument)' |
	cut -d: -f1)
test -n "$terminal_latch_line" && test -n "$terminal_status_line" &&
	test -n "$terminal_callback_line"
test "$terminal_latch_line" -lt "$terminal_status_line"
test "$terminal_status_line" -lt "$terminal_callback_line"

submit_body=$(sed -n '/^drv_usb_urb_submit(/,/^}/p' "$usb")
test "$(printf '%s\n' "$submit_body" |
	grep -c 'endpoint_is_halted(device, urb->endpoint)')" -ge 2
printf '%s\n' "$submit_body" | grep -Fq 'return EPIPE'

clear_body=$(sed -n '/^drv_usb_endpoint_clear_halt(/,/^}/p' "$usb")
clear_binding_line=$(printf '%s\n' "$clear_body" |
	grep -nF 'device_binding_enter(device)' | cut -d: -f1)
clear_owner_line=$(printf '%s\n' "$clear_body" |
	grep -nF 'endpoint_binding_pin(interface, &owner)' | cut -d: -f1)
clear_selection_line=$(printf '%s\n' "$clear_body" |
	grep -nF 'atomic_try_acquire_zero(&device->selection_gate)' | cut -d: -f1)
clear_io_line=$(printf '%s\n' "$clear_body" |
	grep -nF 'io_gate_close_empty(&interface->io_gate)' | cut -d: -f1)
clear_control_line=$(printf '%s\n' "$clear_body" |
	grep -nF 'device_control_try_lock(device)' | cut -d: -f1)
clear_wire_line=$(printf '%s\n' "$clear_body" |
	grep -nF 'endpoint_clear_halt_request(device, endpoint, &accepted)' |
	cut -d: -f1)
clear_hcd_line=$(printf '%s\n' "$clear_body" |
	grep -nF 'ops->endpoint_reset(device->bus->hcd,' | cut -d: -f1)
clear_unlatch_line=$(printf '%s\n' "$clear_body" |
	grep -nF 'endpoint_publish_halted(device, endpoint, 0U)' | cut -d: -f1)
for line in "$clear_binding_line" "$clear_owner_line" \
    "$clear_selection_line" "$clear_io_line" "$clear_control_line" \
    "$clear_wire_line" "$clear_hcd_line" "$clear_unlatch_line"; do
	test -n "$line"
done
test "$clear_binding_line" -lt "$clear_owner_line"
test "$clear_owner_line" -lt "$clear_selection_line"
test "$clear_selection_line" -lt "$clear_io_line"
test "$clear_io_line" -lt "$clear_control_line"
test "$clear_control_line" -lt "$clear_wire_line"
test "$clear_wire_line" -lt "$clear_hcd_line"
test "$clear_hcd_line" -lt "$clear_unlatch_line"
printf '%s\n' "$clear_body" | grep -Fq 'accepted && error != EPIPE'
printf '%s\n' "$clear_body" | grep -Fq 'device_quarantine_recovery'

grep -Fq 'device->recovery_urb = drv_usb_urb_alloc' "$usb"
grep -Fq 'DRV_USB_URB_RECLAIM_SAFE' "$usb"
grep -Fq 'recovery_urb = device->recovery_urb' "$usb"

reset_body=$(sed -n '/^drv_usb_device_reset(/,/^}/p' "$usb")
reset_topology_line=$(printf '%s\n' "$reset_body" |
	grep -nF 'atomic_try_acquire_zero(&usb_topology_gate)' |
	head -n 1 | cut -d: -f1)
reset_binding_line=$(printf '%s\n' "$reset_body" |
	grep -nF 'io_gate_close_empty(&device->binding_transactions)' |
	cut -d: -f1)
reset_disable_line=$(printf '%s\n' "$reset_body" |
	grep -nF 'device_disable_active_endpoints(device)' | cut -d: -f1)
reset_quiesce_line=$(printf '%s\n' "$reset_body" |
	grep -nF 'ops->device_quiesce(bus->hcd, device)' | cut -d: -f1)
reset_port_line=$(printf '%s\n' "$reset_body" |
	grep -nF 'ops->root_port_reset(bus->hcd, device->port)' | cut -d: -f1)
reset_enable_line=$(printf '%s\n' "$reset_body" |
	grep -nF 'ops->device_enable(bus->hcd, device)' | cut -d: -f1)
reset_address_line=$(printf '%s\n' "$reset_body" |
	grep -nF 'ops->device_set_address(bus->hcd, device,' | cut -d: -f1)
reset_restore_line=$(printf '%s\n' "$reset_body" |
	grep -nF 'configuration_restore(device, configuration)' | cut -d: -f1)
test "$reset_topology_line" -lt "$reset_binding_line"
test "$reset_binding_line" -lt "$reset_disable_line"
test "$reset_disable_line" -lt "$reset_quiesce_line"
test "$reset_quiesce_line" -lt "$reset_port_line"
test "$reset_port_line" -lt "$reset_enable_line"
test "$reset_enable_line" -lt "$reset_address_line"
test "$reset_address_line" -lt "$reset_restore_line"
printf '%s\n' "$reset_body" | grep -Fq 'configuration_effective_owner'
printf '%s\n' "$reset_body" | grep -Fq 'device->parent != bus->root_hub'
printf '%s\n' "$reset_body" | grep -Fq '(port_status & 3U) != 3U'
printf '%s\n' "$reset_body" | grep -Fq 'connection_generation != port_generation'
printf '%s\n' "$reset_body" | grep -Fq 'device_quarantine_recovery'

# xHCI retains producer state across schedule-only disable/enable, performs
# explicit Set TR Dequeue restart, and never repairs a nonzero endpoint during
# ordinary enqueue.
xhci_fill=$(sed -n '/^fill_endpoint(/,/^}/p' "$xhci")
xhci_enable=$(sed -n '/^xhci_endpoint_enable(/,/^}/p' "$xhci")
xhci_disable=$(sed -n '/^xhci_endpoint_disable(/,/^}/p' "$xhci")
xhci_recover=$(sed -n '/^xhci_endpoint_recover(/,/^}/p' "$xhci")
xhci_restart=$(sed -n '/^xhci_endpoint_restart_empty(/,/^}/p' "$xhci")
xhci_cancel=$(sed -n '/^xhci_cancel_request(/,/^}/p' "$xhci")
xhci_enqueue=$(sed -n '/^xhci_urb_enqueue(/,/^}/p' "$xhci")
xhci_claim=$(sed -n '/^transfer_claim(/,/^}/p' "$xhci")
xhci_finish=$(sed -n '/^xhci_completion_finish(/,/^}/p' "$xhci")
xhci_root_reset=$(sed -n '/^xhci_root_port_reset(/,/^}/p' "$xhci")
for body in "$xhci_fill" "$xhci_recover" "$xhci_cancel"; do
	printf '%s\n' "$body" | grep -Fq 'ring.enqueue'
	printf '%s\n' "$body" | grep -Fq 'ring.cycle'
done
printf '%s\n' "$xhci_enable" | grep -Fq 'ep->ring.dma.address == NULL'
printf '%s\n' "$xhci_enable" | grep -Fq 'ring_allocated'
if printf '%s\n' "$xhci_disable" | grep -Fq 'ring_free'; then
	echo 'xHCI source gate: endpoint disable releases the retained ring' >&2
	exit 1
fi
printf '%s\n' "$xhci_restart" | grep -Fq 'wr32(c->doorbells'
printf '%s\n' "$xhci_restart" | grep -Fq 'DRV_XHCI_ENDPOINT_RUNNING'
printf '%s\n' "$xhci_recover" | grep -Fq 'xhci_endpoint_restart_empty'
printf '%s\n' "$xhci_cancel" | grep -Fq 'xhci_endpoint_restart_empty'
printf '%s\n' "$xhci_enqueue" | grep -Fq \
	'q != NULL ? xhci_endpoint_recover(c, d, ep, dci)'
printf '%s\n' "$xhci_enqueue" | grep -Fq \
	'xhci_endpoint_state(c, d, dci) == DRV_XHCI_ENDPOINT_RUNNING'
xhci_marker_line=$(printf '%s\n' "$xhci_claim" |
	grep -nF 'request->endpoint->recovering = 1U' | cut -d: -f1)
xhci_unlink_line=$(printf '%s\n' "$xhci_claim" |
	grep -nF 'xhci_request_unlink_locked(c, request)' | cut -d: -f1)
test "$xhci_marker_line" -lt "$xhci_unlink_line"
xhci_publish_line=$(printf '%s\n' "$xhci_finish" |
	grep -nF 'drv_usb_hcd_complete' | cut -d: -f1)
xhci_marker_release_line=$(printf '%s\n' "$xhci_finish" |
	grep -nF 'xhci_recovery_leave_locked(c, stall_endpoint)' | cut -d: -f1)
test "$xhci_publish_line" -lt "$xhci_marker_release_line"
printf '%s\n' "$xhci_root_reset" | grep -Fq 'XHCI_PORT_CSC'
xhci_change_writes=$(printf '%s\n' "$xhci_root_reset" | tr '\n' ' ' |
	sed 's/;/;\n/g' | grep 'wr32.*XHCI_PORT_CHANGE')
test "$(printf '%s\n' "$xhci_change_writes" | grep -c '~XHCI_PORT_CSC')" \
	-ge 2
if printf '%s\n' "$xhci_change_writes" |
    grep -vF '~XHCI_PORT_CSC' >/dev/null; then
	echo 'xHCI source gate: root reset can acknowledge CSC' >&2
	exit 1
fi

# Legacy HCDs set the STALL publication marker before unlink/free/callback,
# never dereference the endpoint after core ownership may release it, and keep
# controller release closed through callback return.  endpoint_reset alone
# commits DATA0 and clears the retained marker.
for legacy in uhci ehci; do
	eval "source=\$$legacy"
	finish=$(sed -n "/^${legacy}_finish_completion(/,/^}/p" "$source")
	if [ -z "$finish" ]; then
		finish=$(sed -n "/^${legacy}_complete_retired_request(/,/^}/p" \
			"$source")
	fi
	reset=$(sed -n "/^${legacy}_endpoint_reset(/,/^}/p" "$source")
	enqueue=$(sed -n "/${legacy}_urb_enqueue(/,/^}/p" "$source")
	marker_line=$(printf '%s\n' "$finish" |
		grep -nF 'ENDPOINT_STALL_PUBLISHING_SLOT, 1U' | cut -d: -f1)
	inflight_line=$(printf '%s\n' "$finish" |
		grep -nF 'completion_inflight++' | cut -d: -f1)
	complete_line=$(printf '%s\n' "$finish" |
		grep -nF 'drv_usb_hcd_complete' | cut -d: -f1)
	release_line=$(printf '%s\n' "$finish" |
		grep -nF 'completion_inflight--' | cut -d: -f1)
	test -n "$marker_line" && test -n "$inflight_line" &&
		test -n "$complete_line" && test -n "$release_line"
	test "$marker_line" -lt "$complete_line"
	test "$inflight_line" -lt "$complete_line"
	test "$complete_line" -lt "$release_line"
	post_complete=$(printf '%s\n' "$finish" |
		sed -n '/drv_usb_hcd_complete/,$p')
	if printf '%s\n' "$post_complete" |
	    grep -Eq 'drv_usb_endpoint_(hcd_data|set_hcd_data)'; then
		echo "$legacy source gate: endpoint accessed after core completion" >&2
		exit 1
	fi
	printf '%s\n' "$enqueue" | grep -Fq 'ENDPOINT_STALL_PUBLISHING_SLOT'
	printf '%s\n' "$reset" | grep -Fq 'endpoint, 0, 0'
	printf '%s\n' "$reset" | grep -Fq 'ENDPOINT_STALL_PUBLISHING_SLOT, 0'
	grep -Fq '.endpoint_reset' "$source"
	grep -Fq 'completion_inflight' "$source"
done

# Mass Storage must use only the common recovery API.
test "$(grep -c 'drv_usb_endpoint_clear_halt' "$storage")" -ge 3
if grep -Eq 'drv_usb_endpoint_(hcd_data|set_hcd_data)' "$storage"; then
	echo 'USB Storage source gate: class driver still repairs HCD-private state' >&2
	exit 1
fi

# Both production x86 configurations compile the core, all three HCDs, and
# Storage.  This is a build gate only; the runner intentionally starts no QEMU.
TMPDIR="$temporary_root" "$make_command" -C "$root" -j16 \
	ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-amd64-xhci.mk \
	build/amd64/kern64/src/drivers/usb.o \
	build/amd64/kern64/src/drivers/pci-xhci.o \
	build/amd64/kern64/src/drivers/pci-uhci.o \
	build/amd64/kern64/src/drivers/pci-ehci.o \
	build/amd64/kern64/src/drivers/usb-storage.o
TMPDIR="$temporary_root" "$make_command" -C "$root" -j16 \
	ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-pcat-xhci.mk \
	build/pcat/drivers/usb.o build/pcat/drivers/pci-xhci.o \
	build/pcat/drivers/pci-uhci.o build/pcat/drivers/pci-ehci.o \
	build/pcat/drivers/usb-storage.o

echo 'HW-T26 USB recovery production-source gate: PASS'
