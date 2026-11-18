/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u (UltraSPARC) platform binding.
 *
 * The HAL names one CMD646 IDE controller; the platform publishes its disk
 * as the single boot device.  The console is the firmware framebuffer and
 * the serial port, drawn and read by the sun4u console driver (the HAL's
 * screen when there is no framebuffer).  The framebuffer is also offered
 * to /dev/graphics, and the ebus 8042's keyboard and mouse, which the HAL
 * names, are polled into the input devices (WS275).
 */

#include <uapi/errno.h>
#include <hal/hal.h>
#include <drivers/disklabel/disklabel.h>
#include <kern/disk.h>
#include <kern/platform.h>
#include <kern/boot.h>
#include "drivers/platform/sun4u/sun4u-cmd646.h"
#include "drivers/platform/sun4u/sun4u-console.h"
#include "drivers/platform/sun4u/sun4u-firmware-fb.h"
#include "drivers/isa/ps2-8042.h"
#include <kern/kcrt.h>
#include <kern/klog.h>

/*
 * Publishes the boot devices described by the sun4u boot handoff.
 *
 * The HAL hands over the common record and names the CMD646 IDE
 * controller's primary channel; it names the channel only for a CMD646.
 * Returns the number of devices published, which is zero for a handoff the
 * platform does not understand.
 */
size_t
kern_platform_init(
	const struct kern_boot_handoff *h,
	struct kern_boot_device *d,
	size_t capacity)
{
	const uint64_t *command_object;
	const uint64_t *control_object;
	int error;

	/* Publishes the console before anything can fail, so the failure is seen. */
	drv_sun4u_console_init();

	/* Offers the framebuffer to /dev/graphics; without one there is no device. */
	error = drv_sun4u_fb_register_backend();
	if (error != 0 && error != ENODEV)
		kern_logf("graphics: sun4u framebuffer not offered (%d)\n", error);

	/* Rejects a handoff that is missing, foreign or truncated. */
	if (!h ||
	    !d ||
	    !capacity ||
	    h->magic != KERN_HANDOFF_MAGIC ||
	    h->version != KERN_HANDOFF_VERSION_MULTIBOOT ||
	    h->size < sizeof(*h))
		return 0;

	/* Finds the IDE controller's primary command block; without it nothing boots. */
	command_object = hal_get_arch_handoff("sun4u.cmd646.primary.command");
	if (command_object == NULL || *command_object > UINT16_MAX)
		return 0;

	/* Finds the IDE controller's primary control block. */
	control_object = hal_get_arch_handoff("sun4u.cmd646.primary.control");
	if (control_object == NULL || *control_object > UINT16_MAX)
		return 0;

	/* Selects the Sun partition scheme and starts the IDE controller. */
	partition_set_scheme(&drv_partition_scheme_sun);
	disk_registry_reset();
	error = drv_sun4u_cmd646_init(
		(uint16_t)*command_object,
		(uint16_t)*control_object);
	if (error != 0)
		return 0;

	/* Publishes the IDE disk as the boot device. */
	kern_memset(d, 0, sizeof(*d));
	d->device_class = KERN_DEV_IDE;
	d->display_index = 0;
	d->bios_id = 0x80;
	d->flags = KERN_DEV_PRESENT | KERN_DEV_BOOT_ORIGIN;
	d->sector_size = 512;

	/* Reports the single published device. */
	return 1;
}

/*
 * Refreshes the published devices; sun4u has no hot-pluggable devices.
 */
void
kern_platform_refresh_devices(
	const struct kern_boot_device *d,
	size_t n)
{
	(void)d;
	(void)n;
}

/*
 * Starts the platform input: the serial port read into the console, and
 * the 8042's keyboard and mouse when the HAL names one.
 */
int
kern_platform_input_init(
	void)
{
	static const struct drv_isa_ps2_8042_config i8042 = {
		.keyboard_irq = -1,
		.mouse_irq = -1,
		.keyboard_name = "sun4u PS/2 keyboard",
		.keyboard_path = "sun4u/i8042/kbd0",
		.mouse_name = "sun4u PS/2 mouse",
		.mouse_path = "sun4u/i8042/aux0",
	};
	struct drv_isa_ps2_8042_config config;
	const uint64_t *data;
	const uint64_t *command;
	int error;

	/* Starts the console's reader. */
	error = drv_sun4u_console_start_input();
	if (error != 0)
		return error;

	/* Finds the 8042; a machine without one types on the serial port only. */
	data = hal_get_arch_handoff("sun4u.i8042.data");
	command = hal_get_arch_handoff("sun4u.i8042.command");
	if (data == NULL || command == NULL)
		return 0;
	if (*data > UINT16_MAX || *command > UINT16_MAX)
		return 0;

	/* Polls the 8042 at the HAL's ports; its interrupts are not routed. */
	config = i8042;
	config.data_port = (uint16_t)*data;
	config.status_port = (uint16_t)*command;
	error = drv_isa_ps2_8042_init(&config);
	if (error != 0)
		kern_logf("input: sun4u 8042 unavailable (%d)\n", error);

	/* Succeeded: typed characters reach the console. */
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

	/* Only the IDE boot device has a disk. */
	if (!d || d->device_class != KERN_DEV_IDE)
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
	void (*console)(int character);

	/* Ignores a missing string. */
	if (s == NULL)
		return;

	/*
	 * Writes nothing once the kernel's console is published: the log
	 * already wrote the record there, serial port included.
	 */
	console = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (console != NULL)
		return;

	/* Emits every byte through the HAL. */
	while (*s != '\0') {
		hal_putc((unsigned char)*s);
		s++;
	}
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
 * Reports that sun4u power-off is not implemented; the caller halts.
 */
int
kern_platform_poweroff(
	void)
{
	/* The board's power controller is not driven, so the caller halts instead. */
	return EOPNOTSUPP;
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
