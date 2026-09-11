/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_DRIVERS_PCI_XHCI_H
#define KERN_DRIVERS_PCI_XHCI_H
int drv_pci_xhci_driver_register(void);
void drv_pci_xhci_probe_roots(void);
#endif
