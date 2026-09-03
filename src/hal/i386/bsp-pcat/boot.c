/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT Multiboot handoff and memory-discovery implementation.
 */

#include <hal/hal.h>
#include <kern/boot.h>

#include "../../x86/boot-parameters.h"
#include "../bsp.h"
#include "../defs.h"
#include "../i386.h"
#include "../multiboot.h"

#define VGA_FONT_HANDOFF 0x00007000U
#define VGA_FONT_MAGIC 0x3854465aU
#define PCAT_BOOT_FONT_GLYPHS 128U
#define PCAT_BOOT_FONT_HEIGHT 16U

struct vga_font_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t height;
	uint16_t glyphs;
	uint16_t reserved[3];
	uint8_t data[PCAT_BOOT_FONT_GLYPHS][PCAT_BOOT_FONT_HEIGHT];
} __attribute__((packed));

static const struct multiboot_info *mbi;
static struct boot_handoff handoff;
static struct boot_device boot_device;
static uint32_t total_memory;
static uint8_t root_bios_id;
static uint8_t root_partition;
static int boot_info_valid;
static uint8_t boot_font[PCAT_BOOT_FONT_GLYPHS][PCAT_BOOT_FONT_HEIGHT];
static int boot_font_valid;
static char boot_command_line[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE];

static int handoff_name_is(const char *name, const char *expected);

/*
 * Parses the PC/AT Multiboot information and optional VGA font handoff.
 */
void
bsp_boot_init(
	const void *raw_boot_info)
{
	const struct vga_font_handoff *font;
	uint32_t upper;
	uint32_t address;
	uint8_t drive;
	uint8_t partition;
	size_t available;
	int result;

	/* Captures a valid loader-supplied VGA font image when present. */
	font = (const struct vga_font_handoff *)(uintptr_t)VGA_FONT_HANDOFF;
	boot_font_valid = font->magic == VGA_FONT_MAGIC && font->version == 1 &&
	    font->height == PCAT_BOOT_FONT_HEIGHT &&
	    font->glyphs == PCAT_BOOT_FONT_GLYPHS;

	/* Copies the loader font only after validating every fixed field. */
	if (boot_font_valid)
		hal_memcpy(boot_font, font->data, sizeof(boot_font));

	/* Requires a Multiboot memory-size record before parsing other fields. */
	mbi = raw_boot_info;
	boot_info_valid = mbi != 0 && (mbi->flags & MBINFO_FLAG_MEMORY) != 0;

	/* Stops parsing when the required Multiboot memory record is absent. */
	if (!boot_info_valid)
		return;

	/* Clips detected RAM to the i386 direct-map capacity. */
	upper = mbi->mem_upper;

	/* Clips upper memory to the 127-MiB direct-map remainder. */
	if (upper > 127U * 1024U)
		upper = 127U * 1024U;
	total_memory = 0x100000U + upper * 1024U;

	/* Selects the default first IDE disk and first MBR partition. */
	root_bios_id = 0x80U;
	root_partition = 1U;

	/* Applies a valid loader-selected BIOS disk and partition. */
	if ((mbi->flags & MBINFO_FLAG_BOOT_DEVICE) != 0U) {
		drive = (uint8_t)(mbi->boot_device >> 24);
		partition = (uint8_t)(mbi->boot_device >> 16);

		/* Accepts only one of the supported BIOS hard-disk identifiers. */
		if (drive >= 0x80U && drive <= 0x83U)
			root_bios_id = drive;

		/* Converts a valid primary partition to kernel numbering. */
		if (partition < 4U)
			root_partition = (uint8_t)(partition + 1U);
	}

	/* Copies a bounded command line or publishes an empty parameter block. */
	if ((mbi->flags & MBINFO_FLAG_CMDLINE) != 0U && mbi->cmdline != 0) {
		address = mbi->cmdline;

		/* Rejects a command-line address beyond detected RAM. */
		if (address >= total_memory) {
			boot_info_valid = 0;
			return;
		}

		/* Clips the source extent to RAM and local parameter storage. */
		available = (size_t)(total_memory - address);

		/* Clips the readable bytes to local command-line storage. */
		if (available > ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE)
			available = ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE;
		result = x86_boot_parameters_copy(
			boot_command_line,
			(const char *)(uintptr_t)address,
			available);

		/* Invalidates boot state when the bounded copy rejects its input. */
		if (result != X86_BOOT_PARAMETERS_OK) {
			boot_info_valid = 0;
			return;
		}
	} else {
		result = x86_boot_parameters_copy(boot_command_line, NULL, 0);

		/* Invalidates boot state if empty-parameter publication fails. */
		if (result != X86_BOOT_PARAMETERS_OK) {
			boot_info_valid = 0;
			return;
		}
	}
}

