/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * AHCI (SATA) host controller driver
 */

#ifndef KERN_DRIVERS_PCI_AHCI_H
#define KERN_DRIVERS_PCI_AHCI_H

/*
 * Registers the AHCI class driver with the PCI bus.
 */
int
drv_pci_ahci_driver_register(void);

/*
 * Brings up the ports of every attached HBA and publishes their disks.
 *
 * The port reset and IDENTIFY wait on the clock and on interrupts, so the
 * platform calls this once interrupts are enabled.
 */
void
drv_pci_ahci_probe_ports(void);

#endif
