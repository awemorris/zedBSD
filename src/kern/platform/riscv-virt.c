/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The binding of the emulators' RISC-V virt board.
 *
 * The console is a teletype on the board's UART: the hardware layer writes
 * its output (to the firmware's console and to the RAM framebuffer it set
 * up), and the UART's receive interrupt types what arrives into it.
 *
 * The board publishes one boot device, the initial ramdisk the firmware
 * loaded, when there is one.  It holds a disk image partitioned like a
 * Raspberry Pi 4's SD card: a FAT boot partition first and the UFS root
 * beside it, so the kernel's root search finds the root as it does there.
 * A GPT disk behind the PCI host, such as the native disk image on an
 * NVMe drive, has its partitions read too, so the boot parameters can name
 * a root there instead.  Halting, rebooting and powering off go through
 * the hardware layer.
 */

#include <stddef.h>
#include <stdint.h>

#include <hal/hal.h>
#include <drivers/disklabel/disklabel.h>
#include <drivers/generic/fdt.h>
#include <drivers/generic/ramdisk.h>
#include <drivers/generic/serial-console.h>
#include <drivers/platform/riscv-virt/virt-pci.h>
#include <drivers/serial/ns16550.h>
#include <kern/boot.h>
#include <kern/disk.h>
#include <kern/klog.h>
#include <kern/partition.h>
#include <kern/platform.h>
#include <kern/pmem.h>

/*
 * How much of memory at the device tree's address may be read.  The
 * hardware layer bounds the tree it accepts the same way.
 */
#define RISCV_VIRT_FDT_READ_LIMIT	(2U * 1024U * 1024U)

/* The block size the ramdisk is cut into; a disk image is laid out in it. */
#define RISCV_VIRT_BLOCK_SIZE		512U

/* The BIOS-style id the boot device has, the one the handoff names. */
#define RISCV_VIRT_BOOT_ID		0x80U

/* Marks a parameter that the kernel's platform interface passes but this board does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The UART the console reads, filled by kern_platform_init() before any
 * interrupt is on and kept for the life of the system, since its interrupt
 * handler reads it.
 */
static struct drv_ns16550 uart;

/*
 * Nonzero when the device tree described the UART and uart was filled;
 * without it the console is output only.  Set with uart.
 */
static int uart_present;

/*
 * The ramdisk's disk, published by kern_platform_init() and kept for the
 * life of the system; NULL when the firmware loaded no ramdisk.
 */
static struct disk *ramdisk;

static void probe_uart(void);
static void probe_pci(void);
static int publish_ramdisk(void);

/*
 * Publishes the boot devices of the virt board: the ramdisk, if any.
 *
 * Returns the number of devices published.
 */
