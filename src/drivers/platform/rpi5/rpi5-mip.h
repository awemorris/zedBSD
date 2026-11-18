/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A BCM2712 MSI receiver (MIP) that takes the message interrupts of the
 * functions behind one PCIe root complex.
 *
 * A function writes a message number to the MIP's doorbell, which the
 * root complex's inbound window carries to the MIP; the MIP turns the
 * number into an edge on one of its GIC lines.  Each message handed out
 * is one line, with the PCI core's handler on it.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_RPI5_MIP_H
#define KERN_DRIVERS_PLATFORM_RPI5_RPI5_MIP_H

#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <drivers/pci/pci.h>

struct drv_rpi5_mip;

/*
 * Starts the MIP at a device tree node: maps its registers, sends every
 * line to the host as an edge, and checks that the GIC has every line.
 * Returns 0, ENODEV for a node that is not a BCM2712 MIP, or another
 * errno, in which case the root complex goes without messages.
 */
int drv_rpi5_mip_start(const struct drv_fdt *fdt, uint32_t node, struct drv_rpi5_mip **result);

/*
 * Takes a free message for (run, argument), with the contract of the PCI
 * core's msi_establish host hook; context is the MIP.
 */
int drv_rpi5_mip_establish(void *context, drv_pci_msi_run_t run, void *argument, unsigned *handle, uint64_t *address, uint32_t *data);

/*
 * Frees a message taken by drv_rpi5_mip_establish(), with the contract of
 * the PCI core's msi_disestablish host hook; context is the MIP.
 */
int drv_rpi5_mip_disestablish(void *context, unsigned handle);

#endif
