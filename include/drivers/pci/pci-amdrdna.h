/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI discovery entry for the amdrdna driver of AMD RDNA GPUs.
 */

#ifndef DRIVERS_PCI_AMDRDNA_H
#define DRIVERS_PCI_AMDRDNA_H

/*
 * Registers the amdrdna PCI driver for AMD display-class functions.
 */
int
drv_pci_amdrdna_driver_register(void);

#endif
