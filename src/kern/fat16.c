/*
 * Boots FAT12/FAT16 driver
 *
 * The directory, chain, and write logic is shared; only the FAT entry
 * width differs, dispatched on the mounted type.
 *
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/fat.h"
#include "kern/fat16.h"

#define FAT16_RESERVED_CLUSTER 0xfff0U
#define FAT16_END_OF_CHAIN 0xffffU
#define FAT12_RESERVED_CLUSTER 0xff0U
#define FAT12_END_OF_CHAIN 0xfffU
#define FAT16_DIRECTORY_ENTRY_SIZE 32U
#define FAT16_ENTRIES_PER_SECTOR (512U / FAT16_DIRECTORY_ENTRY_SIZE)

struct fat16_directory {
	/* Cluster zero denotes FAT16's fixed root-directory table. */
	uint32_t first_cluster;
};

static void copy_bytes(void *destination, const void *source, uint32_t length)
{
	uint8_t *output = destination;
	const uint8_t *input = source;

	while (length--)
		*output++ = *input++;
}

static void clear_bytes(void *destination, uint32_t length)
{
	uint8_t *output = destination;

	while (length--)
		*output++ = 0;
}

static void put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *bytes, uint32_t value)
{
	put16(bytes, (uint16_t)value);
	put16(bytes + 2, (uint16_t)(value >> 16));
}

static enum boots_fs_result fat16_probe(const struct boots_volume *volume)
{
	return boots_fat_probe(volume, BOOTS_FAT16);
}

static enum boots_fs_result fat16_mount(
	struct boots_filesystem *filesystem)
{
	struct boots_fat_state *fat;
	enum boots_fs_result result;
	uint32_t fat_entries;

	result = boots_fat_mount(filesystem, BOOTS_FAT16);
	if (result != BOOTS_FS_OK)
		return result;
	fat = boots_fat_state(filesystem);
	if (!fat->root_entries || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U)
		return BOOTS_FS_CORRUPT;
	fat_entries = fat->fat_sectors * 512U / 2U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT16_RESERVED_CLUSTER)
		return BOOTS_FS_CORRUPT;
	return BOOTS_FS_OK;
}

static int fat16_valid_cluster(const struct boots_fat_state *fat,
			       uint32_t cluster)
{
	return cluster >= 2U && cluster < fat->cluster_count + 2U;
}

static int fat16_is_end(const struct boots_fat_state *fat, uint32_t cluster)
{
	return cluster >= (fat->type == BOOTS_FAT12 ? 0xff8U : 0xfff8U);
}

static uint32_t fat16_reserved_limit(const struct boots_fat_state *fat)
{
	return fat->type == BOOTS_FAT12 ? FAT12_RESERVED_CLUSTER :
		FAT16_RESERVED_CLUSTER;
}

static uint16_t fat16_end_of_chain(const struct boots_fat_state *fat)
{
	return fat->type == BOOTS_FAT12 ? FAT12_END_OF_CHAIN :
		FAT16_END_OF_CHAIN;
}

static uint32_t fat16_entry_offset(const struct boots_fat_state *fat,
				   uint32_t cluster)
{
	return fat->type == BOOTS_FAT12 ? cluster + cluster / 2U :
		cluster * 2U;
}

static enum boots_fs_result fat16_next_cluster(
	struct boots_filesystem *filesystem, uint32_t cluster,
	uint32_t *next_cluster)
{
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	uint32_t offset;
	const uint8_t *sector;
	enum boots_fs_result result;

	if (!next_cluster || !fat16_valid_cluster(fat, cluster))
		return BOOTS_FS_CORRUPT;
	offset = fat16_entry_offset(fat, cluster);
	result = boots_fat_read_sector_result(filesystem,
					       fat->fat_start + (offset >> 9),
					       &sector);
	if (result != BOOTS_FS_OK)
		return result;
	if (fat->type != BOOTS_FAT12) {
		*next_cluster = boots_fat_get16(sector + (offset & 511U));
		return BOOTS_FS_OK;
	}
	{
		/* A 12-bit entry may straddle a sector boundary, and the
		 * sector cache holds one sector, so latch the first byte
		 * before a second read can evict it. */
		uint8_t low = sector[offset & 511U];
		uint8_t high;
		uint32_t value;

		if ((offset & 511U) == 511U) {
			result = boots_fat_read_sector_result(
				filesystem,
				fat->fat_start + (offset >> 9) + 1U, &sector);
			if (result != BOOTS_FS_OK)
				return result;
			high = sector[0];
		} else {
			high = sector[(offset & 511U) + 1U];
		}
		value = (uint32_t)low | ((uint32_t)high << 8);
		*next_cluster = (cluster & 1U) ? value >> 4 : value & 0xfffU;
	}
	return BOOTS_FS_OK;
}

