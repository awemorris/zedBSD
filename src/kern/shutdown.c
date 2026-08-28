/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/system-device.h"
#include "kern/atomic.h"
#include "kern/sched.h"

extern void net_shutdown_for_boot(void) __attribute__((weak));
extern void drv_usb_shutdown(void) __attribute__((weak));
extern void drv_pci_shutdown(void) __attribute__((weak));

static atomic_uint_t shutdown_preparation_state;

void
system_shutdown_prepare(void)
{
	unsigned expected = 0;

	if (!atomic_compare_exchange(&shutdown_preparation_state, &expected, 1U)) {
		while (atomic_load_acquire(&shutdown_preparation_state) == 1U)
			sched_yield();
		return;
	}
	/* Network close joins driver RX/TX producers.  USB then disconnects class
	 * interfaces and drains HCD ownership before PCI shutdown runs. */
	if (net_shutdown_for_boot != NULL)
		net_shutdown_for_boot();
	if (drv_usb_shutdown != NULL)
		drv_usb_shutdown();
	if (drv_pci_shutdown != NULL)
		drv_pci_shutdown();
	atomic_store_release(&shutdown_preparation_state, 2U);
}
