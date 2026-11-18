/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the chip's backplane and its bring-up.
 *
 * Reaches the chip's cores through function 1's backplane window, finds
 * them in the enumeration ROM, writes the firmware and the NVRAM into the
 * chip's RAM, starts the ARM core, and brings the frame function up once
 * the firmware runs.  Used by whoever owns the bus at the time: the
 * bring-up before the bus thread starts, then the bus thread alone.
 */

#ifndef KERN_DRIVERS_BCM43455_CHIP_H
#define KERN_DRIVERS_BCM43455_CHIP_H

#include <stddef.h>
#include <stdint.h>

#include "drivers/wifi/bcm43455/bcm43455.h"

/*
 * The most bytes one backplane transfer moves, and the size of the bounce
 * buffer.
 */
#define BCM43455_CHIP_TRANSFER_MAX	2048U

/*
 * One core of the chip: its id and revision, its registers' backplane
 * address, and the address of its wrapper (its reset and I/O control).
 * A core that was not found has base 0.
 */
struct bcm43455_core {
	uint16_t id;
	uint8_t revision;
	uint32_t base;
	uint32_t wrapper;
};

/*
 * The chip as the driver knows it.
 *
 * window is the backplane address the window registers point at, valid
 * once window_valid is set; a failed window write clears it, so the next
 * access writes all three registers again.  pmu_base is where the PMU's
 * registers are (its own core, or the chip common core), save_restore
 * whether the firmware runs the chip's save-restore, in which case the
 * firmware rather than the host decides the fast clock.  bounce carries
 * data between a caller's constant buffer and the bus.  Only one thread
 * uses a chip at a time (see bcm43455-bus.h).
 */
struct bcm43455_chip {
	const struct bcm43455_bus_ops *ops;
	void *context;

	uint32_t window;
	unsigned window_valid;

	uint16_t id;
	unsigned revision;
	uint32_t capability;
	uint32_t capability_ext;
	struct bcm43455_core chipcommon;
	struct bcm43455_core sdio;
	struct bcm43455_core cr4;
	struct bcm43455_core d11;
	struct bcm43455_core pmu;
	uint32_t pmu_base;
	unsigned pmu_revision;
	unsigned save_restore;
	uint32_t ram_base;
	uint32_t ram_size;

	uint8_t bounce[BCM43455_CHIP_TRANSFER_MAX];
};

/*
 * Starts the backplane clock, identifies the chip, finds its cores and
 * RAM, halts its ARM core and radio, and leaves the frame function off
 * and the chip on its slow clock, ready for the download.
 *
 * Returns ENODEV for a chip that is not a BCM43455 and EIO for an
 * enumeration ROM that does not describe the needed cores.
 */
int bcm43455_chip_identify(struct bcm43455_chip *chip, const struct bcm43455_bus_ops *ops, void *context);

/* Reads one 32-bit backplane register. */
int bcm43455_chip_read32(struct bcm43455_chip *chip, uint32_t address, uint32_t *value);

/* Writes one 32-bit backplane register. */
int bcm43455_chip_write32(struct bcm43455_chip *chip, uint32_t address, uint32_t value);

/* Writes length bytes of backplane memory, a whole number of words. */
int bcm43455_chip_write_memory(struct bcm43455_chip *chip, uint32_t address, const uint8_t *data, size_t length);

/* Reads length bytes of backplane memory, a whole number of words. */
int bcm43455_chip_read_memory(struct bcm43455_chip *chip, uint32_t address, uint8_t *data, size_t length);

/*
 * Writes the firmware and the converted NVRAM into RAM on the slow clock,
 * reads them back whole, and starts the ARM core at the firmware's reset
 * vector.
 *
 * Returns EIO when what was read back differs from what was written, or
 * ETIMEDOUT when the slow clock does not come.
 */
int bcm43455_chip_download(struct bcm43455_chip *chip, const uint8_t *firmware, size_t firmware_length, const uint8_t *nvram, size_t nvram_length);

/*
 * Brings the frame function up behind the running firmware: the protocol
 * version, the fast clock, function 2, the interrupt masks.
 *
 * Returns ETIMEDOUT when the fast clock or function 2 does not come.
 */
int bcm43455_chip_start(struct bcm43455_chip *chip);

/*
 * Points the window at the chip common core, where the frame function's
 * address lies.
 */
int bcm43455_chip_frame_window(struct bcm43455_chip *chip);

#endif