static enum boots_fs_result fat16_set_entry_byte(
	struct boots_filesystem *filesystem, uint32_t copy_start,
	uint32_t offset, uint8_t keep_mask, uint8_t merge_value)
{
	uint8_t *sector;
	enum boots_fs_result result;

	if ((offset >> 9) > 0xffffffffU - copy_start)
		return BOOTS_FS_CORRUPT;
	result = boots_fat_write_sector_result(
		filesystem, copy_start + (offset >> 9), &sector);
	if (result != BOOTS_FS_OK)
		return result;
	sector[offset & 511U] =
		(uint8_t)((sector[offset & 511U] & keep_mask) | merge_value);
	result = boots_fat_mark_sector_dirty(filesystem);
	if (result == BOOTS_FS_OK)
		result = boots_fat_flush(filesystem);
	return result;
}

static enum boots_fs_result fat16_set_cluster(
	struct boots_filesystem *filesystem, uint32_t cluster, uint16_t value)
{
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	uint32_t offset;
	unsigned copy;

	if (!fat16_valid_cluster(fat, cluster))
		return BOOTS_FS_CORRUPT;
	offset = fat16_entry_offset(fat, cluster);
	for (copy = 0; copy < fat->number_of_fats; copy++) {
		uint32_t copy_start;
		enum boots_fs_result result;

		if (copy > (0xffffffffU - fat->fat_start) / fat->fat_sectors)
			return BOOTS_FS_CORRUPT;
		copy_start = fat->fat_start + copy * fat->fat_sectors;
		if (fat->type != BOOTS_FAT12) {
			uint8_t *sector;

			if ((offset >> 9) > 0xffffffffU - copy_start)
				return BOOTS_FS_CORRUPT;
			result = boots_fat_write_sector_result(
				filesystem, copy_start + (offset >> 9),
				&sector);
			if (result != BOOTS_FS_OK)
				return result;
			put16(sector + (offset & 511U), value);
			result = boots_fat_mark_sector_dirty(filesystem);
			if (result == BOOTS_FS_OK)
				result = boots_fat_flush(filesystem);
			if (result != BOOTS_FS_OK)
				return result;
			continue;
		}
		/* Read-modify-write both bytes of the packed 12-bit entry;
		 * they may live in different sectors. */
		if (cluster & 1U) {
			result = fat16_set_entry_byte(
				filesystem, copy_start, offset, 0x0f,
				(uint8_t)((value << 4) & 0xf0));
			if (result == BOOTS_FS_OK)
				result = fat16_set_entry_byte(
					filesystem, copy_start, offset + 1U,
					0x00, (uint8_t)(value >> 4));
		} else {
			result = fat16_set_entry_byte(
				filesystem, copy_start, offset, 0x00,
				(uint8_t)value);
			if (result == BOOTS_FS_OK)
				result = fat16_set_entry_byte(
					filesystem, copy_start, offset + 1U,
					0xf0, (uint8_t)((value >> 8) & 0x0f));
		}
		if (result != BOOTS_FS_OK)
			return result;
	}
	return BOOTS_FS_OK;
}

static enum boots_fs_result fat16_validate_chain(
	struct boots_filesystem *filesystem, uint32_t first_cluster)
{
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	uint32_t slow = first_cluster, fast = first_cluster;
	uint32_t steps;

	if (!fat16_valid_cluster(fat, first_cluster))
		return BOOTS_FS_CORRUPT;
	for (steps = 0; steps <= fat->cluster_count; steps++) {
		uint32_t next;
		enum boots_fs_result result;

		result = fat16_next_cluster(filesystem, slow, &next);
		if (result != BOOTS_FS_OK)
			return result;
		if (fat16_is_end(fat, next))
			return BOOTS_FS_OK;
		if (!fat16_valid_cluster(fat, next))
			return BOOTS_FS_CORRUPT;
		slow = next;

		result = fat16_next_cluster(filesystem, fast, &next);
		if (result != BOOTS_FS_OK)
			return result;
		if (fat16_is_end(fat, next))
			return BOOTS_FS_OK;
		if (!fat16_valid_cluster(fat, next))
			return BOOTS_FS_CORRUPT;
		fast = next;
		result = fat16_next_cluster(filesystem, fast, &next);
		if (result != BOOTS_FS_OK)
			return result;
		if (fat16_is_end(fat, next))
			return BOOTS_FS_OK;
		if (!fat16_valid_cluster(fat, next))
			return BOOTS_FS_CORRUPT;
		fast = next;
		if (slow == fast)
			return BOOTS_FS_CORRUPT;
	}
	return BOOTS_FS_CORRUPT;
}

