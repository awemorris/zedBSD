/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC/AT PCI host bridge.
 */
#ifndef KERN_DRIVERS_PCI_PCAT_H
#define KERN_DRIVERS_PCI_PCAT_H

#include <stdint.h>

int
drv_pci_pcat_init(void);

/*
 * Assigns the memory BARs the firmware left unassigned, once the ACPI
 * namespace is loaded, and attaches the functions that waited for them
 * (pci-pcat-assign.c, BUG-210).
 */
void
drv_pci_pcat_assign_deferred(void);

/*
 * Grows a function's 64-bit prefetchable memory BAR to size at the same
 * address, when its driver asks: the function's other BARs the new range
 * would cover move past it, and the prefetchable windows of the bridges
 * above it grow to hold both, inside the host bridge's window and clear
 * of every range something else decodes.  ENOSPC when the BAR would have
 * to move or there is no room; any failure leaves the BARs and the
 * windows as they were (pci-pcat-assign.c).
 */
struct drv_pci_device;
int
drv_pci_pcat_grow_bar(
	struct drv_pci_device *device,
	unsigned index,
	uint64_t size);

#endif
