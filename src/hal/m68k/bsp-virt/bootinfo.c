/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's boot information and the kernel's handoff.
 *
 * The loader leaves a list of tagged records right after the kernel
 * image: the memory, the ramdisk, the command line and the addresses of
 * the board's devices.  The HAL reads them once, before anything can
 * overwrite them, and hands the kernel the common handoff and the named
 * objects of hal_get_arch_handoff().
 *
 * Values referenced: QEMU v10.0.0 include/standard-headers/asm-m68k/
 * bootinfo.h and bootinfo-virt.h (the record layout and the tag numbers).
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "bsp.h"

/* The tags of the records the HAL reads. */
#define BI_LAST			0x0000
#define BI_MEMCHUNK		0x0005
#define BI_RAMDISK		0x0006
#define BI_COMMAND_LINE		0x0007
#define BI_VIRT_QEMU_VERSION	0x8000
#define BI_VIRT_GF_PIC_BASE	0x8001
#define BI_VIRT_GF_RTC_BASE	0x8002
#define BI_VIRT_GF_TTY_BASE	0x8003
#define BI_VIRT_VIRTIO_BASE	0x8004
#define BI_VIRT_CTRL_BASE	0x8005

/* The most bytes of records read: the loader writes far fewer. */
#define BOOTINFO_LIMIT		4096U

/*
 * A physical range handed to the kernel by name, as two 32-bit words.
 */
struct virt_handoff_range {
	uint32_t phys;
	uint32_t size;
};

/* What the records say, filled once at boot. */
static struct virt_board_info board;

/* The records' own range, handed over as "m68k.bootinfo". */
static struct virt_handoff_range handoff_bootinfo;

/* The ramdisk, handed over as "m68k.initrd" when there is one. */
static struct virt_handoff_range handoff_initrd;

/*
 * The handoff the kernel's entry reads: a single boot device, the first
 * partition of an MBR disk, as on the boards whose loader passes no
 * device table.
 */
static struct kern_boot_handoff kernel_handoff;

/* The command line, handed over as "boot.command-line"; empty when none. */
static char boot_command_line[KERN_BOOT_PARAMETERS_STORAGE_SIZE];

static uint32_t read_long(const uint8_t *bytes);
static void copy_command_line(const uint8_t *text, uint32_t length);
static int name_is(const char *name, const char *wanted);

/*
 * Reads the records at the end of the image and prepares the handoff.
 * Reports the first byte after the records.
 */
uint32_t
virt_bootinfo_parse(
	const void *records)
{
	const uint8_t *record;
	const uint8_t *data;
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
		data = record + offset + 4U;

		/* Keeps what the HAL uses; the first memory chunk is the RAM. */
		switch (tag) {
		case BI_MEMCHUNK:
			if (board.memory_size == 0) {
				board.memory_base = read_long(data);
				board.memory_size = read_long(data + 4);
			}

			break;
		case BI_RAMDISK:
			board.ramdisk_base = read_long(data);
			board.ramdisk_size = read_long(data + 4);
			break;
		case BI_COMMAND_LINE:
			copy_command_line(data, size - 4U);
			break;
		case BI_VIRT_QEMU_VERSION:
			board.qemu_version = read_long(data);
			break;
		case BI_VIRT_GF_PIC_BASE:
			board.pic_base = read_long(data);
			break;
		case BI_VIRT_GF_RTC_BASE:
			board.rtc_base = read_long(data);
			break;
		case BI_VIRT_GF_TTY_BASE:
			board.tty_base = read_long(data);
			break;
		case BI_VIRT_VIRTIO_BASE:
			board.virtio_base = read_long(data);
			board.virtio_irq = read_long(data + 4);
			break;
		case BI_VIRT_CTRL_BASE:
			board.ctrl_base = read_long(data);
			break;
		default:
			break;
		}

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

	/* Succeeded: the end of the records. */
	return (uint32_t)(uintptr_t)records + offset;
}

/*
 * Reports what the records said about the board.
 */
const struct virt_board_info *
virt_board(
	void)
{
	/* Succeeded: the record filled at boot. */
	return &board;
}

/*
 * Reports the handoff the kernel's entry reads.
 */
const struct kern_boot_handoff *
virt_kernel_handoff(
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
