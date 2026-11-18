/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The binding of the Digilent ZYBO (Zynq-7010), and of the emulator's
 * xilinx-zynq-a9 machine, which runs the same kernel.
 *
 * The console is a teletype on the PS's UART1, a Cadence UART: the
 * hardware layer writes its output to the UART, and the UART's receive
 * interrupt types what arrives into it.  The board's loader (the JTAG
 * boot, or the emulator's) passes a device tree that describes no
 * device, so the UART is found at the address and interrupt the SoC
 * fixes.
 *
 * The board publishes up to two boot devices: the initial ramdisk the
 * loader placed, when there is one, and the card in the SD slot (SDIO0),
 * when one comes up.  Both are disk images with an MBR.  A loader that
 * placed a ramdisk (the JTAG boot, the emulator's) boots from it, so the
 * ramdisk is the first device, the one the handoff names, and the card
 * the second; without a ramdisk the card is the first.  A root named by
 * the boot parameters (rootpart=/dev/mmcblk0p2) is found on either.
 * The board's Ethernet port (GEM0 and its RTL8211E) is the network
 * interface en0.
 * Halting and rebooting go through the hardware layer (the SLCR's system
 * reset); the board is powered from USB and has no power-off.  Once
 * interrupts are on, the programmable logic is published as /dev/fpga0.
 *
 * Values referenced: Linux v6.12 arch/arm/boot/dts/xilinx/zynq-7000.dtsi,
 * uart1's reg (0xe0001000) and its shared peripheral interrupt 50, GIC
 * interrupt 82.
 */

#include <stddef.h>
#include <stdint.h>

#include <hal/hal.h>
#include <drivers/disklabel/disklabel.h>
#include <drivers/generic/ramdisk.h>
#include <drivers/generic/serial-console.h>
#include <drivers/platform/zybo/zynq-devcfg.h>
#include <drivers/platform/zybo/zynq-ethernet.h>
#include <drivers/platform/zybo/zynq-sdhci.h>
#include <drivers/serial/cdns-uart.h>
#include <kern/boot.h>
#include <kern/disk.h>
#include <kern/klog.h>
#include <kern/partition.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The console's UART, UART1 of the PS: its registers and its GIC interrupt. */
#define ZYBO_UART1_PHYS			0xe0001000U
#define ZYBO_UART1_IRQ			82

/* The block size the ramdisk is cut into; a disk image is laid out in it. */
#define ZYBO_BLOCK_SIZE			512U

/*
 * The BIOS-style id of the first boot device, the one the handoff names;
 * the second device has the next one.
 */
#define ZYBO_BOOT_ID			0x80U

/* How many boot devices the board publishes at most: the ramdisk and the SD card. */
#define ZYBO_BOOT_DEVICE_MAX		2U

/* Marks a parameter that the kernel's platform interface passes but this board does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The UART the console reads, filled by kern_platform_init() before any
 * interrupt is on and kept for the life of the system, since its interrupt
 * handler reads it.
 */
static struct drv_cdns_uart uart;

/*
 * Nonzero when the UART was mapped and uart was filled; without it the
 * console is output only.  Set with uart.
 */
static int uart_present;

/*
 * The ramdisk's disk, published by kern_platform_init() and kept for the
 * life of the system; NULL when the loader placed no ramdisk.
 */
static struct disk *ramdisk;

/*
 * The SD card's disk, mmcblk0, published by kern_platform_init() and kept
 * for the life of the system; NULL when no card came up.
 */
static struct disk *sd_card;

/*
 * Nonzero once kern_platform_refresh_devices() has tried to publish the
 * programmable logic, so that it is tried only once.
 */
static int fpga_attached;

/*
 * The disks behind the published boot devices, in the order of their
 * BIOS-style ids from ZYBO_BOOT_ID; filled by kern_platform_init() before
 * the devices are handed out and kept with them.
 */
static struct disk *boot_disks[ZYBO_BOOT_DEVICE_MAX];

/* How many entries of boot_disks are published; set with them. */
static size_t boot_disk_count;

static void attach_uart(void);
static int publish_ramdisk(void);
static void add_boot_device(const struct kern_boot_handoff *handoff, struct kern_boot_device *devices, struct disk *disk);

/*
 * Publishes the boot devices of the ZYBO: the ramdisk, if any, and the SD
 * card, if one comes up.
 *
 * Returns the number of devices published.
 */
size_t
kern_platform_init(
	const struct kern_boot_handoff *handoff,
	struct kern_boot_device *devices,
	size_t capacity)
{
	int error;

	/* Rejects a handoff that is not the common record. */
	if (handoff == NULL || devices == NULL || capacity == 0)
		return 0;
	if (handoff->magic != KERN_HANDOFF_MAGIC)
		return 0;
	if (handoff->version != KERN_HANDOFF_VERSION_MULTIBOOT)
		return 0;
	if (handoff->size < sizeof(*handoff))
		return 0;

	/* Publishes the teletype as /dev/console's output. */
	drv_serial_console_init();

	/* Maps the UART the console will read once input starts. */
	attach_uart();

	/* Reads the ramdisk's partitions as an MBR disk's. */
	partition_set_scheme(&drv_partition_scheme_mbr);

	/* Starts the disk registry empty, before any disk is published. */
	disk_registry_reset();

	/* Publishes the ramdisk the loader placed, if it placed one. */
	error = publish_ramdisk();
	if (error != 0 && error != ENOENT)
		kern_logf("zybo: ramdisk not published (%d)\n", error);

	/* Publishes the card in the SD slot; the boot goes on without one. */
	error = drv_zynq_sdhci_init(&sd_card);
	if (error != 0) {
		kern_logf("zybo: SD card unavailable (%d)\n", error);
		sd_card = NULL;
	}

	/* Starts the Ethernet port; the boot goes on without the network. */
	error = drv_zynq_ethernet_init();
	if (error != 0)
		kern_logf("zybo: Ethernet unavailable (%d)\n", error);

	/* The ramdisk comes first: a loader that placed one boots from it. */
	boot_disk_count = 0;
	if (ramdisk != NULL && boot_disk_count < capacity)
		add_boot_device(handoff, devices, ramdisk);

	/* The SD card follows, or is first when there is no ramdisk. */
	if (sd_card != NULL && boot_disk_count < capacity)
		add_boot_device(handoff, devices, sd_card);

	/* Succeeded: the published devices, none when the board has no disk. */
	return boot_disk_count;
}

/*
 * Finishes device discovery once interrupts are enabled: starts the
 * Ethernet port's periodic service and publishes the programmable logic,
 * once.
 */
void
kern_platform_refresh_devices(
	const struct kern_boot_device *devices,
	size_t count)
{
	int error;

	UNUSED_PARAMETER(devices);
	UNUSED_PARAMETER(count);

	/* Starts the service of the link and of missed interrupts; a running one is left alone. */
	drv_zynq_ethernet_refresh();

	/* The logic is published once; a later refresh finds it there. */
	if (fpga_attached)
		return;
	fpga_attached = 1;

	/* Publishes /dev/fpga0; without it the board runs without its logic. */
	error = drv_zynq_devcfg_attach();
	if (error != 0)
		kern_logf("zybo: programmable logic unavailable (%d)\n", error);
}

/*
 * Starts typing what the UART receives into the console.
 */
int
kern_platform_input_init(
	void)
{
	int error;

	/* A UART that could not be mapped gives no input. */
	if (!uart_present)
		return 0;

	/* Has the UART's receive interrupt feed the console. */
	error = drv_cdns_uart_start_input(&uart, drv_serial_console_receive);
	if (error != 0) {
		kern_logf("zybo: serial input unavailable (%d)\n", error);
		return error;
	}

	/* Succeeded: the serial line types into the console. */
	return 0;
}

/*
 * Resolves a published boot device to its disk, the ramdisk or the SD
 * card, by its BIOS-style id.
 */
struct disk *
kern_platform_block_device(
	const struct kern_boot_device *device)
{
	uint32_t index;

	/* Only the board's disk devices have a disk. */
	if (device == NULL || device->device_class != KERN_DEV_SD)
		return NULL;

	/* Refuses an id below the first device's. */
	if (device->bios_id < ZYBO_BOOT_ID)
		return NULL;

	/* Refuses an id past the published devices. */
	index = device->bios_id - ZYBO_BOOT_ID;
	if (index >= boot_disk_count)
		return NULL;

	/* Succeeded: the disk published with that id. */
	return boot_disks[index];
}

/*
 * Writes a debug string to the early console.
 */
void
kern_platform_debug_write(
	const char *text)
{
	/* Ignores a missing string. */
	if (text == NULL)
		return;

	/* Emits every byte through the hardware layer. */
	while (*text != '\0') {
		hal_putc((unsigned char)*text);
		text++;
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
 * Reports that the ZYBO has no power-off; the caller halts.
 */
int
kern_platform_poweroff(
	void)
{
	/* The board is powered from USB, with nothing the PS can switch off. */
	return EOPNOTSUPP;
}

/*
 * Reboots the machine through the SLCR's system reset.
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
 * Maps the UART at the address the SoC fixes, for the console's input.
 *
 * A UART that cannot be mapped leaves the console output only; the boot
 * goes on.
 */
static void
attach_uart(
	void)
{
	int error;

	/* Maps UART1, whose output the hardware layer writes. */
	error = drv_cdns_uart_attach(&uart, ZYBO_UART1_PHYS, ZYBO_UART1_IRQ);
	if (error != 0) {
		kern_logf("zybo: serial port unavailable (%d)\n", error);
		return;
	}

	/* Keeps the UART for the input start and reports where it is. */
	uart_present = 1;
	kern_logf("zybo: serial port at %llx, interrupt %d\n",
		  (unsigned long long)uart.physical,
		  uart.irq);
}

/*
 * Publishes the initial ramdisk the loader placed as a disk.
 *
 * Returns 0, ENOENT when the loader placed none, or another errno.
 */
static int
publish_ramdisk(
	void)
{
	const uint64_t *initrd_range;
	uint64_t physical;
	uint64_t size;
	uint8_t *first;
	uint8_t *last;
	int error;

	/* Finds the ramdisk's physical range; none means no boot device. */
	initrd_range = hal_get_arch_handoff("armv7.initrd");
	if (initrd_range == NULL)
		return ENOENT;
	physical = initrd_range[0];
	size = initrd_range[1];

	/* Uses whole blocks only; a tail shorter than a block holds no data of the image. */
	size -= size % ZYBO_BLOCK_SIZE;
	if (size == 0)
		return ENOENT;

	/*
	 * Finds the ramdisk in the direct map.  RAM is mapped in one piece,
	 * so the range is usable when both its ends are in managed RAM and
	 * their aliases are as far apart as the addresses.
	 */
	first = kern_pmem_to_kernel((hal_physaddr_t)physical);
	if (first == NULL)
		return EFAULT;
	last = kern_pmem_to_kernel((hal_physaddr_t)(physical + size - 1));
	if (last == NULL)
		return EFAULT;
	if ((uint64_t)(last - first) != size - 1)
		return EFAULT;

	/* Publishes the memory as the ramdisk's disk. */
	error = drv_ramdisk_create("ram0", first, size, &ramdisk);
	if (error != 0) {
		kern_logf("zybo: ramdisk unavailable (%d)\n", error);
		return error;
	}

	/* Reports the ramdisk's size and place. */
	kern_logf("zybo: ramdisk ram0 %llu KiB at %llx\n",
		  (unsigned long long)(size / 1024U),
		  (unsigned long long)physical);

	/* Succeeded: the ramdisk is a disk. */
	return 0;
}

/*
 * Describes the next boot device: a disk of 512-byte blocks with the next
 * BIOS-style id, the boot origin when that is the id the handoff names.
 */
static void
add_boot_device(
	const struct kern_boot_handoff *handoff,
	struct kern_boot_device *devices,
	struct disk *disk)
{
	struct kern_boot_device *device;
	size_t index;

	/* Takes the next record and the next id. */
	device = &devices[boot_disk_count];
	device->device_class = KERN_DEV_SD;
	device->display_index = (uint8_t)boot_disk_count;
	device->bios_id = (uint8_t)(ZYBO_BOOT_ID + boot_disk_count);
	device->flags = KERN_DEV_PRESENT;
	if (handoff->boot_bios_id == device->bios_id)
		device->flags |= KERN_DEV_BOOT_ORIGIN;
	device->sector_size = ZYBO_BLOCK_SIZE;
	device->cylinders = 0;
	device->heads = 0;
	device->sectors = 0;
	device->controller_location = 0;

	/* Clears the record's reserved bytes. */
	for (index = 0; index < sizeof(device->reserved); index++)
		device->reserved[index] = 0;

	/*
	 * Keeps the disk under the device's id; the count is what
	 * kern_platform_block_device() resolves ids against.
	 */
	boot_disks[boot_disk_count] = disk;
	boot_disk_count++;
}
