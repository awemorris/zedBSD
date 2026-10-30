/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5 platform binding.
 *
 * The boot handoff names the SD card's controller (the BCM2712's sdio1);
 * the platform publishes the card as the single boot device.  The console
 * is the firmware's framebuffer, and the debug UART when the build asks for
 * it.  The USB ports and the Ethernet port behind the RP1 south bridge
 * start after the SD card; a board whose firmware reset RP1's link boots
 * without them.  The onboard Wi-Fi, and the onboard Bluetooth when the
 * build has its driver (CONFIG_DRIVER_RPI5_BT, off by default), are probed
 * once interrupts are on and come up in the background.  The processor's
 * temperature and the fan connector, when the build has their driver
 * (CONFIG_DRIVER_RPI5_FAN, on by default), start then too.  The external
 * PCIe port of the M.2 HATs starts after RP1 when config.txt enables it,
 * and the NVMe drive behind it is published once interrupts are on.  The V3D render
 * engine, when the build has its driver (CONFIG_DRIVER_BCM2712_GPU, off by
 * default), is found then and waits for root to start it.
 */

#include <uapi/errno.h>
#include <hal/hal.h>
#include <drivers/disklabel/disklabel.h>
#include <kern/disk.h>
#include <kern/partition.h>
#include <kern/platform.h>
#include <kern/boot.h>
#include <kern/klog.h>
#include "drivers/platform/rpi-common/rpi-console.h"
#include "drivers/platform/rpi-common/rpi-sdhci.h"
#include "drivers/platform/rpi5/rp1.h"
#include "drivers/platform/rpi5/rp1-usb.h"
#include "drivers/platform/rpi5/rp1-ethernet.h"
#include "drivers/platform/rpi5/rpi5-wifi.h"
#include "drivers/platform/rpi5/rpi5-serror.h"
#include "drivers/platform/rpi5/rpi5-pcie.h"
#if CONFIG_DRIVER_RPI5_BT
#include "drivers/platform/rpi5/rpi5-bt.h"
#endif
#if CONFIG_DRIVER_RPI5_FAN
#include "drivers/platform/rpi5/rpi5-fan.h"
#endif
#if CONFIG_DRIVER_BCM2712_GPU
#include "drivers/gpu/bcm2711/bcm2712-gpu.h"
#endif
#include <drivers/usb/usb-hid.h>

/* The firmware's BIOS-style number of the boot disk the handoff names. */
#define RPI5_BOOT_DISK_ID	0x80U

/* The bytes of one sector of the SD card. */
#define RPI5_SECTOR_BYTES	512U

/*
 * The device tree's physical address, for the probes that run once
 * interrupts are on.
 *
 * Recorded by kern_platform_init() and never changed afterwards; zero
 * means none.
 */
static uint64_t rpi5_fdt_phys;

static void start_rp1(uint64_t fdt_phys);
static void start_pcie(uint64_t fdt_phys);

/*
 * Publishes the boot devices described by the Raspberry Pi 5 boot handoff.
 *
 * Returns the number of devices published, which is zero for a handoff the
 * platform does not understand or an SD card that does not answer.
 */
