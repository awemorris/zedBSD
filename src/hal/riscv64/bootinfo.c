/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 boot record and the kernel's handoff.
 *
 * The firmware passes the boot hart's id and the device tree.  The HAL
 * keeps what it read from the tree, copies the command line out of it
 * while the tree is known to be intact, and gives the kernel the common
 * handoff and the named objects of hal_get_arch_handoff(): the command
 * line, the tree, the initial ramdisk and the boot hart, and the board's
 * own names.  It also keeps what the UEFI loader's handoff adds: the
 * screen the firmware left on and the RAM the firmware keeps.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "bootinfo.h"
#include "bsp.h"
#include "defs.h"
#include "loader.h"

/*
 * The facts read from the device tree.  Filled once by
 * riscv64_boot_record() on the boot hart before any other hart starts, and
 * read without a lock afterwards, like the two variables below.
 */
static struct riscv64_fdt_info boot_fdt;

/* The device tree's physical address, recorded with boot_fdt. */
static uintptr_t boot_fdt_phys;

/* The id of the boot hart, which entered the kernel first; recorded with boot_fdt. */
static uint64_t boot_hart_id;

/*
 * The device tree's range, the first named object of
 * hal_get_arch_handoff().  The three named objects are filled with the
 * record and not changed afterwards.
 */
static struct riscv64_handoff_range handoff_fdt;

/* The ramdisk's range, a named object; its size is zero when the firmware loaded none. */
static struct riscv64_handoff_range handoff_initrd;

/* The boot hart's id, a named object. */
static uint64_t handoff_boot_hart;

/*
 * The handoff the kernel's entry reads: a single boot device, the first
 * partition of an MBR disk, the way the kernel finds its root on the
 * machines whose loader passes no device table.  Filled with the record.
 */
static struct kern_boot_handoff kernel_handoff;

/*
 * The command line handed to the kernel as "boot.command-line": the
 * firmware's (/chosen/bootargs), copied out of the tree, or the boot
 * parameters the loader passed instead.  The first byte is zero when there
 * is no line.
 */
static char boot_command_line[KERN_BOOT_PARAMETERS_STORAGE_SIZE];

/*
 * What the UEFI loader's handoff added to the first layout: empty when the
 * firmware entered the kernel directly or the loader wrote the first
 * layout.  Filled once by riscv64_boot_take_loader() on the boot hart
 * before the board is set up, and read without a lock afterwards.
 */
static struct riscv64_loader_extras boot_loader_extras;

static void copy_command_line(const struct riscv64_fdt_info *info, uintptr_t fdt_phys);
static void keep_command_line(const char *line, size_t available);
static int name_is(const char *name, const char *wanted);

/*
 * Records the boot hart, the device tree and what was read from it, and
 * prepares the kernel's handoff.
 */
