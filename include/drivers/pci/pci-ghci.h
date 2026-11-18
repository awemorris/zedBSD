/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI discovery entry for the generic driver of GHCI GPUs.
 */

#ifndef DRIVERS_PCI_GHCI_H
#define DRIVERS_PCI_GHCI_H

/*
 * Registers the GHCI PCI driver for 3D and other display controllers.
 */
int
drv_pci_ghci_driver_register(void);

#endif
