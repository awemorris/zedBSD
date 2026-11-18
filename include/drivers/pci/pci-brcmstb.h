/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Broadcom STB PCIe root complex (BCM2711 and BCM2712).
 *
 * The Raspberry Pi 4 reaches its USB controller through the BCM2711's root
 * complex, and the Raspberry Pi 5 its M.2 connector through one of the
 * BCM2712's.  The firmware leaves them in no particular state, so the
 * driver resets the controller, trains the link, opens the memory windows,
 * numbers the bus below the root port and places the endpoints' BARs
 * before the PCI core enumerates it.
 *
 * On the BCM2712 the bridge's reset and the receiver of message
 * interrupts are blocks of their own outside the controller; the platform
 * hands the driver their operations.
 */

#ifndef KERN_DRIVERS_PCI_BRCMSTB_H
#define KERN_DRIVERS_PCI_BRCMSTB_H

#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <drivers/pci/pci.h>

/* The legacy interrupt pins INTA to INTD. */
#define DRV_PCI_BRCMSTB_INTX_COUNT	4U

/* The most inbound windows a BCM2712 description carries: RC BAR1 to BAR3. */
#define DRV_PCI_BRCMSTB_INBOUND_MAX	3U

/* The quartiles of the BCM2712's request queue that each get a priority. */
#define DRV_PCI_BRCMSTB_QOS_LEVELS	4U

struct drv_pci_brcmstb;

/*
 * The generation of the controller, which decides where its reset, its
 * PERST# and its debug register are and how its inbound windows work.
 *
 * Zero is the BCM2711, so a description that is cleared and filled the
 * way the Raspberry Pi 4 always did stays a BCM2711 one.
 */
enum drv_pci_brcmstb_variant {
	DRV_PCI_BRCMSTB_BCM2711 = 0,
	DRV_PCI_BRCMSTB_BCM2712 = 1
};

/*
 * One window through which devices reach the CPU's bus.
 *
 * A device's address pci reaches the CPU's address cpu; the window covers
 * size bytes from there.
 */
struct drv_pci_brcmstb_inbound {
	uint64_t cpu_base;
	uint64_t pci_base;
	uint64_t size;
};

/*
 * What a board does for a BCM2712 root complex that the controller cannot
 * do itself.
 *
 * Every operation is called in thread context with context as its first
 * argument.  bridge_reset asserts or releases the bridge's reset line and,
 * on release, waits until the bridge can be reached; it reports 0 or an
 * errno.  msi_establish and msi_disestablish have the contract of the PCI
 * core's host hooks of the same names; a board without a receiver of
 * message interrupts leaves both NULL, and the root complex then offers
 * only the legacy pins.  The record must outlive the root complex.
 */
struct drv_pci_brcmstb_board {
	void *context;
	int (
		*bridge_reset)(
		void *,
		int);
	int (
		*msi_establish)(
		void *,
		drv_pci_msi_run_t,
		void *,
		unsigned *,
		uint64_t *,
		uint32_t *);
	int (
		*msi_disestablish)(
		void *,
		unsigned);
};

/*
 * Where one root complex sits and what it decodes.
 *
 * The platform fills it from its device tree.  Addresses named cpu are CPU
 * physical addresses; those named pci are addresses on the PCI bus.  The
 * outbound window carries the CPU's accesses to device BARs; the inbound
 * window carries device DMA to system memory.  The interrupt numbers are the
 * kernel's numbers for the lines INTA to INTD, zero where a pin is absent.
 *
 * The fields after intx_irq matter to a BCM2712 only, and stay zero for a
 * BCM2711.  inbound lists every window of the device tree's dma-ranges in
 * its order, the one onto system memory among them, which the single
 * inbound fields above also name.  max_link_speed is the highest PCIe
 * generation the link may train to, zero to leave the controller's own.
 * tperst_ms is how long the reference clock runs, with PERST# still held
 * at the connector, before the endpoint leaves reset; zero releases it
 * at once.
 * qos_valid says whether qos holds the priority of each quartile of the
 * request queue.  The platform sets segment, coherent and board after
 * drv_pci_brcmstb_describe().
 */
struct drv_pci_brcmstb_config {
	uint64_t register_base;
	uint64_t register_size;
	uint64_t outbound_cpu_base;
	uint64_t outbound_pci_base;
	uint64_t outbound_size;
	uint64_t inbound_cpu_base;
	uint64_t inbound_pci_base;
	uint64_t inbound_size;
	unsigned intx_irq[DRV_PCI_BRCMSTB_INTX_COUNT];
	enum drv_pci_brcmstb_variant variant;
	uint16_t segment;
	unsigned inbound_count;
	struct drv_pci_brcmstb_inbound inbound[DRV_PCI_BRCMSTB_INBOUND_MAX];
	unsigned max_link_speed;
	unsigned tperst_ms;
	int qos_valid;
	uint8_t qos[DRV_PCI_BRCMSTB_QOS_LEVELS];
	int coherent;
	const struct drv_pci_brcmstb_board *board;
};

int
drv_pci_brcmstb_describe(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct drv_pci_brcmstb_config *config);

int
drv_pci_brcmstb_start(
	const struct drv_pci_brcmstb_config *config,
	struct drv_pci_brcmstb **result);

int
drv_pci_brcmstb_config_read(
	struct drv_pci_brcmstb *host,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *value);

int
drv_pci_brcmstb_reassign(
	struct drv_pci_brcmstb *host);

int
drv_pci_brcmstb_publish(
	struct drv_pci_brcmstb *host);

#endif
