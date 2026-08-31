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
uhci_fail=$(sed -n '/^uhci_retirement_fail(/,/^}/p' "$uhci")
uhci_request_sets_empty=$(sed -n \
	'/^uhci_request_sets_empty_locked(/,/^}/p' "$uhci")
uhci_quiesce_requests=$(sed -n '/^uhci_quiesce_requests(/,/^}/p' "$uhci")
uhci_wait_submissions=$(sed -n '/^uhci_wait_submissions(/,/^}/p' "$uhci")
uhci_quiesce=$(sed -n '/^uhci_quiesce(/,/^}/p' "$uhci")
uhci_hardware_stop=$(sed -n '/^uhci_hardware_stop(/,/^}/p' "$uhci")
uhci_stop=$(sed -n '/^static void uhci_stop(/,/^}/p' "$uhci")
printf '%s\n' "$uhci_dequeue" | grep -q 'uhci_retirement_begin_locked'
printf '%s\n' "$uhci_dequeue" | grep -q 'UHCI_REQUEST_WAIT_FRAME_CANCEL'
printf '%s\n' "$uhci_dequeue" | grep -q 'UHCI_REQUEST_RETIRED_CANCEL'
printf '%s\n' "$uhci_dequeue" | grep -q \
	'inline_retirement = curthread == c->retirement_worker'
printf '%s\n' "$uhci_dequeue" | grep -q 'if (inline_retirement)'
printf '%s\n' "$uhci_dequeue" | grep -q \
	'uhci_retirement_process_request(c, r)'
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

# Quiesce uses the ordinary checked frame-retirement owner for every request
# that was still ACTIVE when admission closed.  A bounded residual/failure is
# retained, but it must not bypass the independent halt/BME/IRQ fail-safe.
for contract in 'for (request = controller->active' \
    'request->state == UHCI_REQUEST_FAILED' \
    'request->state != UHCI_REQUEST_ACTIVE' \
    'DRV_USB_URB_DISCONNECTED' \
    'request->terminal_td_count = request->td_count' \
    'uhci_retirement_begin_locked(controller, request' \
    'UHCI_REQUEST_WAIT_FRAME_COMPLETE' \
    'inline_retirement = curthread == controller->retirement_worker' \
    'uhci_retirement_process(controller)' \
    'uhci_retirement_defer(controller)' UHCI_RETIRE_TICKS \
    'failure_error != 0 ? failure_error : EBUSY'; do
	printf '%s\n' "$uhci_quiesce_requests" | grep -Fq -- "$contract"
done
for contract in 'controller->retirement_error == 0' \
    'controller->retirement_error = error'; do
	printf '%s\n' "$uhci_fail" | grep -Fq -- "$contract"
done
printf '%s\n' "$uhci_quiesce_requests" | grep -Fq -- \
	'failure_error = controller->retirement_error'
if printf '%s\n' "$uhci_quiesce_requests" | \
    grep -Eq 'uhci_request_free|drv_usb_hcd_complete'; then
	echo 'UHCI source gate: quiesce bypasses the unique retirement owner' >&2
	exit 1
fi
if [ "$(printf '%s\n' "$uhci_finish" | grep -c 'uhci_request_free')" -ne 1 ] || \
    [ "$(printf '%s\n' "$uhci_finish" | grep -c 'drv_usb_hcd_complete')" -ne 1 ]; then
	echo 'UHCI source gate: disconnect retirement lacks one free/publication owner' >&2
	exit 1
fi
# Removing the request from the visible sets precedes free/callback publication,
# so an explicit in-flight owner must keep the drain closed across that window.
printf '%s\n' "$uhci_request_sets_empty" | grep -Fq -- \
	'controller->completion_inflight != 0'
uhci_completion_claim_line=$(printf '%s\n' "$uhci_finish" | \
	grep -nF 'controller->completion_inflight++' | cut -d: -f1)
uhci_active_remove_line=$(printf '%s\n' "$uhci_finish" | \
	grep -nF 'uhci_active_remove_locked(controller, request)' | cut -d: -f1)
uhci_request_free_line=$(printf '%s\n' "$uhci_finish" | \
	grep -nF 'uhci_request_free(controller, request)' | cut -d: -f1)
uhci_completion_publish_line=$(printf '%s\n' "$uhci_finish" | \
	grep -nF 'drv_usb_hcd_complete' | cut -d: -f1)
uhci_completion_release_line=$(printf '%s\n' "$uhci_finish" | \
	grep -nF 'controller->completion_inflight--' | cut -d: -f1)
if [ "$uhci_completion_claim_line" -ge "$uhci_active_remove_line" ] || \
    [ "$uhci_active_remove_line" -ge "$uhci_request_free_line" ] || \
    [ "$uhci_request_free_line" -ge "$uhci_completion_publish_line" ] || \
    [ "$uhci_completion_publish_line" -ge "$uhci_completion_release_line" ]; then
	echo 'UHCI source gate: completion drain owner does not bracket free/publication' >&2
	exit 1
fi