/*
 * Returns one PC/AT architecture-specific boot handoff object.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	/* Returns the copied boot command line under its stable name. */
	if (handoff_name_is(name, "boot.command-line"))
		return boot_command_line;

	/* Rejects the font name without a validated loader font. */
	if (!boot_font_valid)
		return NULL;

	/* Requires the stable VGA-font handoff name. */
	if (!handoff_name_is(name, "pcat.boot-font"))
		return NULL;

	/* Returns the copied VGA font image. */
	return boot_font;
}

/*
 * Reports the PC/AT physical-memory size discovered at boot.
 */
uint32_t
bsp_mem_probe(
	void)
{
	/* Returns the clipped Multiboot memory size. */
	return total_memory;
}

/*
 * Builds the generic kernel handoff from PC/AT Multiboot state.
 */
const void *
bsp_kernel_handoff(
	const void *raw_boot_info)
{
	unsigned i;

	UNUSED_PARAMETER(raw_boot_info);

	/* Requires valid Multiboot state and the kernel's minimum RAM. */
	if (!boot_info_valid || total_memory < 0x00400000U)
		HAL_FATAL("invalid PC/AT Multiboot information");

	/* Describes the selected BIOS IDE boot device. */
	boot_device.device_class = ZEDBSD_DEV_IDE;
	boot_device.display_index = (uint8_t)(root_bios_id - 0x80U);
	boot_device.bios_id = root_bios_id;
	boot_device.flags = ZEDBSD_DEV_PRESENT | ZEDBSD_DEV_BOOT_ORIGIN;
	boot_device.sector_size = 512U;
	boot_device.cylinders = 0;
	boot_device.heads = 0;
	boot_device.sectors = 0;
	boot_device.controller_location = (uint8_t)(root_bios_id - 0x80U);

	/* Clears every reserved boot-device byte. */
	for (i = 0; i < sizeof(boot_device.reserved); i++)
		boot_device.reserved[i] = 0;

	/* Publishes the generic handoff around the single boot device. */
	handoff.magic = ZEDBSD_HANDOFF_MAGIC;
	handoff.version = ZEDBSD_HANDOFF_VERSION_MULTIBOOT;
	handoff.size = sizeof(handoff);
	handoff.device_count = 1;
	handoff.boot_bios_id = root_bios_id;
	handoff.boot_partition_scheme = ZEDBSD_PARTITION_SCHEME_MBR;
	handoff.boot_partition_index = root_partition;
	handoff.device_table = (uint32_t)(uintptr_t)&boot_device;
	handoff.bios_gateway = 0;
	handoff.boot_partition_lba = ZEDBSD_BOOT_PARTITION_LBA_UNKNOWN;

	/* Reports the selected root and optional loader identity. */
	hal_printf(
		"boot: Multiboot root BIOS=%02X MBR partition=%u\n",
		root_bios_id,
		root_partition);

	/* Reports the optional loader identity only when supplied. */
	if ((mbi->flags & MBINFO_FLAG_LOADER_NAME) != 0U &&
	    mbi->boot_loader_name != 0) {
		hal_printf(
			"boot: Multiboot loader %s\n",
			(const char *)(uintptr_t)mbi->boot_loader_name);
	}

	/* Returns the completed kernel handoff. */
	return &handoff;
}

/* Tests one optional handoff object's stable name. */
static int
handoff_name_is(
	const char *name,
	const char *expected)
{
	/* Rejects a missing requested name. */
	if (name == NULL)
		return 0;

	/* Compares both strings through the first mismatch or terminator. */
	while (*name != '\0' && *name == *expected) {
		name++;
		expected++;
	}

	/* Reports two strings ending at the same byte. */
	if (*name == *expected)
		return 1;

	/* Reports different handoff names. */
	return 0;
}
