/*
 * PCI NVMe controller driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DRIVERS_PCI_NVME_H
#define ZEDBSD_DRIVERS_PCI_NVME_H

/* Register the standard PCI class driver. */
int
drv_pci_nvme_driver_register(void);

/*
 * Namespace Identify is deferred until the platform has enabled interrupts.
 * The initial implementation supports one controller and one namespace.
 */
void
drv_pci_nvme_probe_namespaces(void);

#endif
