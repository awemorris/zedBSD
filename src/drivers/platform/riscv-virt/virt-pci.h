/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The emulators' RISC-V virt board: its PCI host and what sits behind it.
 */

#ifndef KERN_DRIVERS_RISCV_VIRT_PCI_H
#define KERN_DRIVERS_RISCV_VIRT_PCI_H

#include <drivers/generic/fdt.h>

/*
 * Starts the PCI core and, when the device tree describes a generic ECAM
 * host, places its BARs and enumerates its bus.  Returns an errno; ENODEV
 * means there is no host, which is not an error for the boot.
 */
int drv_riscv_virt_pci_init(const struct drv_fdt *fdt);

/*
 * Finishes discovery behind the host once interrupts are enabled: the USB
 * host controllers look at their root ports and the NVMe controllers
 * publish their namespaces.
 */
void drv_riscv_virt_pci_refresh(void);

/* Lets the USB keyboards and mice type once the input core is ready. */
void drv_riscv_virt_pci_input_ready(void);

#endif
