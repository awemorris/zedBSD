/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A generic ECAM PCI host described by a device tree
 * (compatible "pci-host-ecam-generic"), as the emulators' RISC-V virt
 * board has.
 *
 * The configuration space is memory mapped, the firmware leaves the BARs
 * unassigned, and the legacy pins arrive on the interrupt controller the
 * node's interrupt map names.  The driver places the BARs of the root bus
 * in the node's first memory window before the PCI core enumerates it.
 */

#ifndef KERN_DRIVERS_PCI_ECAM_FDT_H
#define KERN_DRIVERS_PCI_ECAM_FDT_H

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <drivers/pci/pci.h>

/* The most interrupt-map entries a host keeps. */
#define DRV_PCI_ECAM_FDT_MAP_MAX	64U

/*
 * One entry of the interrupt map: a function's masked device and function
 * bits with its pin, and the interrupt number they arrive on.
 */
struct drv_pci_ecam_fdt_route {
	uint32_t devfn;
	uint32_t pin;
	unsigned irq;
};

/*
 * Where one host sits and what it decodes.
 *
 * The configuration window covers the buses from first_bus on, 1 MiB each.
 * The memory window is the first 32-bit memory range: memory_pci_base on the
 * bus is memory_cpu_base for the CPU.  The map's masks select the bits of a
 * key that the routes compare.
 */
struct drv_pci_ecam_fdt_config {
	uint64_t config_base;
	uint64_t config_size;
	unsigned first_bus;
	uint64_t memory_cpu_base;
	uint64_t memory_pci_base;
	uint64_t memory_size;
	bool coherent;
	uint32_t devfn_mask;
	uint32_t pin_mask;
	unsigned route_count;
	struct drv_pci_ecam_fdt_route routes[DRV_PCI_ECAM_FDT_MAP_MAX];
};

struct drv_pci_ecam_fdt;

int
drv_pci_ecam_fdt_describe(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_ecam_fdt_config *config);

int
drv_pci_ecam_fdt_start(
	const struct drv_pci_ecam_fdt_config *config,
	struct drv_pci_ecam_fdt **result);

int
drv_pci_ecam_fdt_publish(
	struct drv_pci_ecam_fdt *host);

#endif
