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
#include <drivers/generic/fdt.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#if CONFIG_DRIVER_BCM2711_GENET
#include "drivers/platform/rpi4/rpi4-ethernet.h"
#endif
#if CONFIG_DRIVER_USB_HID
#include <drivers/usb/usb-hid.h>
#endif
#if CONFIG_DRIVER_BCM2711_AUDIO
#include "drivers/platform/rpi4/rpi4-audio.h"
#endif
#include "drivers/platform/rpi-common/rpi-console.h"
#include "drivers/platform/rpi4/rpi4-firmware.h"
#include "drivers/platform/rpi-common/rpi-gpio.h"
#include "drivers/platform/rpi4/rpi4-pcie.h"
#include "drivers/platform/rpi-common/rpi-sdhci.h"
#if CONFIG_DRIVER_BCM2711_WIFI
#include "drivers/platform/rpi4/rpi4-wifi.h"
#endif
#if CONFIG_DRIVER_BCM2711_BT
#include "drivers/platform/rpi4/rpi4-bt.h"
#endif
#if CONFIG_DRIVER_BCM2711_GPU
#include "drivers/gpu/bcm2711/bcm2711-gpu.h"
#endif
#if CONFIG_DRIVER_BCM2711_VCHIQ
#include "drivers/platform/rpi4/rpi4-vchiq.h"
#endif
#if CONFIG_DRIVER_BCM2711_VIDEO_PROBE
#include "drivers/platform/rpi4/rpi4-mmal-probe.h"
#endif

/*
 * How much of memory at the device tree's address may be read.
 *
 * The HAL checks the tree against the same bound before the kernel starts,
 * and the tree's header states its real size inside it.
 */
#define RPI4_FDT_READ_LIMIT	(2U * 1024U * 1024U)

#if CONFIG_DRIVER_BCM2711_WIFI
/*
 * The SD controller the SD card driver bound to, for the onboard Wi-Fi's
 * probe.
 *
 * Recorded by kern_platform_init() before any other driver starts and
 * never changed afterwards.  The controller is the handoff's eMMC2, or the
 * legacy controller when QEMU puts the SD card there; the onboard Wi-Fi,
 * which uses the legacy controller on a board, leaves it alone when the SD
 * card has it.  Zero means not yet known.
 */
static uintptr_t rpi4_sd_base;
#endif

#if CONFIG_DRIVER_BCM2711_WIFI || CONFIG_DRIVER_BCM2711_BT
/*
 * The device tree's physical address, for the onboard Wi-Fi's and
 * Bluetooth's probes once interrupts are on.
 *
 * Recorded by kern_platform_init() and never changed afterwards; zero
 * means none.
 */
static uint64_t rpi4_fdt_phys;
#endif

static void prepare_shared_blocks(uint64_t fdt_phys);

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
	const uint64_t *fdt_object;
	const uint64_t *sdhci_object;
	struct kern_boot_device *device;
	uint64_t fdt_phys;
	uint64_t sdhci_phys;
	int sdhci_error;
#if CONFIG_DRIVER_BCM2711_GPU
	struct kern_boot_framebuffer framebuffer;
	struct drv_bcm2711_boot_screen screen;
#endif
	unsigned i;
#if CONFIG_DRIVER_BCM2711_GENET
	int ethernet_error;
#endif

	/* Rejects a handoff that is not the common record. */
	if (!handoff ||
	    !devices ||
	    capacity == 0 ||
	    handoff->magic != KERN_HANDOFF_MAGIC ||
	    handoff->version != KERN_HANDOFF_VERSION_MULTIBOOT ||
	    handoff->size < sizeof(*handoff))
		return 0;

	/* Finds the SD controller the device tree names; without it nothing boots. */
	sdhci_object = hal_get_arch_handoff("rpi4.sdhci");
	if (sdhci_object == NULL)
		return 0;
	sdhci_phys = *sdhci_object;

	/* Finds the device tree; zero tells the drivers below that none came. */
	fdt_object = hal_get_arch_handoff("rpi4.fdt");
	fdt_phys = 0;
	if (fdt_object != NULL)
		fdt_phys = *fdt_object;

	/* Publishes the serial console as /dev/console's output. */
	drv_rpi_console_init(DRV_RPI_BOARD_PI4);

	/* Selects the MBR partition scheme and starts the SD controller. */
	partition_set_scheme(&drv_partition_scheme_mbr);
	disk_registry_reset();
#if CONFIG_DRIVER_BCM2711_WIFI
	rpi4_sd_base = (uintptr_t)sdhci_phys;
#endif
	sdhci_error = drv_rpi_sdhci_init((uintptr_t)sdhci_phys, 0);
	if (sdhci_error != 0) {
		/*
		 * QEMU raspi4b currently attaches -drive if=sd to the legacy
		 * Arasan controller, while real Pi 4 firmware boots from eMMC2.
		 */
		if (sdhci_phys != 0xfe340000ULL)
			return 0;

		/* Tries the legacy controller QEMU attaches the card to. */
		sdhci_error = drv_rpi_sdhci_init(0xfe300000ULL, 0);
		if (sdhci_error != 0)
			return 0;

#if CONFIG_DRIVER_BCM2711_WIFI
		rpi4_sd_base = (uintptr_t)0xfe300000ULL;
#endif
		hal_puts("sdhci: using QEMU legacy-controller fallback\n");
	}