static enum boots_fs_result fat16_free_chain(
	struct boots_filesystem *filesystem, uint32_t first_cluster)
{
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	uint32_t cluster = first_cluster;
	uint32_t steps;
	enum boots_fs_result result;

	if (!first_cluster)
		return BOOTS_FS_OK;
	result = fat16_validate_chain(filesystem, first_cluster);
	if (result != BOOTS_FS_OK)
		return result;
	for (steps = 0; steps < fat->cluster_count; steps++) {
		uint32_t next;

		result = fat16_next_cluster(filesystem, cluster, &next);
		if (result != BOOTS_FS_OK)
			return result;
		result = fat16_set_cluster(filesystem, cluster, 0);
		if (result != BOOTS_FS_OK)
			return result;
		if (fat16_is_end(fat, next))
			return BOOTS_FS_OK;
		cluster = next;
	}
	return BOOTS_FS_CORRUPT;
}

static enum boots_fs_result fat16_find_free_cluster(
	struct boots_filesystem *filesystem, uint32_t *free_cluster)
{
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	uint32_t start = fat->allocation_hint;
	uint32_t index;

	if (!free_cluster || !fat->cluster_count)
		return BOOTS_FS_CORRUPT;
	if (!fat16_valid_cluster(fat, start))
		start = 2;
	for (index = 0; index < fat->cluster_count; index++) {
		uint32_t cluster = 2U +
			((start - 2U + index) % fat->cluster_count);
		uint32_t value;
		enum boots_fs_result result;

		result = fat16_next_cluster(filesystem, cluster, &value);
		if (result != BOOTS_FS_OK)
			return result;
		if (!value) {
			*free_cluster = cluster;
			fat->allocation_hint = cluster + 1U;
			if (!fat16_valid_cluster(fat, fat->allocation_hint))
				fat->allocation_hint = 2;
			return BOOTS_FS_OK;
		}
	}
	return BOOTS_FS_NO_SPACE;
}

static enum boots_fs_result fat16_zero_cluster(
	struct boots_filesystem *filesystem, uint32_t cluster)
{
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	uint32_t index;

	for (index = 0; index < fat->sectors_per_cluster; index++) {
		uint32_t lba;
		uint8_t *sector;
		enum boots_fs_result result;

		result = boots_fat_cluster_lba(filesystem, cluster, index, &lba);
		if (result != BOOTS_FS_OK)
			return result;
		result = boots_fat_write_sector_result(filesystem, lba, &sector);
		if (result != BOOTS_FS_OK)
			return result;
		clear_bytes(sector, 512);
		result = boots_fat_mark_sector_dirty(filesystem);
		if (result == BOOTS_FS_OK)
			result = boots_fat_flush(filesystem);
		if (result != BOOTS_FS_OK)
			return result;
	}
	return BOOTS_FS_OK;
}

static enum boots_fs_result fat16_allocate_cluster(
	struct boots_filesystem *filesystem, uint32_t *cluster)
{
	enum boots_fs_result result;

	result = fat16_find_free_cluster(filesystem, cluster);
	if (result != BOOTS_FS_OK)
		return result;
	result = fat16_zero_cluster(filesystem, *cluster);
	if (result != BOOTS_FS_OK)
		return result;
	return fat16_set_cluster(filesystem, *cluster,
				 fat16_end_of_chain(boots_fat_state(filesystem)));
}

