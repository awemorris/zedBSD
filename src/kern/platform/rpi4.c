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

#include <uapi/errno.h>
#include <hal/hal.h>
#include <drivers/disklabel/disklabel.h>
#include <kern/disk.h>
#include <kern/partition.h>
#include <kern/platform.h>
#include <kern/boot.h>
#if CONFIG_DRIVER_USB_HID
#include <drivers/usb/usb-hid.h>
#endif
#include "drivers/platform/rpi4/rpi4-console.h"
#include "drivers/platform/rpi4/rpi4-pcie.h"
#include "drivers/platform/rpi4/rpi4-sdhci.h"
#if CONFIG_DRIVER_BCM2711_GPU
#include "drivers/gpu/bcm2711/bcm2711-gpu.h"
#endif

/*
 * Publishes the boot devices described by the Raspberry Pi 4 boot handoff.
 *
 * Returns the number of devices published, which is zero for a handoff the
 * platform does not understand.
 */
size_t
kern_platform_init(
	const struct kern_boot_handoff *handoff,
	struct kern_boot_device *devices,
	size_t capacity)
{
	const struct rpi4_boot_handoff *rpi4;
	struct kern_boot_device *device;
#if CONFIG_DRIVER_BCM2711_GPU
	struct drv_bcm2711_boot_screen screen;
#endif
	unsigned i;

	rpi4 = (const void *)handoff;

	/* Rejects a handoff that is not an exact Raspberry Pi 4 description. */
	if (!handoff ||
	    !devices ||
	    capacity == 0 ||
	    handoff->magic != KERN_HANDOFF_MAGIC ||
	    handoff->version != KERN_HANDOFF_VERSION_MULTIBOOT ||
	    handoff->size < sizeof(*rpi4) ||
	    rpi4->extension_magic != KERN_RPI4_HANDOFF_MAGIC ||
	    rpi4->extension_version != 1 ||
	    rpi4->extension_size < sizeof(*rpi4) - sizeof(rpi4->common) ||
	    rpi4->sdhci_phys == 0)
		return 0;

	/* Publishes the serial console as /dev/console's output. */
	drv_rpi4_console_init();

	/* Selects the MBR partition scheme and starts the SD controller. */
	partition_set_scheme(&drv_partition_scheme_mbr);
	disk_registry_reset();
	if (drv_rpi4_sdhci_init((uintptr_t)rpi4->sdhci_phys) != 0) {
		/*
		 * QEMU raspi4b currently attaches -drive if=sd to the legacy
		 * Arasan controller, while real Pi 4 firmware boots from eMMC2.
		 */
		if (rpi4->sdhci_phys != 0xfe340000ULL)
			return 0;
		if (drv_rpi4_sdhci_init(0xfe300000ULL) != 0)
			return 0;
		hal_puts("sdhci: using QEMU legacy-controller fallback\n");
	}

	/*
	 * Brings up PCIe and what sits behind it.  A board or emulator without
	 * a usable controller boots without PCI devices.
	 */
	(void)drv_rpi4_pcie_init(rpi4->fdt_phys);

#if CONFIG_DRIVER_BCM2711_GPU
	/* Describes the firmware's screen to the graphics driver. */
	screen.physical = rpi4->framebuffer_phys;
	screen.size = rpi4->framebuffer_size;
	screen.width = rpi4->framebuffer_width;
	screen.height = rpi4->framebuffer_height;
	screen.pitch = rpi4->framebuffer_pitch;
	screen.format = rpi4->framebuffer_format;

	/*
	 * Finds the display path and the V3D engine.  A board or emulator
	 * without them, or a boot with rpi4gpu.off=1, goes on without them.
	 */
	(void)drv_bcm2711_gpu_attach(rpi4->fdt_phys, &screen);
#endif

	/* Publishes the SD card as the boot device. */
	device = &devices[0];
	device->device_class = KERN_DEV_SD;
	device->display_index = 0;
	device->bios_id = 0x80;
	device->flags = KERN_DEV_PRESENT;
	if (handoff->boot_bios_id == 0x80)
		device->flags |= KERN_DEV_BOOT_ORIGIN;
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
 * Finishes device discovery once interrupts are enabled.
 *
 * The SD card is the boot device and is published already; the USB host
 * controllers behind PCIe look at their root ports now.
 */
void
kern_platform_refresh_devices(
	const struct kern_boot_device *d,
	size_t n)
{
	(void)d;
	(void)n;

	/* Has the USB controllers behind PCIe find their devices. */
	drv_rpi4_pcie_refresh();
}

/*
 * Enables USB input and the serial console after the input core is ready.
 */
int
kern_platform_input_init(
	void)
{
	int error;

#if CONFIG_DRIVER_USB_HID
	/* Activates pending HID devices after the console claims event0. */
	drv_usb_hid_input_ready();
#endif

	/* Starts the serial reader alongside USB keyboards and mice. */
	error = drv_rpi4_console_start_input();
	if (error != 0)
		return error;

	/* Succeeded: platform input is available to the console and desktop. */
	return 0;
}

/*
 * Resolves a published boot device to its disk.
 */
struct disk *
kern_platform_block_device(
	const struct kern_boot_device *d)
{
	struct disk *disk;

	/* Only the SD boot device has a disk. */
	if (!d || d->device_class != KERN_DEV_SD)
		return NULL;

	/* Resolves the SDHCI disk. */
	disk = drv_rpi4_sdhci_disk();

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
		/* Emits every byte through the early console. */
		while (*s != '\0')
			hal_putc((unsigned char)*s++);
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
 * Turns the power off through the HAL, halting if that returns.
 */
int
kern_platform_poweroff(
	void)
{
	/* The HAL's power-off does not return on this platform. */
	hal_poweroff();
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
