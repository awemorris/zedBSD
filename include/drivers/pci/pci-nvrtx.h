/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI discovery entry for the nvrtx driver of NVIDIA Turing and later GPUs.
 */

#ifndef DRIVERS_PCI_NVRTX_H
#define DRIVERS_PCI_NVRTX_H

/*
 * Registers the nvrtx PCI driver for NVIDIA display-class functions.
 */
int
drv_pci_nvrtx_driver_register(void);

#endif