static enum boots_fs_result fat16_directory_entry(
	struct boots_filesystem *filesystem,
	const struct fat16_directory *directory, uint32_t index,
	uint32_t *entry_lba, uint16_t *entry_offset, const uint8_t **raw)
{
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	uint32_t lba;
	uint16_t offset;
	const uint8_t *sector;
	enum boots_fs_result result;

	if (directory->first_cluster == 0) {
		if (index >= fat->root_entries)
			return BOOTS_FS_NOT_FOUND;
		lba = fat->root_start + index / FAT16_ENTRIES_PER_SECTOR;
		offset = (uint16_t)((index % FAT16_ENTRIES_PER_SECTOR) *
				    FAT16_DIRECTORY_ENTRY_SIZE);
	} else {
		uint32_t entries_per_cluster =
			(uint32_t)fat->sectors_per_cluster *
			FAT16_ENTRIES_PER_SECTOR;
		uint32_t cluster = directory->first_cluster;
		uint32_t cluster_index;
		uint32_t sector_index;

		if (!fat16_valid_cluster(fat, cluster) || !entries_per_cluster)
			return BOOTS_FS_CORRUPT;
		cluster_index = index / entries_per_cluster;
		index %= entries_per_cluster;
		while (cluster_index--) {
			uint32_t next;

			result = fat16_next_cluster(filesystem, cluster, &next);
			if (result != BOOTS_FS_OK)
				return result;
			if (fat16_is_end(fat, next))
				return BOOTS_FS_NOT_FOUND;
			if (!fat16_valid_cluster(fat, next))
				return BOOTS_FS_CORRUPT;
			cluster = next;
		}
		sector_index = index / FAT16_ENTRIES_PER_SECTOR;
		result = boots_fat_cluster_lba(filesystem, cluster,
					       sector_index, &lba);
		if (result != BOOTS_FS_OK)
			return result;
		offset = (uint16_t)((index % FAT16_ENTRIES_PER_SECTOR) *
				    FAT16_DIRECTORY_ENTRY_SIZE);
	}
	result = boots_fat_read_sector_result(filesystem, lba, &sector);
	if (result != BOOTS_FS_OK)
		return result;
	*entry_lba = lba;
	*entry_offset = offset;
	*raw = sector + offset;
	return BOOTS_FS_OK;
}

static enum boots_fs_result fat16_find_entry(
	struct boots_filesystem *filesystem,
	const struct fat16_directory *directory, const char canonical[11],
	uint32_t *entry_lba, uint16_t *entry_offset,
	uint32_t *free_lba, uint16_t *free_offset)
{
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	uint32_t limit = directory->first_cluster == 0 ? fat->root_entries :
		fat->cluster_count * (uint32_t)fat->sectors_per_cluster *
		FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;
	int have_free = 0;

	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		enum boots_fs_result result;

		result = fat16_directory_entry(filesystem, directory, index,
					       &lba, &offset, &raw);
		if (result == BOOTS_FS_NOT_FOUND)
			break;
		if (result != BOOTS_FS_OK)
			return result;
		if ((raw[0] == 0 || raw[0] == 0xe5) && !have_free) {
			*free_lba = lba;
			*free_offset = offset;
			have_free = 1;
		}
		if (!raw[0])
			break;
		if (raw[0] == 0xe5 || raw[11] == 0x0f || (raw[11] & 0x08U))
			continue;
		if (boots_fat_name_matches(raw, canonical)) {
			*entry_lba = lba;
			*entry_offset = offset;
			return BOOTS_FS_OK;
		}
	}
	return have_free ? BOOTS_FS_NOT_FOUND : BOOTS_FS_NO_SPACE;
}

static enum boots_fs_result fat16_resolve_parent(
	struct boots_filesystem *filesystem, const char *path,
	struct fat16_directory *parent, char canonical[11])
{
	const char *cursor = path;

	parent->first_cluster = 0;
	if (*cursor == '/')
		cursor++;
	if (!*cursor)
		return BOOTS_FS_INVALID_PATH;
	for (;;) {
		char component[13];
		unsigned length = 0;
		const char *separator;
		uint32_t lba = 0, free_lba = 0;
		uint16_t offset = 0, free_offset = 0;
		const uint8_t *sector;
		enum boots_fs_result result;

		separator = cursor;
		while (*separator && *separator != '/')
			separator++;
		length = (unsigned)(separator - cursor);
		if (!length || length >= sizeof(component))
			return BOOTS_FS_INVALID_PATH;
		copy_bytes(component, cursor, length);
		component[length] = '\0';
		if (!boots_fat_short_name(component, canonical))
			return BOOTS_FS_INVALID_PATH;
		if (!*separator)
			return BOOTS_FS_OK;
		cursor = separator + 1;
		if (!*cursor)
			return BOOTS_FS_INVALID_PATH;
		result = fat16_find_entry(filesystem, parent, canonical, &lba,
					  &offset, &free_lba, &free_offset);
		if (result != BOOTS_FS_OK)
			return result == BOOTS_FS_NO_SPACE ?
				BOOTS_FS_NOT_FOUND : result;
		result = boots_fat_read_sector_result(filesystem, lba, &sector);
		if (result != BOOTS_FS_OK)
			return result;
		if (!(sector[offset + 11] & 0x10U))
			return BOOTS_FS_NOT_FOUND;
		parent->first_cluster = boots_fat_get16(sector + offset + 26);
		if (!fat16_valid_cluster(boots_fat_state(filesystem),
					 parent->first_cluster))
			return BOOTS_FS_CORRUPT;
	}
}

