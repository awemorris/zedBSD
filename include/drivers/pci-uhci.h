/* PCI UHCI host controller. Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DRIVERS_PCI_UHCI_H
#define ZEDBSD_DRIVERS_PCI_UHCI_H

int drv_pci_uhci_driver_register(void);
void drv_pci_uhci_probe_roots(void);

#endif
