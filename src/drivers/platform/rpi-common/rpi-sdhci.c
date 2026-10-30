/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4's and Pi 5's SD controller, handed to the SDHCI driver.
 *
 * The firmware has switched the controller's clock and pins on and left the
 * card powered; the glue only maps the registers and passes on the base
 * clock the platform names where the capabilities register does not.
 */

#include "drivers/platform/rpi-common/rpi-sdhci.h"

#include <stddef.h>

#include <drivers/mmc/sdhci.h>
#include <kern/pmem.h>

/* The bytes of the controller's registers the glue maps: one page holds them all. */
#define RPI_SDHCI_WINDOW 0x1000U

/* The unit number of the SD card slot, which names its disk mmcblk0. */
#define RPI_SDHCI_UNIT 0U

/*
 * The SD card's disk, or NULL while no attach has succeeded.
 *
 * It is set once at boot by drv_rpi_sdhci_init() and kept for the kernel's
 * life.
 */
static struct disk *rpi_sdhci_disk;

/*
 * Hands the SD controller at a physical address to the SDHCI driver.
 *
 * base_clock_hz is the controller's base clock when the board names one
 * (the Pi 5's runs from a fixed clock whatever its capabilities say), or 0
 * to take the capabilities register's.  Returns 0 once the card is a disk,
 * or the driver's errno.  A failed call may be repeated with another
 * controller.
 */
int
drv_rpi_sdhci_init(
	uintptr_t physical_base,
	uint64_t base_clock_hz)
{
	struct drv_sdhci_config config;
	struct disk *disk;
	void *registers;
	int error;

	/* Maps the controller's registers. */
	error = kern_device_map(physical_base, RPI_SDHCI_WINDOW, KERN_DEVICE_UNCACHED, &registers);
	if (error != 0)
		return error;

	/* Describes the controller through the mapping, with no card detect. */
	config.registers = registers;
	config.physical_base = physical_base;
	config.base_clock_hz = base_clock_hz;
	config.max_clock_hz = 0;
	config.quirks = 0;
	config.card_present = NULL;
	config.context = NULL;
	config.unit = RPI_SDHCI_UNIT;

	/* Brings the card up and publishes it; a controller that does not answer is unmapped again. */
	error = drv_sdhci_attach(&config, &disk);
	if (error != 0) {
		(void)kern_device_unmap(registers, RPI_SDHCI_WINDOW);
		return error;
	}

	/* Succeeded: the card is the board's SD disk. */
	rpi_sdhci_disk = disk;
	return 0;
}

/*
 * Reports the SD card's disk, or NULL when no attach has succeeded.
 */
struct disk *
drv_rpi_sdhci_disk(
	void)
{
	/* Reports the disk the attach published. */
	return rpi_sdhci_disk;
}