static enum boots_fs_result fat16_resolve_entry(
	struct boots_filesystem *filesystem, const char *path,
	uint32_t *lba, uint16_t *offset, const uint8_t **raw)
{
	struct fat16_directory parent;
	char canonical[11];
	uint32_t free_lba = 0;
	uint16_t free_offset = 0;
	const uint8_t *sector;
	enum boots_fs_result result;

	result = fat16_resolve_parent(filesystem, path, &parent, canonical);
	if (result != BOOTS_FS_OK)
		return result;
	result = fat16_find_entry(filesystem, &parent, canonical, lba, offset,
				  &free_lba, &free_offset);
	if (result != BOOTS_FS_OK)
		return result == BOOTS_FS_NO_SPACE ? BOOTS_FS_NOT_FOUND : result;
	result = boots_fat_read_sector_result(filesystem, *lba, &sector);
	if (result != BOOTS_FS_OK)
		return result;
	*raw = sector + *offset;
	return BOOTS_FS_OK;
}

static enum boots_fs_result fat16_populate_file(
	struct boots_file *file, uint32_t lba, uint16_t offset,
	const uint8_t raw[32])
{
	struct boots_fat_state *fat = boots_fat_state(file->filesystem);
	struct boots_fat_file_state *state = boots_fat_file_state(file);

	state->first_cluster = boots_fat_get16(raw + 26);
	state->directory_lba = lba;
	state->directory_offset = offset;
	state->directory_dirty = 0;
	file->size = boots_fat_get32(raw + 28);
	if (!file->size && !state->first_cluster)
		return BOOTS_FS_OK;
	return fat16_valid_cluster(fat, state->first_cluster) ?
		BOOTS_FS_OK : BOOTS_FS_CORRUPT;
}

static enum boots_fs_result fat16_open(
	struct boots_filesystem *filesystem, const char *path,
	struct boots_file *file)
{
	uint32_t lba = 0;
	uint16_t offset = 0;
	const uint8_t *raw;
	enum boots_fs_result result;

	result = fat16_resolve_entry(filesystem, path, &lba, &offset, &raw);
	if (result != BOOTS_FS_OK)
		return result;
	if (raw[11] & 0x10U)
		return BOOTS_FS_INVALID_PATH;
	return fat16_populate_file(file, lba, offset, raw);
}

static enum boots_fs_result fat16_flush_file(struct boots_file *file)
{
	struct boots_fat_file_state *state = boots_fat_file_state(file);
	uint8_t *sector;
	enum boots_fs_result result;

	if (!file->filesystem->volume.write)
		return BOOTS_FS_READ_ONLY;
	result = boots_fat_flush(file->filesystem);
	if (result != BOOTS_FS_OK || !state->directory_dirty)
		return result;
	if (file->size > 0xffffffffU)
		return BOOTS_FS_INVALID_ARGUMENT;
	result = boots_fat_write_sector_result(file->filesystem,
						state->directory_lba, &sector);
	if (result != BOOTS_FS_OK)
		return result;
	put16(sector + state->directory_offset + 26,
	      (uint16_t)state->first_cluster);
	put32(sector + state->directory_offset + 28, (uint32_t)file->size);
	result = boots_fat_mark_sector_dirty(file->filesystem);
	if (result == BOOTS_FS_OK)
		result = boots_fat_flush(file->filesystem);
	if (result == BOOTS_FS_OK)
		state->directory_dirty = 0;
	return result;
}

