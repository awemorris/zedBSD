/* PC/AT MBR primary partition scheme.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/pcat/partition.h"

#define MBR_TABLE 0x1beU
#define MBR_ENTRY_SIZE 16U

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int
mbr_scan(const struct partition_scheme *scheme, struct disk *disk,
    struct partition *entries, unsigned capacity)
{
	uint8_t sector[512];
	unsigned count = capacity < 4U ? capacity : 4U;
	(void)scheme;
	if (disk->d_block_size != 512U || disk_read(disk, 0, 1, sector) != 0)
		return -1;
	if (sector[510] != 0x55U || sector[511] != 0xaaU) return -1;
	for (unsigned index = 0; index < count; index++) {
		const uint8_t *raw = sector + MBR_TABLE + index * MBR_ENTRY_SIZE;
		struct partition *entry = &entries[index];
		uint32_t start = get32(raw + 8), blocks = get32(raw + 12);
		uint8_t type = raw[4];
		entry->p_parent = disk; entry->p_disk = 0; entry->p_index = index;
		entry->p_start_block = start; entry->p_data_block = start;
		entry->p_block_count = 0; entry->p_flags = 0;
		entry->p_label[0]='m'; entry->p_label[1]='b'; entry->p_label[2]='r';
		entry->p_label[3]=(char)('1'+index); entry->p_label[4]='\0';
		if (raw[0] != 0 && raw[0] != 0x80U) continue;
		if (type == 0 || blocks == 0 || type == 0x05U || type == 0x0fU ||
		    type == 0x85U || type == 0xeeU) continue;
		if ((uint64_t)start + blocks > disk->d_block_count) continue;
		entry->p_block_count = blocks;
		if (raw[0] == 0x80U) entry->p_flags |= PARTITION_BOOTABLE;
	}
	return (int)count;
}

const struct partition_scheme partition_scheme_mbr = {
	.name = "mbr", .scan = mbr_scan,
};
