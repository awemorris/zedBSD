/* zedBSD X68000 raw disk boot layout. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_BOOT_X68K_LAYOUT_H
#define ZEDBSD_BOOT_X68K_LAYOUT_H

#define X68K_SECTOR_SIZE 512
#define X68K_STAGE1_DISK_OFFSET 1024
#define X68K_STAGE1_MAX_SIZE 1024
#define X68K_PARTITION_DISK_OFFSET 2048
#define X68K_RAW_BOOT_OFFSET 4096
#define X68K_MANIFEST_LBA 8
#define X68K_MANIFEST_ADDRESS 0x0001f000
/* The internal SCSI IPL loads disk sector 1 at 0x2400 (FD uses 0x2000). */
#define X68K_STAGE1_ADDRESS 0x00002400
#define X68K_STAGE2_DEFAULT_ADDRESS 0x00020000
#define X68K_STAGE2_LIMIT 0x00040000
#define X68K_STAGE2_STACK 0x00050000
#define X68K_STAGE2_BOUNCE 0x00060000
#define X68K_STAGE2_BOUNCE_SIZE 4096
#define X68K_HANDOFF_ADDRESS 0x00008000
#define X68K_DEVICE_TABLE_ADDRESS 0x00009000
#define X68K_KERNEL_LOW_MIN 0x00010000
#define X68K_KERNEL_LOW_END 0x00020000
#define X68K_KERNEL_HIGH_MIN 0x00100000
#define X68K_ROOT_LBA 4096

#define X68K_MANIFEST_MAGIC 0x5a36384d
#define X68K_MANIFEST_VERSION 1
#define X68K_MANIFEST_SIZE 64

#define X68K_MANIFEST_MAGIC_OFFSET 0
#define X68K_MANIFEST_VERSION_OFFSET 4
#define X68K_MANIFEST_HEADER_SIZE_OFFSET 6
#define X68K_MANIFEST_STAGE2_LBA_OFFSET 8
#define X68K_MANIFEST_STAGE2_BYTES_OFFSET 12
#define X68K_MANIFEST_STAGE2_LOAD_OFFSET 16
#define X68K_MANIFEST_STAGE2_ENTRY_OFFSET 20
#define X68K_MANIFEST_STAGE2_CRC_OFFSET 24
#define X68K_MANIFEST_KERNEL_LBA_OFFSET 28
#define X68K_MANIFEST_KERNEL_BYTES_OFFSET 32
#define X68K_MANIFEST_KERNEL_CRC_OFFSET 36
#define X68K_MANIFEST_ROOT_LBA_OFFSET 40
#define X68K_MANIFEST_ROOT_SECTORS_OFFSET 44
#define X68K_MANIFEST_RAM_BYTES_OFFSET 48
#define X68K_MANIFEST_FLAGS_OFFSET 52

#ifndef _ASM_SRC_
#include <stdint.h>

struct x68k_boot_manifest {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t stage2_lba;
	uint32_t stage2_bytes;
	uint32_t stage2_load;
	uint32_t stage2_entry;
	uint32_t stage2_crc32;
	uint32_t kernel_lba;
	uint32_t kernel_bytes;
	uint32_t kernel_crc32;
	uint32_t root_lba;
	uint32_t root_sectors;
	uint32_t ram_bytes;
	uint32_t flags;
	uint32_t reserved[2];
} __attribute__((packed));

_Static_assert(sizeof(struct x68k_boot_manifest) == X68K_MANIFEST_SIZE,
	       "X68k boot manifest must remain one 64-byte header");
#endif

#endif