static enum boots_fs_result fat16_cluster_at(
	struct boots_file *file, uint32_t cluster_index, int allocate,
	uint32_t *found_cluster)
{
	struct boots_fat_state *fat = boots_fat_state(file->filesystem);
	struct boots_fat_file_state *state = boots_fat_file_state(file);
	uint32_t cluster = state->first_cluster;
	uint32_t index;
	enum boots_fs_result result;

	if (!cluster) {
		if (!allocate)
			return BOOTS_FS_CORRUPT;
		result = fat16_allocate_cluster(file->filesystem, &cluster);
		if (result != BOOTS_FS_OK)
			return result;
		state->first_cluster = cluster;
		state->directory_dirty = 1;
	}
	if (!fat16_valid_cluster(fat, cluster))
		return BOOTS_FS_CORRUPT;
	for (index = 0; index < cluster_index; index++) {
		uint32_t next;

		if (index >= fat->cluster_count)
			return BOOTS_FS_CORRUPT;
		result = fat16_next_cluster(file->filesystem, cluster, &next);
		if (result != BOOTS_FS_OK)
			return result;
		if (fat16_is_end(fat, next)) {
			if (!allocate)
				return BOOTS_FS_CORRUPT;
			result = fat16_allocate_cluster(file->filesystem, &next);
			if (result != BOOTS_FS_OK)
				return result;
			result = fat16_set_cluster(file->filesystem, cluster,
						   (uint16_t)next);
			if (result != BOOTS_FS_OK)
				return result;
		} else if (!fat16_valid_cluster(fat, next)) {
			return BOOTS_FS_CORRUPT;
		}
		cluster = next;
	}
	*found_cluster = cluster;
	return BOOTS_FS_OK;
}

static enum boots_fs_result fat16_write_bytes(
	struct boots_file *file, uint32_t offset, const uint8_t *input,
	uint32_t length, int zero)
{
	struct boots_fat_state *fat = boots_fat_state(file->filesystem);
	uint32_t cluster_bytes = (uint32_t)fat->sectors_per_cluster * 512U;
	uint32_t position = offset;

	while (length) {
		uint32_t cluster_index = position / cluster_bytes;
		uint32_t in_cluster = position % cluster_bytes;
		uint32_t sector_index = in_cluster / 512U;
		uint32_t within = in_cluster & 511U;
		uint32_t chunk = 512U - within;
		uint32_t cluster, lba;
		uint8_t *sector;
		enum boots_fs_result result;

		if (chunk > length)
			chunk = length;
		result = fat16_cluster_at(file, cluster_index, 1, &cluster);
		if (result != BOOTS_FS_OK)
			return result;
		result = boots_fat_cluster_lba(file->filesystem, cluster,
						sector_index, &lba);
		if (result != BOOTS_FS_OK)
			return result;
		result = boots_fat_write_sector_result(file->filesystem, lba,
						  &sector);
		if (result != BOOTS_FS_OK)
			return result;
		if (zero)
			clear_bytes(sector + within, chunk);
		else
			copy_bytes(sector + within, input, chunk);
		result = boots_fat_mark_sector_dirty(file->filesystem);
		if (result == BOOTS_FS_OK)
			result = boots_fat_flush(file->filesystem);
		if (result != BOOTS_FS_OK)
			return result;
		if (!zero)
			input += chunk;
		position += chunk;
		length -= chunk;
	}
	return BOOTS_FS_OK;
}

static enum boots_fs_result fat16_write(
	struct boots_file *file, uint64_t offset, const void *buffer,
	uint32_t length)
{
	uint64_t end;
	enum boots_fs_result result;

	if (!file->filesystem->volume.write)
		return BOOTS_FS_READ_ONLY;
	if ((!buffer && length) || offset > 0xffffffffU ||
	    (uint64_t)length > 0xffffffffU - offset)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!length)
		return BOOTS_FS_OK;
	if (boots_fat_file_state(file)->first_cluster) {
		result = fat16_validate_chain(file->filesystem,
			boots_fat_file_state(file)->first_cluster);
		if (result != BOOTS_FS_OK)
			return result;
	}
	end = offset + length;
	if (offset > file->size) {
		result = fat16_write_bytes(file, (uint32_t)file->size, 0,
					   (uint32_t)(offset - file->size), 1);
		if (result != BOOTS_FS_OK)
			return result;
	}
	result = fat16_write_bytes(file, (uint32_t)offset, buffer, length, 0);
	if (result != BOOTS_FS_OK)
		return result;
	if (end > file->size) {
		file->size = end;
		boots_fat_file_state(file)->directory_dirty = 1;
	}
	return BOOTS_FS_OK;
}

static enum boots_fs_result fat16_truncate(
	struct boots_file *file, uint64_t size)
{
	struct boots_fat_state *fat = boots_fat_state(file->filesystem);
	struct boots_fat_file_state *state = boots_fat_file_state(file);
	uint32_t old_first = state->first_cluster;
	uint64_t old_size = file->size;
	enum boots_fs_result result;

