/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the Raspberry Pi HAL keeps from the boot and hands to the kernel:
 * the device tree's record, the kernel's handoff, and the named objects of
 * hal_get_arch_handoff().
 *
 * The names start with the board's ("rpi4.fdt", "rpi5.fdt"), which
 * board.h gives.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "../bsp.h"
#include "../defs.h"
#include "board.h"

/* How many values of the boot framebuffer the kernel reads by name. */
#define RPI_FRAMEBUFFER_VALUE_COUNT 6U

/*
 * The device tree's record and the tree's physical address.
 *
 * Filled once by rpi_boot_set_info() on the boot CPU before any other CPU
 * starts; afterwards only the framebuffer's reservation is added to it, on
 * the same CPU.  Every other reader only reads it.
 */
static struct rpi_fdt_info boot_info;
static uintptr_t boot_fdt;

/*
 * The handoff the kernel's entry reads: the common record alone, a single
 * SD boot device on the first partition of an MBR disk.  Filled once with
 * the boot record; what is particular to the board goes by name.
 */
static struct kern_boot_handoff kernel_handoff;

/*
 * The firmware's command line (/chosen/bootargs, from cmdline.txt), copied
 * out of the device tree while the tree is known to be intact, and handed
 * to the kernel as "boot.command-line".  Filled once at boot; the first
 * byte is zero when the firmware passed no line.
 */
static char boot_command_line[KERN_BOOT_PARAMETERS_STORAGE_SIZE];

/*
 * The named objects of hal_get_arch_handoff(): the device tree's physical
 * address ("<board>.fdt") and the SD controller's ("<board>.sdhci"), each
 * a 64-bit word.  Filled once at boot and not changed afterwards; zero
 * means the firmware named none, and the name then reports no object.
 */
static uint64_t handoff_fdt_phys;
static uint64_t handoff_sdhci_phys;

#ifdef ARM64_BOARD_SDHCI_CLOCK_HZ
/*
 * The SD controller's base clock ("<board>.sdhci.clock"), a 64-bit word in
 * hertz, on a board whose controller runs from a clock its capabilities do
 * not report (board.h).  Never changed.
 */
static const uint64_t handoff_sdhci_clock_hz = ARM64_BOARD_SDHCI_CLOCK_HZ;
#endif

/*
 * The names under which the kernel reads the boot framebuffer, one per
 * value, in the order of framebuffer_values[].
 */
static const char *const framebuffer_value_names[RPI_FRAMEBUFFER_VALUE_COUNT] = {
	ARM64_BOARD_HANDOFF ".framebuffer.phys",
	ARM64_BOARD_HANDOFF ".framebuffer.size",
	ARM64_BOARD_HANDOFF ".framebuffer.width",
	ARM64_BOARD_HANDOFF ".framebuffer.height",
	ARM64_BOARD_HANDOFF ".framebuffer.pitch",
	ARM64_BOARD_HANDOFF ".framebuffer.format",
};

/*
 * The firmware's framebuffer as the kernel reads it, each value a 64-bit
 * word: the physical base, the size in bytes, the width and the height in
 * pixels, the pitch in bytes and the firmware's pixel order (0 blue first,
 * 1 red first).  Filled once when the firmware allocates the framebuffer
 * and not changed afterwards; the names report no object while the size
 * is zero.
 */
static uint64_t framebuffer_values[RPI_FRAMEBUFFER_VALUE_COUNT];

static void copy_command_line(const struct rpi_fdt_info *info, uintptr_t fdt_phys);
static int handoff_name_is(const char *name, const char *wanted);

/*
 * Keeps the device tree's record and prepares the kernel's handoff.
 */
void
rpi_boot_set_info(
	const struct rpi_fdt_info *info,
	uintptr_t fdt_phys)
{
	/* Keeps the record and where the tree is, for the rest of the HAL. */
	boot_info = *info;
	boot_fdt = fdt_phys;

	/* Describes one SD boot device, the first partition of an MBR disk. */
	hal_memset(&kernel_handoff, 0, sizeof(kernel_handoff));
	kernel_handoff.magic = KERN_HANDOFF_MAGIC;
	kernel_handoff.version = KERN_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.size = sizeof(kernel_handoff);
	kernel_handoff.device_count = 1;
	kernel_handoff.boot_bios_id = 0x80;
	kernel_handoff.boot_partition_scheme = KERN_PARTITION_SCHEME_MBR;
	kernel_handoff.boot_partition_index = 1;
	kernel_handoff.boot_partition_lba = KERN_BOOT_PARTITION_LBA_UNKNOWN;

	/* No framebuffer yet: its names report nothing until one is allocated. */
	hal_memset(framebuffer_values, 0, sizeof(framebuffer_values));

	/* Copies the command line while the tree is known to be intact. */
	copy_command_line(info, fdt_phys);

	/* Keeps the tree's and the SD controller's addresses for their names. */
	handoff_fdt_phys = fdt_phys;
	handoff_sdhci_phys = info->sdhci_base;
}

/*
 * Keeps the firmware's framebuffer for the kernel and out of the allocator.
 */
void
rpi_boot_set_framebuffer(
	uint64_t phys,
	uint64_t size,
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	uint32_t format)
{
	struct rpi_fdt_range *range;

	/* Publishes the values under their names, in the order of the names. */
	framebuffer_values[0] = phys;
	framebuffer_values[1] = size;
	framebuffer_values[2] = width;
	framebuffer_values[3] = height;
	framebuffer_values[4] = pitch;
	framebuffer_values[5] = format;

	/* An empty framebuffer reserves nothing, and a full table takes no more. */
	if (size == 0)
		return;
	if (boot_info.reserved_count >= RPI_FDT_MAX_RESERVED)
		return;

	/* Keeps the framebuffer's memory out of the page allocator. */
	range = &boot_info.reserved[boot_info.reserved_count];
	range->base = phys;
	range->size = size;
	boot_info.reserved_count++;
}

