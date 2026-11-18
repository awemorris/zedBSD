/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000 native platform binding.
 *
 * The HAL names the internal SCSI controller's registers and the host
 * adapter's ID; every attached SPC disk is published as a boot device, with
 * the unit the IPL booted from marked as the boot origin.  The console is
 * the HAL's text screen.
 */

#include <uapi/errno.h>
#include <hal/hal.h>
#include <drivers/disklabel/disklabel.h>
#include <kern/disk.h>
#include <kern/platform.h>
#include <kern/partition.h>
#include "drivers/platform/x68k/x68k-console.h"
#include "drivers/platform/x68k/x68k-mb89352.h"
#include "drivers/platform/x68k/x68k-spc-disk.h"
#include <kern/kcrt.h>

/* The controller's registers lie every second byte, on the odd byte lane. */
#define X68K_SPC_REGISTER_STRIDE	2U

/* How long a controller wait polls before it gives up. */
#define X68K_SPC_POLL_LIMIT		10000000U

/*
 * The kernel address of the internal SCSI controller's first register,
 * from the HAL's "x68k.spc.registers"; set once by kern_platform_init().
 */
static uintptr_t spc_registers;

static uint8_t spc_read_register(void *cookie, unsigned reg);
static void spc_write_register(void *cookie, unsigned reg, uint8_t value);
static void spc_relax(void *cookie);

/*
 * Publishes the SCSI disks found on the X68000 bus as boot devices.
 *
 * Returns the number of devices published, which is zero for a handoff the
 * platform does not understand or a bus without disks.
 */
size_t
kern_platform_init(
	const struct kern_boot_handoff *common,
	struct kern_boot_device *devices,
	size_t capacity)
{
	const uint64_t *registers_object;
	const uint64_t *initiator_object;
	struct x68k_spc_bus bus;
	struct kern_boot_device *device;
	struct disk *disk;
	unsigned initiator;
	unsigned target;
	size_t count;
	int found;

	/* Publishes the console before anything can fail, so the failure is seen. */
	drv_x68k_console_init();

	/* Rejects a missing device table or a handoff that is not the X68000 loader's. */
	count = 0;
	if (devices == NULL || capacity == 0 || common == NULL)
		return 0;
	if (common->magic != KERN_HANDOFF_MAGIC || common->version != KERN_HANDOFF_VERSION_X68K)
		return 0;
	if (common->size < sizeof(*common) || common->boot_bios_id > 6U)
		return 0;

	/* Finds the SCSI controller's registers and the host adapter's ID from the HAL. */
	registers_object = hal_get_arch_handoff("x68k.spc.registers");
	if (registers_object == NULL)
		return 0;
	initiator_object = hal_get_arch_handoff("x68k.scsi.initiator");
	if (initiator_object == NULL || *initiator_object > 7U)
		return 0;
	spc_registers = (uintptr_t)*registers_object;
	initiator = (unsigned)*initiator_object;

	/* Describes the controller to the disk driver. */
	bus.cookie = NULL;
	bus.read = spc_read_register;
	bus.write = spc_write_register;
	bus.relax = spc_relax;
	bus.poll_limit = X68K_SPC_POLL_LIMIT;

	/* Selects the X68000 partition scheme and probes the SCSI bus. */
	partition_set_scheme(&drv_partition_scheme_x68k);
	disk_registry_reset();
	found = drv_x68k_spc_disk_init(&bus, initiator, common->boot_bios_id);
	if (found == 0)
		return 0;

	/* Publishes every attached target that fits the table. */
	for (target = 0; target < 7U && count < capacity; target++) {
		/* Skips a target with no disk. */
		disk = drv_x68k_spc_disk_target(target);
		if (disk == NULL)
			continue;

		/* Describes one present SCSI target for the boot record. */
		device = &devices[count];
		kern_memset(device, 0, sizeof(*device));
		device->device_class = KERN_DEV_SCSI;
		device->display_index = (uint8_t)count;
		device->bios_id = (uint8_t)target;
		device->flags = KERN_DEV_PRESENT;
		if (target == common->boot_bios_id)
			device->flags |= KERN_DEV_BOOT_ORIGIN;
		device->sector_size = X68K_SCSI_BLOCK_SIZE;
		device->controller_location = (uint8_t)target;
		count++;
	}

	/* Reports the number of published devices. */
	return count;
}

/*
 * Refreshes the published devices; the X68000 has no hot-pluggable devices.
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
 * Initializes platform input devices; the X68000 has none here.
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
	const struct kern_boot_device *device)
{
	struct disk *disk;

	/* Only a SCSI boot device has a disk. */
	if (device == NULL || device->device_class != KERN_DEV_SCSI)
		return NULL;

	/* Resolves the SPC target that the device names. */
	disk = drv_x68k_spc_disk_target(device->bios_id);

	/* Reports the disk. */
	return disk;
}

/*
 * Writes a debug string to the console.
 */
void
kern_platform_debug_write(
	const char *text)
{
	/* Ignores a missing string. */
	if (text == NULL)
		return;

	/* Emits every byte through the HAL. */
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

/* Reads one register of the SCSI controller. */
static uint8_t
spc_read_register(
	void *cookie,
	unsigned reg)
{
	uint8_t value;

	(void)cookie;

	/* Loads the register on its byte lane. */
	value = hal_mmio_read8((const volatile void *)(spc_registers + (uintptr_t)reg * X68K_SPC_REGISTER_STRIDE));

	/* Succeeded: the register. */
	return value;
}

/* Writes one register of the SCSI controller. */
static void
spc_write_register(
	void *cookie,
	unsigned reg,
	uint8_t value)
{
	(void)cookie;

	/* Stores the register on its byte lane. */
	hal_mmio_write8((volatile void *)(spc_registers + (uintptr_t)reg * X68K_SPC_REGISTER_STRIDE), value);
}

/* Spends a moment between two polls of the controller. */
static void
spc_relax(
	void *cookie)
{
	(void)cookie;

	/* One instruction is enough: the poll limit bounds the wait. */
	__asm__ volatile("nop");
}
