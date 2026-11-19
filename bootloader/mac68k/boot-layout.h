/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 68040 Macintosh boot layout shared by the boot stages and the disk
 * image's tool.
 *
 * The ROM reads the first two sectors of the HFS boot partition (the boot
 * block) and enters it.  The boot block carries, after the header the ROM
 * reads, the places of the second stage and of the kernel in the
 * partition, which the image's tool writes; the second stage carries, at
 * its start, the place and the number of its relocations, which the tool
 * that finishes it writes.
 */

#ifndef KERN_BOOT_MAC68K_LAYOUT_H
#define KERN_BOOT_MAC68K_LAYOUT_H

/* The disk's block, which the boot partition's offsets count in. */
#define MAC68K_SECTOR_SIZE			512

/* The boot block: two sectors at the start of the HFS partition. */
#define MAC68K_BOOT_BLOCK_SIZE			1024

/*
 * The boot block header the ROM reads: the signature, the entry and the
 * version, then the names and the heap sizes, up to its end.
 *
 * Values referenced: Apple, Inside Macintosh: Files (1992), p. 2-57
 * "Boot Blocks" (the boot block header's fields and their offsets, the
 * signature 'LK').
 */
#define MAC68K_BOOT_SIGNATURE			0x4c4b
#define MAC68K_BOOT_VERSION_OFFSET		6
#define MAC68K_BOOT_HEADER_END			148

/*
 * The boot block's version word: its high bits ask the ROM to run the
 * boot code.
 *
 * Value referenced: EMILE (github.com/vivier/EMILE, master)
 * first/first.S (the boot block version word of a boot block that the
 * ROM runs on the 68020, 68030 and 68040 Macintoshes).
 */
#define MAC68K_BOOT_VERSION			0x4418

/* The zedBSD fields after the header, and their identity. */
#define MAC68K_BOOT_INFO_OFFSET			MAC68K_BOOT_HEADER_END
#define MAC68K_BOOT_INFO_MAGIC			0x5a454442
#define MAC68K_BOOT_INFO_VERSION		1
#define MAC68K_BOOT_INFO_SIZE			44

/* Where the boot stages find each field, from the start of the boot block. */
#define MAC68K_BOOT_MAGIC_OFFSET		(MAC68K_BOOT_INFO_OFFSET + 0)
#define MAC68K_BOOT_LAYOUT_VERSION_OFFSET	(MAC68K_BOOT_INFO_OFFSET + 4)
#define MAC68K_BOOT_PARTITION_BLOCK_OFFSET	(MAC68K_BOOT_INFO_OFFSET + 8)
#define MAC68K_BOOT_STAGE2_OFFSET_OFFSET	(MAC68K_BOOT_INFO_OFFSET + 12)
#define MAC68K_BOOT_STAGE2_BYTES_OFFSET		(MAC68K_BOOT_INFO_OFFSET + 16)
#define MAC68K_BOOT_STAGE2_MEMORY_OFFSET	(MAC68K_BOOT_INFO_OFFSET + 20)
#define MAC68K_BOOT_STAGE2_CRC_OFFSET		(MAC68K_BOOT_INFO_OFFSET + 24)
#define MAC68K_BOOT_KERNEL_OFFSET_OFFSET	(MAC68K_BOOT_INFO_OFFSET + 28)
#define MAC68K_BOOT_KERNEL_BYTES_OFFSET		(MAC68K_BOOT_INFO_OFFSET + 32)
#define MAC68K_BOOT_KERNEL_CRC_OFFSET		(MAC68K_BOOT_INFO_OFFSET + 36)
#define MAC68K_BOOT_IMAGE_MARK_OFFSET		(MAC68K_BOOT_INFO_OFFSET + 40)

/* Where the boot block's own code starts, after its fields. */
#define MAC68K_BOOT_CODE_OFFSET			192

/*
 * The second stage's header: a branch over it, its identity, the offset
 * and the count of the 32-bit relocation offsets that follow its image
 * in the file, and the memory it needs (the image, its BSS and the
 * relocations, which the BSS may cover).
 */
#define MAC68K_STAGE2_MAGIC			0x5a425332
#define MAC68K_STAGE2_MAGIC_OFFSET		4
#define MAC68K_STAGE2_RELOC_OFFSET_OFFSET	8
#define MAC68K_STAGE2_RELOC_COUNT_OFFSET	12
#define MAC68K_STAGE2_MEMORY_OFFSET		16
#define MAC68K_STAGE2_HEADER_SIZE		20

/* The largest second stage the boot block reads, file and memory. */
#define MAC68K_STAGE2_LIMIT			0x00040000

/*
 * The codes the boot block stops with, through the ROM's system error
 * alert, when it cannot start the second stage.
 */
#define MAC68K_BOOT_ERROR_PROCESSOR		101
#define MAC68K_BOOT_ERROR_MEMORY		102
#define MAC68K_BOOT_ERROR_DRIVE			103
#define MAC68K_BOOT_ERROR_READ			104
#define MAC68K_BOOT_ERROR_STAGE2		105

#endif
