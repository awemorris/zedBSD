/*
 * PCI xHCI command and resource lifecycle decisions
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_DRIVERS_PCI_XHCI_LIFECYCLE_H
#define ZEDBSD_DRIVERS_PCI_XHCI_LIFECYCLE_H

#include <stddef.h>
#include <stdint.h>

/* Endpoint State values from the xHCI Endpoint Context. */
enum drv_xhci_endpoint_state {
	DRV_XHCI_ENDPOINT_DISABLED = 0,
	DRV_XHCI_ENDPOINT_RUNNING = 1,
	DRV_XHCI_ENDPOINT_HALTED = 2,
	DRV_XHCI_ENDPOINT_STOPPED = 3,
	DRV_XHCI_ENDPOINT_ERROR = 4
};

enum drv_xhci_cancel_action {
	/* The controller is quiescent, so no endpoint command is required. */
	DRV_XHCI_CANCEL_COMPLETE = 0,
	DRV_XHCI_CANCEL_STOP_ENDPOINT,
	DRV_XHCI_CANCEL_RESET_ENDPOINT,
	DRV_XHCI_CANCEL_SET_TR_DEQUEUE,
	/* No endpoint command is safe; quiesce the whole controller. */
	DRV_XHCI_CANCEL_QUIESCE_CONTROLLER
};

/*
 * Select only commands valid for the current controller-owned Endpoint State.
 * Callers must reread the output context after each successful command rather
 * than predicting the next state.  Context State Error therefore naturally
 * causes state re-evaluation instead of a blind command retry.
 */
static inline enum drv_xhci_cancel_action
drv_xhci_cancel_action(enum drv_xhci_endpoint_state state,
	int controller_quiesced)
{
	if (controller_quiesced)
		return DRV_XHCI_CANCEL_COMPLETE;

	switch (state) {
	case DRV_XHCI_ENDPOINT_RUNNING:
		return DRV_XHCI_CANCEL_STOP_ENDPOINT;
	case DRV_XHCI_ENDPOINT_HALTED:
		return DRV_XHCI_CANCEL_RESET_ENDPOINT;
	case DRV_XHCI_ENDPOINT_STOPPED:
	case DRV_XHCI_ENDPOINT_ERROR:
		return DRV_XHCI_CANCEL_SET_TR_DEQUEUE;
	case DRV_XHCI_ENDPOINT_DISABLED:
	default:
		return DRV_XHCI_CANCEL_QUIESCE_CONTROLLER;
	}
}

/*
 * Select the command needed before a new TD can be submitted.  A completed
 * STALL leaves an xHCI endpoint Halted even after the USB device accepts
 * CLEAR_FEATURE(ENDPOINT_HALT).  Reset Endpoint changes it to Stopped, then
 * Set TR Dequeue moves past the failed TD to the software producer.  Running
 * is already ready and must not be stopped merely to submit another TD.
 */
static inline enum drv_xhci_cancel_action
drv_xhci_recovery_action(enum drv_xhci_endpoint_state state)
{
	switch (state) {
	case DRV_XHCI_ENDPOINT_RUNNING:
		return DRV_XHCI_CANCEL_COMPLETE;
	case DRV_XHCI_ENDPOINT_HALTED:
		return DRV_XHCI_CANCEL_RESET_ENDPOINT;
	case DRV_XHCI_ENDPOINT_STOPPED:
	case DRV_XHCI_ENDPOINT_ERROR:
		return DRV_XHCI_CANCEL_SET_TR_DEQUEUE;
	case DRV_XHCI_ENDPOINT_DISABLED:
	default:
		return DRV_XHCI_CANCEL_QUIESCE_CONTROLLER;
	}
}

/* Compute bytes transferred through the Normal TRB which raised a Short
 * Packet Event.  Event residual is relative to that TRB, not the full TD.
 * Unlike a Control Data-stage short event, a Normal short retires its TD and
 * is terminal even when the event points at a non-final chained TRB. */
static inline int
drv_xhci_normal_short_actual(uint64_t address, size_t length,
	unsigned trb_offset, size_t residual, size_t *actual)
{
	size_t consumed = 0;
	unsigned current;

	if (actual == NULL)
		return 0;
	for (current = 0; current <= trb_offset; current++) {
		size_t chunk;

		if (length == 0)
			return 0;
		chunk = 0x10000U - (size_t)(address & 0xffffU);
		if (chunk > length)
			chunk = length;
		if (current == trb_offset) {
			if (residual > chunk)
				return 0;
			*actual = consumed + chunk - residual;
			return 1;
		}
		consumed += chunk;
		address += chunk;
		length -= chunk;
	}
	return 0;
}

/* TD Size is the number of packets remaining after the current TRB, capped
 * by the five-bit field.  xHCI requires zero on the final TRB; it is not the
 * number of TRBs remaining. */
static inline unsigned
drv_xhci_normal_td_size(size_t total_length, size_t cumulative_length,
	size_t maximum_packet_size, int final_trb)
{
	size_t packets, completed, remaining;

	if (final_trb || maximum_packet_size == 0 ||
	    cumulative_length > total_length)
		return 0;
	packets = total_length / maximum_packet_size;
	if (total_length % maximum_packet_size != 0)
		packets++;
	completed = cumulative_length / maximum_packet_size;
	remaining = packets > completed ? packets - completed : 0;
	return remaining > 31U ? 31U : (unsigned)remaining;
}

/*
 * A Command Completion Event identifies its command with the complete 64-bit
 * Command TRB Pointer.  Reserved low bits are not silently masked: malformed
 * or unrelated events must not satisfy the pending command.
 */
static inline int
drv_xhci_command_completion_matches(uint64_t submitted_trb,
	uint64_t completed_trb)
{
	return (submitted_trb & 15U) == 0 &&
	    (completed_trb & 15U) == 0 && submitted_trb == completed_trb;
}

/*
 * A request, its bounce buffer, and its URB association remain owned until
 * the transfer dequeue is moved successfully or all controller DMA is proven
 * quiescent.
 */
static inline int
drv_xhci_request_resources_releasable(int set_dequeue_succeeded,
	int controller_quiesced)
{
	return set_dequeue_succeeded || controller_quiesced;
}

/*
 * Endpoint rings, device contexts, the DCBAA entry, and the slot remain owned
 * until Disable Slot succeeds or all controller DMA is proven quiescent.
 */
static inline int
drv_xhci_device_resources_releasable(int disable_slot_succeeded,
	int controller_quiesced)
{
	return disable_slot_succeeded || controller_quiesced;
}

#endif