for contract in 'controller->quiescing = 1' uhci_wait_submissions \
    uhci_root_worker_stop 'controller->submitting == 0' \
    uhci_quiesce_requests \
    uhci_request_sets_empty_locked uhci_retirement_worker_stop \
    'uhci_hardware_stop(controller, "quiesce")' \
    'if (builders_error != 0)' 'return builders_error' \
    'if (requests_error != 0)' 'return requests_error' \
    'if (root_error != 0)' 'return root_error' \
    'if (worker_error != 0)' 'return worker_error' \
    'if (hardware_error != 0)' 'return hardware_error'; do
	printf '%s\n' "$uhci_quiesce" | grep -Fq -- "$contract"
done
for contract in UHCI_QUIESCE_TICKS sched_ticks sched_yield 'return EBUSY'; do
	printf '%s\n' "$uhci_wait_submissions" | grep -Fq -- "$contract"
done
uhci_builder_wait_line=$(printf '%s\n' "$uhci_quiesce" | \
	grep -nF 'builders_error = uhci_wait_submissions(controller)' | cut -d: -f1)
uhci_root_stop_line=$(printf '%s\n' "$uhci_quiesce" | \
	grep -nF 'root_error = uhci_root_worker_stop(controller)' | cut -d: -f1)
uhci_request_drain_line=$(printf '%s\n' "$uhci_quiesce" | \
	grep -nF 'requests_error = uhci_quiesce_requests(controller)' | cut -d: -f1)
uhci_worker_stop_line=$(printf '%s\n' "$uhci_quiesce" | \
	grep -nF 'worker_error = uhci_retirement_worker_stop(controller)' | cut -d: -f1)
uhci_global_stop_line=$(printf '%s\n' "$uhci_quiesce" | \
	grep -nF 'uhci_hardware_stop(controller, "quiesce")' | cut -d: -f1)
uhci_builder_return_line=$(printf '%s\n' "$uhci_quiesce" | \
	grep -nF 'return builders_error' | cut -d: -f1)
uhci_request_return_line=$(printf '%s\n' "$uhci_quiesce" | \
	grep -nF 'return requests_error' | cut -d: -f1)
uhci_root_return_line=$(printf '%s\n' "$uhci_quiesce" | \
	grep -nF 'return root_error' | cut -d: -f1)
uhci_worker_return_line=$(printf '%s\n' "$uhci_quiesce" | \
	grep -nF 'return worker_error' | cut -d: -f1)
if [ "$uhci_builder_wait_line" -ge "$uhci_request_drain_line" ] || \
    [ "$uhci_request_drain_line" -ge "$uhci_root_stop_line" ] || \
    [ "$uhci_root_stop_line" -ge "$uhci_worker_stop_line" ] || \
    [ "$uhci_worker_stop_line" -ge "$uhci_global_stop_line" ] || \
    [ "$uhci_global_stop_line" -ge "$uhci_builder_return_line" ] || \
    [ "$uhci_global_stop_line" -ge "$uhci_request_return_line" ] || \
    [ "$uhci_global_stop_line" -ge "$uhci_root_return_line" ] || \
    [ "$uhci_global_stop_line" -ge "$uhci_worker_return_line" ]; then
	echo 'UHCI source gate: drain/join error bypasses ordered hardware stop' >&2
	exit 1
fi
if printf '%s\n' "$uhci_quiesce" | \
    sed -n "1,${uhci_global_stop_line}p" | \
    grep -Eq '^[[:space:]]*return[[:space:]]'; then
	echo 'UHCI source gate: quiesce returns before the hardware fail-safe' >&2
	exit 1
fi
for contract in UHCI_USBINTR '~UHCI_CMD_RUN' uhci_bus_master_disable \
    uhci_irq_disestablish 'controller->dma_quiesced = 1'; do
	printf '%s\n' "$uhci_hardware_stop" | grep -Fq -- "$contract"
done
for contract in 'controller->dma_quiesced' \
    'uhci_request_sets_empty_locked(controller)' \
    'controller->retirement_worker == NULL' uhci_schedule_release; do
	printf '%s\n' "$uhci_stop" | grep -Fq -- "$contract"
done

ehci_dequeue=$(sed -n '/ehci_urb_dequeue(/,/^}/p' "$ehci")
ehci_irq=$(sed -n '/ehci_irq(/,/^}/p' "$ehci")
ehci_toggle=$(sed -n '/ehci_request_commit_toggle(/,/^}/p' "$ehci")
ehci_complete=$(sed -n '/ehci_complete_retired_request(/,/^}/p' "$ehci")
ehci_worker=$(sed -n '/ehci_retirement_worker(void /,/^}/p' "$ehci")
ehci_periodic=$(sed -n '/ehci_retire_periodic_request(/,/^}/p' "$ehci")
ehci_fail=$(sed -n '/^ehci_controller_fail_locked(/,/^}/p' "$ehci")
ehci_quiesce=$(sed -n '/^ehci_quiesce(/,/^}/p' "$ehci")
printf '%s\n' "$ehci_dequeue" | grep -q 'ehci_retirement_begin_locked'
printf '%s\n' "$ehci_dequeue" | grep -q 'EHCI_REQUEST_DEACTIVATING'
printf '%s\n' "$ehci_dequeue" | grep -q 'EHCI_REQUEST_WAIT_IAA'
printf '%s\n' "$ehci_dequeue" | grep -q 'EHCI_REQUEST_WAIT_PERIODIC'
printf '%s\n' "$ehci_dequeue" | grep -q 'EHCI_RETIRE_CANCEL'
printf '%s\n' "$ehci_dequeue" | grep -q 'EHCI_REQUEST_RETIRED_CANCEL'
printf '%s\n' "$ehci_dequeue" | grep -q \
	'inline_retirement = curthread == controller->retirement_worker'
