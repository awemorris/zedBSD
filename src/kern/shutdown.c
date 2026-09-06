/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shutdown preparation.
 *
 * The first caller quiesces the network, USB, and PCI layers in dependency
 * order; every later caller waits until that preparation has finished.
 */

#include "kern/system-device.h"
#include "kern/atomic.h"
#include "kern/sched.h"

extern void net_shutdown_for_boot(void) __attribute__((weak));
extern void drv_usb_shutdown(void) __attribute__((weak));
extern void drv_pci_shutdown(void) __attribute__((weak));

static atomic_uint_t shutdown_preparation_state;

/*
 * Quiesces the device layers before a shutdown or reboot.
 *
 * The preparation runs exactly once; concurrent callers yield until the
 * first caller has finished it.
 */
void
system_shutdown_prepare(
	void)
{
	unsigned expected;

	expected = 0;

	/* Waits for a preparation already claimed by another caller. */
	if (!atomic_compare_exchange(&shutdown_preparation_state, &expected, 1U)) {
		while (atomic_load_acquire(&shutdown_preparation_state) == 1U)
			sched_yield();
		return;
	}

	/*
	 * Network close joins driver RX/TX producers.  USB then disconnects
	 * class interfaces and drains HCD ownership before PCI shutdown runs.
	 */
	if (net_shutdown_for_boot != NULL)
		net_shutdown_for_boot();
	if (drv_usb_shutdown != NULL)
		drv_usb_shutdown();
	if (drv_pci_shutdown != NULL)
		drv_pci_shutdown();

	/* Publishes the finished preparation to the waiting callers. */
	atomic_store_release(&shutdown_preparation_state, 2U);
}