/*
 * Tells whether the range from base for size bytes lies wholly inside a
 * range the device tree marks no-map.
 */
int
rpi_boot_is_no_map(
	uint64_t base,
	uint64_t size)
{
	const struct rpi_fdt_range *range;
	unsigned index;

	/* Looks for a no-map range that holds the whole of it. */
	for (index = 0; index < boot_info.no_map_count; index++) {
		range = &boot_info.no_map[index];
		if (base >= range->base && base - range->base + size <= range->size)
			return 1;
	}

	/* No range holds it. */
	return 0;
}

/*
 * Reports the device tree's record kept at boot.
 */
const struct rpi_fdt_info *
rpi_boot_info(
	void)
{
	/* Succeeded: the record, which lives as long as the kernel. */
	return &boot_info;
}

/*
 * Reports the device tree's physical address.
 */
uintptr_t
rpi_boot_fdt_phys(
	void)
{
	/* Succeeded: the address the firmware passed. */
	return boot_fdt;
}

/*
 * Reports the handoff the kernel's entry reads.
 */
const void *
rpi_kernel_handoff(
	void)
{
	/* Succeeded: the handoff, filled by rpi_boot_set_info(). */
	return &kernel_handoff;
}

/*
 * Returns one named architecture handoff object.
 *
 * "boot.command-line" is the firmware's command line; "<board>.fdt" and
 * "<board>.sdhci" are the physical addresses of the device tree and of the
 * SD controller it names, each a 64-bit word; "<board>.sdhci.clock" is the
 * SD controller's base clock in hertz, a 64-bit word, on a board whose
 * board.h names one; "<board>.framebuffer.phys",
 * ".size", ".width", ".height", ".pitch" and ".format" are the firmware's
 * framebuffer, each a 64-bit word, all present or all absent.  <board> is
 * "rpi4" on a Pi 4 and "rpi5" on a Pi 5.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	unsigned index;
	int match;

	/* Exposes the firmware's command line when it passed one. */
	if (boot_command_line[0] != '\0') {
		match = handoff_name_is(name, "boot.command-line");
		if (match)
			return boot_command_line;
	}

	/* Exposes the device tree when the firmware passed one. */
	if (handoff_fdt_phys != 0) {
		match = handoff_name_is(name, ARM64_BOARD_HANDOFF ".fdt");
		if (match)
			return &handoff_fdt_phys;
	}

	/* Exposes the SD controller when the device tree names one. */
	if (handoff_sdhci_phys != 0) {
		match = handoff_name_is(name, ARM64_BOARD_HANDOFF ".sdhci");
		if (match)
			return &handoff_sdhci_phys;
	}

#ifdef ARM64_BOARD_SDHCI_CLOCK_HZ
	/* Exposes the SD controller's base clock on a board that names one. */
	if (handoff_sdhci_phys != 0) {
		match = handoff_name_is(name, ARM64_BOARD_HANDOFF ".sdhci.clock");
		if (match)
			return (void *)&handoff_sdhci_clock_hz;
	}
#endif

	/* Exposes the framebuffer's values when the firmware allocated one. */
	if (framebuffer_values[1] != 0) {
		/* Returns one of the framebuffer's values under its own name. */
		for (index = 0; index < RPI_FRAMEBUFFER_VALUE_COUNT; index++) {
			match = handoff_name_is(name, framebuffer_value_names[index]);
			if (match)
				return &framebuffer_values[index];
		}
	}

	/* Reports an unavailable or unknown architecture handoff. */
	return NULL;
}

/*
 * Copies the firmware's command line out of the device tree.
 *
 * A line longer than the kernel keeps is cut at the last separator that
 * fits, so no parameter is handed over in part.
 */
static void
copy_command_line(
	const struct rpi_fdt_info *info,
	uintptr_t fdt_phys)
{
	const char *line;
	size_t length;
	size_t limit;

	/* Starts with no line, which is what an absent one leaves. */
	boot_command_line[0] = '\0';

	/* Leaves the line empty when the firmware passed none. */
	if (info->bootargs_offset == 0U)
		return;
	if (info->bootargs_length == 0U)
		return;

	/* Measures the line up to its terminator, within the property. */
	line = (const char *)(ARM64_DIRECT_BASE + fdt_phys + info->bootargs_offset);
	length = 0;
	while (length < info->bootargs_length && line[length] != '\0')
		length++;

	/* Cuts a line the kernel cannot keep at its last separator. */
	limit = sizeof(boot_command_line) - 1U;
	if (length > limit) {
		length = limit;
		while (length > 0U && line[length] != ' ')
			length--;
	}

	/* Copies the line and terminates it. */
	hal_memcpy(boot_command_line, line, length);
	boot_command_line[length] = '\0';
}

/* Tells whether a handoff name is the one wanted. */
static int
handoff_name_is(
	const char *name,
	const char *wanted)
{
	size_t index;

	/* Refuses a missing name. */
	if (name == NULL)
		return 0;

	/* Compares the names character by character up to the wanted one's end. */
	for (index = 0; wanted[index] != '\0'; index++) {
		if (name[index] != wanted[index])
			return 0;
	}

	/* A longer name is a different one. */
	if (name[index] != '\0')
		return 0;

	/* Succeeded: the names are the same. */
	return 1;
}
