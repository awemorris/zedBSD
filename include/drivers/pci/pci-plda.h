/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PLDA PCIe root complex, as the StarFive JH7110 has two of.
 *
 * Each root complex has a register block, a configuration window the
 * driver opens with an address translation entry, and memory windows the
 * device tree's ranges describe.  The four legacy pins and the root
 * complex's own events share one interrupt line: the driver reads the
 * status, runs the pins' lines through the PCI core's pin hooks, and
 * acknowledges.  A root complex the device tree calls an MSI controller
 * also receives its functions' message interrupts on that line and hands
 * them to the PCI core's message hooks.  What lies outside the block -- the reset line PERST#, the
 * link state and the selection of the root port's function -- belongs to
 * the board, which hands the driver its operations.
 */

#ifndef KERN_DRIVERS_PCI_PLDA_H
#define KERN_DRIVERS_PCI_PLDA_H

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <drivers/pci/pci.h>

/* The legacy interrupt pins INTA to INTD. */
#define DRV_PCI_PLDA_INTX_COUNT		4U

/* The most memory windows a root complex translates. */
#define DRV_PCI_PLDA_WINDOW_MAX		4U

struct drv_pci_plda;

/*
 * One memory window from the CPU to the PCI bus.
 *
 * The CPU reaches PCI address pci_base at cpu_base; prefetchable and
 * wide state the device tree's space code.
 */
struct drv_pci_plda_window {
	uint64_t cpu_base;
	uint64_t pci_base;
	uint64_t size;
	bool wide;
	bool prefetchable;
};

/*
 * Where one root complex sits and what it decodes.
 *
 * The platform fills it with drv_pci_plda_describe() and may then adjust
 * what the device tree does not say.  intx_vector[n] is the host's own
 * number for pin n + 1, the interrupt map's specifier, zero where the map
 * routes no pin.  segment tells the root complexes apart in the PCI core.
 * msi says the node is an MSI controller (msi-controller), so the host
 * offers MSI to its functions.
 */
struct drv_pci_plda_config {
	uint64_t register_base;
	uint64_t register_size;
	uint64_t config_base;
	uint64_t config_size;
	unsigned irq;
	unsigned segment;
	bool coherent;
	unsigned window_count;
	struct drv_pci_plda_window windows[DRV_PCI_PLDA_WINDOW_MAX];
	unsigned intx_vector[DRV_PCI_PLDA_INTX_COUNT];
	bool msi;
};

/*
 * What the board does for a root complex.
 *
 * perst drives PERST# (nonzero asserts it), link_active reports whether
 * the data link layer is up, and select_function chooses which of the
 * root port's functions the register block's function registers reach.
 * Each runs in thread context and may not fail.
 */
struct drv_pci_plda_board {
	void *context;
	void (
		*perst)(
		void *,
		int);
	int (
		*link_active)(
		void *);
	void (
		*select_function)(
		void *,
		unsigned);
};

/*
 * Reads a root complex's device tree node.
 *
 * Reports ENODEV for a node the tree marks as not present.
 */
int
drv_pci_plda_describe(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_plda_config *config);

/*
 * Configures a root complex, resets the endpoint, trains the link and
 * places the endpoint's BARs.
 *
 * A link that does not come up is not an error: the host is returned and
 * publishes a root port with nothing below it.
 */
int
drv_pci_plda_start(
	const struct drv_pci_plda_config *config,
	const struct drv_pci_plda_board *board,
	struct drv_pci_plda **result);

/*
 * Hands a started root complex to the PCI core, which enumerates it.
 */
int
drv_pci_plda_publish(
	struct drv_pci_plda *host);

#endif
