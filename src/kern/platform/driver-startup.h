/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef DRIVERS_PLATFORM_DRIVER_STARTUP_H
#define DRIVERS_PLATFORM_DRIVER_STARTUP_H

/*
 * Registers the drivers for the PCI/PCIe devices.
 */
int
kern_driver_startup_pci_devices(
	void);

/*
 * Registers the drivers for the USB devices.
 */
int
kern_driver_startup_usb_devices(
	void);

/*
 * Registers the drivers for the ISA devices.
 */
int
kern_driver_startup_isa_devices(
	void);

/*
 * Probes the USB roots and readies the PCI WLAN devices.
 */
void
kern_driver_probe_usb_roots_and_pci_wlan(
	void);

#endif
