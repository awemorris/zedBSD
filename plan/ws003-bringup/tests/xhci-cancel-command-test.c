/*
 * WS003 BR-T28/BR-T29 xHCI cancellation and command lifecycle fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/pci-xhci-lifecycle.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void
test_endpoint_recovery_actions(void)
{
	assert(drv_xhci_cancel_action(DRV_XHCI_ENDPOINT_RUNNING, 0) ==
	    DRV_XHCI_CANCEL_STOP_ENDPOINT);
	assert(drv_xhci_cancel_action(DRV_XHCI_ENDPOINT_HALTED, 0) ==
	    DRV_XHCI_CANCEL_RESET_ENDPOINT);
	assert(drv_xhci_cancel_action(DRV_XHCI_ENDPOINT_STOPPED, 0) ==
	    DRV_XHCI_CANCEL_SET_TR_DEQUEUE);
	assert(drv_xhci_cancel_action(DRV_XHCI_ENDPOINT_ERROR, 0) ==
	    DRV_XHCI_CANCEL_SET_TR_DEQUEUE);
	assert(drv_xhci_cancel_action(DRV_XHCI_ENDPOINT_DISABLED, 0) ==
	    DRV_XHCI_CANCEL_QUIESCE_CONTROLLER);
	assert(drv_xhci_cancel_action((enum drv_xhci_endpoint_state)5, 0) ==
	    DRV_XHCI_CANCEL_QUIESCE_CONTROLLER);
}

static void
test_recovery_sequence(void)
{
	/* Running must become Stopped before the dequeue can be moved. */
	assert(drv_xhci_cancel_action(DRV_XHCI_ENDPOINT_RUNNING, 0) ==
	    DRV_XHCI_CANCEL_STOP_ENDPOINT);
	assert(!drv_xhci_request_resources_releasable(0, 0));
	assert(drv_xhci_cancel_action(DRV_XHCI_ENDPOINT_STOPPED, 0) ==
	    DRV_XHCI_CANCEL_SET_TR_DEQUEUE);
	assert(drv_xhci_request_resources_releasable(1, 0));

	/* Halted must be reset; a successful reset is not itself a release. */
	assert(drv_xhci_cancel_action(DRV_XHCI_ENDPOINT_HALTED, 0) ==
	    DRV_XHCI_CANCEL_RESET_ENDPOINT);
	assert(!drv_xhci_request_resources_releasable(0, 0));
	assert(drv_xhci_cancel_action(DRV_XHCI_ENDPOINT_STOPPED, 0) ==
	    DRV_XHCI_CANCEL_SET_TR_DEQUEUE);

	/* Error may use Set TR Dequeue directly, but failure retains ownership. */
	assert(drv_xhci_cancel_action(DRV_XHCI_ENDPOINT_ERROR, 0) ==
	    DRV_XHCI_CANCEL_SET_TR_DEQUEUE);
	assert(!drv_xhci_request_resources_releasable(0, 0));
}

static void
test_new_td_recovery_sequence(void)
{
	/* An idle Running endpoint accepts a new TD without Stop Endpoint. */
	assert(drv_xhci_recovery_action(DRV_XHCI_ENDPOINT_RUNNING) ==
	    DRV_XHCI_CANCEL_COMPLETE);
	/* STALL recovery is Reset Endpoint followed by Set TR Dequeue. */
	assert(drv_xhci_recovery_action(DRV_XHCI_ENDPOINT_HALTED) ==
	    DRV_XHCI_CANCEL_RESET_ENDPOINT);
	assert(drv_xhci_recovery_action(DRV_XHCI_ENDPOINT_STOPPED) ==
	    DRV_XHCI_CANCEL_SET_TR_DEQUEUE);
	assert(drv_xhci_recovery_action(DRV_XHCI_ENDPOINT_ERROR) ==
	    DRV_XHCI_CANCEL_SET_TR_DEQUEUE);
	assert(drv_xhci_recovery_action(DRV_XHCI_ENDPOINT_DISABLED) ==
	    DRV_XHCI_CANCEL_QUIESCE_CONTROLLER);
	assert(drv_xhci_recovery_action(
	    (enum drv_xhci_endpoint_state)UINT32_MAX) ==
	    DRV_XHCI_CANCEL_QUIESCE_CONTROLLER);
}

static void
test_command_completion_pointer(void)
{
	const uint64_t submitted = UINT64_C(0x0000001234567890);

	assert(drv_xhci_command_completion_matches(submitted, submitted));
	assert(!drv_xhci_command_completion_matches(submitted,
	    submitted + 16U));
	assert(!drv_xhci_command_completion_matches(submitted,
	    submitted - 16U));
	assert(!drv_xhci_command_completion_matches(submitted,
	    submitted | 1U));
	assert(!drv_xhci_command_completion_matches(submitted | 1U,
	    submitted | 1U));
	assert(!drv_xhci_command_completion_matches(submitted,
	    UINT64_C(0x0000000034567890)));
}

static void
test_request_release_boundary(void)
{
	/* A failed Stop, Reset, or Set TR Dequeue is not a release barrier. */
	assert(!drv_xhci_request_resources_releasable(0, 0));
	assert(drv_xhci_request_resources_releasable(1, 0));
	assert(drv_xhci_request_resources_releasable(0, 1));
	assert(drv_xhci_request_resources_releasable(1, 1));
}

static void
test_device_release_boundary(void)
{
	/* A failed Disable Slot retains rings, contexts, DCBAA, and the slot. */
	assert(!drv_xhci_device_resources_releasable(0, 0));
	assert(drv_xhci_device_resources_releasable(1, 0));
	assert(drv_xhci_device_resources_releasable(0, 1));
	assert(drv_xhci_device_resources_releasable(1, 1));
}

static void
test_controller_quiesce_boundary(void)
{
	unsigned state;

	for (state = DRV_XHCI_ENDPOINT_DISABLED;
	     state <= DRV_XHCI_ENDPOINT_ERROR; state++)
		assert(drv_xhci_cancel_action(
		    (enum drv_xhci_endpoint_state)state, 1) ==
		    DRV_XHCI_CANCEL_COMPLETE);
	assert(drv_xhci_cancel_action(
	    (enum drv_xhci_endpoint_state)UINT32_MAX, 1) ==
	    DRV_XHCI_CANCEL_COMPLETE);
}

int
main(void)
{
	test_endpoint_recovery_actions();
	test_recovery_sequence();
	test_new_td_recovery_sequence();
	test_command_completion_pointer();
	test_request_release_boundary();
	test_device_release_boundary();
	test_controller_quiesce_boundary();
	puts("xHCI cancel/command lifecycle test: PASS");
	return 0;
}
