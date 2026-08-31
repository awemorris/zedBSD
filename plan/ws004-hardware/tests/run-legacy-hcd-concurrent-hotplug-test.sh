#!/bin/sh
# ws004-p031 deterministic legacy-HCD concurrency/hotplug model gate.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$root/build/q047-tmp"}
mkdir -p "$temporary_root"
work=$(mktemp -d "$temporary_root/legacy-hcd-concurrent.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc=${CC:-cc}
make_command=${MAKE:-make}
common="-std=c11 -Wall -Wextra -Werror"
fixture="$root/plan/ws004-hardware/tests/legacy-hcd-concurrent-hotplug-test.c"
uhci="$root/src/drivers/pci-uhci.c"
ehci="$root/src/drivers/pci-ehci.c"
usb="$root/src/drivers/usb.c"
usb_storage="$root/src/drivers/usb-storage.c"
usb_hid_checkpoint="$root/src/drivers/usb-hid-checkpoint.c"
qemu_runner="$root/plan/ws004-hardware/tests/run-legacy-hcd-concurrent-hotplug-qemu.sh"
qemu_config="$root/plan/ws004-hardware/tests/config-amd64-legacy-hcd.mk"

# shellcheck disable=SC2086
$cc $common "$fixture" -o "$work/legacy-hcd-concurrent"
"$work/legacy-hcd-concurrent"

# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$fixture" -o "$work/legacy-hcd-concurrent-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/legacy-hcd-concurrent-sanitize"

# GCC's analyzer is a compile-only ownership pass over the deterministic
# schedule/lifecycle model.  The production UHCI source/object gates below
# prevent the model from standing in for the real controller implementation.
# shellcheck disable=SC2086
$cc $common -fanalyzer -c "$fixture" \
	-o "$work/legacy-hcd-concurrent-analyzer.o"

uhci_build=$(sed -n '/uhci_build_request(/,/^}/p' "$uhci")
uhci_interval=$(sed -n '/uhci_periodic_level(/,/^}/p' "$uhci")
uhci_parameters=$(sed -n '/^uhci_endpoint_parameters(/,/^}/p' "$uhci")
uhci_admit=$(sed -n '/uhci_periodic_admit_locked(/,/^}/p' "$uhci")
uhci_release=$(sed -n '/uhci_periodic_release_locked(/,/^}/p' "$uhci")
uhci_advance_snapshot=$(sed -n \
	'/^uhci_request_advance_snapshot(/,/^}/p' "$uhci")
uhci_qh_progress=$(sed -n \
	'/^uhci_request_qh_progress_locked(/,/^}/p' "$uhci")
uhci_progress_frame=$(sed -n \
	'/^uhci_progress_frame_sample(/,/^}/p' "$uhci")
uhci_progress_watchdog=$(sed -n \
	'/^uhci_qh_progress_watchdog(/,/^}/p' "$uhci")
uhci_retirement_worker=$(sed -n \
	'/^uhci_retirement_worker(void /,/^}/p' "$uhci")
uhci_watchdog_arm=$(sed -n \
	'/^uhci_retirement_watchdog_arm(/,/^}/p' "$uhci")
uhci_enqueue=$(sed -n '/uhci_urb_enqueue(/,/^}/p' "$uhci")
uhci_irq=$(sed -n '/uhci_irq(/,/^}/p' "$uhci")
uhci_root=$(sed -n '/uhci_root_worker(void /,/^}/p' "$uhci")
uhci_root_arm=$(sed -n '/uhci_root_worker_arm(/,/^}/p' "$uhci")
uhci_root_request_stop=$(sed -n \
	'/^uhci_root_worker_request_stop(/,/^}/p' "$uhci")
uhci_root_stop=$(sed -n '/^uhci_root_worker_stop(/,/^}/p' "$uhci")
uhci_retirement_stop=$(sed -n \
	'/^uhci_retirement_worker_stop(/,/^}/p' "$uhci")
uhci_retirement_defer=$(sed -n '/^uhci_retirement_defer(/,/^}/p' "$uhci")
uhci_builder_leave=$(sed -n '/^uhci_builder_leave(/,/^}/p' "$uhci")
uhci_builder_discard=$(sed -n '/^uhci_builder_discard(/,/^}/p' "$uhci")
uhci_wait_submissions=$(sed -n '/^uhci_wait_submissions(/,/^}/p' "$uhci")
uhci_start=$(sed -n '/^static int uhci_start(/,/^}/p' "$uhci")
uhci_hardware_stop=$(sed -n '/^uhci_hardware_stop(/,/^}/p' "$uhci")
uhci_quiesce=$(sed -n '/^uhci_quiesce(/,/^}/p' "$uhci")
uhci_stop=$(sed -n '/^static void uhci_stop(/,/^}/p' "$uhci")
uhci_shutdown=$(sed -n '/^uhci_report_shutdown_evidence(/,/^}/p' "$uhci")
uhci_pci_release=$(sed -n '/^uhci_pci_release(/,/^}/p' "$uhci")
uhci_port=$(sed -n '/uhci_root_port_update(/,/^}/p' "$uhci")
uhci_control=$(sed -n '/uhci_root_hub_control(/,/^}/p' "$uhci")
uhci_cleanup=$(sed -n '/uhci_cleanup(/,/^}/p' "$uhci")
uhci_finish=$(sed -n '/uhci_finish_completion(/,/^}/p' "$uhci")
uhci_dequeue=$(sed -n '/uhci_urb_dequeue(/,/^}/p' "$uhci")
uhci_schedule_initialize=$(sed -n \
	'/^uhci_schedule_initialize(/,/^}/p' "$uhci")
uhci_schedule_release=$(sed -n '/^uhci_schedule_release(/,/^}/p' "$uhci")
uhci_reclaim_acquire=$(sed -n \
	'/^uhci_reclaim_request_acquire(/,/^}/p' "$uhci")
uhci_request_free=$(sed -n \
	'/^static void uhci_request_free(/,/^}/p' "$uhci")
uhci_request_sets_empty=$(sed -n \
	'/^uhci_request_sets_empty_locked(/,/^}/p' "$uhci")
uhci_retirement_fail=$(sed -n '/^uhci_retirement_fail(/,/^}/p' "$uhci")
uhci_retirement_start=$(sed -n \
	'/^uhci_retirement_worker_start(/,/^}/p' "$uhci")
uhci_root_start=$(sed -n '/^uhci_root_worker_start(/,/^}/p' "$uhci")
uhci_attach=$(sed -n '/^uhci_attach(/,/^}/p' "$uhci")

# Production-source gates keep the behavioral model tied to the UHCI
# implementation.  Builders validate every allocation/copy bound, USB 1.1
# endpoint encoding, and the 255-TD schedule capacity before allocating DMA.
for contract in 'length > SIZE_MAX - 8U' 'uhci_endpoint_parameters' \
    'uhci_required_td_count' 'length > packet' \
    'r->td_count != required_tds' 'EOVERFLOW'; do
	printf '%s\n' "$uhci_build" | grep -q "$contract"
done

# Reclaim-safe recovery must use the single request/schedule/bounce graph
# allocated at controller start.  The acquisition function is an explicit
# allocation-forbidden region, and ambiguous retirement keeps that graph busy
# instead of returning it to a later URB.
for contract in DRV_USB_URB_RECLAIM_SAFE \
    'uhci_reclaim_request_acquire(c, length, &r)' \
    'if (!r->reclaim_reserved)' drv_dma_alloc_coherent hal_malloc; do
	printf '%s\n' "$uhci_build" | grep -Fq -- "$contract"
done
for contract in DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE \
    'sizeof(struct drv_usb_control_request)' reclaim_request_busy EBUSY \
    'request->reclaim_reserved = true'; do
	printf '%s\n' "$uhci_reclaim_acquire" | grep -Fq -- "$contract"
done
if printf '%s\n' "$uhci_reclaim_acquire" | \
    grep -Eq 'hal_malloc|drv_dma_alloc_coherent'; then
	echo 'UHCI source gate: reclaim enqueue can allocate memory' >&2
	exit 1
fi
for contract in 'DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE +' \
    'sizeof(struct drv_usb_control_request)' \
    '&controller->reclaim_request.schedule' \
    '&controller->reclaim_request.bounce'; do
	printf '%s\n' "$uhci_schedule_initialize" | grep -Fq -- "$contract"
done
for contract in 'if (r->reclaim_reserved)' \
    'c->reclaim_request_busy = 0' 'r->schedule = schedule' \
    'r->bounce = bounce'; do
	printf '%s\n' "$uhci_request_free" | grep -Fq -- "$contract"
done
for contract in reclaim_request_busy reclaim_request.schedule \
    reclaim_request.bounce; do
	printf '%s\n' "$uhci_schedule_release" | grep -Fq -- "$contract"
done
printf '%s\n' "$uhci_request_sets_empty" | grep -Fq -- \
	'controller->reclaim_request_busy != 0'
printf '%s\n' "$uhci_pci_release" | grep -Fq -- \
	'controller->reclaim_request_busy != 0'
printf '%s\n' "$uhci_retirement_fail" | grep -Fq -- \
	'request->state = UHCI_REQUEST_FAILED'
if printf '%s\n' "$uhci_retirement_fail" | grep -Fq 'uhci_request_free'; then
	echo 'UHCI source gate: ambiguous retirement releases reclaim DMA' >&2
	exit 1
fi
grep -q '(descriptor->maximum_packet_size & 0xf800U) != 0' "$uhci"
grep -q 'data_count > UHCI_MAX_TDS - 2U' "$uhci"
printf '%s\n' "$uhci_interval" | grep -q \
	'normalized > UHCI_MAX_PERIODIC_INTERVAL'
printf '%s\n' "$uhci_interval" | grep -q \
	'normalized = UHCI_MAX_PERIODIC_INTERVAL'
if printf '%s\n' "$uhci_build" | grep -q \
	'descriptor->interval > UHCI_MAX_PERIODIC_INTERVAL'; then
	echo 'UHCI source gate: bInterval 129..255 is rejected instead of capped' >&2
	exit 1
fi
for contract in 'descriptor->interval == 0' \
    'speed == DRV_USB_SPEED_LOW && descriptor->interval < 10U'; do
	printf '%s\n' "$uhci_parameters" | grep -Fq -- "$contract"
done

# Periodic admission assumes all endpoints overlap one worst-case frame and
# preserves a fixed asynchronous reserve.  A reservation must survive schedule
# unlink and be released only by the checked completion/cancel retirement path.
for contract in UHCI_PERIODIC_BUDGET_BIT_TIMES \
    'controller->periodic_bit_times +=' 'request->periodic_reserved = true'; do
	printf '%s\n' "$uhci_admit" | grep -q "$contract"
done
printf '%s\n' "$uhci_release" | grep -q \
	'controller->periodic_bit_times -='
printf '%s\n' "$uhci_finish" | grep -q \
	'uhci_periodic_release_locked(controller, request)'
printf '%s\n' "$uhci_dequeue" | grep -q \
	'uhci_periodic_release_locked(c, r)'

# Early Intel UHCI can leave QH.element on an inactive successful TD.  The
# production workaround must observe the exact same request-local candidate
# across later healthy raw FRNUM and the established 200-ms stuck threshold,
# then copy the immutable TD link.  A successor made inactive by normal HC
# progress is left to the IRQ terminal path.  The workaround must not synthesize
# a pointer, advance an error/final/SPD-short TD, or run from IRQ.
for contract in advance_candidate advance_element advance_status \
    advance_frame advance_started_tick; do
	grep -q "$contract" "$uhci"
done
for contract in hal_io_rmb 'request->qh->element' UHCI_LINK_ADDRESS \
    UHCI_LINK_QH UHCI_LINK_RESERVED UHCI_TD_ACTIVE UHCI_TD_ERRORS \
	'link = request->tds[index].link' UHCI_TD_SHORT_PACKET \
	'actual < expected' 'next_status = request->tds[index + 1U].status' \
	'(next_status & UHCI_TD_ACTIVE) == 0'; do
	printf '%s\n' "$uhci_advance_snapshot" | grep -Fq -- "$contract"
done
printf '%s\n' "$uhci_advance_snapshot" | \
	grep -A1 -F '(next_status & UHCI_TD_ACTIVE) == 0' | grep -Fq 'return 0'
snapshot_barrier_line=$(printf '%s\n' "$uhci_advance_snapshot" | \
	grep -n 'hal_io_rmb' | head -1 | cut -d: -f1)
snapshot_element_line=$(printf '%s\n' "$uhci_advance_snapshot" | \
	grep -n 'element = request->qh->element' | head -1 | cut -d: -f1)
if [ -z "$snapshot_barrier_line" ] || [ -z "$snapshot_element_line" ] || \
    [ "$snapshot_barrier_line" -ge "$snapshot_element_line" ]; then
	echo 'UHCI source gate: QH candidate is read before the DMA read barrier' >&2
	exit 1
fi
for contract in 'request->state != UHCI_REQUEST_ACTIVE' \
    '!request->scheduled' 'request->advance_candidate == index + 1U' \
    'request->advance_element == element' \
    'request->advance_status == status' \
    'frame != request->advance_frame' UHCI_QH_STALL_TICKS \
    'request->qh->element = link' \
    hal_io_wmb UHCI_RETIRE_TICKS; do
	printf '%s\n' "$uhci_qh_progress" | grep -Fq -- "$contract"
done
fresh_line=$(printf '%s\n' "$uhci_qh_progress" | \
	grep -n 'frame != request->advance_frame' | head -1 | cut -d: -f1)
stable_line=$(printf '%s\n' "$uhci_qh_progress" | \
	grep -n 'UHCI_QH_STALL_TICKS' | head -1 | cut -d: -f1)
element_write_line=$(printf '%s\n' "$uhci_qh_progress" | \
	grep -n 'request->qh->element = link' | head -1 | cut -d: -f1)
write_barrier_line=$(printf '%s\n' "$uhci_qh_progress" | \
	grep -n 'hal_io_wmb' | head -1 | cut -d: -f1)
if [ -z "$fresh_line" ] || [ -z "$stable_line" ] || \
    [ -z "$element_write_line" ] || \
    [ -z "$write_barrier_line" ] || \
    [ "$fresh_line" -ge "$stable_line" ] || \
    [ "$stable_line" -ge "$element_write_line" ] || \
    [ "$element_write_line" -ge "$write_barrier_line" ]; then
	echo 'UHCI source gate: QH repair lacks fresh-frame/stability/write order' >&2
	exit 1
fi
grep -q '^#define UHCI_QH_STALL_TICKS 20U$' "$uhci"
for contract in UHCI_FRNUM UHCI_USBSTS UHCI_USBCMD UHCI_FRNUM_MASK \
    UHCI_CMD_RUN UHCI_STS_HOST_SYSTEM_ERROR UHCI_STS_PROCESS_ERROR \
    UHCI_STS_HALTED hal_io_mb; do
	printf '%s\n' "$uhci_progress_frame" | grep -Fq -- "$contract"
done
for contract in spin_lock_irqsave 'request->state != UHCI_REQUEST_ACTIVE' \
    '!request->scheduled' uhci_request_qh_progress_locked \
    'controller->quiescing = 1' 'controller->quarantined = 1' \
    uhci_root_worker_request_stop UHCI_USBINTR '~UHCI_CMD_RUN' \
    'DMA retained'; do
	printf '%s\n' "$uhci_progress_watchdog" | grep -Fq -- "$contract"
done
for contract in uhci_qh_progress_watchdog retirement_wake_generation \
    sched_sleep_locked UHCI_ADVANCE_POLL_TICKS; do
	printf '%s\n' "$uhci_retirement_worker" | grep -Fq -- "$contract"
done
printf '%s\n' "$uhci_watchdog_arm" | grep -Fq -- \
	'controller->retirement_wake_generation++'
printf '%s\n' "$uhci_watchdog_arm" | grep -Fq -- \
	'kernel_notify_task(worker->task)'
printf '%s\n' "$uhci_enqueue" | grep -Fq -- \
	'uhci_retirement_watchdog_arm(c)'
printf '%s\n' "$uhci_retirement_defer" | grep -Fq -- \
	'controller->retirement_wake_generation++'
printf '%s\n' "$uhci_retirement_stop" | grep -Fq -- \
	'controller->retirement_wake_generation++'
if printf '%s\n%s\n' "$uhci_irq" "$uhci_root" | \
    grep -Fq -- 'uhci_qh_progress_watchdog'; then
	echo 'UHCI source gate: QH watchdog runs from IRQ/root topology context' >&2
	exit 1
fi

# The root worker scans from process context on every bounded poll, even after
# a change bit was consumed, so a retained disconnect generation is retried.
printf '%s\n' "$uhci_root" | grep -q '(void)uhci_root_ports_changed'
printf '%s\n' "$uhci_root" | grep -q 'drv_usb_hcd_root_hub_changed'
printf '%s\n' "$uhci_root" | grep -q 'UHCI_ROOT_POLL_TICKS'
printf '%s\n' "$uhci_root" | grep -q 'root_wake_generation'
printf '%s\n' "$uhci_root" | grep -q 'sched_sleep_locked'
grep -q '^#define UHCI_ROOT_POLL_TICKS 10U$' "$uhci"

# Worker notification and reap are serialized by active_lock.  Joining blocks
# every notifier while an auxiliary thread reference keeps the published pointer
# valid across thread_wait; only a successful reap clears that pointer, so a
# failed or bounded-out join remains retryable.
printf '%s\n' "$uhci_root_arm" | grep -q 'root_wake_generation++'
printf '%s\n' "$uhci_root_arm" | grep -q 'kernel_notify_task(worker->task)'
arm_notify_line=$(printf '%s\n' "$uhci_root_arm" | \
	grep -n 'kernel_notify_task(worker->task)' | cut -d: -f1)
arm_unlock_line=$(printf '%s\n' "$uhci_root_arm" | \
	grep -n 'spin_unlock_irqrestore' | tail -1 | cut -d: -f1)
if [ "$arm_notify_line" -ge "$arm_unlock_line" ]; then
	echo 'UHCI source gate: root arm publishes an unprotected worker pointer' >&2
	exit 1
fi
for contract in root_joining 'controller->root_worker = NULL' \
    'thread_wait(worker, NULL)' thread_ref thread_release \
    UHCI_QUIESCE_TICKS 'return EBUSY'; do
	printf '%s\n' "$uhci_root_stop" | grep -q "$contract"
done
printf '%s\n' "$uhci_root_request_stop" | grep -Fq -- \
	'worker != NULL && !controller->root_joining'
stop_clear_line=$(printf '%s\n' "$uhci_root_stop" | \
	grep -n 'controller->root_worker = NULL' | cut -d: -f1)
stop_wait_line=$(printf '%s\n' "$uhci_root_stop" | \
	grep -n 'thread_wait(worker, NULL)' | cut -d: -f1)
stop_success_line=$(printf '%s\n' "$uhci_root_stop" | \
	grep -n 'if (error == 0)' | cut -d: -f1)
stop_ref_line=$(printf '%s\n' "$uhci_root_stop" | \
	grep -n 'thread_ref(worker)' | cut -d: -f1)
stop_release_line=$(printf '%s\n' "$uhci_root_stop" | \
	grep -n 'thread_release(worker)' | tail -1 | cut -d: -f1)
if [ "$stop_ref_line" -ge "$stop_wait_line" ] || \
    [ "$stop_wait_line" -ge "$stop_success_line" ] || \
    [ "$stop_success_line" -ge "$stop_clear_line" ] || \
    [ "$stop_clear_line" -ge "$stop_release_line" ] || \
    [ "$(printf '%s\n' "$uhci_root_stop" | \
    grep -c 'thread_release(worker)')" -ne 2 ]; then
	echo 'UHCI source gate: root worker lacks a retry-safe reap reference' >&2
	exit 1
fi
for contract in retirement_joining 'controller->retirement_worker = NULL' \
    'thread_wait(worker, NULL)' thread_ref thread_release \
    UHCI_QUIESCE_TICKS 'return EBUSY'; do
	printf '%s\n' "$uhci_retirement_stop" | grep -q "$contract"
done
retirement_clear_line=$(printf '%s\n' "$uhci_retirement_stop" | \
	grep -n 'controller->retirement_worker = NULL' | cut -d: -f1)
retirement_wait_line=$(printf '%s\n' "$uhci_retirement_stop" | \
	grep -n 'thread_wait(worker, NULL)' | cut -d: -f1)
retirement_success_line=$(printf '%s\n' "$uhci_retirement_stop" | \
	grep -n 'if (error == 0)' | cut -d: -f1)
retirement_ref_line=$(printf '%s\n' "$uhci_retirement_stop" | \
	grep -n 'thread_ref(worker)' | cut -d: -f1)
retirement_release_line=$(printf '%s\n' "$uhci_retirement_stop" | \
	grep -n 'thread_release(worker)' | tail -1 | cut -d: -f1)
if [ "$retirement_ref_line" -ge "$retirement_wait_line" ] || \
    [ "$retirement_wait_line" -ge "$retirement_success_line" ] || \
    [ "$retirement_success_line" -ge "$retirement_clear_line" ] || \
    [ "$retirement_clear_line" -ge "$retirement_release_line" ] || \
    [ "$(printf '%s\n' "$uhci_retirement_stop" | \
    grep -c 'thread_release(worker)')" -ne 2 ]; then
	echo 'UHCI source gate: retirement worker lacks a retry-safe reap reference' >&2
	exit 1
fi
for notifier in "$uhci_retirement_defer" "$uhci_watchdog_arm"; do
	printf '%s\n' "$notifier" | grep -Fq -- \
		'worker != NULL && !controller->retirement_joining'
done
retirement_notify_line=$(printf '%s\n' "$uhci_retirement_defer" | \
	grep -n 'kernel_notify_task(worker->task)' | cut -d: -f1)
retirement_unlock_line=$(printf '%s\n' "$uhci_retirement_defer" | \
	grep -n 'spin_unlock_irqrestore' | tail -1 | cut -d: -f1)
if [ "$retirement_notify_line" -ge "$retirement_unlock_line" ]; then
	echo 'UHCI source gate: retirement notify publishes an unlocked worker pointer' >&2
	exit 1
fi

# A rejected private builder releases its DMA graph before leaving the counted
# lifetime, and admission closure waits only a bounded interval before carrying
# the builder error through the independent hardware fail-safe.
for contract in 'controller->submitting--' spin_lock_irqsave; do
	printf '%s\n' "$uhci_builder_leave" | grep -Fq -- "$contract"
done
builder_free_line=$(printf '%s\n' "$uhci_builder_discard" | \
	grep -n 'uhci_request_free(controller, request)' | cut -d: -f1)
builder_leave_line=$(printf '%s\n' "$uhci_builder_discard" | \
	grep -n 'uhci_builder_leave(controller)' | cut -d: -f1)
if [ "$builder_free_line" -ge "$builder_leave_line" ]; then
	echo 'UHCI source gate: builder leaves its lifetime before freeing DMA' >&2
	exit 1
fi
for contract in UHCI_QUIESCE_TICKS sched_ticks sched_yield 'return EBUSY'; do
	printf '%s\n' "$uhci_wait_submissions" | grep -Fq -- "$contract"
done
printf '%s\n' "$uhci_quiesce" | grep -Fq -- \
	'builders_error = uhci_wait_submissions(controller)'
printf '%s\n' "$uhci_quiesce" | grep -Fq -- 'return builders_error'

# Quiesce joins both process-context workers before it can publish the DMA
# barrier checkpoint.  stop() independently refuses to release schedule DMA
# while either the worker pointer or its single-owner join is still live.
for contract in uhci_root_worker_stop uhci_retirement_worker_stop \
    uhci_report_shutdown_evidence; do
	printf '%s\n' "$uhci_quiesce" | grep -q "$contract"
done
for contract in 'controller->root_worker == NULL' \
    '!controller->root_joining' 'controller->retirement_worker == NULL' \
    '!controller->retirement_joining'; do
	printf '%s\n' "$uhci_stop" | grep -q "$contract"
	printf '%s\n' "$uhci_shutdown" | grep -q "$contract"
done
printf '%s\n' "$uhci_shutdown" | grep -q \
	'uhci: checked shutdown workers joined'
grep -q 'ehci: checked shutdown workers joined' "$ehci"

# A RUN-visible frame graph is fail-closed even when HCD registration itself
# fails: start and attach cleanup use one checked halt/BME/IRQ barrier, while
# PCI release independently refuses to restore the saved BME lease early.
for contract in 'controller->dma_quiesced = 0' 'run_started = 1' \
    'UHCI_CMD_CF | UHCI_CMD_RUN' 'uhci_wait_running(controller)' \
    'uhci_hardware_stop(controller, "start failure")' \
    'schedule retained'; do
	printf '%s\n' "$uhci_start" | grep -q "$contract"
done
start_barrier_line=$(printf '%s\n' "$uhci_start" | \
	grep -n 'controller->dma_quiesced = 0' | head -1 | cut -d: -f1)
start_run_line=$(printf '%s\n' "$uhci_start" | \
	grep -n 'UHCI_CMD_CF | UHCI_CMD_RUN' | head -1 | cut -d: -f1)
if [ "$start_barrier_line" -ge "$start_run_line" ]; then
	echo 'UHCI source gate: RUN precedes publication of DMA ownership' >&2
	exit 1
fi
for contract in UHCI_USBINTR '~UHCI_CMD_RUN' UHCI_STS_HALTED \
    uhci_bus_master_disable uhci_irq_disestablish \
    'controller->dma_quiesced = 1'; do
	printf '%s\n' "$uhci_hardware_stop" | grep -q "$contract"
done
for contract in 'controller->frame_list.address != NULL' \
    'controller->skeleton_memory.address != NULL' \
    '!controller->dma_quiesced'; do
	printf '%s\n' "$uhci_pci_release" | grep -q "$contract"
done
printf '%s\n' "$uhci_cleanup" | grep -q \
	'uhci_hardware_stop(controller, "attach cleanup")'

# PORTSC writes are rebuilt from explicit R/W state and one requested W1C
# acknowledgement.  The root-control dispatcher must never echo PORTSC itself.
for contract in UHCI_PORT_RW_BITS UHCI_PORT_CHANGE_BITS \
    'value = current & UHCI_PORT_RW_BITS' 'value |= acknowledge'; do
	printf '%s\n' "$uhci_port" | grep -q "$contract"
done
if printf '%s\n' "$uhci_control" | grep -q 'out16'; then
	echo 'UHCI source gate: root control bypasses safe PORTSC helper' >&2
	exit 1
fi

# A worker-originated teardown is rejected before either worker is stopped.
printf '%s\n' "$uhci_cleanup" | grep -q \
	'controller->root_worker == curthread'
printf '%s\n' "$uhci_cleanup" | grep -q \
	'controller->retirement_worker == curthread'
preflight_line=$(printf '%s\n' "$uhci_cleanup" | \
	grep -n 'controller->root_worker == curthread' | cut -d: -f1)
stop_line=$(printf '%s\n' "$uhci_cleanup" | \
	grep -n 'uhci_root_worker_stop' | cut -d: -f1)
if [ "$preflight_line" -ge "$stop_line" ]; then
	echo 'UHCI source gate: worker-self EBUSY preflight follows mutation' >&2
	exit 1
fi
grep -q 'error != EBUSY || !uhci_runtime_operational(controller)' "$uhci"
grep -q 'hcd.capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS' "$uhci"

# EHCI must expose the same per-endpoint concurrency contract for both its
# asynchronous and periodic schedules.  Completion/cancel remains request
# local: async retirement crosses IAA, while periodic retirement crosses the
# checked PSS pause/unlink/resume boundary.
ehci_async=$(sed -n '/ehci_publish_async_request(/,/^}/p' "$ehci")
ehci_periodic=$(sed -n '/ehci_publish_periodic_request(/,/^}/p' "$ehci")
ehci_build=$(sed -n '/^ehci_build_request(/,/^}/p' "$ehci")
ehci_retire_periodic=$(sed -n \
	'/ehci_retire_periodic_request(/,/^}/p' "$ehci")
ehci_parameters=$(sed -n '/^ehci_periodic_parameters(/,/^}/p' "$ehci")
ehci_schedule_initialize=$(sed -n \
	'/^ehci_schedule_initialize(/,/^}/p' "$ehci")
ehci_schedule_release=$(sed -n '/^ehci_schedule_release(/,/^}/p' "$ehci")
ehci_reclaim_acquire=$(sed -n \
	'/^ehci_reclaim_request_acquire(/,/^}/p' "$ehci")
ehci_request_free=$(sed -n '/^ehci_request_free(/,/^}/p' "$ehci")
ehci_unpublished_discard=$(sed -n \
	'/^ehci_unpublished_request_discard(/,/^}/p' "$ehci")
ehci_controller_fail=$(sed -n \
	'/^ehci_controller_fail_locked(/,/^}/p' "$ehci")
ehci_shutdown=$(sed -n \
	'/^ehci_report_shutdown_evidence(/,/^}/p' "$ehci")
ehci_quiesce_requests=$(sed -n \
	'/^ehci_quiesce_requests(/,/^}/p' "$ehci")
ehci_quiesce=$(sed -n '/^ehci_quiesce(/,/^}/p' "$ehci")
ehci_stop=$(sed -n '/^ehci_stop(/,/^}/p' "$ehci")
ehci_pci_release=$(sed -n '/^ehci_pci_release(/,/^}/p' "$ehci")
ehci_begin_iaa=$(sed -n \
	'/^ehci_retirement_begin_iaa_locked(/,/^}/p' "$ehci")
ehci_observe_iaa=$(sed -n \
	'/^ehci_retirement_observe_iaa_locked(/,/^}/p' "$ehci")
ehci_async_insert=$(sed -n '/^ehci_async_insert_locked(/,/^}/p' "$ehci")
ehci_periodic_insert=$(sed -n \
	'/^ehci_periodic_insert_locked(/,/^}/p' "$ehci")
ehci_retirement_start=$(sed -n \
	'/^ehci_retirement_worker_start(/,/^}/p' "$ehci")
ehci_root_start=$(sed -n '/^ehci_root_worker_start(/,/^}/p' "$ehci")
ehci_cleanup=$(sed -n '/^ehci_cleanup(/,/^}/p' "$ehci")
ehci_attach=$(sed -n '/^ehci_attach(/,/^}/p' "$ehci")
ehci_start=$(sed -n '/^ehci_start(/,/^}/p' "$ehci")
ehci_terminal=$(sed -n '/^ehci_request_terminal(/,/^}/p' "$ehci")
ehci_port_control=$(sed -n '/^ehci_root_control(/,/^}/p' "$ehci")
ehci_runtime=$(sed -n '/^ehci_runtime_operational(/,/^}/p' "$ehci")
ehci_detach=$(sed -n '/^ehci_detach(/,/^}/p' "$ehci")
ehci_root=$(sed -n '/ehci_root_worker(void /,/^}/p' "$ehci")
ehci_irq=$(sed -n '/ehci_irq(/,/^}/p' "$ehci")
grep -q 'hcd.capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS' "$ehci"
grep -q 'ehci_endpoint_owner_locked' "$ehci"
grep -q 'request = request->active_next' "$ehci"
printf '%s\n' "$ehci_async" | grep -q \
	'ehci_endpoint_owner_locked(controller,'
printf '%s\n' "$ehci_async" | grep -q \
	'ehci_active_insert_locked(controller, request)'
printf '%s\n' "$ehci_async" | grep -q \
	'ehci_async_insert_locked(controller, request)'
printf '%s\n' "$ehci_periodic" | grep -q \
	'ehci_endpoint_owner_locked(controller,'
printf '%s\n' "$ehci_periodic" | grep -q \
	'ehci_active_insert_locked(controller, request)'
printf '%s\n' "$ehci_periodic" | grep -q \
	'ehci_periodic_insert_locked(controller, request)'

# EHCI has the same controller-start reclaim reserve.  Its unpublished discard
# keeps the builder lifetime through the final reserve/DMA release, while every
# teardown proof independently refuses to free a busy reserve.
for contract in DRV_USB_URB_RECLAIM_SAFE \
    'ehci_reclaim_request_acquire(controller, length,' \
    'if (!request->reclaim_reserved)' drv_dma_alloc_coherent hal_malloc; do
	printf '%s\n' "$ehci_build" | grep -Fq -- "$contract"
done
for contract in DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE \
    'sizeof(struct drv_usb_control_request)' reclaim_request_busy EBUSY \
    'request->reclaim_reserved = true'; do
	printf '%s\n' "$ehci_reclaim_acquire" | grep -Fq -- "$contract"
done
if printf '%s\n' "$ehci_reclaim_acquire" | \
    grep -Eq 'hal_malloc|drv_dma_alloc_coherent'; then
	echo 'EHCI source gate: reclaim enqueue can allocate memory' >&2
	exit 1
fi
for contract in 'DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE +' \
    'sizeof(struct drv_usb_control_request)' \
    '&controller->reclaim_request.schedule' \
    '&controller->reclaim_request.bounce'; do
	printf '%s\n' "$ehci_schedule_initialize" | grep -Fq -- "$contract"
done
for contract in 'if (request->reclaim_reserved)' \
    'controller->reclaim_request_busy = 0' \
    'request->schedule = schedule' 'request->bounce = bounce'; do
	printf '%s\n' "$ehci_request_free" | grep -Fq -- "$contract"
done
discard_free_line=$(printf '%s\n' "$ehci_unpublished_discard" | \
	grep -n 'ehci_request_free(controller, request)' | cut -d: -f1)
discard_leave_line=$(printf '%s\n' "$ehci_unpublished_discard" | \
	grep -n 'ehci_builder_leave(controller)' | cut -d: -f1)
if [ -z "$discard_free_line" ] || [ -z "$discard_leave_line" ] || \
    [ "$discard_free_line" -ge "$discard_leave_line" ]; then
	echo 'EHCI source gate: unpublished request outlives builder pin' >&2
	exit 1
fi
for contract in reclaim_request_busy reclaim_request.schedule \
    reclaim_request.bounce; do
	printf '%s\n' "$ehci_schedule_release" | grep -Fq -- "$contract"
done
for body in "$ehci_quiesce_requests" "$ehci_shutdown" "$ehci_quiesce" \
    "$ehci_stop" \
    "$ehci_pci_release"; do
	printf '%s\n' "$body" | grep -Fq -- 'reclaim_request_busy'
done
printf '%s\n' "$ehci_controller_fail" | grep -Fq -- \
	'request->state = EHCI_REQUEST_FAILED'
if printf '%s\n' "$ehci_controller_fail" | grep -Fq 'ehci_request_free'; then
	echo 'EHCI source gate: ambiguous retirement releases reclaim DMA' >&2
	exit 1
fi

# High-speed bInterval is an exponent in microframes, not a frame interval.
# Prove every legal service-mask form, the 1,024-frame representable limit,
# bit-reversed tree placement, and publication into the selected skeleton node.
for contract in 'interval == 0' 'interval > 14U' \
    'microframes = 1U << (interval - 1U)' '*service_mask = 0xffU' \
    '*service_mask = 0x55U' '*service_mask = 0x11U' \
    '*service_mask = 0x01U' '*period = microframes / 8U'; do
	printf '%s\n' "$ehci_parameters" | grep -Fq -- "$contract"
done
for contract in 'ehci_periodic_parameters(descriptor->interval' \
    'request->periodic_period = toggle' \
    'request->qh->capabilities = request->schedule_class ==' \
    'service_mask | (mult << 30)'; do
	printf '%s\n' "$ehci_build" | grep -Fq -- "$contract"
done
for contract in 'EHCI_PERIODIC_NODES' \
	'ehci_reverse_bits(index, EHCI_PERIODIC_LEVELS - 1U)' \
	'frames[index] = ehci_skeleton_link(controller,' \
	'qh->capabilities = (1U << 30) | 0x01U'; do
	printf '%s\n' "$ehci_schedule_initialize" | grep -Fq -- "$contract"
done
for contract in 'while ((1U << level) < request->periodic_period)' \
	'frame_start = controller->periodic_phase_next[level] &' \
	'controller->periodic_microframe_phase_next[' \
	'base_service_mask <<' \
	'ehci_periodic_reserve_locked(controller, request,' \
	'ehci_reverse_bits(request->periodic_phase, level)' \
    'request->periodic_node' 'ehci_periodic_insert_locked(controller, request)'; do
	printf '%s\n' "$ehci_periodic" | grep -Fq -- "$contract"
done

# Controller startup must observe HCHalted before HCRESET.  Periodic QHs have
# RL=0, while async control/bulk QHs alone receive a NAK reload value.
run_clear_line=$(printf '%s\n' "$ehci_start" | \
	grep -n '~EHCI_CMD_RUN' | head -1 | cut -d: -f1)
halt_wait_line=$(printf '%s\n' "$ehci_start" | \
	grep -n 'ehci_wait_schedule_status(controller, EHCI_STS_HALTED, 0)' | \
	head -1 | cut -d: -f1)
reset_line=$(printf '%s\n' "$ehci_start" | \
	grep -n 'EHCI_USBCMD, EHCI_CMD_RESET' | head -1 | cut -d: -f1)
if [ -z "$run_clear_line" ] || [ -z "$halt_wait_line" ] || \
    [ -z "$reset_line" ] || [ "$run_clear_line" -ge "$halt_wait_line" ] || \
    [ "$halt_wait_line" -ge "$reset_line" ]; then
	echo 'EHCI source gate: RUN-clear/HCHalted/HCRESET order is invalid' >&2
	exit 1
fi
for contract in 'controller->port_power_control' \
    'EHCI_PORT_POWER, 0, 0' 'EHCI_PORT_POWER_GOOD_TICKS'; do
	printf '%s\n' "$ehci_start" | grep -Fq -- "$contract"
done
grep -Fq 'EHCI_HCSPARAMS_PPC' "$ehci"
printf '%s\n' "$ehci_build" | grep -Fq -- \
	'request->schedule_class == EHCI_SCHEDULE_ASYNC ?'
if printf '%s\n' "$ehci_build" | grep -Fq -- \
	'((packet & 0x7ffU) << 16) | (4U << 28)'; then
	echo 'EHCI source gate: periodic QH has a nonzero NAK reload value' >&2
	exit 1
fi

# Starting a reset clears PED as required, and only a HALTED-only qTD maps to
# endpoint STALL.  Transaction/babble/buffer errors remain I/O failures.
printf '%s\n' "$ehci_port_control" | grep -Fq -- \
	'EHCI_PORT_RESET | EHCI_PORT_POWER, EHCI_PORT_ENABLE, 0'
if [ "$(printf '%s\n' "$ehci_terminal" | \
	grep -c 'DRV_USB_URB_STALL')" -ne 2 ] || \
    printf '%s\n' "$ehci_terminal" | grep -Fq -- \
	'& EHCI_QTD_HALTED) != 0 ?'; then
	echo 'EHCI source gate: qTD/QH error classification is ambiguous' >&2
	exit 1
fi

# Each insertion helper initializes the private tail, orders it, and only then
# changes the controller-visible predecessor/head or periodic skeleton link.
async_private_line=$(printf '%s\n' "$ehci_async_insert" | \
	grep -n 'request->qh->horizontal =' | head -1 | cut -d: -f1)
async_first_barrier_line=$(printf '%s\n' "$ehci_async_insert" | \
	grep -n 'hal_io_wmb()' | head -1 | cut -d: -f1)
async_last_barrier_line=$(printf '%s\n' "$ehci_async_insert" | \
	grep -n 'hal_io_wmb()' | tail -1 | cut -d: -f1)
async_previous_line=$(printf '%s\n' "$ehci_async_insert" | \
	grep -n 'controller->async_last->qh->horizontal =' | \
	head -1 | cut -d: -f1)
async_head_line=$(printf '%s\n' "$ehci_async_insert" | \
	grep -n 'controller->async_head->horizontal =' | head -1 | cut -d: -f1)
if [ "$async_private_line" -ge "$async_first_barrier_line" ] ||
    [ "$async_first_barrier_line" -ge "$async_previous_line" ] ||
    [ "$async_private_line" -ge "$async_last_barrier_line" ] ||
    [ "$async_last_barrier_line" -ge "$async_head_line" ]; then
	echo 'EHCI source gate: async QH becomes reachable before DMA publication barrier' >&2
	exit 1
fi
periodic_private_line=$(printf '%s\n' "$ehci_periodic_insert" | \
	grep -n 'request->qh->horizontal =' | head -1 | cut -d: -f1)
periodic_barrier_line=$(printf '%s\n' "$ehci_periodic_insert" | \
	grep -n 'hal_io_wmb()' | head -1 | cut -d: -f1)
periodic_publish_line=$(printf '%s\n' "$ehci_periodic_insert" | \
	grep -n 'periodic_skeleton\[request->periodic_node\].horizontal =' | \
	head -1 | cut -d: -f1)
if [ "$periodic_private_line" -ge "$periodic_barrier_line" ] ||
    [ "$periodic_barrier_line" -ge "$periodic_publish_line" ]; then
	echo 'EHCI source gate: periodic QH becomes reachable before DMA publication barrier' >&2
	exit 1
fi
for contract in ehci_periodic_update_acquire ehci_periodic_pause \
    ehci_periodic_unlink_locked EHCI_REQUEST_WAIT_PERIODIC \
    ehci_periodic_resume ehci_retirement_finish_locked; do
	printf '%s\n' "$ehci_retire_periodic" | grep -q "$contract"
done
grep -q 'ehci_retirement_begin_iaa_locked' "$ehci"
grep -q 'ehci_retirement_observe_iaa_locked' "$ehci"

# IAA is an untagged hardware status bit.  The production proof must clear and
# read back any stale bit before unlink/IAAD, publish exactly one software
# owner, acknowledge in IRQ context, and consume that owner's boolean latch
# only after both IAA and IAAD are observed clear.
iaa_stale_line=$(printf '%s\n' "$ehci_begin_iaa" | \
	grep -n '(status & EHCI_STS_IAA) != 0' | head -1 | cut -d: -f1)
iaa_clear_line=$(printf '%s\n' "$ehci_begin_iaa" | \
	grep -n 'wr32(controller->operational, EHCI_USBSTS, EHCI_STS_IAA)' | \
	head -1 | cut -d: -f1)
iaa_readback_line=$(printf '%s\n' "$ehci_begin_iaa" | \
	grep -n 'status = rd32(controller->operational, EHCI_USBSTS)' | \
	tail -1 | cut -d: -f1)
iaa_unlink_line=$(printf '%s\n' "$ehci_begin_iaa" | \
	grep -n 'ehci_async_unlink_locked(controller, request)' | \
	head -1 | cut -d: -f1)
iaa_barrier_line=$(printf '%s\n' "$ehci_begin_iaa" | \
	grep -n 'hal_io_wmb()' | head -1 | cut -d: -f1)
iaa_owner_line=$(printf '%s\n' "$ehci_begin_iaa" | \
	grep -n 'controller->iaa_owner = request' | head -1 | cut -d: -f1)
iaa_doorbell_line=$(printf '%s\n' "$ehci_begin_iaa" | \
	grep -n 'command | EHCI_CMD_IAAD' | head -1 | cut -d: -f1)
if [ -z "$iaa_stale_line" ] || [ -z "$iaa_clear_line" ] ||
    [ -z "$iaa_readback_line" ] || [ -z "$iaa_unlink_line" ] ||
    [ -z "$iaa_barrier_line" ] || [ -z "$iaa_owner_line" ] ||
    [ -z "$iaa_doorbell_line" ] ||
    [ "$iaa_stale_line" -ge "$iaa_clear_line" ] ||
    [ "$iaa_clear_line" -ge "$iaa_readback_line" ] ||
    [ "$iaa_readback_line" -ge "$iaa_unlink_line" ] ||
    [ "$iaa_unlink_line" -ge "$iaa_barrier_line" ] ||
    [ "$iaa_barrier_line" -ge "$iaa_owner_line" ] ||
    [ "$iaa_owner_line" -ge "$iaa_doorbell_line" ]; then
	echo 'EHCI source gate: stale IAA clear/owner/doorbell ordering is incomplete' >&2
	exit 1
fi
for contract in 'request->state != EHCI_REQUEST_DEACTIVATING' \
    'controller->retirement_head != request' \
    'controller->iaa_owner != NULL' '(command & EHCI_CMD_IAAD) != 0' \
    'request->iaa_observed = 0' 'controller->iaa_owner = request'; do
	printf '%s\n' "$ehci_begin_iaa" | grep -Fq -- "$contract"
done
for contract in 'controller->iaa_owner != request' \
    'request->retirement_generation != controller->retirement_generation' \
    '!request->iaa_observed' '(status & EHCI_STS_IAA) != 0' \
    '(command & EHCI_CMD_IAAD) != 0' 'request->iaa_observed = 0' \
    'controller->iaa_owner = NULL'; do
	printf '%s\n' "$ehci_observe_iaa" | grep -Fq -- "$contract"
done
irq_ack_line=$(printf '%s\n' "$ehci_irq" | \
	grep -n 'wr32(controller->operational, EHCI_USBSTS, acknowledge)' | \
	head -1 | cut -d: -f1)
irq_readback_line=$(printf '%s\n' "$ehci_irq" | \
	grep -n 'readback = rd32(controller->operational, EHCI_USBSTS)' | \
	head -1 | cut -d: -f1)
irq_observe_line=$(printf '%s\n' "$ehci_irq" | \
	grep -n 'iaa_request->iaa_observed = 1' | head -1 | cut -d: -f1)
if [ -z "$irq_ack_line" ] || [ -z "$irq_readback_line" ] ||
    [ -z "$irq_observe_line" ] || [ "$irq_ack_line" -ge "$irq_readback_line" ] ||
    [ "$irq_readback_line" -ge "$irq_observe_line" ]; then
	echo 'EHCI source gate: IRQ publishes IAA evidence before W1C readback' >&2
	exit 1
fi
for contract in 'struct ehci_request *iaa_request = NULL' \
    'controller->iaa_owner != NULL' \
    'iaa_request->state != EHCI_REQUEST_WAIT_IAA' \
    'iaa_request->retirement_generation !=' \
    'controller->retirement_generation' 'iaa_request != NULL'; do
	printf '%s\n' "$ehci_irq" | grep -Fq -- "$contract"
done
if printf '%s\n' "$ehci_retire_periodic" | grep -q 'EHCI_STS_IAA'; then
	echo 'EHCI source gate: periodic retirement incorrectly consumes IAA' >&2
	exit 1
fi
grep -q 'ehci: concurrent async/periodic scheduling active' "$ehci"
grep -q 'ehci: root hotplug worker active' "$ehci"

# Partial attach must have one reverse-order unwind for schedule allocation,
# HCD registration, and each independently published worker.  Creation precedes
# pointer publication, which precedes thread_start, for all four HCD workers.
if [ "$(printf '%s\n' "$uhci_schedule_initialize" | \
	grep -c 'drv_dma_alloc_coherent')" -ne 4 ] ||
    [ "$(printf '%s\n' "$ehci_schedule_initialize" | \
	grep -c 'drv_dma_alloc_coherent')" -ne 5 ]; then
	echo 'legacy HCD source gate: schedule allocation stages changed' >&2
	exit 1
fi
printf '%s\n' "$uhci_schedule_initialize" | grep -q 'uhci_schedule_release'
printf '%s\n' "$ehci_schedule_initialize" | grep -q 'ehci_schedule_release'
for body in "$uhci_retirement_start" "$uhci_root_start" \
    "$ehci_retirement_start" "$ehci_root_start"; do
	create_line=$(printf '%s\n' "$body" | \
		grep -n 'kthread_create' | head -1 | cut -d: -f1)
	publish_line=$(printf '%s\n' "$body" | \
		grep -n 'worker = worker' | head -1 | cut -d: -f1)
	thread_start_line=$(printf '%s\n' "$body" | \
		grep -n 'thread_start(worker)' | head -1 | cut -d: -f1)
	if [ -z "$create_line" ] || [ -z "$publish_line" ] ||
	    [ -z "$thread_start_line" ] || [ "$create_line" -ge "$publish_line" ] ||
	    [ "$publish_line" -ge "$thread_start_line" ]; then
		echo 'legacy HCD source gate: worker creation/publication/start order changed' >&2
		exit 1
	fi
done
for body in "$uhci_attach" "$ehci_attach"; do
	for contract in 'stage = "HCD registration"' 'drv_usb_hcd_register' \
	    'controller->hcd_registered = 1' 'retirement_worker_start(controller)' \
	    'root_worker_start(controller)' 'cleanup(controller)' \
	    'controller->quarantined = 1' 'cleanup failed'; do
		printf '%s\n' "$body" | grep -Fq -- "$contract"
	done
	register_line=$(printf '%s\n' "$body" | \
		grep -n 'drv_usb_hcd_register' | head -1 | cut -d: -f1)
	retirement_start_line=$(printf '%s\n' "$body" | \
		grep -n 'retirement_worker_start(controller)' | head -1 | cut -d: -f1)
	root_start_line=$(printf '%s\n' "$body" | \
		grep -n 'root_worker_start(controller)' | head -1 | cut -d: -f1)
	cleanup_line=$(printf '%s\n' "$body" | \
		grep -n 'cleanup(controller)' | tail -1 | cut -d: -f1)
	if [ "$register_line" -ge "$retirement_start_line" ] ||
	    [ "$retirement_start_line" -ge "$root_start_line" ] ||
	    [ "$root_start_line" -ge "$cleanup_line" ]; then
		echo 'legacy HCD source gate: partial-start unwind order changed' >&2
		exit 1
	fi
done
for contract in uhci_root_worker_stop drv_usb_hcd_unregister \
    'uhci_hardware_stop(controller, "attach cleanup")' \
    uhci_schedule_release; do
	printf '%s\n' "$uhci_cleanup" | grep -Fq -- "$contract"
done
for contract in ehci_root_worker_stop drv_usb_hcd_unregister \
    ehci_retirement_worker_stop \
    'ehci_hardware_stop(controller, "attach cleanup")' \
    ehci_schedule_release; do
	printf '%s\n' "$ehci_cleanup" | grep -Fq -- "$contract"
done
for contract in 'controller->hcd_registered' '!controller->quarantined' \
    '!controller->quiescing' '!controller->dma_quiesced' \
    'controller->retirement_worker != NULL' \
    'controller->root_worker != NULL'; do
	printf '%s\n' "$ehci_runtime" | grep -Fq -- "$contract"
done
printf '%s\n' "$ehci_detach" | grep -Fq -- \
	'error != EBUSY || !ehci_runtime_operational(controller)'

# The root worker is independent of request retirement and invokes the common
# topology rescan only from process context.  Its unconditional bounded poll
# retries a retained disconnect generation even without another PCD edge.
printf '%s\n' "$ehci_root" | grep -q 'drv_usb_hcd_root_hub_changed'
printf '%s\n' "$ehci_root" | grep -q 'EHCI_ROOT_POLL_TICKS'
printf '%s\n' "$ehci_root" | grep -q 'sched_sleep'
printf '%s\n' "$ehci_irq" | grep -q 'ehci_root_event_defer(controller)'
if printf '%s\n' "$ehci_irq" | grep -q \
	'drv_usb_hcd_root_hub_changed'; then
	echo 'EHCI source gate: IRQ performs synchronous topology rescan' >&2
	exit 1
fi
grep -q 'struct thread \*retirement_worker;' "$ehci"
grep -q 'struct thread \*root_worker;' "$ehci"

# HCSPARAMS N_CC is authoritative for companion handoff.  A high-speed
# controller without companions remains usable; only a low/full-speed handoff
# fails, and a claimed owner bit is read back before success is reported.
grep -q 'EHCI_HCSPARAMS_N_CC_MASK' "$ehci"
grep -q 'unsigned companion_count;' "$ehci"
grep -q 'ehci_port_handoff(controller, port, status)' "$ehci"
ehci_handoff=$(sed -n '/^ehci_port_handoff(/,/^}/p' "$ehci")
printf '%s\n' "$ehci_handoff" | grep -q 'companion_count == 0'
printf '%s\n' "$ehci_handoff" | grep -q 'EHCI_PORT_OWNER'
printf '%s\n' "$ehci_handoff" | grep -q 'rd32'
grep -q 'EHCI_PERIODIC_BUDGET_BITS' "$ehci"
grep -q 'uint16_t periodic_budget\[' "$ehci"
grep -q 'ehci_periodic_reserve_locked(controller, request' "$ehci"
grep -q 'ehci_periodic_release_locked(controller, request)' "$ehci"
grep -q 'return ENOSPC' "$ehci"

# USB core treats a reconnect/change observation as a new physical device
# generation.  Failed destruction remains retryable because DISCONNECTING is
# tested again by each legacy-HCD worker's periodic common rescan.
usb_root=$(sed -n '/void drv_usb_hcd_root_hub_changed(/,/^}/p' "$usb")
usb_shutdown=$(sed -n '/void drv_usb_shutdown(/,/^}/p' "$usb")
usb_unregister=$(sed -n '/int drv_usb_hcd_unregister(/,/^}/p' "$usb")
storage_transfer=$(sed -n '/^storage_urb_transfer(/,/^}/p' "$usb_storage")
checkpoint_attach=$(sed -n '/^checkpoint_attach(/,/^}/p' \
	"$usb_hid_checkpoint")
checkpoint_detach=$(sed -n '/^checkpoint_detach(/,/^}/p' \
	"$usb_hid_checkpoint")
printf '%s\n' "$usb_root" | grep -q 'connection_changed ='
printf '%s\n' "$usb_root" | grep -q 'enable_changed ='
printf '%s\n' "$usb_root" | grep -q 'state_changed ='
printf '%s\n' "$usb_root" | grep -q 'port_disabled = connected && !enabled'
printf '%s\n' "$usb_root" | grep -q 'replace_generation = 1'
printf '%s\n' "$usb_root" | grep -q \
	'device_is_disconnecting(present)'
printf '%s\n' "$usb_root" | grep -q 'destroy_device(bus, present)'
printf '%s\n' "$usb_root" | grep -q 'enumerate_port(bus, port, status)'
printf '%s\n' "$usb_root" | grep -Fq '(status & 3U) == 3U'
grep -q '(hcd->capabilities & ~DRV_USB_HCD_CAP_CONCURRENT_URBS) != 0' \
	"$usb"

# Blocking HCD quiesce runs without the topology gate.  The lifecycle claim
# keeps the borrowed bus/root-hub allocation alive until the caller reacquires
# the gate and either releases the claim or unlinks the bus.
grep -q 'unsigned lifecycle_claimed;' "$usb"
grep -q 'uint64_t shutdown_attempt_generation;' "$usb"
printf '%s\n' "$usb_shutdown" | grep -q \
	'bus->shutdown_attempt_generation != generation'
printf '%s\n' "$usb_shutdown" | grep -q \
	'bus->shutdown_attempt_generation = generation'
for body in "$usb_shutdown" "$usb_unregister"; do
	printf '%s\n' "$body" | grep -q 'lifecycle_claimed = 1U'
	printf '%s\n' "$body" | grep -q '!bus->lifecycle_claimed'
	printf '%s\n' "$body" | grep -q 'usb_topology_unlock()'
	printf '%s\n' "$body" | grep -q 'ops->quiesce'
	claim_line=$(printf '%s\n' "$body" | \
		grep -n 'lifecycle_claimed = 1U' | head -1 | cut -d: -f1)
	quiesce_line=$(printf '%s\n' "$body" | \
		grep -n 'ops->quiesce' | head -1 | cut -d: -f1)
	unlock_line=$(printf '%s\n' "$body" | \
		grep -n 'usb_topology_unlock()' | \
		awk -F: -v claim="$claim_line" -v quiesce="$quiesce_line" \
			'$1 > claim && $1 < quiesce { print $1; exit }')
	if [ -z "$unlock_line" ]; then
		echo 'USB source gate: HCD quiesce retains topology gate' >&2
		exit 1
	fi
done
if printf '%s\n' "$usb_shutdown" | grep -q \
	'fetch_or_release(&bus->stopping'; then
	echo 'USB source gate: stopping is mistaken for checked HCD quiesce' >&2
	exit 1
fi
stopping_line=$(printf '%s\n' "$usb_shutdown" | \
	grep -n 'hal_atomic_store_release(&bus->stopping, 1U)' | \
	head -1 | cut -d: -f1)
shutdown_quiesce_line=$(printf '%s\n' "$usb_shutdown" | \
	grep -n 'ops->quiesce' | head -1 | cut -d: -f1)
processed_line=$(printf '%s\n' "$usb_shutdown" | \
	grep -n 'bus->shutdown_processed = 1U' | head -1 | cut -d: -f1)
if [ -z "$stopping_line" ] || [ -z "$shutdown_quiesce_line" ] || \
    [ -z "$processed_line" ] || \
    [ "$stopping_line" -ge "$shutdown_quiesce_line" ] || \
    [ "$shutdown_quiesce_line" -ge "$processed_line" ]; then
	echo 'USB source gate: shutdown processed state precedes checked quiesce' >&2
	exit 1
fi

# HW-T25's runtime overlap oracle comes from the production USB-storage data
# path: accepted is emitted only after submit succeeds while status is still
# PENDING, and completed follows the reusable/DMA ownership barrier.
for contract in 'drv_usb_urb_submit(urb)' \
    'drv_usb_urb_status(urb) == DRV_USB_URB_PENDING' \
    'usb-storage-checkpoint: accepted' 'drv_usb_urb_wait_reusable(urb)' \
    'usb-storage-checkpoint: completed'; do
	printf '%s\n' "$storage_transfer" | grep -q "$contract"
done
storage_accepted_line=$(printf '%s\n' "$storage_transfer" | \
	grep -n 'usb-storage-checkpoint: accepted' | head -1 | cut -d: -f1)
storage_wait_line=$(printf '%s\n' "$storage_transfer" | \
	grep -n 'drv_usb_urb_wait_reusable' | head -1 | cut -d: -f1)
storage_completed_line=$(printf '%s\n' "$storage_transfer" | \
	grep -n 'usb-storage-checkpoint: completed' | head -1 | cut -d: -f1)
if [ "$storage_accepted_line" -ge "$storage_wait_line" ] ||
    [ "$storage_wait_line" -ge "$storage_completed_line" ]; then
	echo 'USB-storage source gate: checkpoint does not bracket reusable wait' >&2
	exit 1
fi
for body in "$checkpoint_attach" "$checkpoint_detach"; do
	printf '%s\n' "$body" | grep -q \
		'drv_usb_interface_set_driver_data(interface, NULL)'
done

# The test-only HID probe publishes driver_data before creating its worker, so
# the kthread failure branch must clear that publication and release the URB,
# buffer, and checkpoint.  Successful creation publishes the worker only after
# kthread_create and starts it only after that pointer is stored.
hid_publish_line=$(printf '%s\n' "$checkpoint_attach" | \
	grep -n 'drv_usb_interface_set_driver_data(interface, checkpoint)' | \
	head -1 | cut -d: -f1)
hid_create_line=$(printf '%s\n' "$checkpoint_attach" | \
	grep -n 'kthread_create(checkpoint_worker' | head -1 | cut -d: -f1)
hid_clear_line=$(printf '%s\n' "$checkpoint_attach" | \
	grep -n 'drv_usb_interface_set_driver_data(interface, NULL)' | \
	head -1 | cut -d: -f1)
hid_worker_publish_line=$(printf '%s\n' "$checkpoint_attach" | \
	grep -n 'checkpoint->worker = worker' | head -1 | cut -d: -f1)
hid_start_line=$(printf '%s\n' "$checkpoint_attach" | \
	grep -n 'thread_start(worker)' | head -1 | cut -d: -f1)
if [ "$hid_publish_line" -ge "$hid_create_line" ] ||
    [ "$hid_create_line" -ge "$hid_clear_line" ] ||
    [ "$hid_clear_line" -ge "$hid_worker_publish_line" ] ||
    [ "$hid_worker_publish_line" -ge "$hid_start_line" ]; then
	echo 'USB HID checkpoint source gate: attach publication/unwind order changed' >&2
	exit 1
fi
for contract in 'drv_usb_urb_free(checkpoint->urb)' \
    'hal_free(checkpoint->buffer)' 'hal_free(checkpoint)' 'return error'; do
	after_clear=$(printf '%s\n' "$checkpoint_attach" | \
		awk -v clear="$hid_clear_line" 'NR > clear { print }')
	printf '%s\n' "$after_clear" | grep -Fq -- "$contract"
done
for contract in checkpoint_close_admission 'drv_usb_urb_cancel(checkpoint->urb)' \
    'drv_usb_urb_drain(checkpoint->urb' checkpoint_join_worker \
    'drv_usb_interface_set_driver_data(interface, NULL)' \
    'drv_usb_urb_free(checkpoint->urb)' 'hal_free(checkpoint->buffer)' \
    'hal_free(checkpoint)'; do
	printf '%s\n' "$checkpoint_detach" | grep -Fq -- "$contract"
done

# The runtime runner must create a real overlap window, reject unsupported
# QEMU topology syntax before boot, and require checked worker joins at reboot.
for contract in 'storage_read_bps=2048' 'throttle-group,id=aux_throttle' \
	'driver=throttle,throttle-group=aux_throttle' \
	'ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-amd64-legacy-hcd.mk' \
	'"$make_command" -B -j16 -C "$root"' \
	'canonical_image=build/amd64/hdd-image.img' \
	'HW-T25-THROTTLE-PRIMED' 'qemu_topology_parser_preflight=pass' \
	'bs=4096 skip=$test_prime_skip count=1' \
	'bs=4096 skip=$test_target_skip count=1' \
	'bs=4096 skip=$test_post_skip count=1' \
	'usb-storage-checkpoint: accepted' \
	'usb-storage-checkpoint: completed' '1[+]0 records in' \
    'port 1 enumeration failed (13)' \
    'uhci: checked shutdown workers joined' \
    'ehci: checked shutdown workers joined'; do
	grep -Fq -- "$contract" "$qemu_runner"
done
if grep -Fq -- "'1+0 records in'" "$qemu_runner"; then
	echo 'HW-T25 QEMU source gate: dd record marker treats + as a regex' >&2
	exit 1
fi
grep -q '^ZEDBSD_USER_PROGRAMS := dd cmp sleep$' "$qemu_config"

# Compile both production ABI variants under the Phase-owned configurations.
TMPDIR="$temporary_root" "$make_command" -B -C "$root" \
	ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-amd64-legacy-hcd.mk \
	build/amd64/kern64/src/drivers/pci-uhci.o \
	build/amd64/kern64/src/drivers/pci-ehci.o \
	build/amd64/kern64/src/drivers/usb.o \
	build/amd64/kern64/src/drivers/usb-storage.o \
	build/amd64/kern64/src/drivers/usb-hid-checkpoint.o
TMPDIR="$temporary_root" "$make_command" -B -C "$root" \
	ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-pcat-xhci.mk \
	build/pcat/drivers/pci-uhci.o build/pcat/drivers/pci-ehci.o \
	build/pcat/drivers/usb.o build/pcat/drivers/usb-storage.o

echo 'legacy HCD concurrency/hotplug model gate: PASS'
