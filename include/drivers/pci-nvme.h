/*
 * PCI NVMe controller driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_DRIVERS_PCI_NVME_H
#define KERN_DRIVERS_PCI_NVME_H

/* Register the standard PCI class driver. */
int
drv_pci_nvme_driver_register(void);

/*
 * Namespace Identify is deferred until the platform has enabled interrupts.
 * Each registered controller publishes its supported active namespace.
 */
void
drv_pci_nvme_probe_namespaces(void);

#endif
