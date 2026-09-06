/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4 platform binding.
 *
 * The boot handoff names the SDHCI controller; the platform publishes its
 * SD card as the single boot device.
 */

#include <errno.h>
#include <hal/hal.h>
#include <drivers/disklabel.h>
#include <kern/disk.h>
#include <kern/partition.h>
#include <kern/platform.h>
#include <kern/rpi4/boot.h>
#include "drivers/rpi4-sdhci.h"

/*
 * Publishes the boot devices described by the Raspberry Pi 4 boot handoff.
 *
 * Returns the number of devices published, which is zero for a handoff the
 * platform does not understand.
 */
size_t
kern_platform_init(
	const struct boot_handoff *handoff,
	struct boot_device *devices,
	size_t capacity)
{
	const struct rpi4_boot_handoff *rpi4;
	struct boot_device *device;
	unsigned i;

	rpi4 = (const void *)handoff;

	/* Rejects a handoff that is not an exact Raspberry Pi 4 description. */
	if (!handoff ||
	    !devices ||
	    capacity == 0 ||
	    handoff->magic != ZEDBSD_HANDOFF_MAGIC ||
	    handoff->version != ZEDBSD_HANDOFF_VERSION_MULTIBOOT ||
	    handoff->size < sizeof(*rpi4) ||
	    rpi4->extension_magic != ZEDBSD_RPI4_HANDOFF_MAGIC ||
	    rpi4->extension_version != 1 ||
	    rpi4->extension_size < sizeof(*rpi4) - sizeof(rpi4->common) ||
	    rpi4->sdhci_phys == 0)
		return 0;

	/* Selects the MBR partition scheme and starts the SD controller. */
	partition_set_scheme(&partition_scheme_mbr);
	disk_registry_reset();
	if (rpi4_sdhci_init((uintptr_t)rpi4->sdhci_phys) != 0) {
		/*
		 * QEMU raspi4b currently attaches -drive if=sd to the legacy
		 * Arasan controller, while real Pi 4 firmware boots from eMMC2.
		 */
		if (rpi4->sdhci_phys != 0xfe340000ULL)
			return 0;
		if (rpi4_sdhci_init(0xfe300000ULL) != 0)
			return 0;
		hal_puts("sdhci: using QEMU legacy-controller fallback\n");
	}

	/* Publishes the SD card as the boot device. */
	device = &devices[0];
	device->device_class = ZEDBSD_DEV_SD;
	device->display_index = 0;
	device->bios_id = 0x80;
	device->flags = ZEDBSD_DEV_PRESENT;
	if (handoff->boot_bios_id == 0x80)
		device->flags |= ZEDBSD_DEV_BOOT_ORIGIN;
	device->sector_size = 512;
	device->cylinders = 0;
	device->heads = 0;
	device->sectors = 0;
	device->controller_location = 0;
	for (i = 0; i < sizeof(device->reserved); i++)
		device->reserved[i] = 0;

	/* Reports the single published device. */
	return 1;
}

/*
 * Refreshes the published devices; the Pi 4 has no hot-pluggable devices.
 */
void
kern_platform_refresh_devices(
	const struct boot_device *d,
	size_t n)
{
	(void)d;
	(void)n;
}

/*
 * Initializes platform input devices; the Pi 4 has none.
 */
int
kern_platform_input_init(
	void)
{
	/* Reports success without any input device. */
	return 0;
}

/*
 * Resolves a published boot device to its disk.
 */
struct disk *
kern_platform_block_device(
	const struct boot_device *d)
{
	struct disk *disk;

	/* Only the SD boot device has a disk. */
	if (!d || d->device_class != ZEDBSD_DEV_SD)
		return NULL;

	/* Resolves the SDHCI disk. */
	disk = rpi4_sdhci_disk();

	/* Reports the disk. */
	return disk;
}

/*
 * Writes a debug string to the console.
 */
void
kern_platform_debug_write(
	const char *s)
{
	/* Ignores a missing string. */
	if (s)
		hal_cons_write(s);
}

/*
 * Halts the machine permanently.
 */
void
kern_platform_halt(
	void)
{
	/* Halts with interrupts disabled. */
	(void)hal_irq_disable();
	for (;;)
		hal_halt();
}

/*
 * Reboots the machine.
 */
void
kern_platform_reboot(
	void)
{
	/* Halts in case the reset request returns. */
	hal_reset();
	for (;;)
		hal_halt();
}
