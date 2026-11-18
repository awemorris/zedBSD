/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's PCI host bridge (UniNorth): its configuration ports
 * and the bus behind it.
 */

#ifndef KERN_DRIVERS_PLATFORM_MAC99_PCI_H
#define KERN_DRIVERS_PLATFORM_MAC99_PCI_H

/*
 * Starts the PCI core and, when the hardware layer names the host
 * bridge's register block, enumerates the bus behind it.  Returns an
 * errno; ENODEV means there is no host, which is not an error for the
 * boot.
 */
int drv_mac99_pci_init(void);

#endif