	if (!file->filesystem->volume.write)
		return BOOTS_FS_READ_ONLY;
	if (size > 0xffffffffU)
		return BOOTS_FS_INVALID_ARGUMENT;
	/* A zero-length file may still own a cluster chain.  Creating an
	 * existing file has truncate semantics and must release that chain. */
	if (size == file->size && (size || !state->first_cluster))
		return BOOTS_FS_OK;
	if (state->first_cluster) {
		result = fat16_validate_chain(file->filesystem,
					      state->first_cluster);
		if (result != BOOTS_FS_OK)
			return result;
	}
	if (size > file->size) {
		result = fat16_write_bytes(file, (uint32_t)file->size, 0,
					   (uint32_t)(size - file->size), 1);
		if (result != BOOTS_FS_OK)
			return result;
		file->size = size;
		state->directory_dirty = 1;
		return BOOTS_FS_OK;
	}
	if (!size) {
		state->first_cluster = 0;
		file->size = 0;
		state->directory_dirty = 1;
		result = fat16_flush_file(file);
		if (result != BOOTS_FS_OK) {
			state->first_cluster = old_first;
			file->size = old_size;
			state->directory_dirty = 1;
			return result;
		}
		return fat16_free_chain(file->filesystem, old_first);
	}
	{
		uint32_t cluster_bytes = (uint32_t)fat->sectors_per_cluster * 512U;
		uint32_t keep_index = ((uint32_t)size - 1U) / cluster_bytes;
		uint32_t keep, tail;

		result = fat16_cluster_at(file, keep_index, 0, &keep);
		if (result != BOOTS_FS_OK)
			return result;
		result = fat16_next_cluster(file->filesystem, keep, &tail);
		if (result != BOOTS_FS_OK)
			return result;
		file->size = size;
		state->directory_dirty = 1;
		result = fat16_flush_file(file);
		if (result != BOOTS_FS_OK)
			return result;
		if (fat16_is_end(fat, tail))
			return BOOTS_FS_OK;
		if (!fat16_valid_cluster(fat, tail))
			return BOOTS_FS_CORRUPT;
		result = fat16_set_cluster(file->filesystem, keep,
					   fat16_end_of_chain(fat));
		if (result != BOOTS_FS_OK)
			return result;
		return fat16_free_chain(file->filesystem, tail);
	}
}

static enum boots_fs_result fat16_create(
	struct boots_filesystem *filesystem, const char *path,
	struct boots_file *file)
{
	struct fat16_directory parent;
	char canonical[11];
	uint32_t lba = 0, free_lba = 0;
	uint16_t offset = 0, free_offset = 0;
	uint8_t *sector;
	enum boots_fs_result result;

	if (!filesystem->volume.write)
		return BOOTS_FS_READ_ONLY;
	result = fat16_resolve_parent(filesystem, path, &parent, canonical);
	if (result != BOOTS_FS_OK)
		return result;
	result = fat16_find_entry(filesystem, &parent, canonical, &lba, &offset,
				  &free_lba, &free_offset);
	if (result == BOOTS_FS_OK) {
		const uint8_t *read_sector;

		result = boots_fat_read_sector_result(filesystem, lba,
						       &read_sector);
		if (result != BOOTS_FS_OK)
			return result;
		if (read_sector[offset + 11] & 0x10U)
			return BOOTS_FS_INVALID_PATH;
		result = fat16_populate_file(file, lba, offset,
					     read_sector + offset);
		if (result != BOOTS_FS_OK)
			return result;
		return fat16_truncate(file, 0);
	}
	if (result != BOOTS_FS_NOT_FOUND)
		return result;
	result = boots_fat_write_sector_result(filesystem, free_lba, &sector);
	if (result != BOOTS_FS_OK)
		return result;
	clear_bytes(sector + free_offset, 32);
	copy_bytes(sector + free_offset, canonical, 11);
	sector[free_offset + 11] = 0x20;
	result = boots_fat_mark_sector_dirty(filesystem);
	if (result == BOOTS_FS_OK)
		result = boots_fat_flush(filesystem);
	if (result != BOOTS_FS_OK)
		return result;
	return fat16_populate_file(file, free_lba, free_offset,
				   sector + free_offset);
}

static enum boots_fs_result fat16_read(
	struct boots_file *file, uint64_t offset, void *buffer, uint32_t length,
	boots_read_progress_t progress, void *progress_context)
{
	return boots_fat_read_chain(
		file, offset, buffer, length, progress, progress_context,
		fat16_next_cluster,
		fat16_reserved_limit(boots_fat_state(file->filesystem)));
}