size_t
kern_platform_init(
	const struct kern_boot_handoff *handoff,
	struct kern_boot_device *devices,
	size_t capacity)
{
	const uint64_t *sdhci_object;
	const uint64_t *sdhci_clock_object;
	const uint64_t *fdt_object;
	struct kern_boot_device *device;
	uint64_t sdhci_phys;
	uint64_t sdhci_clock_hz;
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

	/* Finds the SD controller the device tree names; without it nothing boots. */
	sdhci_object = hal_get_arch_handoff("rpi5.sdhci");
	if (sdhci_object == NULL)
		return 0;
	sdhci_phys = *sdhci_object;

	/* Reads the controller's base clock the HAL names; zero takes the controller's own. */
	sdhci_clock_object = hal_get_arch_handoff("rpi5.sdhci.clock");
	sdhci_clock_hz = 0;
	if (sdhci_clock_object != NULL)
		sdhci_clock_hz = *sdhci_clock_object;

	/* Finds the device tree; zero tells the drivers below that none came. */
	fdt_object = hal_get_arch_handoff("rpi5.fdt");
	fdt_phys = 0;
	if (fdt_object != NULL)
		fdt_phys = *fdt_object;
	rpi5_fdt_phys = fdt_phys;

	/* Publishes the console as /dev/console's output. */
	drv_rpi_console_init(DRV_RPI_BOARD_PI5);

	/* Selects the MBR partition scheme the SD card carries. */
	partition_set_scheme(&drv_partition_scheme_mbr);
	disk_registry_reset();

	/* Starts the SD controller and the card the firmware booted from. */
	error = drv_rpi_sdhci_init((uintptr_t)sdhci_phys, sdhci_clock_hz);
	if (error != 0) {
		kern_logf("rpi5: SD card unavailable (%d)\n", error);
		return 0;
	}

	/* Starts the USB ports behind RP1; without RP1 the boot goes on. */
	start_rp1(fdt_phys);

	/* Starts the external PCIe port; without it the boot goes on. */
	start_pcie(fdt_phys);

	/* Publishes the SD card as the boot device. */
	device = &devices[0];
	device->device_class = KERN_DEV_SD;
	device->display_index = 0;
	device->bios_id = RPI5_BOOT_DISK_ID;
	device->flags = KERN_DEV_PRESENT;
	if (handoff->boot_bios_id == RPI5_BOOT_DISK_ID)
		device->flags |= KERN_DEV_BOOT_ORIGIN;
	device->sector_size = RPI5_SECTOR_BYTES;
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
 * The SD card is published already; the USB controllers behind RP1 look
 * at their root ports now.
 */
void
kern_platform_refresh_devices(
	const struct kern_boot_device *d,
	size_t n)
{
	(void)d;
	(void)n;

	/* Has the USB controllers find their devices. */
	drv_rp1_usb_refresh();
	(void)drv_rpi5_serror_check("rp1-usb: finding devices");

	/* Starts the Ethernet port's link and missed-interrupt service. */
	drv_rp1_ethernet_refresh();
	(void)drv_rpi5_serror_check("gem: starting the service");

	/* Publishes the NVMe drive behind the external PCIe port. */
	drv_rpi5_pcie_refresh();
	(void)drv_rpi5_serror_check("rpi5-pcie: finding namespaces");

#if CONFIG_DRIVER_RPI5_FAN
	/*
	 * Starts reading the temperature and driving the fan connector; a
	 * board without RP1 goes on without them.
	 */
	(void)drv_rpi5_fan_start();
	(void)drv_rpi5_serror_check("rpi5-fan: starting the fan");
#endif

	/*
	 * Starts the onboard Wi-Fi's thread; a board without the chip goes on
	 * without it.
	 */
	(void)drv_rpi5_wifi_probe(rpi5_fdt_phys);

#if CONFIG_DRIVER_RPI5_BT
	/*
	 * Starts the onboard Bluetooth's thread; a board without the
	 * controller goes on without it.
	 */
	(void)drv_rpi5_bt_probe(rpi5_fdt_phys);
#endif

#if CONFIG_DRIVER_BCM2712_GPU
	/*
	 * Finds the V3D render engine and holds it for root's start (sysctl
	 * hw.gpu.start=1); a board without it, or a boot with rpi4gpu.off=1,
	 * goes on without it.
	 */
	if (rpi5_fdt_phys != 0)
		(void)drv_bcm2712_gpu_attach(rpi5_fdt_phys);
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
	error = drv_rpi_console_start_input();
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
	disk = drv_rpi_sdhci_disk();

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
	/* Asks the firmware to turn the board off. */
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
	/* Asks the firmware to reset the board. */
	hal_reset();

	/* Halts in case the reset request returns. */
	for (;;)
		hal_halt();
}

/*
 * Finds RP1 and starts the USB controllers and the Ethernet port behind it.
 *
 * A board without RP1, or whose firmware reset the link to it, boots
 * without them; the reason is in the log.
 */
static void
start_rp1(
	uint64_t fdt_phys)
{
	int error;

	/* Leaves a boot without a device tree alone. */
	if (fdt_phys == 0) {
		kern_logf("rpi5: no device tree; no RP1, USB or Ethernet\n");
		return;
	}

	/* Names the SD card's start if it left an SError, so that RP1's steps start clean. */
	(void)drv_rpi5_serror_check("sdhci: starting the SD card");

	/* Finds RP1 as the firmware left it. */
	error = drv_rp1_init(fdt_phys);
	(void)drv_rpi5_serror_check("rp1: finding RP1");
	if (error != 0) {
		kern_logf("rpi5: RP1 unavailable (%d)\n", error);
		return;
	}

	/* Starts the USB controllers. */
	error = drv_rp1_usb_init();
	(void)drv_rpi5_serror_check("rp1-usb: starting the controllers");
	if (error != 0)
		kern_logf("rpi5: USB unavailable (%d)\n", error);

	/* Starts the Ethernet port. */
	error = drv_rp1_ethernet_init();
	(void)drv_rpi5_serror_check("gem: starting the port");
	if (error != 0)
		kern_logf("rpi5: Ethernet unavailable (%d)\n", error);
}

/*
 * Starts the PCIe root complexes other than RP1's: the external port of
 * the M.2 HATs when config.txt enables it.
 *
 * A board without a device tree, or whose port is disabled or has no
 * link, boots without it; the reason is in the log.
 */
static void
start_pcie(
	uint64_t fdt_phys)
{
	int error;

	/* Leaves a boot without a device tree alone. */
	if (fdt_phys == 0)
		return;

	/* Starts the root complexes and the drivers behind them. */
	error = drv_rpi5_pcie_init(fdt_phys);
	(void)drv_rpi5_serror_check("rpi5-pcie: starting the root complexes");
	if (error != 0)
		kern_logf("rpi5: external PCIe unavailable (%d)\n", error);
}
