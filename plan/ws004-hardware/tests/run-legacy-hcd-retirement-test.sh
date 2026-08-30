#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$root/build/q041-tmp"}
mkdir -p "$temporary_root"
work=$(mktemp -d "$temporary_root/legacy-hcd-retirement.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc=${CC:-cc}
make_command=${MAKE:-make}
common="-std=c11 -Wall -Wextra -Werror -pthread"
fixture="$root/plan/ws004-hardware/tests/legacy-hcd-retirement-test.c"
uhci="$root/src/drivers/pci-uhci.c"
ehci="$root/src/drivers/pci-ehci.c"
usb="$root/src/drivers/usb.c"

# shellcheck disable=SC2086
$cc $common "$fixture" -o "$work/legacy-hcd-retirement"
"$work/legacy-hcd-retirement"

# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$fixture" -o "$work/legacy-hcd-retirement-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/legacy-hcd-retirement-sanitize"

# shellcheck disable=SC2086
$cc $common -fanalyzer -c "$fixture" \
	-o "$work/legacy-hcd-retirement-analyzer.o"

# Production source gates complement the behavioral model.  Do not accept a
# model-only implementation: normal completion and dequeue must enter the same
# per-controller state machine, while request_free remains after the checked
# hardware boundary.
for source in "$uhci" "$ehci"; do
	grep -q 'retirement_worker' "$source"
	grep -q 'retirement_pending' "$source"
	grep -q 'REQUEST_FAILED' "$source"
	grep -q 'quarantined = 1' "$source"
	grep -q 'quiescing = 1' "$source"
done

uhci_dequeue=$(sed -n '/uhci_urb_dequeue(/,/^}/p' "$uhci")
uhci_irq=$(sed -n '/uhci_irq(/,/^}/p' "$uhci")
uhci_begin=$(sed -n '/uhci_retirement_begin_locked(/,/^}/p' "$uhci")
uhci_wait=$(sed -n '/uhci_wait_frame_advance(/,/^}/p' "$uhci")
uhci_toggle=$(sed -n '/uhci_request_commit_toggle(/,/^}/p' "$uhci")
uhci_finish=$(sed -n '/uhci_finish_completion(/,/^}/p' "$uhci")
uhci_worker=$(sed -n '/uhci_retirement_worker(void /,/^}/p' "$uhci")
printf '%s\n' "$uhci_dequeue" | grep -q 'uhci_retirement_begin_locked'
printf '%s\n' "$uhci_dequeue" | grep -q 'UHCI_REQUEST_WAIT_FRAME_CANCEL'
printf '%s\n' "$uhci_dequeue" | grep -q 'UHCI_REQUEST_RETIRED_CANCEL'
printf '%s\n' "$uhci_dequeue" | grep -q \
	'inline_retirement = curthread == c->retirement_worker'
printf '%s\n' "$uhci_dequeue" | grep -q 'if (inline_retirement)'
printf '%s\n' "$uhci_dequeue" | grep -q 'uhci_retirement_process(c)'
printf '%s\n' "$uhci_dequeue" | grep -q 'uhci_retirement_defer(c)'
printf '%s\n' "$uhci_dequeue" | grep -q '^[[:space:]]*else$'
printf '%s\n' "$uhci_worker" | grep -q 'uhci_retirement_process(controller)'
printf '%s\n' "$uhci_irq" | grep -q 'uhci_retirement_begin_locked'
printf '%s\n' "$uhci_irq" | grep -q 'UHCI_REQUEST_WAIT_FRAME_COMPLETE'
if printf '%s\n' "$uhci_irq" | grep -q 'uhci_request_free'; then
	echo 'UHCI source gate: IRQ still frees request before worker proof' >&2
	exit 1
fi
for contract in UHCI_FRNUM unlink_frame uhci_wait_frame_advance \
    uhci_retirement_fail 'retaining QH/TD/bounce DMA'; do
	grep -q "$contract" "$uhci"
done
printf '%s\n' "$uhci_begin" | grep -q \
	'unlink_frame = in16(controller->io_base + UHCI_FRNUM)'
if printf '%s\n' "$uhci_begin" | grep 'unlink_frame = ' | \
	grep -q 'UHCI_FRNUM_MASK'; then
	echo 'UHCI source gate: unlink FRNUM snapshot is masked' >&2
	exit 1
fi
for contract in 'status = in16(controller->io_base + UHCI_USBSTS)' \
    'command = in16(controller->io_base + UHCI_USBCMD)' \
    'unlink_frame == UINT16_MAX' 'frame == UINT16_MAX' \
    'status == UINT16_MAX' 'command == UINT16_MAX' \
    '~UHCI_FRNUM_MASK' 'UHCI_STS_HOST_SYSTEM_ERROR' \
    'UHCI_STS_PROCESS_ERROR' 'UHCI_STS_HALTED' 'UHCI_CMD_RUN' \
    'if (frame != unlink_frame)'; do
	printf '%s\n' "$uhci_wait" | grep -q "$contract"
done
uhci_change_line=$(printf '%s\n' "$uhci_wait" | \
	grep -nF 'if (frame != unlink_frame)' | head -n 1 | cut -d: -f1)
for contract in 'controller->retirement_stopping ||' \
    'unlink_frame == UINT16_MAX' 'frame == UINT16_MAX' \
    'status == UINT16_MAX' 'command == UINT16_MAX' \
    '(unlink_frame & (uint16_t)~UHCI_FRNUM_MASK) != 0' \
    '(frame & (uint16_t)~UHCI_FRNUM_MASK) != 0' \
    '(command & UHCI_CMD_RUN) == 0' \
    '(status & (UHCI_STS_HOST_SYSTEM_ERROR' \
    'UHCI_STS_PROCESS_ERROR' 'UHCI_STS_HALTED'; do
	uhci_health_line=$(printf '%s\n' "$uhci_wait" | \
		grep -nF "$contract" | head -n 1 | cut -d: -f1)
	if [ "$uhci_health_line" -ge "$uhci_change_line" ]; then
		echo "UHCI source gate: $contract is checked after frame change" >&2
		exit 1
	fi
done
for contract in 'drv_usb_urb_control_request(request->urb)' \
    'UHCI_TD_ACTIVE | UHCI_TD_ERRORS' 'token >> 19' \
    'drv_usb_endpoint_set_hcd_data'; do
	printf '%s\n' "$uhci_toggle" | grep -q "$contract"
done
printf '%s\n' "$uhci_finish" | grep -q 'uhci_request_commit_toggle(request)'
printf '%s\n' "$uhci_dequeue" | grep -q 'uhci_request_commit_toggle(r)'

ehci_dequeue=$(sed -n '/ehci_urb_dequeue(/,/^}/p' "$ehci")
ehci_irq=$(sed -n '/ehci_irq(/,/^}/p' "$ehci")
ehci_toggle=$(sed -n '/ehci_request_commit_toggle(/,/^}/p' "$ehci")
ehci_complete=$(sed -n '/ehci_complete_retired_request(/,/^}/p' "$ehci")
ehci_worker=$(sed -n '/ehci_retirement_worker(void /,/^}/p' "$ehci")
printf '%s\n' "$ehci_dequeue" | grep -q 'EHCI_REQUEST_DEACTIVATING_CANCEL'
printf '%s\n' "$ehci_dequeue" | grep -q 'EHCI_REQUEST_RETIRED_CANCEL'
printf '%s\n' "$ehci_dequeue" | grep -q \
	'inline_retirement = curthread == c->retirement_worker'
printf '%s\n' "$ehci_dequeue" | grep -q 'if (inline_retirement)'
printf '%s\n' "$ehci_dequeue" | grep -q 'ehci_retirement_progress(c)'
printf '%s\n' "$ehci_dequeue" | grep -q 'ehci_retirement_worker_wakeup(c)'
printf '%s\n' "$ehci_dequeue" | grep -q '^[[:space:]]*else$'
printf '%s\n' "$ehci_worker" | grep -q 'ehci_retirement_progress(controller)'
printf '%s\n' "$ehci_irq" | grep -q 'EHCI_REQUEST_DEACTIVATING_COMPLETE'
if printf '%s\n' "$ehci_irq" | grep -q 'request_free'; then
	echo 'EHCI source gate: IRQ still frees request before IAA proof' >&2
	exit 1
fi
for contract in EHCI_CMD_IAAD EHCI_STS_IAA EHCI_STS_ASYNC \
    retirement_generation ehci_request_deactivate_locked \
    ehci_retirement_begin_iaa_locked ehci_retirement_observe_iaa_locked \
    'request and DMA retained'; do
	grep -q "$contract" "$ehci"
done

# A generic wire SET_ADDRESS transaction must still target USB address zero.
# The allocated address remains reserved for checked failure cleanup and is
# restored immediately after the synchronous request.  xHCI uses the explicit
# device_set_address callback and does not take this path.
wire_address=$(sed -n '/if (bus->hcd->ops->device_set_address != NULL)/,/device->state = DRV_USB_STATE_ADDRESS/p' "$usb")
printf '%s\n' "$wire_address" | grep -q 'device->address = 0;'
printf '%s\n' "$wire_address" | grep -q 'USB_REQ_SET_ADDRESS'
printf '%s\n' "$wire_address" | grep -q 'device->address = (unsigned)address;'
for contract in '!request->control' 'request->qh->token >> 31' \
    'drv_usb_endpoint_set_hcd_data'; do
	printf '%s\n' "$ehci_toggle" | grep -q "$contract"
done
printf '%s\n' "$ehci_complete" | grep -q \
	'ehci_request_commit_toggle(request)'
printf '%s\n' "$ehci_dequeue" | grep -q 'ehci_request_commit_toggle(r)'

TMPDIR="$temporary_root" "$make_command" -C "$root" -j16 \
	ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-amd64-legacy-hcd.mk \
	build/amd64/kern64/src/drivers/pci-uhci.o \
	build/amd64/kern64/src/drivers/pci-ehci.o \
	build/amd64/kern64/src/drivers/usb.o \
	build/amd64/kern64/src/drivers/usb-storage.o
TMPDIR="$temporary_root" "$make_command" -C "$root" -j16 \
	ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-pcat-xhci.mk \
	build/pcat/drivers/pci-uhci.o build/pcat/drivers/pci-ehci.o \
	build/pcat/drivers/usb.o build/pcat/drivers/usb-storage.o

echo 'legacy HCD checked retirement source/build gate: PASS'
