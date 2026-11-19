/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Macintosh's boot information and the kernel's handoff.
 *
 * The loader leaves a list of tagged records right after the kernel
 * image, in the form the m68k Linux kernel reads on a Macintosh: the
 * machine type, the memory banks, the command line, the Gestalt model
 * and the screen the ROM set up.  The emulator's q800 machine writes the
 * same records when it starts a kernel directly.  The HAL reads them
 * once, before anything can overwrite them, and hands the kernel the
 * common handoff and the named objects of hal_get_arch_handoff().
 *
 * Values referenced: QEMU v10.0.0 include/standard-headers/asm-m68k/
 * bootinfo.h and bootinfo-mac.h (the record layout, the tag numbers, the
 * Macintosh machine type, and the screen's dimensions as the height in
 * the high half and the width in the low half).
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "bsp.h"

/* The tags of the records the HAL reads. */
#define BI_LAST			0x0000
#define BI_MACHTYPE		0x0001
#define BI_MEMCHUNK		0x0005
#define BI_RAMDISK		0x0006
#define BI_COMMAND_LINE		0x0007
#define BI_MAC_MODEL		0x8000
#define BI_MAC_VADDR		0x8001
#define BI_MAC_VDEPTH		0x8002
#define BI_MAC_VROW		0x8003
#define BI_MAC_VDIM		0x8004
#define BI_MAC_SCCBASE		0x8006
#define BI_MAC_ROMBASE		0x800b

/* The machine type of a Macintosh. */
#define MACH_MAC		3U

/* The most bytes of records read: the loader writes far fewer. */
#define BOOTINFO_LIMIT		4096U

/*
 * A physical range handed to the kernel by name, as two 32-bit words.
 */
struct mac68k_handoff_range {
	uint32_t phys;
	uint32_t size;
};

/* What the records say, filled once at boot. */
static struct mac68k_board_info board;

/* The records' own range, handed over as "m68k.bootinfo". */
static struct mac68k_handoff_range handoff_bootinfo;

/* The ramdisk, handed over as "m68k.initrd" when there is one. */
static struct mac68k_handoff_range handoff_initrd;

/*
 * The handoff the kernel's entry reads.  The machine publishes no boot
 * device yet; the boot disk is an Apple Partition Map disk.
 */
static struct kern_boot_handoff kernel_handoff;

/* The command line, handed over as "boot.command-line"; empty when none. */
static char boot_command_line[KERN_BOOT_PARAMETERS_STORAGE_SIZE];

static void keep_record(uint16_t tag, const uint8_t *data, uint32_t length);
static uint32_t read_long(const uint8_t *bytes);
static void copy_command_line(const uint8_t *text, uint32_t length);
static int name_is(const char *name, const char *wanted);

/*
 * Reads the records at the end of the image and prepares the handoff.
 * Reports the first byte after the records.
 */
uint32_t
mac68k_bootinfo_parse(
	const void *records)
{
	const uint8_t *record;
	uint32_t offset;
	uint16_t tag;
	uint16_t size;

	/* Walks the records up to the last one, within the limit. */
	record = records;
	offset = 0;
	for (;;) {
		if (offset + 4U > BOOTINFO_LIMIT)
			HAL_FATAL("m68k boot information has no end");
		tag = (uint16_t)((record[offset] << 8) | record[offset + 1U]);
		size = (uint16_t)((record[offset + 2U] << 8) | record[offset + 3U]);
		if (tag == BI_LAST)
			break;
		if (size < 4U || offset + size > BOOTINFO_LIMIT)
			HAL_FATAL("m68k boot information record is malformed");

		/* Keeps what the HAL uses. */
		keep_record(tag, record + offset + 4U, size - 4U);

		/* Moves on to the next record. */
		offset += size;
	}

	/* Counts the last record's header too. */
	offset += 4U;

	/* The named objects. */
	handoff_bootinfo.phys = (uint32_t)(uintptr_t)records;
	handoff_bootinfo.size = offset;
	handoff_initrd.phys = board.ramdisk_base;
	handoff_initrd.size = board.ramdisk_size;

	/* The common handoff: no boot device yet, an Apple Partition Map disk. */
	hal_memset(&kernel_handoff, 0, sizeof(kernel_handoff));
	kernel_handoff.magic = KERN_HANDOFF_MAGIC;
	kernel_handoff.version = KERN_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.size = sizeof(kernel_handoff);
	kernel_handoff.device_count = 0;
	kernel_handoff.boot_partition_scheme = KERN_PARTITION_SCHEME_APM;
	kernel_handoff.boot_partition_index = KERN_PARTITION_INDEX_UNKNOWN;
	kernel_handoff.boot_partition_lba = KERN_BOOT_PARTITION_LBA_UNKNOWN;

	/* Succeeded: the end of the records. */
	return (uint32_t)(uintptr_t)records + offset;
}

