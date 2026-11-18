/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A1v: reading the video BIOS image and its data tables.
 *
 * The image comes from the ACPI table in which the firmware leaves the
 * images of its display devices, or, when that table has none for this
 * GPU, from the board's flash through the ROM address and data registers.
 * The stage keeps the image for the later stages (memory training, the
 * SMU's power table, the display's command routines), reads what its
 * tables say, and plans the VRAM layout again with the ranges the image
 * keeps.  It runs on the bring-up thread once the root file system is up,
 * which is after ACPI found its tables.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/mount.h>
#include <kern/sched.h>
#include <uapi/amdrdna.h>
#include <uapi/errno.h>

#if CONFIG_DRIVER_ACPI
#include <drivers/acpi/acpi.h>
#endif

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/* How long the stage waits for the root file system, and the step of the wait. */
#define VBIOS_ROOT_WAIT_TICKS		kern_ms_to_ticks(60000U)
#define VBIOS_ROOT_POLL_TICKS		kern_ms_to_ticks(100U)

/*
 * The bytes read from the flash before the image's length is known, and
 * the largest image the stage takes from the flash.
 */
#define VBIOS_ROM_HEAD_BYTES		512U
#define VBIOS_ROM_MOST_BYTES		(256U * 1024U)

/*
 * The capability bit that asks for the second, long stage of memory
 * training, and the boot database's bit that allows it.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/atomfirmware.h
 * (ATOM_FIRMWARE_CAP_ENABLE_2STAGE_BIST_TRAINING) and
 * drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h
 * (BOOT_CFG_FEATURE_TWO_STAGE_DRAM_TRAINING).
 */
#define VBIOS_CAPABILITY_LONG_TRAINING	0x00000400U
#define VBIOS_DATABASE_LONG_TRAINING	0x00000002U

/* The ACPI table that carries the images of the display devices. */
#define VBIOS_ACPI_SIGNATURE		"VFCT"

static void vbios_wait_root(void);
static int vbios_from_acpi(struct amdrdna_device *device);
static int vbios_from_rom(struct amdrdna_device *device);
static void vbios_rom_read(struct amdrdna_device *device, uint32_t address_index, uint32_t data_index, uint8_t *buffer, size_t length);
static const char *vbios_source_name(uint32_t source);

/*
 * Runs stage A1v: finds, keeps and reads the video BIOS image.
 *
 * Returns 0 with the image kept and read, ENOENT when neither the ACPI
 * table nor the flash holds one, or EINVAL for an image the stage cannot
 * read.  A failure leaves the GPU as it was; the later stages then skip
 * what needs the image.
 */
int
amdrdna_a1v_vbios(
	struct amdrdna_device *device)
{
	struct amdrdna_vbios_info *info;
	uint32_t scratch_index;
	int error;

	/* Says the stage starts. */
	amdrdna_mark("A1v begin");

	/* Waits until ACPI has found its tables (they are found before the root is mounted). */
	vbios_wait_root();

	/* Takes the image from the ACPI table, or else from the flash. */
	error = vbios_from_acpi(device);
	if (error != 0)
		error = vbios_from_rom(device);
	if (error != 0) {
		amdrdna_mark("A1v fail: no vbios (%d)", error);
		return ENOENT;
	}

	/* Reads the tables the image carries. */
	info = &device->vbios_info;
	error = amdrdna_vbios_parse(device->vbios, device->vbios_bytes, info);
	if (error != 0) {
		amdrdna_mark("A1v fail: vbios from %s unreadable (%d)", vbios_source_name(device->vbios_source), error);
		amdrdna_vbios_release(device);
		return error;
	}

	/* The image's first scratch register should be the one the post check read. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_SCRATCH, AMDRDNA_NBIO_REG_BIOS_SCRATCH_FIRST, &scratch_index);
	if (error == 0 &&
	    info->scratch_first != 0U &&
	    info->scratch_first != scratch_index)
		amdrdna_mark("A1v note: vbios scratch 0x%x, driver scratch 0x%x", (unsigned)info->scratch_first, (unsigned)scratch_index);

	/* Plans the VRAM again with the ranges the image keeps. */
	error = amdrdna_plan_layout(device);
	if (error != 0) {
		amdrdna_mark("A1v fail: VRAM layout not planned (%d)", error);
		return error;
	}