void
riscv64_boot_record(
	uint64_t hart_id,
	uintptr_t fdt_phys,
	const struct riscv64_fdt_info *info)
{
	/* Keeps the facts for the rest of the HAL. */
	boot_fdt = *info;
	boot_fdt_phys = fdt_phys;
	boot_hart_id = hart_id;

	/* The named objects: the tree, the ramdisk and the boot hart. */
	handoff_fdt.phys = fdt_phys;
	handoff_fdt.size = info->totalsize;
	handoff_initrd.phys = 0;
	handoff_initrd.size = 0;
	if (info->initrd_end > info->initrd_start) {
		handoff_initrd.phys = info->initrd_start;
		handoff_initrd.size = info->initrd_end - info->initrd_start;
	}

	/* Names the boot hart. */
	handoff_boot_hart = hart_id;

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
 * Replaces the command line with the boot parameters the loader passed.
 *
 * The loader's parameters are the zedbsd.cfg lines; the firmware's own
 * line in the tree is not the kernel's when a loader ran.
 */
void
riscv64_boot_set_command_line(
	const char *text,
	size_t available)
{
	/* Keeps the loader's line in place of the tree's. */
	keep_command_line(text, available);
}

/*
 * Takes what a UEFI loader's handoff adds to the first layout, once the
 * device tree's facts are recorded.
 *
 * Returns RISCV64_LOADER_OK, or why the firmware's RAM cannot all be kept
 * from the allocator; the caller stops the boot then, once the screen can
 * show why.
 */
int
riscv64_boot_take_loader(
	const struct zbl6_riscv64_handoff *handoff)
{
	const struct riscv64_framebuffer *framebuffer;
	int error;

	/* Takes the screen and the firmware's RAM against the tree's banks and reservations. */
	error = riscv64_loader_take(handoff, &boot_fdt, &boot_loader_extras);

	/* Describes what the second layout gave, for the boot log. */
	if (boot_loader_extras.layout >= 2U) {
		hal_printf("RISCV64 LOADER layout=2 reserved=%u kept=%u\n",
			   boot_loader_extras.reserved_given,
			   boot_loader_extras.reserved_count);
	}

	/* Describes the screen, or why it was not taken. */
	framebuffer = &boot_loader_extras.framebuffer;
	if (boot_loader_extras.screen == RISCV64_LOADER_SCREEN_TAKEN) {
		hal_printf("RISCV64 LOADER framebuffer %ux%u pitch=%u format=%u phys=%lx size=%lx\n",
			   framebuffer->width,
			   framebuffer->height,
			   framebuffer->pitch,
			   framebuffer->format,
			   framebuffer->phys,
			   framebuffer->size);
	} else if (boot_loader_extras.screen == RISCV64_LOADER_SCREEN_REFUSED) {
		hal_printf("RISCV64 LOADER framebuffer refused\n");
	}

	/* Reports why the firmware's RAM cannot all be kept. */
	if (error != RISCV64_LOADER_OK)
		return error;

	/* Succeeded: the extras are kept. */
	return RISCV64_LOADER_OK;
}

/*
 * Reports the screen the loader handed over, or NULL when there is none
 * the console and the kernel can draw on.
 */
const struct riscv64_framebuffer *
riscv64_boot_framebuffer(
	void)
{
	/* Only a screen that passed the checks is reported. */
	if (boot_loader_extras.screen != RISCV64_LOADER_SCREEN_TAKEN)
		return NULL;

	/* Succeeded: the screen taken at boot. */
	return &boot_loader_extras.framebuffer;
}

/*
 * Reports the ranges of RAM the firmware keeps that the device tree does
 * not reserve, and how many there are.
 */
const struct riscv64_fdt_range *
riscv64_boot_firmware_reserved(
	unsigned *count)
{
	/* Succeeded: the ranges taken at boot, none without a loader's second layout. */
	*count = boot_loader_extras.reserved_count;
	return boot_loader_extras.reserved;
}

/*
 * Reports the facts read from the device tree.
 */
const struct riscv64_fdt_info *
riscv64_boot_fdt(
	void)
{
	/* Succeeded: the record filled at boot. */
	return &boot_fdt;
}

/*
 * Reports the device tree's physical address.
 */
uintptr_t
riscv64_boot_fdt_phys(
	void)
{
	/* Succeeded: the address the firmware passed. */
	return boot_fdt_phys;
}

/*
 * Reports the boot hart's id.
 */
uint64_t
riscv64_boot_hart(
	void)
{
	/* Succeeded: the id the firmware passed. */
	return boot_hart_id;
}

/*
 * Reports the command line, empty when the firmware passed none.
 */
const char *
riscv64_boot_command_line(
	void)
{
	/* Succeeded: the copy made at boot. */
	return boot_command_line;
}

/*
 * Reports whether a word stands on its own on the command line.
 */
bool
riscv64_boot_parameter_given(
	const char *word)
{
	const char *line;
	unsigned position;
	unsigned matched;
	int at_word;

	/* Looks for the word at the start of a word of the line. */
	line = boot_command_line;
	at_word = 1;
	for (position = 0; line[position] != '\0'; position++) {
		/* A space starts the next word. */
		if (line[position] == ' ') {
			at_word = 1;
			continue;
		}

		/* Inside a word the wanted one cannot start. */
		if (!at_word)
			continue;
		at_word = 0;

		/* Compares the word. */
		for (matched = 0; word[matched] != '\0'; matched++) {
			if (line[position + matched] != word[matched])
				break;
		}

		/* Skips a word that differs from the wanted one before the wanted one ends. */
		if (word[matched] != '\0')
			continue;

		/* The word is given only when the line's word ends with it. */
		if (line[position + matched] == ' ' || line[position + matched] == '\0')
			return true;
	}

	/* The word is absent. */
	return false;
}

/*
 * Reads a decimal parameter KEY=N from the command line; 0 when absent.
 *
 * key includes its '='.  The key must start a word.
 */
unsigned
riscv64_boot_parameter_number(
	const char *key)
{
	const char *line;
	unsigned position;
	unsigned matched;
	unsigned value;
	int at_word;

	/* Looks for the key at the start of a word. */
	line = boot_command_line;
	at_word = 1;
	for (position = 0; line[position] != '\0'; position++) {
		/* A space starts the next word. */
		if (line[position] == ' ') {
			at_word = 1;
			continue;
		}

		/* Inside a word the key cannot start. */
		if (!at_word)
			continue;
		at_word = 0;

		/* Compares the key. */
		for (matched = 0; key[matched] != '\0'; matched++) {
			if (line[position + matched] != key[matched])
				break;
		}

		/* Skips a word that does not start with the whole key. */
		if (key[matched] != '\0')
			continue;

		/* Reads the decimal value after the key, capped so it cannot overflow. */
		value = 0;
		position += matched;
		while (line[position] >= '0' && line[position] <= '9') {
			value = value * 10U + (unsigned)(line[position] - '0');
			if (value > 1000U)
				value = 1000U;
			position++;
		}

		/* Succeeded: the value given. */
		return value;
	}

	/* The parameter is absent. */
	return 0;
}

/*
 * Reports the handoff the kernel's entry reads.
 */
const void *
riscv64_kernel_handoff(
	void)
{
	/* Succeeded: the handoff filled with the boot record. */
	return &kernel_handoff;
}

/*
 * Returns one named architecture handoff object.
 *
 * "boot.command-line" is the firmware's command line; "riscv64.fdt" and
 * "riscv64.initrd" are physical ranges (struct riscv64_handoff_range, two
 * 64-bit words), the second only when the firmware loaded a ramdisk; and
 * "riscv64.boot-hart" is the boot hart's id as a 64-bit word.  Every other
 * name is the board's: the VisionFive 2 names the screen the firmware left
 * on ("riscv64.framebuffer.*").
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	void *object;
	int match;

	/* A missing name names nothing. */
	if (name == NULL)
		return NULL;

	/* The firmware's command line, when it passed one. */
	match = name_is(name, "boot.command-line");
	if (match) {
		if (boot_command_line[0] == '\0')
			return NULL;
		return boot_command_line;
	}

	/* The device tree. */
	match = name_is(name, "riscv64.fdt");
	if (match)
		return &handoff_fdt;

	/* The initial ramdisk, when there is one. */
	match = name_is(name, "riscv64.initrd");
	if (match) {
		if (handoff_initrd.size == 0)
			return NULL;
		return &handoff_initrd;
	}

	/* The boot hart. */
	match = name_is(name, "riscv64.boot-hart");
	if (match)
		return &handoff_boot_hart;

	/* Any other name is the board's to answer, NULL when it has no such object. */
	object = riscv64_board_handoff(name);

	/* Succeeded: the board's object, or NULL for an unknown name. */
	return object;
}

