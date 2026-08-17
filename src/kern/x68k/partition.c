/* X68000 SCSI disk mark and partition decoder. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <kern/x68k-partition.h>

#define X68K_TABLE_OFFSET 2048U
#define X68K_ENTRY_OFFSET 16U
#define X68K_ENTRY_SIZE   16U

static uint32_t
be24(const uint8_t *p)
{
	return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		(uint32_t)p[2] << 8 | p[3];
}

static int
bytes_equal(const uint8_t *p, const char *s, unsigned count)
{
	unsigned index;
	for (index = 0; index < count; index++)
		if (p[index] != (uint8_t)s[index])
			return 0;
	return 1;
}

int
x68k_partition_decode(const uint8_t *boot_area, size_t size,
		      uint64_t disk_sectors, struct partition *entries,
		      unsigned capacity)
{
	const uint8_t *table;
	uint32_t declared_blocks;
	unsigned count, index;

	if (boot_area == NULL || entries == NULL ||
	    size < X68K_PARTITION_BOOT_BYTES || disk_sectors < 8U ||
	    !bytes_equal(boot_area, "X68SCSI1", 8U))
		return -1;
	table = boot_area + X68K_TABLE_OFFSET;
	if (!bytes_equal(table, "X68K", 4U))
		return -1;
	declared_blocks = be32(table + 8U);
	if (declared_blocks == 0 || (uint64_t)declared_blocks * 2U >
	    disk_sectors)
		return -1;
	count = capacity < X68K_PARTITION_COUNT ? capacity :
		X68K_PARTITION_COUNT;
	for (index = 0; index < count; index++) {
		const uint8_t *raw = table + X68K_ENTRY_OFFSET +
			index * X68K_ENTRY_SIZE;
		struct partition *entry = &entries[index];
		uint32_t start1024 = be24(raw + 9U);
		uint32_t count1024 = be32(raw + 12U);
		uint64_t start = (uint64_t)start1024 * 2U;
		uint64_t blocks = (uint64_t)count1024 * 2U;
		unsigned at = 0, character;

		entry->p_parent = NULL;
		entry->p_disk = NULL;
		entry->p_index = index;
		entry->p_start_block = start;
		entry->p_data_block = start;
		entry->p_block_count = 0;
		entry->p_flags = 0;
		for (character = 0; character < 8U && at <
		    PARTITION_LABEL_MAX - 1U; character++) {
			uint8_t c = raw[character];
			if (c == 0 || c == ' ')
				break;
			entry->p_label[at++] = c >= 0x20U && c <= 0x7eU ?
				(char)c : '?';
		}
		entry->p_label[at] = '\0';
		/* Bit zero marks an unused/disabled entry. */
		if ((raw[8] & 1U) != 0 || count1024 == 0)
			continue;
		if (start < 8U || start >= disk_sectors ||
		    blocks > disk_sectors - start)
			continue;
		entry->p_block_count = blocks;
		if (raw[8] == 0)
			entry->p_flags |= PARTITION_BOOTABLE;
	}
	return (int)count;
}

static int
x68k_scan(const struct partition_scheme *scheme, struct disk *disk,
	  struct partition *entries, unsigned capacity)
{
	uint8_t boot_area[X68K_PARTITION_BOOT_BYTES];
	int count, index;
	(void)scheme;
	if (disk == NULL || disk->d_block_size != 512U ||
	    disk_read(disk, 0, X68K_PARTITION_BOOT_BYTES / 512U,
	    boot_area) != 0)
		return -1;
	count = x68k_partition_decode(boot_area, sizeof(boot_area),
		disk->d_block_count, entries, capacity);
	for (index = 0; index < count; index++)
		entries[index].p_parent = disk;
	return count;
}

const struct partition_scheme partition_scheme_x68k = {
	.name = "x68k",
	.scan = x68k_scan,
};
