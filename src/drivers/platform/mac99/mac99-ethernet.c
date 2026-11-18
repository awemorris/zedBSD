/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's built-in Ethernet: the board's side of the GMAC.
 *
 * The station address is the local-mac-address the firmware's tree
 * gives the controller's node, which the hardware layer hands over as
 * "mac99.ethernet" with the host and the configuration address of each
 * controller.  The PHY's reset line is a GPIO of the I/O controller
 * (KeyLargo, Pangea or Intrepid), whose register block the hardware layer
 * hands over as "mac99.macio.base": driven low it holds the PHY in reset,
 * and released it lets the PHY run.
 */

#include "drivers/platform/mac99/mac99-ethernet.h"
#include <drivers/ethernet/sungem.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * The words of the hardware layer's "mac99.ethernet": the count, then
 * four a controller: its host (the PCI segment), its configuration
 * address, its address's first four octets (the first in the top byte)
 * and its last two (in the top half).
 */
#define MAC99_ETHERNET_FIRST		1U
#define MAC99_ETHERNET_WORDS		4U
#define MAC99_ETHERNET_HOST		0U
#define MAC99_ETHERNET_ADDRESS		1U
#define MAC99_ETHERNET_HIGH		2U
#define MAC99_ETHERNET_LOW		3U

/*
 * The I/O controller's GPIO that resets the Ethernet PHY, the values
 * that hold the PHY in reset (the output driven, low) and let it run
 * (the output released, high), how long each is held, and the part of
 * the I/O controller mapped to reach it.
 *
 * Values referenced: the Linux kernel's
 * arch/powerpc/include/asm/keylargo.h (v7.2: KEYLARGO_GPIO_0,
 * KL_GPIO_ETH_PHY_RESET, KEYLARGO_GPIO_OUTPUT_ENABLE,
 * KEYLARGO_GPIO_OUTOUT_DATA) and
 * arch/powerpc/platforms/powermac/feature.c (v7.2:
 * core99_gmac_phy_reset(), the 10 ms after each write).
 */
#define MACIO_GPIO_ETH_PHY_RESET	(0x6aU + 0x10U)
#define MACIO_GPIO_OUTPUT_ENABLE	0x04U
#define MACIO_GPIO_OUTPUT_DATA		0x01U
#define MACIO_PHY_RESET_US		10000U
#define MACIO_GPIO_WINDOW		0x1000U

static int station_address(const struct drv_pci_address *address, uint8_t *mac);
static int reset_phy(void);
static void wait_microseconds(unsigned microseconds);

/* The board's hooks the GEM driver keeps, constant for the life of the system. */
static const struct drv_sungem_board mac99_ethernet_board = {
	.station_address = station_address,
	.reset_phy = reset_phy,
};

/*
 * The I/O controller's GPIO block, mapped on the first PHY reset and kept
 * for the life of the system; NULL until then.
 */
static volatile uint8_t *macio_gpio;

/*
 * Gives the GEM driver the board's hooks and registers the driver.
 */
int
drv_mac99_ethernet_init(
	void)
{
	int error;

	/* Hands the driver the board's addresses and reset line. */
	drv_sungem_set_board(&mac99_ethernet_board);

	/* Registers the driver, which binds when the buses are enumerated. */
	error = drv_pci_sungem_driver_register();
	if (error != 0)
		return error;

	/* Succeeded: the driver waits for its controllers. */
	return 0;
}

/*
 * Copies the station address the firmware gives the controller at a
 * PCI address; ENOENT when it gives none.
 */
static int
station_address(
	const struct drv_pci_address *address,
	uint8_t *mac)
{
	const uint32_t *table;
	const uint32_t *record;
	uint32_t wanted;
	uint32_t index;

	/* The firmware's addresses; without them the controller's registers must do. */
	table = kern_boot_handoff("mac99.ethernet");
	if (table == NULL)
		return ENOENT;

	/* The controller's configuration address in the table's form. */
	wanted = ((uint32_t)address->bus << 16) |
		 ((uint32_t)address->device << 11) |
		 ((uint32_t)address->function << 8);

	/* Finds the record of the same host and address. */
	for (index = 0; index < table[0]; index++) {
		record = &table[MAC99_ETHERNET_FIRST + index * MAC99_ETHERNET_WORDS];
		if (record[MAC99_ETHERNET_HOST] != address->segment)
			continue;
		if (record[MAC99_ETHERNET_ADDRESS] != wanted)
			continue;

		/* Succeeded: the octets, the first from the top byte. */
		mac[0] = (uint8_t)(record[MAC99_ETHERNET_HIGH] >> 24);
		mac[1] = (uint8_t)(record[MAC99_ETHERNET_HIGH] >> 16);
		mac[2] = (uint8_t)(record[MAC99_ETHERNET_HIGH] >> 8);
		mac[3] = (uint8_t)record[MAC99_ETHERNET_HIGH];
		mac[4] = (uint8_t)(record[MAC99_ETHERNET_LOW] >> 24);
		mac[5] = (uint8_t)(record[MAC99_ETHERNET_LOW] >> 16);
		return 0;
	}

	/* The firmware gives this controller no address. */
	return ENOENT;
}

/*
 * Holds the PHY in reset through the I/O controller's GPIO, then lets it
 * run.  ENODEV for a board without the I/O controller.
 */
static int
reset_phy(
	void)
{
	const uint64_t *base;
	void *window;
	int error;

	/* Maps the I/O controller's first page, where the GPIOs are, once. */
	if (macio_gpio == NULL) {
		base = kern_boot_handoff("mac99.macio.base");
		if (base == NULL)
			return ENODEV;
		error = kern_device_map(*base, MACIO_GPIO_WINDOW, KERN_DEVICE_UNCACHED, &window);
		if (error != 0) {
			kern_logf("mac99: Ethernet PHY reset line unavailable (%d)\n", error);
			return error;
		}
		macio_gpio = window;
	}

	/* Drives the line low, which holds the PHY in reset, and reads it back so the write lands. */
	kern_mmio_write8(macio_gpio + MACIO_GPIO_ETH_PHY_RESET, MACIO_GPIO_OUTPUT_ENABLE);
	(void)kern_mmio_read8(macio_gpio + MACIO_GPIO_ETH_PHY_RESET);
	wait_microseconds(MACIO_PHY_RESET_US);

	/* Releases the line high, which lets the PHY run. */
	kern_mmio_write8(macio_gpio + MACIO_GPIO_ETH_PHY_RESET, MACIO_GPIO_OUTPUT_DATA);
	(void)kern_mmio_read8(macio_gpio + MACIO_GPIO_ETH_PHY_RESET);
	wait_microseconds(MACIO_PHY_RESET_US);

	/* Succeeded: the PHY is out of reset. */
	return 0;
}

/*
 * Spins for at least the given time on the monotonic counter; the boot
 * has no scheduler to sleep on yet.  Without a counter it returns at once.
 */
static void
wait_microseconds(
	unsigned microseconds)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t duration;
	bool counted;

	/* The start and the length of the wait in counter ticks. */
	counted = kern_rtc_read_counter(&start, &frequency);
	if (!counted || frequency == 0)
		return;
	duration = frequency * microseconds / 1000000U + 1U;

	/* Spins until the counter has moved that far. */
	for (;;) {
		counted = kern_rtc_read_counter(&now, &frequency);
		if (!counted)
			return;
		if (now - start >= duration)
			return;
	}
}
