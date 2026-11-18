/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The placement of a BAR the firmware left unassigned inside the memory
 * windows of a PCI host bridge (BUG-210).
 */

#ifndef DRIVERS_PCI_PCI_WINDOW_H
#define DRIVERS_PCI_PCI_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

/* The least space a placed BAR takes, so that no two functions share a page. */
#define DRV_PCI_WINDOW_MIN_SPAN		0x1000U

/*
 * One range of bus addresses: a window the host bridge forwards to its
 * bus, or a range something already decodes.  A length of zero is no
 * range at all.
 */
struct drv_pci_range {
	uint64_t base;
	uint64_t length;
};

int
drv_pci_window_place(
	const struct drv_pci_range *windows,
	unsigned window_count,
	const struct drv_pci_range *busy,
	unsigned busy_count,
	uint64_t size,
	bool wide,
	uint64_t *address,
	unsigned *window);

#endif