#if CONFIG_DRIVER_BCM2711_WIFI || CONFIG_DRIVER_BCM2711_BT
	/* Keeps the tree's address for the Wi-Fi and Bluetooth probes discovery starts later. */
	rpi4_fdt_phys = fdt_phys;
#endif

	/* Prepares the firmware mailbox and the GPIO block the drivers below share. */
	prepare_shared_blocks(fdt_phys);

#if CONFIG_DRIVER_BCM2711_GENET
	/* Starts onboard Ethernet independently of USB and PCIe discovery. */
	ethernet_error = drv_rpi4_ethernet_init(fdt_phys);
	if (ethernet_error != 0) {
		kern_logf(
			"genet: onboard Ethernet unavailable (%d)\n",
			ethernet_error);
	}
#endif

	/*
	 * Brings up PCIe and what sits behind it.  A board or emulator without
	 * a usable controller boots without PCI devices.
	 */
	(void)drv_rpi4_pcie_init(fdt_phys);

#if CONFIG_DRIVER_BCM2711_AUDIO
	/* Publishes the headphone jack; a board without it boots without sound. */
	(void)drv_rpi4_audio_init(fdt_phys);
#endif

#if CONFIG_DRIVER_BCM2711_VCHIQ
	/*
	 * Starts the link to the VideoCore firmware's services before the GPU,
	 * whose video decode uses it; a board without it boots without them.
	 */
	(void)drv_rpi4_vchiq_init(fdt_phys);
#endif
#if CONFIG_DRIVER_BCM2711_VIDEO_PROBE
	/* A test kernel publishes the decoder probe's device; a write to it runs the probe. */
	(void)drv_rpi4_mmal_probe_start();
#endif

#if CONFIG_DRIVER_BCM2711_GPU
	/*
	 * Describes the firmware's screen to the graphics driver; a boot whose
	 * firmware allocated none describes an empty one, which the record is
	 * left as.
	 */
	(void)kern_boot_framebuffer("rpi4.framebuffer", &framebuffer);
	screen.physical = framebuffer.physical_base;
	screen.size = framebuffer.size;
	screen.width = framebuffer.width;
	screen.height = framebuffer.height;
	screen.pitch = framebuffer.pitch;
	screen.format = framebuffer.format;

	/*
	 * Finds the display path and the V3D engine.  A board or emulator
	 * without them, or a boot with rpi4gpu.off=1, goes on without them.
	 */
	(void)drv_bcm2711_gpu_attach(fdt_phys, &screen);
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

#if CONFIG_DRIVER_BCM2711_GENET
	/* Starts periodic link and missed-interrupt service after discovery. */
	drv_rpi4_ethernet_refresh();
#endif

	/* Has the USB controllers behind PCIe find their devices. */
	drv_rpi4_pcie_refresh();

#if CONFIG_DRIVER_BCM2711_WIFI
	/*
	 * Starts the onboard Wi-Fi's thread; a board or emulator without the
	 * chip, or with the SD card on its controller, goes on without it.
	 */
	(void)drv_rpi4_wifi_probe(rpi4_fdt_phys, rpi4_sd_base);
#endif

#if CONFIG_DRIVER_BCM2711_BT
	/*
	 * Starts the onboard Bluetooth's thread; QEMU, a tree with disable-bt,
	 * or a build whose PL011 is the serial console goes on without it.
	 */
	(void)drv_rpi4_bt_probe(rpi4_fdt_phys);
#endif
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
	error = drv_rpi_console_start_input();
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
	disk = drv_rpi_sdhci_disk();

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
 * Stops the onboard Wi-Fi's publishing thread before a shutdown or reboot.
 */
void
kern_platform_shutdown_prepare(
	void)
{
#if CONFIG_DRIVER_BCM2711_WIFI
	/* Ends a bring-up still in progress; a published wlanN is closed by the network's shutdown. */
	drv_rpi4_wifi_stop();
#endif
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

/*
 * Prepares the firmware mailbox and the GPIO block once, before any driver
 * that shares them starts.
 *
 * Neither preparation touches a register: the mailbox gets its request
 * buffer and mapping, the GPIO block its mapping.  So it is as harmless on
 * QEMU's raspi4b as on a board, and a failure only leaves the drivers that
 * need them without; the boot goes on.
 */
static void
prepare_shared_blocks(
	uint64_t fdt_phys)
{
	struct drv_fdt fdt;
	const void *blob;
	int error;

	/* Skips a handoff without a tree: nothing names the blocks. */
	if (fdt_phys == 0)
		return;

	/* Finds the device tree in the direct map. */
	blob = kern_pmem_to_kernel((hal_physaddr_t)fdt_phys);
	if (blob == NULL)
		return;

	/* Refuses a tree whose header does not check out. */
	error = drv_fdt_open(&fdt, blob, RPI4_FDT_READ_LIMIT);
	if (error != 0) {
		kern_logf("rpi4: unreadable device tree (%d)\n", error);
		return;
	}

	/* Prepares the mailbox the wireless, PCIe and graphics drivers ask the firmware through. */
	error = drv_rpi4_firmware_init(&fdt);
	if (error != 0)
		kern_logf("rpi4-firmware: mailbox unavailable (%d)\n", error);

	/* Maps the GPIO block the wireless and Bluetooth drivers set their pins through. */
	error = drv_rpi_gpio_init(&fdt);
	if (error != 0)
		kern_logf("rpi4-gpio: GPIO block unavailable (%d)\n", error);
}
