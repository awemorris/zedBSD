/* PC/AT MBR primary partition scheme.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/mbr-partition.h"

#include <string.h>

#define MBR_TABLE 0x1beU
#define MBR_ENTRY_SIZE 16U

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4U) << 32);
}

static char hex(unsigned value)
{
	return (char)(value < 10U ? '0' + value : 'a' + value - 10U);
}

static void mbr_partuuid(char output[PARTITION_UUID_MAX], uint32_t signature,
	unsigned index)
{
	unsigned at = 0, shift;
	if (signature == 0) { output[0] = '\0'; return; }
	for (shift = 32; shift != 0; shift -= 4)
		output[at++] = hex((signature >> (shift - 4)) & 15U);
	output[at++] = '-';
	output[at++] = hex(((index + 1U) >> 4) & 15U);
	output[at++] = hex((index + 1U) & 15U);
	output[at] = '\0';
}

static void guid_text(char output[PARTITION_UUID_MAX], const uint8_t *guid)
{
	static const uint8_t order[16] = {
		3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15
	};
	unsigned at = 0, i;
	for (i = 0; i < 16U; i++) {
		if (i == 4U || i == 6U || i == 8U || i == 10U) output[at++] = '-';
		output[at++] = hex(guid[order[i]] >> 4);
		output[at++] = hex(guid[order[i]] & 15U);
	}
	output[at] = '\0';
}

static void apply_gpt_identity(struct disk *disk, struct partition *entries,
	unsigned count)
{
	uint8_t header[512], table[512];
	uint64_t table_lba;
	unsigned entry_size, entry_count, i, slot;
	if (disk_read(disk, 1, 1, header) != 0 ||
	    memcmp(header, "EFI PART", 8U) != 0) return;
	table_lba = get64(header + 72U);
	entry_count = get32(header + 80U);
	entry_size = get32(header + 84U);
	if (entry_size < 128U || entry_size > sizeof(table) || entry_count == 0 ||
	    table_lba >= disk->d_block_count || disk_read(disk, table_lba, 1, table) != 0)
		return;
	if (entry_count > sizeof(table) / entry_size)
		entry_count = sizeof(table) / entry_size;
	for (i = 0; i < entry_count; i++) {
		const uint8_t *raw = table + i * entry_size;
		uint64_t first = get64(raw + 32U), last = get64(raw + 40U);
		if (first == 0 || last < first) continue;
		for (slot = 0; slot < count; slot++) {
			struct partition *part = &entries[slot];
			unsigned at = 0, character;
			if (part->p_block_count == 0 || part->p_start_block != first ||
			    part->p_block_count != last - first + 1U) continue;
			guid_text(part->p_uuid, raw + 16U);
			part->p_flags |= PARTITION_HAS_UUID;
			for (character = 0; character < 36U &&
			    at + 1U < PARTITION_LABEL_MAX; character++) {
				uint16_t c = (uint16_t)raw[56U + character * 2U] |
				    (uint16_t)((uint16_t)raw[57U + character * 2U] << 8);
				if (c == 0) break;
				part->p_label[at++] = c >= 0x20U && c <= 0x7eU ?
				    (char)c : '?';
			}
			part->p_label[at] = '\0';
			if (at != 0) part->p_flags |= PARTITION_HAS_LABEL;
		}
	}
}

static int
mbr_scan(const struct partition_scheme *scheme, struct disk *disk,
    struct partition *entries, unsigned capacity)
{
	uint8_t sector[512];
	uint32_t signature;
	unsigned count = capacity < 4U ? capacity : 4U;
	(void)scheme;
	if (disk->d_block_size != 512U || disk_read(disk, 0, 1, sector) != 0)
		return -1;
	if (sector[510] != 0x55U || sector[511] != 0xaaU)
		return -1;
	signature = get32(sector + 0x1b8U);
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
		mbr_partuuid(entry->p_uuid, signature, index);
		if (entry->p_uuid[0] != '\0') entry->p_flags |= PARTITION_HAS_UUID;
		if (raw[0] != 0 && raw[0] != 0x80U) continue;
		if (type == 0 || blocks == 0 || type == 0x05U || type == 0x0fU ||
		    type == 0x85U || type == 0xeeU) continue;
		if ((uint64_t)start + blocks > disk->d_block_count) continue;
		entry->p_block_count = blocks;
		if (raw[0] == 0x80U) entry->p_flags |= PARTITION_BOOTABLE;
	}
	apply_gpt_identity(disk, entries, count);
	return (int)count;
}

const struct partition_scheme partition_scheme_mbr = {
	.name = "mbr", .scan = mbr_scan,
};