printf '%s\n' "$ehci_dequeue" | grep -q 'if (inline_retirement)'
printf '%s\n' "$ehci_dequeue" | grep -q \
	'ehci_retirement_progress(controller)'
printf '%s\n' "$ehci_dequeue" | grep -q \
	'ehci_retirement_worker_wakeup(controller)'
printf '%s\n' "$ehci_dequeue" | grep -q '^[[:space:]]*else$'
printf '%s\n' "$ehci_worker" | grep -q 'ehci_retirement_progress(controller)'
printf '%s\n' "$ehci_irq" | grep -q 'ehci_retirement_begin_locked'
printf '%s\n' "$ehci_irq" | grep -q 'EHCI_RETIRE_COMPLETE'
if printf '%s\n' "$ehci_irq" | grep -q 'request_free'; then
	echo 'EHCI source gate: IRQ still frees request before IAA proof' >&2
	exit 1
fi
for contract in EHCI_CMD_IAAD EHCI_STS_IAA EHCI_STS_ASYNC \
    retirement_generation ehci_async_unlink_locked \
    ehci_retirement_begin_iaa_locked ehci_retirement_observe_iaa_locked \
    'request and DMA retained'; do
	grep -q "$contract" "$ehci"
done
for contract in ehci_periodic_update_acquire ehci_periodic_pause \
    ehci_periodic_unlink_locked EHCI_REQUEST_WAIT_PERIODIC \
    ehci_periodic_resume ehci_retirement_finish_locked; do
	printf '%s\n' "$ehci_periodic" | grep -q "$contract"
done
for contract in 'request->state == EHCI_REQUEST_COMPLETING' \
    'request->state == EHCI_REQUEST_RETIRED_CANCEL' \
    'request->state = EHCI_REQUEST_FAILED'; do
	printf '%s\n' "$ehci_fail" | grep -Fq -- "$contract"
done
ehci_completing_line=$(printf '%s\n' "$ehci_fail" | \
	grep -nF 'request->state == EHCI_REQUEST_COMPLETING' | cut -d: -f1)
ehci_cancel_line=$(printf '%s\n' "$ehci_fail" | \
	grep -nF 'request->state == EHCI_REQUEST_RETIRED_CANCEL' | cut -d: -f1)
ehci_continue_line=$(printf '%s\n' "$ehci_fail" | \
	grep -nF 'continue;' | cut -d: -f1)
ehci_failed_line=$(printf '%s\n' "$ehci_fail" | \
	grep -nF 'request->state = EHCI_REQUEST_FAILED' | cut -d: -f1)
if [ "$ehci_completing_line" -ge "$ehci_continue_line" ] || \
    [ "$ehci_cancel_line" -ge "$ehci_continue_line" ] || \
    [ "$ehci_continue_line" -ge "$ehci_failed_line" ]; then
	echo 'EHCI source gate: fatalization steals a checked terminal owner' >&2
	exit 1
fi
for contract in 'request_error = ehci_quiesce_requests(controller)' \
    'error = ehci_hardware_stop(controller, "quiesce")' \
    'controller->dma_quiesced = 1' 'if (request_error != 0)' \
    'return request_error'; do
	printf '%s\n' "$ehci_quiesce" | grep -Fq -- "$contract"
done
ehci_request_error_line=$(printf '%s\n' "$ehci_quiesce" | \
	grep -nF 'request_error = ehci_quiesce_requests(controller)' | cut -d: -f1)
ehci_stop_line=$(printf '%s\n' "$ehci_quiesce" | \
	grep -nF 'error = ehci_hardware_stop(controller, "quiesce")' | cut -d: -f1)
ehci_dma_quiesced_line=$(printf '%s\n' "$ehci_quiesce" | \
	grep -nF 'controller->dma_quiesced = 1' | cut -d: -f1)
ehci_return_error_line=$(printf '%s\n' "$ehci_quiesce" | \
	grep -nF 'return request_error' | cut -d: -f1)
if [ "$ehci_request_error_line" -ge "$ehci_stop_line" ] || \
    [ "$ehci_stop_line" -ge "$ehci_dma_quiesced_line" ] || \
    [ "$ehci_dma_quiesced_line" -ge "$ehci_return_error_line" ]; then
	echo 'EHCI source gate: request failure bypasses checked DMA stop' >&2
	exit 1
fi

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
printf '%s\n' "$ehci_dequeue" | grep -q \
	'ehci_request_commit_toggle(request)'

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
