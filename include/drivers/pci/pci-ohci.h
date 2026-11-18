/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI OHCI (USB 1.1) host controller.
 */
#ifndef KERN_DRIVERS_PCI_OHCI_H
#define KERN_DRIVERS_PCI_OHCI_H

int
drv_pci_ohci_driver_register(void);

void
drv_pci_ohci_probe_roots(void);

void
drv_pci_ohci_report(void);

#endif