/*
 * Reports what the records said about the machine.
 */
const struct mac68k_board_info *
mac68k_board(
	void)
{
	/* Succeeded: the record filled at boot. */
	return &board;
}

/*
 * Reports the handoff the kernel's entry reads.
 */
const struct kern_boot_handoff *
mac68k_kernel_handoff(
	void)
{
	/* Succeeded: the handoff filled at boot. */
	return &kernel_handoff;
}

/*
 * Returns one named architecture handoff object.
 *
 * "boot.command-line" is the loader's command line; "m68k.bootinfo" and
 * "m68k.initrd" are physical ranges (two 32-bit words, the address and
 * the size), the second only when the loader placed a ramdisk.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	int match;

	/* A missing name names nothing. */
	if (name == NULL)
		return NULL;

	/* The command line, when the loader passed one. */
	match = name_is(name, "boot.command-line");
	if (match) {
		if (boot_command_line[0] == '\0')
			return NULL;
		return boot_command_line;
	}

	/* The boot information. */
	match = name_is(name, "m68k.bootinfo");
	if (match)
		return &handoff_bootinfo;

	/* The initial ramdisk, when there is one. */
	match = name_is(name, "m68k.initrd");
	if (match) {
		if (handoff_initrd.size == 0)
			return NULL;
		return &handoff_initrd;
	}

	/* An unknown handoff. */
	return NULL;
}

/* Keeps what one record says, by its tag; a record too short is ignored. */
static void
keep_record(
	uint16_t tag,
	const uint8_t *data,
	uint32_t length)
{
	uint32_t dimensions;
	uint32_t machine;
	uint32_t count;

	/* The command line is text of any length. */
	if (tag == BI_COMMAND_LINE) {
		copy_command_line(data, length);
		return;
	}

	/* Every other record the HAL reads starts with a 32-bit word. */
	if (length < 4U)
		return;

	/* Chooses by the tag. */
	switch (tag) {
	case BI_MACHTYPE:
		/* Another machine's information means the image was started wrongly. */
		machine = read_long(data);
		if (machine != MACH_MAC)
			HAL_FATAL("m68k boot information is not a Macintosh's");
		break;
	case BI_MEMCHUNK:
		/* Each bank in turn, up to the most the pages can describe. */
		if (length < 8U)
			break;
		count = board.memory_bank_count;
		if (count >= M68040_MEMORY_BANKS_MAX)
			break;
		board.memory_banks[count].base = read_long(data);
		board.memory_banks[count].size = read_long(data + 4);
		board.memory_bank_count = count + 1U;
		break;
	case BI_RAMDISK:
		if (length < 8U)
			break;
		board.ramdisk_base = read_long(data);
		board.ramdisk_size = read_long(data + 4);
		break;
	case BI_MAC_MODEL:
		board.model = read_long(data);
		break;
	case BI_MAC_VADDR:
		board.video_base = read_long(data);
		break;
	case BI_MAC_VDEPTH:
		board.video_depth = read_long(data);
		break;
	case BI_MAC_VROW:
		board.video_row_bytes = read_long(data);
		break;
	case BI_MAC_VDIM:
		/* The height in the high half, the width in the low half. */
		dimensions = read_long(data);
		board.video_height = dimensions >> 16;
		board.video_width = dimensions & 0xffffU;
		break;
	case BI_MAC_SCCBASE:
		board.scc_base = read_long(data);
		break;
	case BI_MAC_ROMBASE:
		board.rom_base = read_long(data);
		break;
	default:
		break;
	}
}

/* Reads a big-endian 32-bit word that may be only two-byte aligned. */
static uint32_t
read_long(
	const uint8_t *bytes)
{
	/* Most significant byte first. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
}

/*
 * Copies the command line out of its record, cut at the last separator
 * that fits when the kernel cannot keep it whole.
 */
static void
copy_command_line(
	const uint8_t *text,
	uint32_t length)
{
	size_t used;
	size_t limit;

	/* Measures up to the terminator, within the record. */
	used = 0;
	while (used < length && text[used] != '\0')
		used++;

	/* Cuts a line the kernel cannot keep. */
	limit = sizeof(boot_command_line) - 1U;
	if (used > limit) {
		used = limit;
		while (used > 0U && text[used] != ' ')
			used--;
	}

	/* Copies and terminates it. */
	hal_memcpy(boot_command_line, text, used);
	boot_command_line[used] = '\0';
}

/* Tells whether a handoff name is the one wanted. */
static int
name_is(
	const char *name,
	const char *wanted)
{
	size_t index;

	/* Compares character by character, then requires the same end. */
	for (index = 0; wanted[index] != '\0'; index++) {
		if (name[index] != wanted[index])
			return 0;
	}

	/* The names are the same only when the name ends there too. */
	if (name[index] != '\0')
		return 0;
	return 1;
}
