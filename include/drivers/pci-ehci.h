/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI EHCI host controller.
 */
#ifndef ZEDBSD_DRIVERS_PCI_EHCI_H
#define ZEDBSD_DRIVERS_PCI_EHCI_H

int
drv_pci_ehci_driver_register(void);
void
drv_pci_ehci_probe_roots(void);

#endif