static enum boots_fs_result fat16_readdir(
	struct boots_filesystem *filesystem, const char *path, unsigned wanted,
	struct boots_dirent *entry)
{
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	struct fat16_directory directory = { 0 };
	uint32_t limit;
	unsigned visible = 0;
	uint32_t index;

	if (*path && !(path[0] == '/' && !path[1])) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		enum boots_fs_result result = fat16_resolve_entry(
			filesystem, path, &lba, &offset, &raw);

		if (result != BOOTS_FS_OK)
			return result;
		if (!(raw[11] & 0x10U))
			return BOOTS_FS_INVALID_PATH;
		directory.first_cluster = boots_fat_get16(raw + 26);
		if (!fat16_valid_cluster(fat, directory.first_cluster))
			return BOOTS_FS_CORRUPT;
	}
	limit = directory.first_cluster == 0 ? fat->root_entries :
		fat->cluster_count * (uint32_t)fat->sectors_per_cluster *
		FAT16_ENTRIES_PER_SECTOR;
	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		enum boots_fs_result result;

		result = fat16_directory_entry(filesystem, &directory, index,
					       &lba, &offset, &raw);
		if (result == BOOTS_FS_NOT_FOUND)
			return result;
		if (result != BOOTS_FS_OK)
			return result;
		if (!raw[0])
			return BOOTS_FS_NOT_FOUND;
		if (raw[0] == 0xe5 || raw[11] == 0x0f ||
		    (raw[11] & 0x08U) || raw[0] == '.')
			continue;
		if (visible++ != wanted)
			continue;
		boots_fat_decode_dirent(raw, entry);
		return BOOTS_FS_OK;
	}
	return BOOTS_FS_NOT_FOUND;
}

static enum boots_fs_result fat16_stat(
	struct boots_filesystem *filesystem, const char *path,
	struct boots_dirent *entry)
{
	uint32_t lba = 0;
	uint16_t offset = 0;
	const uint8_t *raw;
	enum boots_fs_result result;

	result = fat16_resolve_entry(filesystem, path, &lba, &offset, &raw);
	if (result != BOOTS_FS_OK)
		return result;
	boots_fat_decode_dirent(raw, entry);
	return BOOTS_FS_OK;
}

static enum boots_fs_result fat16_contiguous_lba(
	struct boots_file *file, uint32_t *absolute_lba)
{
	return boots_fat_contiguous_lba(file, absolute_lba,
					 fat16_next_cluster);
}

static enum boots_fs_result fat12_probe(const struct boots_volume *volume)
{
	return boots_fat_probe(volume, BOOTS_FAT12);
}

static enum boots_fs_result fat12_mount(
	struct boots_filesystem *filesystem)
{
	struct boots_fat_state *fat;
	enum boots_fs_result result;
	uint32_t fat_entries;

	result = boots_fat_mount(filesystem, BOOTS_FAT12);
	if (result != BOOTS_FS_OK)
		return result;
	fat = boots_fat_state(filesystem);
	if (!fat->root_entries || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U)
		return BOOTS_FS_CORRUPT;
	/* Three bytes hold two packed 12-bit entries. */
	fat_entries = fat->fat_sectors * 512U / 3U * 2U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT12_RESERVED_CLUSTER)
		return BOOTS_FS_CORRUPT;
	return BOOTS_FS_OK;
}

const struct boots_filesystem_driver boots_fat12_driver = {
	.name = "fat12",
	.probe = fat12_probe,
	.mount = fat12_mount,
	.create = fat16_create,
	.open = fat16_open,
	.read = fat16_read,
	.write = fat16_write,
	.truncate = fat16_truncate,
	.flush = fat16_flush_file,
	.readdir = fat16_readdir,
	.stat = fat16_stat,
	.contiguous_lba = fat16_contiguous_lba,
};

const struct boots_filesystem_driver boots_fat16_driver = {
	.name = "fat16",
	.probe = fat16_probe,
	.mount = fat16_mount,
	.create = fat16_create,
	.open = fat16_open,
	.read = fat16_read,
	.write = fat16_write,
	.truncate = fat16_truncate,
	.flush = fat16_flush_file,
	.readdir = fat16_readdir,
	.stat = fat16_stat,
	.contiguous_lba = fat16_contiguous_lba,
};
