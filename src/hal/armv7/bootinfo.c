/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 boot record and the kernel's handoff.
 *
 * The loader passes the device tree.  The HAL keeps what it read from the
 * tree, copies the command line out of it while the tree is known to be
 * intact, and gives the kernel the common handoff and the named objects
 * of hal_get_arch_handoff(): the command line, the tree and the initial
 * ramdisk.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "bootinfo.h"
#include "defs.h"
#include "space.h"

/*
 * The facts read from the device tree and the tree's physical address.
 * Filled once by armv7_boot_record() on the boot processor before any
 * other starts, and read without a lock afterwards.
 */
static struct armv7_fdt_info boot_fdt;
static uintptr_t boot_fdt_phys;

/*
 * The named objects of hal_get_arch_handoff(), filled with the record and
 * not changed afterwards.  The ramdisk's size is zero when the loader
 * placed none.
 */
static struct armv7_handoff_range handoff_fdt;
static struct armv7_handoff_range handoff_initrd;

/*
 * The handoff the kernel's entry reads: a single boot device, the first
 * partition of an MBR disk, the way the kernel finds its root on the
 * machines whose loader passes no device table.  Filled with the record.
 */
static struct kern_boot_handoff kernel_handoff;

/*
 * The loader's command line (/chosen/bootargs), copied out of the tree
 * and handed to the kernel as "boot.command-line".  The first byte is zero
 * when the loader passed no line.
 */
static char boot_command_line[KERN_BOOT_PARAMETERS_STORAGE_SIZE];

static void copy_command_line(const struct armv7_fdt_info *info, uintptr_t fdt_phys);
static int name_is(const char *name, const char *wanted);

/*
 * Records the device tree and what was read from it, and prepares the
 * kernel's handoff.
 */
void
armv7_boot_record(
	uintptr_t fdt_phys,
	const struct armv7_fdt_info *info)
{
	/* Keeps the facts for the rest of the HAL. */
	boot_fdt = *info;
	boot_fdt_phys = fdt_phys;

	/* The named objects: the tree and the ramdisk. */
	handoff_fdt.phys = fdt_phys;
	handoff_fdt.size = info->totalsize;
	handoff_initrd.phys = 0;
	handoff_initrd.size = 0;
	if (info->initrd_end > info->initrd_start) {
		handoff_initrd.phys = info->initrd_start;
		handoff_initrd.size = info->initrd_end - info->initrd_start;
	}

	/* The common handoff: one boot device, the first MBR partition, its id 0x80. */
	hal_memset(&kernel_handoff, 0, sizeof(kernel_handoff));
	kernel_handoff.magic = KERN_HANDOFF_MAGIC;
	kernel_handoff.version = KERN_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.size = sizeof(kernel_handoff);
	kernel_handoff.device_count = 1;
	kernel_handoff.boot_bios_id = 0x80;
	kernel_handoff.boot_partition_scheme = KERN_PARTITION_SCHEME_MBR;
	kernel_handoff.boot_partition_index = 1;
	kernel_handoff.boot_partition_lba = KERN_BOOT_PARTITION_LBA_UNKNOWN;

	/* The command line, out of the tree while it is intact. */
	copy_command_line(info, fdt_phys);
}

/*
 * Reports the facts read from the device tree.
 */
const struct armv7_fdt_info *
armv7_boot_fdt(
	void)
{
	/* Succeeded: the record filled at boot. */
	return &boot_fdt;
}

/*
 * Reports the device tree's physical address.
 */
uintptr_t
armv7_boot_fdt_phys(
	void)
{
	/* Succeeded: the address the loader passed. */
	return boot_fdt_phys;
}

/*
 * Reports the command line, empty when the loader passed none.
 */
const char *
armv7_boot_command_line(
	void)
{
	/* Succeeded: the copy made at boot. */
	return boot_command_line;
}

/*
 * Reports whether the command line holds a word, alone or as the name of
 * a name=value parameter.
 */
int
armv7_boot_has_parameter(
	const char *name)
{
	size_t start;
	size_t index;
	char next;

	/* Looks at the start of each word of the line. */
	start = 0;
	while (boot_command_line[start] != '\0') {
		/* Compares the word's first characters with the name. */
		for (index = 0; name[index] != '\0'; index++) {
			if (boot_command_line[start + index] != name[index])
				break;
		}

		/* The whole name, followed by the word's end or by its value, is a match. */
		if (name[index] == '\0') {
			next = boot_command_line[start + index];
			if (next == '\0' ||
			    next == ' ' ||
			    next == '=')
				return 1;
		}

		/* Moves past the word and the spaces after it. */
		while (boot_command_line[start] != '\0' && boot_command_line[start] != ' ')
			start++;
		while (boot_command_line[start] == ' ')
			start++;
	}

	/* The line does not name it. */
	return 0;
}

/*
 * Reports the handoff the kernel's entry reads.
 */
const void *
armv7_kernel_handoff(
	void)
{
	/* Succeeded: the handoff filled with the boot record. */
	return &kernel_handoff;
}

/*
 * Returns one named architecture handoff object.
 *
 * "boot.command-line" is the loader's command line; "armv7.fdt" and
 * "armv7.initrd" are physical ranges (struct armv7_handoff_range, two
 * 64-bit words), the second only when the loader placed a ramdisk.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	int match;

	/* A missing name names nothing. */
	if (name == NULL)
		return NULL;

	/* The loader's command line, when it passed one. */
	match = name_is(name, "boot.command-line");
	if (match) {
		if (boot_command_line[0] == '\0')
			return NULL;
		return boot_command_line;
	}

	/* The device tree. */
	match = name_is(name, "armv7.fdt");
	if (match)
		return &handoff_fdt;

	/* The initial ramdisk, when there is one. */
	match = name_is(name, "armv7.initrd");
	if (match) {
		if (handoff_initrd.size == 0)
			return NULL;
		return &handoff_initrd;
	}

	/* An unknown handoff. */
	return NULL;
}

/*
 * Copies the loader's command line out of the device tree.
 *
 * A line longer than the kernel keeps is cut at the last separator that
 * fits, so no parameter is handed over in part.
 */
static void
copy_command_line(
	const struct armv7_fdt_info *info,
	uintptr_t fdt_phys)
{
	const char *line;
	size_t length;
	size_t limit;

	/* Starts with no line, which is what an absent one leaves. */
	boot_command_line[0] = '\0';

	/* Leaves the line empty when the loader passed none. */
	if (info->bootargs_offset == 0U || info->bootargs_length == 0U)
		return;

	/* Measures the line up to its terminator, within the property. */
	line = (const char *)armv7_phys_to_direct((uint64_t)fdt_phys + info->bootargs_offset);
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
name_is(
	const char *name,
	const char *wanted)
{
	size_t index;

	/* Compares the names character by character. */
	for (index = 0; wanted[index] != '\0'; index++) {
		if (name[index] != wanted[index])
			return 0;
	}

	/* The names are the same only when the name ends there too. */
	if (name[index] != '\0')
		return 0;
	return 1;
}
