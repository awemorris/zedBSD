/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DesignWare Mobile Storage Host controller with an SD memory card.
 */

#ifndef KERN_DRIVERS_MMC_DW_MMC_H
#define KERN_DRIVERS_MMC_DW_MMC_H

#include <stdint.h>

struct disk;

/* How many controllers the driver can serve at once. */
#define DRV_DW_MMC_UNIT_MAX	2U

/*
 * What a board's glue tells the driver about one controller.
 *
 * The glue has mapped the registers and switched the controller's clocks,
 * reset and pins on before it hands the controller over; the driver only
 * talks to the controller and the card behind it.  The structure is read
 * during the attach and not kept.
 */
struct drv_dw_mmc_config {
	/* The controller's registers, mapped as device memory. */
	volatile uint8_t *registers;

	/* The rate of the card interface clock (ciu) in Hz, which the card clock divides. */
	uint64_t clock_rate;

	/* The FIFO's depth in entries; zero reads it from the controller. */
	uint32_t fifo_depth;

	/* How many data lines the slot wires: 1 or 4. */
	uint32_t bus_width;

	/* The fastest card clock the slot allows in Hz; zero allows high speed's 50 MHz. */
	uint64_t max_frequency;

	/* Nonzero when the slot may run a card at high speed. */
	int high_speed;

	/* How long the card needs after its power is switched on, in milliseconds. */
	uint32_t power_on_delay_ms;

	/*
	 * Reports whether a card is in the slot, or NULL when the slot has no
	 * card detect and a card is taken to be in.  It returns 0 or an errno.
	 */
	int (*card_present)(void *context, int *present);
	void *context;

	/*
	 * The controller's sample phase as the glue found it, how many
	 * phases the glue can set, and the hook that sets one; a count of 0
	 * or a NULL hook leaves the phase alone.  The driver moves the phase
	 * only when a read at high speed fails with a data CRC, start bit or
	 * end bit error (see the driver's adapt_bus()).
	 */
	uint32_t sample_phase;
	uint32_t sample_phase_count;
	void (*set_sample_phase)(void *context, uint32_t phase);
	void *phase_context;

	/* The unit number, which names the disk mmcblk<unit>. */
	unsigned unit;
};

/*
 * What the driver found in the slot, for the boot log and the tests.
 */
struct drv_dw_mmc_card {
	/* The controller's version register and its FIFO's shape. */
	uint32_t controller_version;
	uint32_t fifo_width;
	uint32_t fifo_depth;

	/* The card's version 1 or 2 CSD, and whether blocks are addressed by number. */
	unsigned csd_version;
	int high_capacity;

	/* The card's size in 512-byte blocks. */
	uint64_t block_count;

	/* The card's address on the bus and its operating conditions. */
	uint32_t rca;
	uint32_t ocr;

	/* The card's identity from its CID. */
	uint32_t manufacturer;
	char oem[3];
	char product[6];
	uint32_t revision;
	uint32_t serial;

	/* How the bus runs once the card is ready. */
	uint32_t bus_width;
	uint64_t card_clock;
	int high_speed;

	/* The sample phase in use, and whether data errors moved the bus to the default speed. */
	uint32_t sample_phase;
	int slowed;
};

int drv_dw_mmc_attach(const struct drv_dw_mmc_config *config, struct disk **disk);
int drv_dw_mmc_card_info(unsigned unit, struct drv_dw_mmc_card *card);

#endif
