/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An SD Host Controller with an SD memory card, driven by programmed I/O.
 */

#ifndef KERN_DRIVERS_MMC_SDHCI_H
#define KERN_DRIVERS_MMC_SDHCI_H

#include <stdint.h>

struct disk;

/* How many controllers the driver can serve at once. */
#define DRV_SDHCI_UNIT_MAX	2U

/*
 * What a board's glue tells the driver about one controller.
 *
 * The glue has mapped the registers and switched the controller's clocks,
 * reset and pins on before it hands the controller over; the driver only
 * talks to the controller and the card behind it.  The structure is read
 * during the attach; the driver keeps a copy of the card-detect hook.
 */
struct drv_sdhci_config {
	/* The controller's registers, mapped as device memory. */
	volatile uint8_t *registers;

	/* The controller's bus address, which the boot log names. */
	uint64_t physical_base;

	/*
	 * The rate of the controller's base clock in Hz, which the card clock
	 * divides.  Zero takes the rate the capabilities register advertises,
	 * and 100 MHz when that is zero too.
	 */
	uint64_t base_clock_hz;

	/* The fastest card clock once the card is identified, in Hz; zero means 25 MHz. */
	uint64_t max_clock_hz;

	/* The controller's deviations from the specification (DRV_SDHCI_QUIRK_*). */
	unsigned quirks;
#define DRV_SDHCI_QUIRK_FORCE_CD	0x0001U

	/*
	 * Reports whether a card is in the slot, or NULL when the slot has no
	 * card detect.  It returns 0 or an errno.  The answer goes to the boot
	 * log only: the card is initialized whatever it says, and an empty slot
	 * shows as a failed initialization.
	 */
	int (*card_present)(void *context, int *present);
	void *context;

	/* The unit number, which names the disk mmcblk<unit>. */
	unsigned unit;
};

/*
 * What the driver found in the slot, for the boot log and the tests.
 */
struct drv_sdhci_card {
	/* The controller's capabilities register. */
	uint32_t capabilities;

	/* The card's address on the bus, and whether blocks are addressed by number. */
	uint32_t rca;
	int high_capacity;

	/* The card's size in 512-byte blocks; 0xffffffff while it is not known. */
	uint64_t block_count;

	/* How the bus runs once the card is ready. */
	unsigned bus_width;
	uint64_t card_clock;
};

int drv_sdhci_attach(const struct drv_sdhci_config *config, struct disk **disk);
int drv_sdhci_card_info(unsigned unit, struct drv_sdhci_card *card);

#endif
