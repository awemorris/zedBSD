/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 PCI driver
 */

#ifndef KERN_DRIVERS_PCI_INTEL_AX211_H
#define KERN_DRIVERS_PCI_INTEL_AX211_H

/*
 * Registers the exact Intel AX211 PCI transport driver.
 */
int
drv_pci_intel_ax211_driver_register(void);

/* Publishes retained controllers after platform interrupt initialization. */
void
drv_pci_intel_ax211_devices_ready(void);

#endif