	/* Marks what the image says. */
	amdrdna_mark("A1v ok vbios=%s %uB fwinfo=%u.%u caps=0x%x train=%u sclk=%uMHz mclk=%uMHz pptid=%u scratch=0x%x fwtop=%uK",
	    vbios_source_name(device->vbios_source),
	    (unsigned)device->vbios_bytes,
	    (unsigned)info->firmware_format,
	    (unsigned)info->firmware_content,
	    (unsigned)info->capability_bits,
	    amdrdna_training_allowed(device) ? 1U : 0U,
	    (unsigned)(info->boot_engine_clock_10khz / 100U),
	    (unsigned)(info->boot_memory_clock_10khz / 100U),
	    (unsigned)info->power_table_id,
	    (unsigned)info->scratch_first,
	    (unsigned)info->reserved_top_kib);
	amdrdna_mark("A1v tables fwrsv=%u.%u fw=%uK@%uK drv=%uK@%uK pp=%u.%u rev%u %uB shutdown=%uC dpm=%u.%u",
	    (unsigned)info->reserve_format,
	    (unsigned)info->reserve_content,
	    (unsigned)info->firmware_kib,
	    (unsigned)info->firmware_start_kib,
	    (unsigned)info->driver_kib,
	    (unsigned)info->driver_start_kib,
	    (unsigned)info->power_format,
	    (unsigned)info->power_content,
	    (unsigned)info->power_revision,
	    (unsigned)info->power_bytes,
	    (unsigned)info->shutdown_temperature,
	    (unsigned)info->board_format,
	    (unsigned)info->board_content);
	amdrdna_mark("A1v cmds post=0x%x enc=0x%x pclk=0x%x pg=0x%x crtc=0x%x dceclk=0x%x tx=0x%x aux=0x%x",
	    (unsigned)info->commands[AMDRDNA_VBIOS_COMMAND_POST].offset,
	    (unsigned)info->commands[AMDRDNA_VBIOS_COMMAND_ENCODER].offset,
	    (unsigned)info->commands[AMDRDNA_VBIOS_COMMAND_PIXEL_CLOCK].offset,
	    (unsigned)info->commands[AMDRDNA_VBIOS_COMMAND_PIPE_POWER].offset,
	    (unsigned)info->commands[AMDRDNA_VBIOS_COMMAND_TIMING_ENABLE].offset,
	    (unsigned)info->commands[AMDRDNA_VBIOS_COMMAND_DISPLAY_CLOCK].offset,
	    (unsigned)info->commands[AMDRDNA_VBIOS_COMMAND_TRANSMITTER].offset,
	    (unsigned)info->commands[AMDRDNA_VBIOS_COMMAND_AUX].offset);

	/* Succeeded: the image is kept and read. */
	return 0;
}

/*
 * Frees the kept video BIOS image.
 */
void
amdrdna_vbios_release(
	struct amdrdna_device *device)
{
	/* Frees the copy and forgets where it came from. */
	if (device->vbios != NULL)
		kern_free(device->vbios);
	device->vbios = NULL;
	device->vbios_bytes = 0U;
	device->vbios_source = AMDRDNA_VBIOS_SOURCE_NONE;
}

/*
 * Reports whether the board asks for the long stage of memory training:
 * the image's capability bit, and the boot database's bit when there is a
 * database.
 */
bool
amdrdna_training_allowed(
	const struct amdrdna_device *device)
{
	/* Without the image's capability bit the board does not train. */
	if ((device->vbios_info.capability_bits & VBIOS_CAPABILITY_LONG_TRAINING) == 0U)
		return false;

	/* Without a database, or without its configuration, the capability decides. */
	if (!device->boot_database.present)
		return true;
	if (!device->boot_database.config_read)
		return true;

	/* With one, the database must allow it too. */
	if ((device->boot_database.config_bits & VBIOS_DATABASE_LONG_TRAINING) == 0U)
		return false;

	/* Succeeded: both allow the long training. */
	return true;
}

/* Waits, for a bounded time, until the kernel has a live root file system. */
static void
vbios_wait_root(void)
{
	struct mount *root;
	uint64_t deadline;
	uint64_t now;

	/* Sleeps in short steps until the root is live or the bound is spent. */
	deadline = sched_ticks() + VBIOS_ROOT_WAIT_TICKS;
	for (;;) {
		/* Stops as soon as the kernel has a live root. */
		root = mount_root_get_ref();
		if (root != NULL) {
			mount_release(root);
			return;
		}

		/* The bound is spent without a root; the stage goes on without it. */
		now = sched_ticks();
		if (now >= deadline)
			return;

		/* Sleeps one step; the bring-up thread may sleep. */
		sched_sleep(now + VBIOS_ROOT_POLL_TICKS);
	}
}

/*
 * Takes this GPU's image from the ACPI table of display images.
 *
 * The lookup takes the ACPI interpreter's lock, so it runs only here, on
 * the bring-up thread, which may sleep.
 */
static int
vbios_from_acpi(
	struct amdrdna_device *device)
{
#if CONFIG_DRIVER_ACPI
	struct amdrdna_vbios_owner owner;
	const uint8_t *table;
	size_t table_length;
	size_t image_offset;
	size_t image_length;
	uint8_t *copy;
	int error;

	/* Borrows the ACPI table's bytes. */
	error = drv_acpi_table_lookup(VBIOS_ACPI_SIGNATURE, &table, &table_length);
	if (error != 0) {
		amdrdna_mark("A1v no ACPI image table (%d)", error);
		return error;
	}