/*
 * Reports whether a handoff name is the one wanted, for the board's own
 * names.
 */
bool
riscv64_boot_name_is(
	const char *name,
	const char *wanted)
{
	int match;

	/* Compares the names as hal_get_arch_handoff() does. */
	match = name_is(name, wanted);
	if (!match)
		return false;

	/* Succeeded: the names are the same. */
	return true;
}

/*
 * Copies the firmware's command line out of the device tree, while the
 * tree is known to be intact.
 */
static void
copy_command_line(
	const struct riscv64_fdt_info *info,
	uintptr_t fdt_phys)
{
	const char *line;

	/* Starts with no line, which is what an absent one leaves. */
	boot_command_line[0] = '\0';

	/* Leaves the line empty when the firmware passed none. */
	if (info->bootargs_offset == 0U || info->bootargs_length == 0U)
		return;

	/* Keeps the property's line. */
	line = (const char *)(RISCV64_DIRECT_BASE + fdt_phys + info->bootargs_offset);
	keep_command_line(line, info->bootargs_length);
}

/*
 * Keeps a command line of at most available bytes, ending at its
 * terminator if it has one.
 *
 * A line longer than the kernel keeps is cut at the last separator that
 * fits, so no parameter is handed over in part.
 */
static void
keep_command_line(
	const char *line,
	size_t available)
{
	size_t length;
	size_t limit;

	/* Measures the line up to its terminator, within what is available. */
	length = 0;
	while (length < available && line[length] != '\0')
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
