/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Cadence Gigabit Ethernet MAC (GEM) with the PHY on its MDIO bus.
 *
 * The board's glue switches the controller's clocks on, maps its registers,
 * creates the DMA device the controller reaches memory through, and
 * describes the controller; the driver resets the controller, has the glue
 * bring the PHY up over the controller's MDIO bus, and publishes the port
 * as a network interface with one receive ring and one transmit ring.
 */

#ifndef DRIVERS_ETHERNET_CADENCE_GEM_H
#define DRIVERS_ETHERNET_CADENCE_GEM_H

#include <drivers/ethernet/mii-phy.h>

#include <stdint.h>

struct drv_dma_device;

/*
 * One attached controller.
 *
 * The driver keeps it, with its rings, slots and interface, for as long as
 * the kernel runs once the attachment has succeeded.
 */
struct drv_cadence_gem;

/* How many controllers the driver serves at once. */
#define DRV_CADENCE_GEM_UNIT_MAX			2U

/* The descriptor formats: two words with 32-bit addresses, or four words with 64-bit ones. */
#define DRV_CADENCE_GEM_DESCRIPTOR_32			32U
#define DRV_CADENCE_GEM_DESCRIPTOR_64			64U

/*
 * The board's address is taken from the controller's first unicast filter
 * when the configuration carries none, as firmware that has used the
 * controller leaves it there.
 */
#define DRV_CADENCE_GEM_FLAG_MAC_FROM_FILTERS		0x1U

/*
 * Receive is turned off and on again when the controller reports that it
 * read a used receive descriptor, which restarts a receive DMA that a
 * controller of this kind can leave stopped under load.
 */
#define DRV_CADENCE_GEM_FLAG_RESTART_RX_ON_USED_BIT	0x2U

/* The user I/O register selects the RGMII interface. */
#define DRV_CADENCE_GEM_FLAG_USRIO_RGMII		0x4U

/*
 * One controller as the board describes it.
 *
 * The registers are mapped by the glue and stay mapped for the life of the
 * system.  pclk_hz is the clock the MDIO clock is divided from.  When
 * phy_scan is set and no PHY answers at phy_address, the first address
 * from 0 to 31 that answers is taken.  An all-zero or multicast mac is no
 * address.
 *
 * The hooks get context.  reset_phy and setup_phy run once in thread
 * context during the attachment, setup_phy with the identity the driver
 * read at the PHY's address (zero when it read none), which it may replace
 * with the one it reads itself.  read_link and set_tx_clock run with the
 * controller's lock held and interrupts off, so they must not sleep;
 * set_tx_clock is called when the link comes up at a new speed.
 * establish_irq routes the controller's interrupt to handler, called with
 * argument, and is required.  reset_phy, setup_phy and set_tx_clock are
 * left NULL by a board without such a step; read_link left NULL reads the
 * PHY's standard registers.
 */
struct drv_cadence_gem_config {
	const char *name;
	volatile uint8_t *registers;
	unsigned descriptor_width;
	struct drv_dma_device *dma;
	uint64_t pclk_hz;
	unsigned phy_address;
	int phy_scan;
	uint8_t mac[6];
	unsigned flags;
	void *context;
	int (*reset_phy)(void *context);
	int (*setup_phy)(void *context, const struct drv_mii_bus *bus, unsigned phy, uint32_t *identity);
	int (*read_link)(void *context, const struct drv_mii_bus *bus, unsigned phy, struct drv_mii_link *link);
	void (*set_tx_clock)(void *context, unsigned speed_mbps);
	int (*establish_irq)(void *context, void (*handler)(void *argument), void *argument);
};

/*
 * Resets a controller, brings up its PHY and publishes it as a network
 * interface.
 *
 * Stores the controller in *gem and returns 0, or returns EINVAL for a
 * configuration it cannot use, EBUSY when every unit is taken, ENODEV when
 * there is no address or no PHY answers, or the errno of the step that
 * failed; a failed attachment keeps what it took.
 */
int drv_cadence_gem_attach(const struct drv_cadence_gem_config *config, struct drv_cadence_gem **gem);

/*
 * Starts the periodic service of the link and of missed interrupts, once
 * interrupts are enabled.  A repeated call does nothing.
 */
void drv_cadence_gem_start_service(struct drv_cadence_gem *gem);

#endif
