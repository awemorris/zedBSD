/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Amlogic SD/eMMC controller with an SD memory card.
 */

#ifndef KERN_DRIVERS_MMC_MESON_MMC_H
#define KERN_DRIVERS_MMC_MESON_MMC_H

#include <stdint.h>

struct disk;

/* How many controllers the driver can serve at once. */
#define DRV_MESON_MMC_UNIT_MAX		2U

/*
 * The clock sources the glue can feed the controller with: the slow one
 * (the crystal) for identification and the default speed, the fast one
 * for high speed.
 */
enum drv_meson_mmc_source {
	DRV_MESON_MMC_SOURCE_SLOW = 0,
	DRV_MESON_MMC_SOURCE_FAST
};

/*
 * What a board's glue tells the driver about one controller.
 *
 * The glue has mapped the registers, switched the controller's gate, pins
 * and the card's supplies on, and owns the bounce buffer the controller
 * moves data through; the driver only talks to the controller and the
 * card behind it.  The structure is read during the attach and not kept.
 */
struct drv_meson_mmc_config {
	/* The controller's registers, mapped as device memory. */
	volatile uint8_t *registers;

	/*
	 * The bounce buffer: its kernel address, the address the controller
	 * reaches it at (below 4 GiB), and its size, at least one block.
	 * The driver keeps the CPU's caches in step with it.
	 */
	uint8_t *bounce;
	uint64_t bounce_bus_address;
	uint32_t bounce_bytes;

	/* The rates of the two clock sources in Hz, and the hook that selects one. */
	uint64_t slow_rate;
	uint64_t fast_rate;
	int (*select_source)(void *context, enum drv_meson_mmc_source source);

	/* How many data lines the slot wires: 1 or 4. */
	uint32_t bus_width;

	/* The fastest card clock the slot allows in Hz; zero allows high speed's 50 MHz. */
	uint64_t max_frequency;

	/* Nonzero when the slot may run a card at high speed. */
	int high_speed;

	/*
	 * Reports whether a card is in the slot, or NULL when the slot has no
	 * card detect and a card is taken to be in.  It returns 0 or an errno.
	 */
	int (*card_present)(void *context, int *present);
	void *context;

	/* The unit number, which names the disk mmcblk<unit>. */
	unsigned unit;
};

/*
 * What the driver found in the slot, for the boot log and the tests.
 */
struct drv_meson_mmc_card {
	/* The interface condition's echo (zero for a card that did not answer CMD8). */
	uint32_t interface_echo;

	/* The card's version 1 or 2 CSD, and whether blocks are addressed by number. */
	unsigned csd_version;
	int high_capacity;

	/* The card's size in 512-byte blocks. */
	uint64_t block_count;

	/* The card's address on the bus and its operating conditions. */
	uint32_t rca;
	uint32_t ocr;

	/* The card's raw CID and CSD, word 0 holding bits 31:0. */
	uint32_t cid[4];
	uint32_t csd[4];

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

	/* Whether data errors moved the bus down to the default speed. */
	int slowed;
};

int drv_meson_mmc_attach(const struct drv_meson_mmc_config *config, struct disk **disk);
int drv_meson_mmc_card_info(unsigned unit, struct drv_meson_mmc_card *card);
int drv_meson_mmc_read(unsigned unit, uint64_t block, uint32_t count, uint8_t *data);

#endif