	/* Finds the record of this function. */
	owner.bus = device->address.bus;
	owner.device = device->address.device;
	owner.function = device->address.function;
	owner.vendor = AMDRDNA_PCI_VENDOR;
	owner.product = device->product;
	error = amdrdna_vbios_find_in_acpi(table, table_length, &owner, &image_offset, &image_length);
	if (error != 0) {
		amdrdna_mark("A1v ACPI image table has no image for this GPU (%d)", error);
		return error;
	}

	/* The image must be one the parser reads. */
	error = amdrdna_vbios_check(table + image_offset, image_length);
	if (error != 0) {
		amdrdna_mark("A1v ACPI image is not a video BIOS (%d)", error);
		return error;
	}

	/* Keeps a copy; the ACPI bytes are only borrowed. */
	copy = kern_malloc(image_length);
	if (copy == NULL)
		return ENOMEM;

	/* Copies the image and records where it came from. */
	kern_memcpy(copy, table + image_offset, image_length);
	device->vbios = copy;
	device->vbios_bytes = (uint32_t)image_length;
	device->vbios_source = AMDRDNA_VBIOS_SOURCE_ACPI;

	/* Succeeded: the image is kept. */
	return 0;
#else
	UNUSED_PARAMETER(device);

	/* A kernel without ACPI has no image table. */
	return ENODEV;
#endif
}

/*
 * Takes the image from the board's flash through the ROM registers.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c (amdgpu_read_bios_from_rom,
 * amdgpu_soc15_read_bios_from_rom).
 */
static int
vbios_from_rom(
	struct amdrdna_device *device)
{
	uint8_t head[VBIOS_ROM_HEAD_BYTES];
	uint32_t address_index;
	uint32_t data_index;
	uint32_t declared;
	uint8_t *copy;
	int error;

	/* The ROM's address and data registers. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_SMUIO, AMDRDNA_SMUIO_WINDOW_MAIN, AMDRDNA_SMUIO_REG_ROM_ADDRESS, &address_index);
	if (error != 0)
		return error;

	/* The data register the dwords are read through. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_SMUIO, AMDRDNA_SMUIO_WINDOW_MAIN, AMDRDNA_SMUIO_REG_ROM_DATA, &data_index);
	if (error != 0)
		return error;

	/* Reads the image's start, which carries the option-ROM mark and the length. */
	vbios_rom_read(device, address_index, data_index, head, sizeof(head));
	declared = amdrdna_vbios_declared_length(head);
	if (declared == 0U) {
		amdrdna_mark("A1v flash holds no image (%02x %02x)", (unsigned)head[0], (unsigned)head[1]);
		return ENOENT;
	}

	/* The declared length must be one the stage takes. */
	if (declared < VBIOS_ROM_HEAD_BYTES || declared > VBIOS_ROM_MOST_BYTES) {
		amdrdna_mark("A1v flash image of %u bytes refused", (unsigned)declared);
		return EINVAL;
	}

	/* Reads the whole image. */
	copy = kern_malloc(declared);
	if (copy == NULL)
		return ENOMEM;

	/* Reads the whole image into the buffer. */
	vbios_rom_read(device, address_index, data_index, copy, declared);

	/* The image must be one the parser reads. */
	error = amdrdna_vbios_check(copy, declared);
	if (error != 0) {
		kern_free(copy);
		amdrdna_mark("A1v flash image is not a video BIOS (%d)", error);
		return error;
	}

	/* Keeps the image. */
	device->vbios = copy;
	device->vbios_bytes = declared;
	device->vbios_source = AMDRDNA_VBIOS_SOURCE_ROM;

	/* Succeeded: the image is kept. */
	return 0;
}

/* Reads bytes from the start of the flash: the address register, then each dword through the data register. */
static void
vbios_rom_read(
	struct amdrdna_device *device,
	uint32_t address_index,
	uint32_t data_index,
	uint8_t *buffer,
	size_t length)
{
	uint32_t word;
	size_t at;

	/* Points the ROM at its start; each read of the data register moves it on by a dword. */
	amdrdna_write(device, address_index, 0U);

	/* Reads every dword into the buffer, low byte first. */
	for (at = 0U; at < length; at += 4U) {
		word = amdrdna_read(device, data_index);
		buffer[at] = (uint8_t)word;
		buffer[at + 1U] = (uint8_t)(word >> 8);
		buffer[at + 2U] = (uint8_t)(word >> 16);
		buffer[at + 3U] = (uint8_t)(word >> 24);
	}
}

/* Names where the image came from, for the log. */
static const char *
vbios_source_name(
	uint32_t source)
{
	/* Each source the stage knows. */
	switch (source) {
	case AMDRDNA_VBIOS_SOURCE_ACPI:
		return "VFCT";
	case AMDRDNA_VBIOS_SOURCE_ROM:
		return "ROM";
	default:
		break;
	}

	/* No image. */
	return "none";
}