size_t
kern_platform_init(
	const struct kern_boot_handoff *handoff,
	struct kern_boot_device *devices,
	size_t capacity)
{
	struct kern_boot_device *device;
	size_t index;
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

	/* Finds the UART the console will read once input starts. */
	probe_uart();

	/* Starts the PCI host and binds the devices behind it. */
	probe_pci();

	/*
	 * Selects the partition scheme that tells GPT from MBR: the ramdisk's
	 * image is an MBR disk, the native disk image a GPT disk.
	 */
	partition_set_scheme(&drv_partition_scheme_pcat_auto);

	/* Starts the disk registry empty, before any disk is published. */
	disk_registry_reset();

	/* Publishes the ramdisk; without one the board has no boot device. */
	error = publish_ramdisk();
	if (error != 0)
		return 0;

	/* Describes the ramdisk as the boot device the handoff names. */
	device = &devices[0];
	device->device_class = KERN_DEV_SD;
	device->display_index = 0;
	device->bios_id = RISCV_VIRT_BOOT_ID;
	device->flags = KERN_DEV_PRESENT;
	if (handoff->boot_bios_id == RISCV_VIRT_BOOT_ID)
		device->flags |= KERN_DEV_BOOT_ORIGIN;
	device->sector_size = RISCV_VIRT_BLOCK_SIZE;
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
 * Finishes device discovery once interrupts are enabled: looks behind the
 * PCI host's USB root ports and publishes the NVMe namespaces.
 */
void
kern_platform_refresh_devices(
	const struct kern_boot_device *devices,
	size_t count)
{
	UNUSED_PARAMETER(devices);
	UNUSED_PARAMETER(count);

	/* Looks behind the USB root ports and publishes the NVMe namespaces. */
	drv_riscv_virt_pci_refresh();
}

/*
 * Starts typing what the UART receives into the console.
 */
int
kern_platform_input_init(
	void)
{
	int error;

	/* Lets USB keyboards type into the console. */
	drv_riscv_virt_pci_input_ready();

	/* A board without a UART in its tree has no input. */
	if (!uart_present)
		return 0;

	/* Has the UART's receive interrupt feed the console. */
	error = drv_ns16550_start_input(&uart, drv_serial_console_receive);
	if (error != 0) {
		kern_logf("riscv-virt: serial input unavailable (%d)\n", error);
		return error;
	}

	/* Succeeded: the serial line types into the console. */
	return 0;
}

/*
 * Resolves the published boot device to the ramdisk.
 */
struct disk *
kern_platform_block_device(
	const struct kern_boot_device *device)
{
	/* Only the ramdisk's device has a disk. */
	if (device == NULL || device->device_class != KERN_DEV_SD)
		return NULL;

	/* Succeeded: the ramdisk, or none when the firmware loaded none. */
	return ramdisk;
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
 * Turns the power off through the firmware, halting if that returns.
 */
int
kern_platform_poweroff(
	void)
{
	/* The hardware layer's power-off does not return when the firmware can turn the power off. */
	hal_poweroff();
	for (;;)
		hal_halt();
}

/*
 * Reboots the machine through the firmware.
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
 * Finds the UART the device tree describes, for the console's input.
 *
 * A tree that cannot be read, or one without a UART, leaves the console
 * output only; the boot goes on.
 */
static void
probe_uart(
	void)
{
	const uint64_t *fdt_range;
	struct drv_fdt fdt;
	const void *blob;
	int error;

	/* Finds the device tree's physical range the hardware layer kept. */
	fdt_range = hal_get_arch_handoff("riscv64.fdt");
	if (fdt_range == NULL)
		return;

	/* Finds the tree in the direct map. */
	blob = kern_pmem_to_kernel((hal_physaddr_t)fdt_range[0]);
	if (blob == NULL)
		return;

	/* Refuses a tree whose header does not check out. */
	error = drv_fdt_open(&fdt, blob, RISCV_VIRT_FDT_READ_LIMIT);
	if (error != 0) {
		kern_logf("riscv-virt: unreadable device tree (%d)\n", error);
		return;
	}

	/* Maps the UART the tree describes. */
	error = drv_ns16550_probe(&fdt, &uart);
	if (error != 0) {
		kern_logf("riscv-virt: no serial port (%d)\n", error);
		return;
	}

	/* Keeps the UART for the input start and reports where it is. */
	uart_present = 1;
	kern_logf("riscv-virt: serial port at %llx, interrupt %d\n",
		  (unsigned long long)uart.physical,
		  uart.irq);
}

/*
 * Publishes the initial ramdisk the firmware loaded as a disk.
 *
 * Returns 0, ENOENT when the firmware loaded none, or another errno.
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
	initrd_range = hal_get_arch_handoff("riscv64.initrd");
	if (initrd_range == NULL)
		return ENOENT;
	physical = initrd_range[0];
	size = initrd_range[1];

	/* Uses whole blocks only; a tail shorter than a block holds no data of the image. */
	size -= size % RISCV_VIRT_BLOCK_SIZE;
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
		kern_logf("riscv-virt: ramdisk unavailable (%d)\n", error);
		return error;
	}

	/* Reports the ramdisk's size and place. */
	kern_logf("riscv-virt: ramdisk ram0 %llu KiB at %llx\n",
		  (unsigned long long)(size / 1024U),
		  (unsigned long long)physical);

	/* Succeeded: the ramdisk is a disk. */
	return 0;
}

/*
 * Starts the PCI host the device tree describes.
 *
 * A tree that cannot be read, or one without a host, leaves the board
 * without PCI devices; the boot goes on.
 */
static void
probe_pci(
	void)
{
	const uint64_t *fdt_range;
	struct drv_fdt fdt;
	const void *blob;
	int error;

	/* Finds the device tree's physical range the hardware layer kept. */
	fdt_range = hal_get_arch_handoff("riscv64.fdt");
	if (fdt_range == NULL)
		return;

	/* Finds the tree in the direct map. */
	blob = kern_pmem_to_kernel((hal_physaddr_t)fdt_range[0]);
	if (blob == NULL)
		return;

	/* Refuses a tree whose header does not check out. */
	error = drv_fdt_open(&fdt, blob, RISCV_VIRT_FDT_READ_LIMIT);
	if (error != 0)
		return;

	/* Starts the host; a board without one is not an error. */
	error = drv_riscv_virt_pci_init(&fdt);
	if (error != 0 && error != ENODEV)
		kern_logf("riscv-virt: PCI unavailable (%d)\n", error);
}
