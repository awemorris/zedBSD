/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's RP1 south bridge, as the firmware leaves it.
 *
 * RP1 is a PCI device behind the BCM2712's PCIe x4 root complex.  With
 * pciex4_reset=0 in config.txt the firmware keeps the link up and RP1's
 * BARs placed, so its peripherals (USB, Ethernet, GPIO, UARTs) can be
 * reached at the addresses the device tree gives under its rp1 node.  This
 * module finds RP1, checks that the firmware's set-up is there, turns on
 * its memory decode and bus mastering, and translates the rp1 node's
 * addresses to CPU addresses for the drivers of its peripherals.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_RP1_H
#define KERN_DRIVERS_PLATFORM_RPI5_RP1_H

#include <stdint.h>

#include <drivers/generic/fdt.h>

/*
 * Finds RP1 in the device tree at fdt_phys and checks it is reachable.
 * Returns 0, or ENODEV when there is no RP1 or the firmware reset its
 * link (config.txt without pciex4_reset=0), or another errno.
 */
int drv_rp1_init(uint64_t fdt_phys);

/* Reports the device tree RP1 was found in; NULL before drv_rp1_init() succeeded. */
const struct drv_fdt *drv_rp1_fdt(void);

/* Reports the rp1 node, the bus of RP1's peripherals; DRV_FDT_NO_NODE before drv_rp1_init(). */
uint32_t drv_rp1_node(void);

/* Reports the root complex above RP1; DRV_FDT_NO_NODE before drv_rp1_init(). */
uint32_t drv_rp1_root_complex_node(void);

/*
 * Finds the next child of the rp1 node with a compatible string, after the
 * child named by after (DRV_FDT_NO_NODE for the first).  ENOENT when there
 * is no later one.
 */
int drv_rp1_find_child(const char *compatible, uint32_t after, uint32_t *node);

/*
 * Reads one address and size pair of an rp1 child's reg and translates the
 * address to the CPU's physical address space.
 */
int drv_rp1_reg(uint32_t node, unsigned index, uint64_t *cpu_address, uint64_t *size);

/*
 * Maps one address and size pair of an rp1 child's reg as uncached device
 * memory, reporting the mapping and the bytes it covers.
 */
int drv_rp1_map(uint32_t node, unsigned index, volatile uint8_t **mapped, uint64_t *size);

/*
 * Translates an address of the rp1 node's own space to the address on the
 * PCI bus, inside RP1's peripheral BAR.
 */
int drv_rp1_to_pci(uint64_t rp1_address, uint64_t *pci_address);

/*
 * Reports how RP1's peripherals reach memory when they master: a device
 * address is the physical address plus bus_offset, for physical addresses
 * up to physical_limit (inclusive).  coherent is nonzero when the device
 * tree says their accesses snoop the CPU caches.
 */
int drv_rp1_dma_window(uint64_t *bus_offset, uint64_t *physical_limit, int *coherent);

/* Reads one 32-bit register of RP1's PCI configuration space. */
int drv_rp1_config_read32(unsigned offset, uint32_t *value);

/* Writes one 32-bit register of RP1's PCI configuration space. */
int drv_rp1_config_write32(unsigned offset, uint32_t value);

/*
 * Reports the address on the PCI bus of one of RP1's own BARs (0 for the
 * MSI-X table's, 1 for the peripherals'), as the firmware placed it.
 */
int drv_rp1_bar_pci_address(unsigned bar, uint64_t *pci_address);

/*
 * Translates an address on the PCI bus through the root complex's outbound
 * window to the CPU's.  ENOENT when no window holds it.
 */
int drv_rp1_pci_to_cpu(uint64_t pci_address, uint64_t *cpu_address);

#endif
