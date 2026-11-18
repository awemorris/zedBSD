/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ODROID-C5 platform binding.
 *
 * The device tree the bootloader passed names the SD card's controller
 * (the S905X5M's sd_emmc_b); the platform publishes the card as the single
 * boot device.  The console is the debug UART and the framebuffer the
 * bootloader left on HDMI.  The USB host and the Ethernet port start after
 * the SD card, and a failure of either leaves the boot going without it.
 * The Mali GPU, when the build has its driver (CONFIG_DRIVER_MALI_GPU, off
 * by default), is found once interrupts are on.
 */

#include <uapi/errno.h>
#include <hal/hal.h>
#include <drivers/disklabel/disklabel.h>
#include <kern/disk.h>
#include <kern/partition.h>
#include <kern/platform.h>
#include <kern/boot.h>
#include <kern/klog.h>
#include "drivers/platform/s905x5m/s905x5m-console.h"
#include "drivers/platform/s905x5m/s905x5m-mmc.h"
#include "drivers/platform/s905x5m/s905x5m-usb.h"
#include "drivers/platform/s905x5m/s905x5m-ethernet.h"
#if CONFIG_DRIVER_MALI_GPU
#include "drivers/gpu/mali/mali-attach.h"
#endif
#include <drivers/usb/usb-hid.h>

/* The BIOS-style number of the boot disk the published device carries. */
#define ODROID_C5_BOOT_DISK_ID	0x80U

/* The bytes of one sector of the SD card. */
#define ODROID_C5_SECTOR_BYTES	512U

/*
 * The board's debug UART, its UART connector: the S905X5M's uart_B.
 *
 * Value referenced: Hardkernel Linux odroids7d-6.12.y common_drivers
 * arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (uart_B under apb4's
 * ranges).
 */
#define ODROID_C5_DEBUG_UART	0xfe07a000ULL

#if CONFIG_DRIVER_MALI_GPU
/*
 * The device tree's physical address, for the GPU probe that runs once
 * interrupts are on.
 *
 * Recorded by kern_platform_init() and never changed afterwards; zero
 * means none.
 */
static uint64_t odroid_c5_fdt_phys;
#endif

static void start_peripherals(uint64_t fdt_phys);

/*
 * Publishes the boot devices of the ODROID-C5.
 *
 * Returns the number of devices published, which is zero for a handoff the
 * platform does not understand, a boot without a device tree, or an SD card
 * that does not answer.
 */
size_t
kern_platform_init(
	const struct kern_boot_handoff *handoff,
	struct kern_boot_device *devices,
	size_t capacity)
{
	const uint64_t *fdt_object;
	struct kern_boot_device *device;
	uint64_t fdt_phys;
	unsigned index;
	int error;

	/* Rejects a missing handoff or nowhere to publish. */
	if (handoff == NULL)
		return 0;
	if (devices == NULL)
		return 0;
	if (capacity == 0)
		return 0;

	/* Rejects a handoff that is not the common record. */
	if (handoff->magic != KERN_HANDOFF_MAGIC)
		return 0;
	if (handoff->version != KERN_HANDOFF_VERSION_MULTIBOOT)
		return 0;
	if (handoff->size < sizeof(*handoff))
		return 0;

	/* Publishes the console as /dev/console's output. */
	drv_s905x5m_console_init(ODROID_C5_DEBUG_UART, "odroidc5.framebuffer");

	/* Finds the device tree, which names every controller; without it nothing boots. */
	fdt_object = hal_get_arch_handoff("odroidc5.fdt");
	if (fdt_object == NULL) {
		kern_logf("odroid-c5: no device tree\n");
		return 0;
	}
	fdt_phys = *fdt_object;
#if CONFIG_DRIVER_MALI_GPU
	odroid_c5_fdt_phys = fdt_phys;
#endif

	/* Selects the MBR partition scheme the SD card carries. */
	partition_set_scheme(&drv_partition_scheme_mbr);
	disk_registry_reset();

	/* Starts the microSD slot's controller and the card the bootloader read. */
	error = drv_s905x5m_mmc_init(fdt_phys);
	if (error != 0) {
		kern_logf("odroid-c5: SD card unavailable (%d)\n", error);
		return 0;
	}

	/* Starts the USB host and the Ethernet port; the boot goes on without either. */
	start_peripherals(fdt_phys);

	/* Publishes the SD card as the boot device. */
	device = &devices[0];
	device->device_class = KERN_DEV_SD;
	device->display_index = 0;
	device->bios_id = ODROID_C5_BOOT_DISK_ID;
	device->flags = KERN_DEV_PRESENT;
	if (handoff->boot_bios_id == ODROID_C5_BOOT_DISK_ID)
		device->flags |= KERN_DEV_BOOT_ORIGIN;
	device->sector_size = ODROID_C5_SECTOR_BYTES;
	device->cylinders = 0;
	device->heads = 0;
	device->sectors = 0;
	device->controller_location = 0;

	/* Clears the record's reserved bytes. */
	for (index = 0; index < sizeof(device->reserved); index++)
		device->reserved[index] = 0;

	/* Succeeded: the single published device. */
	return 1;
}

/*
 * Finishes device discovery once interrupts are enabled.
 *
 * The SD card is published already; the USB host looks at its ports and
 * the Ethernet port starts its link service now.
 */
void
kern_platform_refresh_devices(
	const struct kern_boot_device *d,
	size_t n)
{
	(void)d;
	(void)n;

	/* Has the USB host find its devices. */
	drv_s905x5m_usb_refresh();

	/* Starts the Ethernet port's link service. */
	drv_s905x5m_ethernet_refresh();

#if CONFIG_DRIVER_MALI_GPU
	/*
	 * Finds the Mali GPU and its display; a board without the device tree
	 * goes on without them.
	 */
	if (odroid_c5_fdt_phys != 0)
		(void)drv_mali_gpu_attach(odroid_c5_fdt_phys);
#endif
}

/*
 * Starts the console's serial input once the input core is ready.
 */
int
kern_platform_input_init(
	void)
{
	int error;

#if CONFIG_DRIVER_USB_HID
	/* Activates pending USB keyboards and mice after the console claims event0. */
	drv_usb_hid_input_ready();
#endif

	/* Starts the serial reader, which a build without the serial console leaves off. */
	error = drv_s905x5m_console_start_input();
	if (error != 0)
		return error;

	/* Succeeded: platform input is available to the console. */
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
	if (d == NULL)
		return NULL;
	if (d->device_class != KERN_DEV_SD)
		return NULL;

	/* Resolves the SD card's disk. */
	disk = drv_s905x5m_mmc_disk();

	/* Succeeded: the SD card's disk, or none before it was published. */
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
	 * already wrote the record there, serial line included.
	 */
	console = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (console != NULL)
		return;

	/* Emits every byte through the early console. */
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
 * Turns the power off through the HAL, halting if that returns.
 */
int
kern_platform_poweroff(
	void)
{
	/* Asks the secure firmware to turn the board off. */
	hal_poweroff();

	/* Halts in case the firmware refused. */
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
	/* Asks the secure firmware to reset the board. */
	hal_reset();

	/* Halts in case the reset request returns. */
	for (;;)
		hal_halt();
}

/*
 * Starts the USB host and the Ethernet port.
 *
 * Either one that fails leaves the boot going without it; the reason is
 * in the log.
 */
static void
start_peripherals(
	uint64_t fdt_phys)
{
	int error;

	/* Starts the USB host. */
	error = drv_s905x5m_usb_init(fdt_phys);
	if (error != 0)
		kern_logf("odroid-c5: USB unavailable (%d)\n", error);

	/* Starts the Ethernet port. */
	error = drv_s905x5m_ethernet_init(fdt_phys);
	if (error != 0)
		kern_logf("odroid-c5: Ethernet unavailable (%d)\n", error);
}
