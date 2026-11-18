/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Synopsys DesignWare Ethernet QoS controller (versions 4 and 5) with
 * the RGMII PHY on its MDIO bus.
 *
 * The board's glue switches the controller's clocks and resets on, maps
 * its registers and describes it; the driver resets the controller,
 * brings up the PHY, and publishes the port as a network interface that
 * sends and receives through one DMA channel.
 */

#ifndef DRIVERS_ETHERNET_DW_EQOS_H
#define DRIVERS_ETHERNET_DW_EQOS_H

#include <drivers/ethernet/yt8531.h>

#include <stdint.h>

struct drv_mii_bus;

/* How many burst lengths an AXI description lists at most. */
#define DRV_DW_EQOS_BURST_LENGTHS	7U

/*
 * One controller as the board describes it.
 *
 * The registers are mapped by the caller and stay mapped for the life of
 * the system.  A zero FIFO size means the size the controller reports; a
 * zero burst means 8 beats.  The PHY settings are used when the PHY is a
 * YT8531; another PHY is run on its standard registers alone.  set_speed,
 * when present, is called from thread context before the controller runs
 * at a new link speed, for a board whose transmit clock must follow it.
 * configure_phy, when present, is called from thread context after the
 * reset of a PHY other than a YT8531, with the PHY's identity, for a board
 * whose PHY needs settings of its own (its RGMII delays) before it
 * negotiates; it returns 0 or an errno, and an errno fails the attach.
 */
struct drv_dw_eqos_config {
	const char *name;
	void *registers;
	unsigned irq;
	int dma_coherent;
	uint64_t csr_clock_hz;
	unsigned phy_address;
	struct drv_yt8531_config yt8531;
	uint32_t tx_fifo_bytes;
	uint32_t rx_fifo_bytes;
	uint32_t tx_burst;
	uint32_t rx_burst;
	int burst_x8;
	int fixed_burst;
	int mixed_burst;
	int address_aligned;
	int threshold_mode;
	int axi_present;
	int axi_lpi;
	uint32_t axi_write_limit;
	uint32_t axi_read_limit;
	uint32_t axi_burst_lengths[DRV_DW_EQOS_BURST_LENGTHS];
	int has_mac;
	uint8_t mac[6];
	int (*set_speed)(void *context, unsigned speed_mbps);
	void *context;
	int (*configure_phy)(void *context, const struct drv_mii_bus *bus, unsigned phy, uint32_t identity);
	void *phy_context;
};

/*
 * Resets a controller, brings up its PHY and publishes it as a network
 * interface.
 *
 * Reports ENODEV when no controller or PHY answers, ETIMEDOUT when the
 * controller's reset does not finish (its receive clock from the PHY is
 * missing), ENOTSUP when the board's DMA is not coherent and the kernel
 * has no uncached memory, or the errno of the step that failed.
 */
int drv_dw_eqos_attach(const struct drv_dw_eqos_config *config);

#endif
