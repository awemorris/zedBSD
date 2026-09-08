/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u (UltraSPARC) platform binding.
 *
 * The boot handoff describes one CMD646 IDE controller; the platform
 * publishes its disk as the single boot device.
 */

#include <errno.h>
#include <hal/hal.h>
#include <drivers/disklabel.h>
#include <kern/disk.h>
#include <kern/platform.h>
#include <kern/sun4u/boot.h>
#include "drivers/platform/sun4u/sun4u-cmd646.h"

/*
 * Publishes the boot devices described by the sun4u boot handoff.
 *
 * Returns the number of devices published, which is zero for a handoff the
 * platform does not understand.
 */
size_t
kern_platform_init(
	const struct boot_handoff *h,
	struct boot_device *d,
	size_t capacity)
{
	const struct sun4u_boot_handoff *s;

	s = (const void *)h;

	/* Rejects a handoff that is not an exact sun4u CMD646 description. */
	if (!h ||
	    !d ||
	    !capacity ||
	    h->magic != ZEDBSD_HANDOFF_MAGIC ||
	    h->version != ZEDBSD_HANDOFF_VERSION_SUN4U ||
	    h->size < sizeof(*s) ||
	    s->extension_magic != ZEDBSD_SUN4U_HANDOFF_MAGIC ||
	    s->extension_version != ZEDBSD_SUN4U_HANDOFF_VERSION ||
	    s->ide_vendor != 0x1095 ||
	    s->ide_device != 0x0646)
		return 0;

	/* Selects the Sun partition scheme and starts the IDE controller. */
	partition_set_scheme(&drv_partition_scheme_sun);
	disk_registry_reset();
	if (drv_sun4u_cmd646_init(s->ide_primary_command, s->ide_primary_control) != 0)
		return 0;

	/* Publishes the IDE disk as the boot device. */
	hal_memset(d, 0, sizeof(*d));
	d->device_class = ZEDBSD_DEV_IDE;
	d->display_index = 0;
	d->bios_id = 0x80;
	d->flags = ZEDBSD_DEV_PRESENT | ZEDBSD_DEV_BOOT_ORIGIN;
	d->sector_size = 512;

	/* Reports the single published device. */
	return 1;
}

/*
 * Refreshes the published devices; sun4u has no hot-pluggable devices.
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
 * Initializes platform input devices; sun4u has none.
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

	/* Only the IDE boot device has a disk. */
	if (!d || d->device_class != ZEDBSD_DEV_IDE)
		return NULL;

	/* Resolves the CMD646 disk. */
	disk = drv_sun4u_cmd646_disk();

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
