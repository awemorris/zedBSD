/*
 * zedBSD FAT12/FAT16/FAT32 cluster and directory engine
 *
 * The directory, chain, and write logic is shared; only the FAT entry
 * width differs, dispatched on the mounted type.
 *
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/fat.h"
#include "kern/fat-lfn.h"
#include "kern/fat16.h"
#include "kern/fat32.h"

#define FAT16_RESERVED_CLUSTER 0xfff0U
#define FAT16_END_OF_CHAIN 0xffffU
#define FAT12_RESERVED_CLUSTER 0xff0U
#define FAT12_END_OF_CHAIN 0xfffU
#define FAT32_RESERVED_CLUSTER 0x0ffffff0U
#define FAT32_END_OF_CHAIN 0x0fffffffU
#define FAT16_DIRECTORY_ENTRY_SIZE 32U
#define FAT16_ENTRIES_PER_SECTOR (512U / FAT16_DIRECTORY_ENTRY_SIZE)
#define FAT_MUTATION __attribute__((section(".hightext")))

struct fat16_directory {
	/* Cluster zero denotes FAT16's fixed root-directory table. */
	uint32_t first_cluster;
};

enum fat_name_match {
	FAT_NAME_EXACT,
	FAT_NAME_CASEFOLD,
};

struct fat_component {
	char text[ZEDBSD_PATH_MAX];
	char sfn[11];
};

static int text_equal(const char *left, const char *right)
{
	while (*left && *left == *right) {
		left++;
		right++;
	}
	return *left == *right;
}

static void text_copy(char *destination, const char *source, size_t capacity)
{
	if (capacity == 0)
		return;
	while (--capacity && *source)
		*destination++ = *source++;
	*destination = '\0';
}

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

static enum zedbsd_fs_result fat16_probe(const struct zedbsd_volume *volume)
{
	return zedbsd_fat_probe(volume, ZEDBSD_FAT16);
}

static enum zedbsd_fs_result fat16_mount(
	struct zedbsd_filesystem *filesystem)
{
	struct zedbsd_fat_state *fat;
	enum zedbsd_fs_result result;
	uint32_t fat_entries;

	result = zedbsd_fat_mount(filesystem, ZEDBSD_FAT16);
	if (result != ZEDBSD_FS_OK)
		return result;
	fat = zedbsd_fat_state(filesystem);
	if (!fat->root_entries || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U)
		return ZEDBSD_FS_CORRUPT;
	fat_entries = fat->fat_sectors * 512U / 2U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT16_RESERVED_CLUSTER)
		return ZEDBSD_FS_CORRUPT;
	return ZEDBSD_FS_OK;
}

static int fat16_valid_cluster(const struct zedbsd_fat_state *fat,
			       uint32_t cluster)
{
	return cluster >= 2U && cluster < fat->cluster_count + 2U;
}

static int fat16_is_end(const struct zedbsd_fat_state *fat, uint32_t cluster)
{
	return cluster >= (fat->type == ZEDBSD_FAT12 ? 0xff8U :
		fat->type == ZEDBSD_FAT16 ? 0xfff8U : 0x0ffffff8U);
}

static uint32_t fat16_reserved_limit(const struct zedbsd_fat_state *fat)
{
	return fat->type == ZEDBSD_FAT12 ? FAT12_RESERVED_CLUSTER :
		fat->type == ZEDBSD_FAT16 ? FAT16_RESERVED_CLUSTER :
		FAT32_RESERVED_CLUSTER;
}

static uint32_t fat16_end_of_chain(const struct zedbsd_fat_state *fat)
{
	return fat->type == ZEDBSD_FAT12 ? FAT12_END_OF_CHAIN :
		fat->type == ZEDBSD_FAT16 ? FAT16_END_OF_CHAIN :
		FAT32_END_OF_CHAIN;
}

static uint32_t fat16_entry_offset(const struct zedbsd_fat_state *fat,
				   uint32_t cluster)
{
	return fat->type == ZEDBSD_FAT12 ? cluster + cluster / 2U :
		fat->type == ZEDBSD_FAT16 ? cluster * 2U : cluster * 4U;
}

static enum zedbsd_fs_result fat16_next_cluster(
	struct zedbsd_filesystem *filesystem, uint32_t cluster,
	uint32_t *next_cluster)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t offset;
	const uint8_t *sector;
	enum zedbsd_fs_result result;

	if (!next_cluster || !fat16_valid_cluster(fat, cluster))
		return ZEDBSD_FS_CORRUPT;
	offset = fat16_entry_offset(fat, cluster);
	result = zedbsd_fat_read_sector_result(filesystem,
					       fat->fat_start + (offset >> 9),
					       &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	if (fat->type == ZEDBSD_FAT32) {
		*next_cluster = zedbsd_fat_get32(sector + (offset & 511U)) &
			0x0fffffffU;
		return ZEDBSD_FS_OK;
	}
	if (fat->type == ZEDBSD_FAT16) {
		*next_cluster = zedbsd_fat_get16(sector + (offset & 511U));
		return ZEDBSD_FS_OK;
	}
	{
		/* A 12-bit entry may straddle a sector boundary, and the
		 * sector cache holds one sector, so latch the first byte
		 * before a second read can evict it. */
		uint8_t low = sector[offset & 511U];
		uint8_t high;
		uint32_t value;

		if ((offset & 511U) == 511U) {
			result = zedbsd_fat_read_sector_result(
				filesystem,
				fat->fat_start + (offset >> 9) + 1U, &sector);
			if (result != ZEDBSD_FS_OK)
				return result;
			high = sector[0];
		} else {
			high = sector[(offset & 511U) + 1U];
		}
		value = (uint32_t)low | ((uint32_t)high << 8);
		*next_cluster = (cluster & 1U) ? value >> 4 : value & 0xfffU;
	}
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result fat16_set_entry_byte(
	struct zedbsd_filesystem *filesystem, uint32_t copy_start,
	uint32_t offset, uint8_t keep_mask, uint8_t merge_value)
{
	uint8_t *sector;
	enum zedbsd_fs_result result;

	if ((offset >> 9) > 0xffffffffU - copy_start)
		return ZEDBSD_FS_CORRUPT;
	result = zedbsd_fat_write_sector_result(
		filesystem, copy_start + (offset >> 9), &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	sector[offset & 511U] =
		(uint8_t)((sector[offset & 511U] & keep_mask) | merge_value);
	result = zedbsd_fat_mark_sector_dirty(filesystem);
	if (result == ZEDBSD_FS_OK)
		result = zedbsd_fat_flush(filesystem);
	return result;
}

static enum zedbsd_fs_result fat16_set_cluster(
	struct zedbsd_filesystem *filesystem, uint32_t cluster, uint32_t value)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t offset;
	unsigned copy;

	if (!fat16_valid_cluster(fat, cluster))
		return ZEDBSD_FS_CORRUPT;
	offset = fat16_entry_offset(fat, cluster);
	for (copy = 0; copy < fat->number_of_fats; copy++) {
		uint32_t copy_start;
		enum zedbsd_fs_result result;

		if (copy > (0xffffffffU - fat->fat_start) / fat->fat_sectors)
			return ZEDBSD_FS_CORRUPT;
		copy_start = fat->fat_start + copy * fat->fat_sectors;
		if (fat->type != ZEDBSD_FAT12) {
			uint8_t *sector;

			if ((offset >> 9) > 0xffffffffU - copy_start)
				return ZEDBSD_FS_CORRUPT;
			result = zedbsd_fat_write_sector_result(
				filesystem, copy_start + (offset >> 9),
				&sector);
			if (result != ZEDBSD_FS_OK)
				return result;
			if (fat->type == ZEDBSD_FAT32) {
				uint8_t *entry = sector + (offset & 511U);
				uint32_t old = zedbsd_fat_get32(entry);
				put32(entry, (old & 0xf0000000U) |
				      (value & 0x0fffffffU));
			} else {
				put16(sector + (offset & 511U), (uint16_t)value);
			}
			result = zedbsd_fat_mark_sector_dirty(filesystem);
			if (result == ZEDBSD_FS_OK)
				result = zedbsd_fat_flush(filesystem);
			if (result != ZEDBSD_FS_OK)
				return result;
			continue;
		}
		/* Read-modify-write both bytes of the packed 12-bit entry;
		 * they may live in different sectors. */
		if (cluster & 1U) {
			result = fat16_set_entry_byte(
				filesystem, copy_start, offset, 0x0f,
				(uint8_t)((value << 4) & 0xf0));
			if (result == ZEDBSD_FS_OK)
				result = fat16_set_entry_byte(
					filesystem, copy_start, offset + 1U,
					0x00, (uint8_t)(value >> 4));
		} else {
			result = fat16_set_entry_byte(
				filesystem, copy_start, offset, 0x00,
				(uint8_t)value);
			if (result == ZEDBSD_FS_OK)
				result = fat16_set_entry_byte(
					filesystem, copy_start, offset + 1U,
					0xf0, (uint8_t)((value >> 8) & 0x0f));
		}
		if (result != ZEDBSD_FS_OK)
			return result;
	}
	return ZEDBSD_FS_OK;
}

static uint32_t fat16_dir_cluster(const struct zedbsd_fat_state *fat,
				  const uint8_t raw[32])
{
	uint32_t cluster = zedbsd_fat_get16(raw + 26);

	if (fat->type == ZEDBSD_FAT32)
		cluster |= (uint32_t)zedbsd_fat_get16(raw + 20) << 16;
	return cluster & 0x0fffffffU;
}

static void fat16_put_dir_cluster(const struct zedbsd_fat_state *fat,
				  uint8_t raw[32], uint32_t cluster)
{
	put16(raw + 26, (uint16_t)cluster);
	if (fat->type == ZEDBSD_FAT32)
		put16(raw + 20, (uint16_t)(cluster >> 16));
}

static uint32_t fat16_root_cluster(const struct zedbsd_fat_state *fat)
{
	return fat->type == ZEDBSD_FAT32 ? fat->root_cluster : 0;
}

static enum zedbsd_fs_result fat16_validate_chain(
	struct zedbsd_filesystem *filesystem, uint32_t first_cluster)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t slow = first_cluster, fast = first_cluster;
	uint32_t steps;

	if (!fat16_valid_cluster(fat, first_cluster))
		return ZEDBSD_FS_CORRUPT;
	for (steps = 0; steps <= fat->cluster_count; steps++) {
		uint32_t next;
		enum zedbsd_fs_result result;

		result = fat16_next_cluster(filesystem, slow, &next);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (fat16_is_end(fat, next))
			return ZEDBSD_FS_OK;
		if (!fat16_valid_cluster(fat, next))
			return ZEDBSD_FS_CORRUPT;
		slow = next;

		result = fat16_next_cluster(filesystem, fast, &next);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (fat16_is_end(fat, next))
			return ZEDBSD_FS_OK;
		if (!fat16_valid_cluster(fat, next))
			return ZEDBSD_FS_CORRUPT;
		fast = next;
		result = fat16_next_cluster(filesystem, fast, &next);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (fat16_is_end(fat, next))
			return ZEDBSD_FS_OK;
		if (!fat16_valid_cluster(fat, next))
			return ZEDBSD_FS_CORRUPT;
		fast = next;
		if (slow == fast)
			return ZEDBSD_FS_CORRUPT;
	}
	return ZEDBSD_FS_CORRUPT;
}

static enum zedbsd_fs_result fat16_free_chain(
	struct zedbsd_filesystem *filesystem, uint32_t first_cluster)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t cluster = first_cluster;
	uint32_t steps;
	enum zedbsd_fs_result result;

	if (!first_cluster)
		return ZEDBSD_FS_OK;
	result = fat16_validate_chain(filesystem, first_cluster);
	if (result != ZEDBSD_FS_OK)
		return result;
	for (steps = 0; steps < fat->cluster_count; steps++) {
		uint32_t next;

		result = fat16_next_cluster(filesystem, cluster, &next);
		if (result != ZEDBSD_FS_OK)
			return result;
		result = fat16_set_cluster(filesystem, cluster, 0);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (fat16_is_end(fat, next))
			return ZEDBSD_FS_OK;
		cluster = next;
	}
	return ZEDBSD_FS_CORRUPT;
}

static enum zedbsd_fs_result fat16_find_free_cluster(
	struct zedbsd_filesystem *filesystem, uint32_t *free_cluster)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t start = fat->allocation_hint;
	uint32_t index;

	if (!free_cluster || !fat->cluster_count)
		return ZEDBSD_FS_CORRUPT;
	if (!fat16_valid_cluster(fat, start))
		start = 2;
	for (index = 0; index < fat->cluster_count; index++) {
		uint32_t cluster = 2U +
			((start - 2U + index) % fat->cluster_count);
		uint32_t value;
		enum zedbsd_fs_result result;

		result = fat16_next_cluster(filesystem, cluster, &value);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (!value) {
			*free_cluster = cluster;
			fat->allocation_hint = cluster + 1U;
			if (!fat16_valid_cluster(fat, fat->allocation_hint))
				fat->allocation_hint = 2;
			return ZEDBSD_FS_OK;
		}
	}
	return ZEDBSD_FS_NO_SPACE;
}

static enum zedbsd_fs_result fat16_zero_cluster(
	struct zedbsd_filesystem *filesystem, uint32_t cluster)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t index;

	for (index = 0; index < fat->sectors_per_cluster; index++) {
		uint32_t lba;
		uint8_t *sector;
		enum zedbsd_fs_result result;

		result = zedbsd_fat_cluster_lba(filesystem, cluster, index, &lba);
		if (result != ZEDBSD_FS_OK)
			return result;
		result = zedbsd_fat_write_sector_result(filesystem, lba, &sector);
		if (result != ZEDBSD_FS_OK)
			return result;
		clear_bytes(sector, 512);
		result = zedbsd_fat_mark_sector_dirty(filesystem);
		if (result == ZEDBSD_FS_OK)
			result = zedbsd_fat_flush(filesystem);
		if (result != ZEDBSD_FS_OK)
			return result;
	}
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result fat16_allocate_cluster(
	struct zedbsd_filesystem *filesystem, uint32_t *cluster)
{
	enum zedbsd_fs_result result;

	result = fat16_find_free_cluster(filesystem, cluster);
	if (result != ZEDBSD_FS_OK)
		return result;
	result = fat16_zero_cluster(filesystem, *cluster);
	if (result != ZEDBSD_FS_OK)
		return result;
	return fat16_set_cluster(filesystem, *cluster,
				 fat16_end_of_chain(zedbsd_fat_state(filesystem)));
}

static enum zedbsd_fs_result fat16_directory_entry(
	struct zedbsd_filesystem *filesystem,
	const struct fat16_directory *directory, uint32_t index,
	uint32_t *entry_lba, uint16_t *entry_offset, const uint8_t **raw)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t lba;
	uint16_t offset;
	const uint8_t *sector;
	enum zedbsd_fs_result result;

	if (directory->first_cluster == 0) {
		if (index >= fat->root_entries)
			return ZEDBSD_FS_NOT_FOUND;
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
			return ZEDBSD_FS_CORRUPT;
		cluster_index = index / entries_per_cluster;
		index %= entries_per_cluster;
		while (cluster_index--) {
			uint32_t next;

			result = fat16_next_cluster(filesystem, cluster, &next);
			if (result != ZEDBSD_FS_OK)
				return result;
			if (fat16_is_end(fat, next))
				return ZEDBSD_FS_NOT_FOUND;
			if (!fat16_valid_cluster(fat, next))
				return ZEDBSD_FS_CORRUPT;
			cluster = next;
		}
		sector_index = index / FAT16_ENTRIES_PER_SECTOR;
		result = zedbsd_fat_cluster_lba(filesystem, cluster,
					       sector_index, &lba);
		if (result != ZEDBSD_FS_OK)
			return result;
		offset = (uint16_t)((index % FAT16_ENTRIES_PER_SECTOR) *
				    FAT16_DIRECTORY_ENTRY_SIZE);
	}
	result = zedbsd_fat_read_sector_result(filesystem, lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	*entry_lba = lba;
	*entry_offset = offset;
	*raw = sector + offset;
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result fat16_find_entry(
	struct zedbsd_filesystem *filesystem,
	const struct fat16_directory *directory,
	const struct fat_component *component, enum fat_name_match match,
	uint32_t *entry_lba, uint16_t *entry_offset,
	uint32_t *free_lba, uint16_t *free_offset,
	char found_name[ZEDBSD_PATH_MAX])
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	struct fat_lfn_state lfn;
	uint32_t limit = directory->first_cluster == 0 ? fat->root_entries :
		fat->cluster_count * (uint32_t)fat->sectors_per_cluster *
		FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;
	int have_free = 0;

	fat_lfn_reset(&lfn);

	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		enum zedbsd_fs_result result;

		result = fat16_directory_entry(filesystem, directory, index,
					       &lba, &offset, &raw);
		if (result == ZEDBSD_FS_NOT_FOUND)
			break;
		if (result != ZEDBSD_FS_OK)
			return result;
		if ((raw[0] == 0 || raw[0] == 0xe5) && !have_free &&
		    free_lba != 0 && free_offset != 0) {
			*free_lba = lba;
			*free_offset = offset;
			have_free = 1;
		}
		if (!raw[0]) {
			fat_lfn_reset(&lfn);
			break;
		}
		if (raw[0] == 0xe5) {
			fat_lfn_reset(&lfn);
			continue;
		}
		if (raw[11] == 0x0f) {
			if (fat->type == ZEDBSD_FAT32)
				(void)fat_lfn_feed(&lfn, raw);
			continue;
		}
		if (raw[11] & 0x08U) {
			fat_lfn_reset(&lfn);
			continue;
		}
		if (fat->type == ZEDBSD_FAT32) {
			char decoded[ZEDBSD_PATH_MAX];
			int matches;

			if (!fat_lfn_finish(&lfn, raw, decoded, sizeof(decoded)))
				fat_sfn_decode_preserve(raw, decoded,
						      sizeof(decoded));
			matches = match == FAT_NAME_EXACT ?
				text_equal(decoded, component->text) :
				fat_utf8_casefold_equal(decoded, component->text);
			if (!matches)
				continue;
			if (found_name != 0)
				text_copy(found_name, decoded, ZEDBSD_PATH_MAX);
		} else if (!fat_sfn_equal(raw, component->sfn)) {
			continue;
		}
		{
			*entry_lba = lba;
			*entry_offset = offset;
			return ZEDBSD_FS_OK;
		}
	}
	return have_free ? ZEDBSD_FS_NOT_FOUND : ZEDBSD_FS_NO_SPACE;
}

static enum zedbsd_fs_result fat16_resolve_parent(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct fat16_directory *parent, struct fat_component *component)
{
	const char *cursor = path;

	parent->first_cluster = fat16_root_cluster(zedbsd_fat_state(filesystem));
	if (*cursor == '/')
		cursor++;
	if (!*cursor)
		return ZEDBSD_FS_INVALID_PATH;
	for (;;) {
		unsigned length = 0;
		const char *separator;
		uint32_t lba = 0, free_lba = 0;
		uint16_t offset = 0, free_offset = 0;
		const uint8_t *sector;
		enum zedbsd_fs_result result;

		separator = cursor;
		while (*separator && *separator != '/')
			separator++;
		length = (unsigned)(separator - cursor);
		if (!length || length >= sizeof(component->text))
			return ZEDBSD_FS_INVALID_PATH;
		copy_bytes(component->text, cursor, length);
		component->text[length] = '\0';
		if (zedbsd_fat_state(filesystem)->type != ZEDBSD_FAT32 &&
		    !fat_sfn_encode(component->text, component->sfn))
			return ZEDBSD_FS_INVALID_PATH;
		if (!*separator)
			return ZEDBSD_FS_OK;
		cursor = separator + 1;
		if (!*cursor)
			return ZEDBSD_FS_INVALID_PATH;
		result = fat16_find_entry(filesystem, parent, component,
					  FAT_NAME_EXACT, &lba, &offset,
					  &free_lba, &free_offset, 0);
		if (result != ZEDBSD_FS_OK)
			return result == ZEDBSD_FS_NO_SPACE ?
				ZEDBSD_FS_NOT_FOUND : result;
		result = zedbsd_fat_read_sector_result(filesystem, lba, &sector);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (!(sector[offset + 11] & 0x10U))
			return ZEDBSD_FS_NOT_FOUND;
		parent->first_cluster = fat16_dir_cluster(
			zedbsd_fat_state(filesystem), sector + offset);
		if (!fat16_valid_cluster(zedbsd_fat_state(filesystem),
					 parent->first_cluster))
			return ZEDBSD_FS_CORRUPT;
	}
}

static enum zedbsd_fs_result fat16_resolve_entry(
	struct zedbsd_filesystem *filesystem, const char *path,
	uint32_t *lba, uint16_t *offset, const uint8_t **raw,
	enum fat_name_match match, char found_name[ZEDBSD_PATH_MAX])
{
	struct fat16_directory parent;
	struct fat_component component;
	uint32_t free_lba = 0;
	uint16_t free_offset = 0;
	const uint8_t *sector;
	enum zedbsd_fs_result result;

	result = fat16_resolve_parent(filesystem, path, &parent, &component);
	if (result != ZEDBSD_FS_OK)
		return result;
	result = fat16_find_entry(filesystem, &parent, &component, match,
				  lba, offset, &free_lba, &free_offset,
				  found_name);
	if (result != ZEDBSD_FS_OK)
		return result == ZEDBSD_FS_NO_SPACE ? ZEDBSD_FS_NOT_FOUND : result;
	result = zedbsd_fat_read_sector_result(filesystem, *lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	*raw = sector + *offset;
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result fat16_populate_file(
	struct zedbsd_file *file, uint32_t lba, uint16_t offset,
	const uint8_t raw[32])
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(file->filesystem);
	struct zedbsd_fat_file_state *state = zedbsd_fat_file_state(file);

	state->first_cluster = fat16_dir_cluster(fat, raw);
	state->directory_lba = lba;
	state->directory_offset = offset;
	state->directory_dirty = 0;
	file->size = zedbsd_fat_get32(raw + 28);
	if (!file->size && !state->first_cluster)
		return ZEDBSD_FS_OK;
	return fat16_valid_cluster(fat, state->first_cluster) ?
		ZEDBSD_FS_OK : ZEDBSD_FS_CORRUPT;
}

static enum zedbsd_fs_result fat16_open(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct zedbsd_file *file)
{
	uint32_t lba = 0;
	uint16_t offset = 0;
	const uint8_t *raw;
	enum zedbsd_fs_result result;

	result = fat16_resolve_entry(filesystem, path, &lba, &offset, &raw,
				     FAT_NAME_EXACT, 0);
	if (result != ZEDBSD_FS_OK)
		return result;
	if (raw[11] & 0x10U)
		return ZEDBSD_FS_INVALID_PATH;
	return fat16_populate_file(file, lba, offset, raw);
}

static enum zedbsd_fs_result fat16_flush_file(struct zedbsd_file *file)
{
	struct zedbsd_fat_file_state *state = zedbsd_fat_file_state(file);
	uint8_t *sector;
	enum zedbsd_fs_result result;

	if (!file->filesystem->volume.write)
		return ZEDBSD_FS_READ_ONLY;
	result = zedbsd_fat_flush(file->filesystem);
	if (result != ZEDBSD_FS_OK || !state->directory_dirty)
		return result;
	if (file->size > 0xffffffffU)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	result = zedbsd_fat_write_sector_result(file->filesystem,
						state->directory_lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	fat16_put_dir_cluster(zedbsd_fat_state(file->filesystem),
			      sector + state->directory_offset,
			      state->first_cluster);
	put32(sector + state->directory_offset + 28, (uint32_t)file->size);
	result = zedbsd_fat_mark_sector_dirty(file->filesystem);
	if (result == ZEDBSD_FS_OK)
		result = zedbsd_fat_flush(file->filesystem);
	if (result == ZEDBSD_FS_OK)
		state->directory_dirty = 0;
	return result;
}

static enum zedbsd_fs_result fat16_cluster_at(
	struct zedbsd_file *file, uint32_t cluster_index, int allocate,
	uint32_t *found_cluster)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(file->filesystem);
	struct zedbsd_fat_file_state *state = zedbsd_fat_file_state(file);
	uint32_t cluster = state->first_cluster;
	uint32_t index;
	enum zedbsd_fs_result result;

	if (!cluster) {
		if (!allocate)
			return ZEDBSD_FS_CORRUPT;
		result = fat16_allocate_cluster(file->filesystem, &cluster);
		if (result != ZEDBSD_FS_OK)
			return result;
		state->first_cluster = cluster;
		state->directory_dirty = 1;
	}
	if (!fat16_valid_cluster(fat, cluster))
		return ZEDBSD_FS_CORRUPT;
	for (index = 0; index < cluster_index; index++) {
		uint32_t next;

		if (index >= fat->cluster_count)
			return ZEDBSD_FS_CORRUPT;
		result = fat16_next_cluster(file->filesystem, cluster, &next);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (fat16_is_end(fat, next)) {
			if (!allocate)
				return ZEDBSD_FS_CORRUPT;
			result = fat16_allocate_cluster(file->filesystem, &next);
			if (result != ZEDBSD_FS_OK)
				return result;
			result = fat16_set_cluster(file->filesystem, cluster, next);
			if (result != ZEDBSD_FS_OK)
				return result;
		} else if (!fat16_valid_cluster(fat, next)) {
			return ZEDBSD_FS_CORRUPT;
		}
		cluster = next;
	}
	*found_cluster = cluster;
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result fat16_write_bytes(
	struct zedbsd_file *file, uint32_t offset, const uint8_t *input,
	uint32_t length, int zero)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(file->filesystem);
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
		enum zedbsd_fs_result result;

		if (chunk > length)
			chunk = length;
		result = fat16_cluster_at(file, cluster_index, 1, &cluster);
		if (result != ZEDBSD_FS_OK)
			return result;
		result = zedbsd_fat_cluster_lba(file->filesystem, cluster,
						sector_index, &lba);
		if (result != ZEDBSD_FS_OK)
			return result;
		result = zedbsd_fat_write_sector_result(file->filesystem, lba,
						  &sector);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (zero)
			clear_bytes(sector + within, chunk);
		else
			copy_bytes(sector + within, input, chunk);
		result = zedbsd_fat_mark_sector_dirty(file->filesystem);
		if (result == ZEDBSD_FS_OK)
			result = zedbsd_fat_flush(file->filesystem);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (!zero)
			input += chunk;
		position += chunk;
		length -= chunk;
	}
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result fat16_write(
	struct zedbsd_file *file, uint64_t offset, const void *buffer,
	uint32_t length)
{
	uint64_t end;
	enum zedbsd_fs_result result;

	if (!file->filesystem->volume.write)
		return ZEDBSD_FS_READ_ONLY;
	if ((!buffer && length) || offset > 0xffffffffU ||
	    (uint64_t)length > 0xffffffffU - offset)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!length)
		return ZEDBSD_FS_OK;
	if (zedbsd_fat_file_state(file)->first_cluster) {
		result = fat16_validate_chain(file->filesystem,
			zedbsd_fat_file_state(file)->first_cluster);
		if (result != ZEDBSD_FS_OK)
			return result;
	}
	end = offset + length;
	if (offset > file->size) {
		result = fat16_write_bytes(file, (uint32_t)file->size, 0,
					   (uint32_t)(offset - file->size), 1);
		if (result != ZEDBSD_FS_OK)
			return result;
	}
	result = fat16_write_bytes(file, (uint32_t)offset, buffer, length, 0);
	if (result != ZEDBSD_FS_OK)
		return result;
	if (end > file->size) {
		file->size = end;
		zedbsd_fat_file_state(file)->directory_dirty = 1;
	}
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result fat16_truncate(
	struct zedbsd_file *file, uint64_t size)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(file->filesystem);
	struct zedbsd_fat_file_state *state = zedbsd_fat_file_state(file);
	uint32_t old_first = state->first_cluster;
	uint64_t old_size = file->size;
	enum zedbsd_fs_result result;

	if (!file->filesystem->volume.write)
		return ZEDBSD_FS_READ_ONLY;
	if (size > 0xffffffffU)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	/* A zero-length file may still own a cluster chain.  Creating an
	 * existing file has truncate semantics and must release that chain. */
	if (size == file->size && (size || !state->first_cluster))
		return ZEDBSD_FS_OK;
	if (state->first_cluster) {
		result = fat16_validate_chain(file->filesystem,
					      state->first_cluster);
		if (result != ZEDBSD_FS_OK)
			return result;
	}
	if (size > file->size) {
		result = fat16_write_bytes(file, (uint32_t)file->size, 0,
					   (uint32_t)(size - file->size), 1);
		if (result != ZEDBSD_FS_OK)
			return result;
		file->size = size;
		state->directory_dirty = 1;
		return ZEDBSD_FS_OK;
	}
	if (!size) {
		state->first_cluster = 0;
		file->size = 0;
		state->directory_dirty = 1;
		result = fat16_flush_file(file);
		if (result != ZEDBSD_FS_OK) {
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
		if (result != ZEDBSD_FS_OK)
			return result;
		result = fat16_next_cluster(file->filesystem, keep, &tail);
		if (result != ZEDBSD_FS_OK)
			return result;
		file->size = size;
		state->directory_dirty = 1;
		result = fat16_flush_file(file);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (fat16_is_end(fat, tail))
			return ZEDBSD_FS_OK;
		if (!fat16_valid_cluster(fat, tail))
			return ZEDBSD_FS_CORRUPT;
		result = fat16_set_cluster(file->filesystem, keep,
					   fat16_end_of_chain(fat));
		if (result != ZEDBSD_FS_OK)
			return result;
		return fat16_free_chain(file->filesystem, tail);
	}
}

static enum zedbsd_fs_result fat16_sfn_in_use(
	struct zedbsd_filesystem *filesystem,
	const struct fat16_directory *directory, const uint8_t sfn[11])
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t limit = fat->cluster_count *
		(uint32_t)fat->sectors_per_cluster * FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;

	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		enum zedbsd_fs_result result = fat16_directory_entry(
			filesystem, directory, index, &lba, &offset, &raw);
		(void)lba;
		(void)offset;
		if (result == ZEDBSD_FS_NOT_FOUND)
			return ZEDBSD_FS_NOT_FOUND;
		if (result != ZEDBSD_FS_OK)
			return result;
		if (!raw[0])
			return ZEDBSD_FS_NOT_FOUND;
		if (raw[0] != 0xe5 && raw[11] != 0x0f &&
		    fat_sfn_equal(raw, (const char *)sfn))
			return ZEDBSD_FS_OK;
	}
	return ZEDBSD_FS_CORRUPT;
}

static enum zedbsd_fs_result fat16_extend_directory(
	struct zedbsd_filesystem *filesystem,
	const struct fat16_directory *directory)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t last = directory->first_cluster;
	uint32_t steps, next, added;
	enum zedbsd_fs_result result;

	if (fat->type != ZEDBSD_FAT32 || !fat16_valid_cluster(fat, last))
		return ZEDBSD_FS_NO_SPACE;
	for (steps = 0; steps < fat->cluster_count; steps++) {
		result = fat16_next_cluster(filesystem, last, &next);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (fat16_is_end(fat, next))
			break;
		if (!fat16_valid_cluster(fat, next))
			return ZEDBSD_FS_CORRUPT;
		last = next;
	}
	if (steps == fat->cluster_count)
		return ZEDBSD_FS_CORRUPT;
	result = fat16_allocate_cluster(filesystem, &added);
	if (result != ZEDBSD_FS_OK)
		return result;
	result = fat16_set_cluster(filesystem, last, added);
	if (result != ZEDBSD_FS_OK) {
		(void)fat16_set_cluster(filesystem, added, 0);
		return result;
	}
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result fat16_find_free_run(
	struct zedbsd_filesystem *filesystem,
	const struct fat16_directory *directory, unsigned needed,
	uint32_t *first_index)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t maximum = fat->cluster_count *
		(uint32_t)fat->sectors_per_cluster * FAT16_ENTRIES_PER_SECTOR;
	uint32_t index = 0, run_start = 0;
	unsigned run = 0;
	int after_end = 0;

	while (index < maximum) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		enum zedbsd_fs_result result = fat16_directory_entry(
			filesystem, directory, index, &lba, &offset, &raw);
		(void)lba;
		(void)offset;
		if (result == ZEDBSD_FS_NOT_FOUND) {
			result = fat16_extend_directory(filesystem, directory);
			if (result != ZEDBSD_FS_OK)
				return result;
			continue;
		}
		if (result != ZEDBSD_FS_OK)
			return result;
		if (after_end || raw[0] == 0 || raw[0] == 0xe5) {
			if (run++ == 0)
				run_start = index;
			if (raw[0] == 0)
				after_end = 1;
			if (run == needed) {
				*first_index = run_start;
				return ZEDBSD_FS_OK;
			}
		} else {
			run = 0;
		}
		index++;
	}
	return ZEDBSD_FS_NO_SPACE;
}

static enum zedbsd_fs_result fat16_write_directory_entry(
	struct zedbsd_filesystem *filesystem,
	const struct fat16_directory *directory, uint32_t index,
	const uint8_t entry[32], uint32_t *written_lba,
	uint16_t *written_offset)
{
	uint32_t lba;
	uint16_t offset;
	const uint8_t *raw;
	uint8_t *sector;
	enum zedbsd_fs_result result = fat16_directory_entry(
		filesystem, directory, index, &lba, &offset, &raw);
	(void)raw;
	if (result != ZEDBSD_FS_OK)
		return result;
	result = zedbsd_fat_write_sector_result(filesystem, lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	copy_bytes(sector + offset, entry, 32);
	result = zedbsd_fat_mark_sector_dirty(filesystem);
	if (result == ZEDBSD_FS_OK)
		result = zedbsd_fat_flush(filesystem);
	if (result == ZEDBSD_FS_OK) {
		if (written_lba != 0) *written_lba = lba;
		if (written_offset != 0) *written_offset = offset;
	}
	return result;
}

static void fat16_rollback_directory_entries(
	struct zedbsd_filesystem *filesystem,
	const struct fat16_directory *directory, uint32_t first, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		uint8_t *sector;
		if (fat16_directory_entry(filesystem, directory, first + i,
					  &lba, &offset, &raw) != ZEDBSD_FS_OK)
			continue;
		(void)raw;
		if (zedbsd_fat_write_sector_result(filesystem, lba, &sector) !=
		    ZEDBSD_FS_OK)
			continue;
		sector[offset] = 0xe5;
		(void)zedbsd_fat_mark_sector_dirty(filesystem);
		(void)zedbsd_fat_flush(filesystem);
	}
}

static FAT_MUTATION enum zedbsd_fs_result fat32_create_entry(
	struct zedbsd_filesystem *filesystem,
	const struct fat16_directory *parent,
	const struct fat_component *component, uint8_t attributes,
	uint32_t first_cluster, uint32_t size, uint32_t *entry_lba,
	uint16_t *entry_offset)
{
	uint16_t units[FAT_LFN_MAX_UNITS];
	uint8_t sfn[11], raw[32];
	unsigned unit_count, lfn_count, serial, written = 0, i;
	uint32_t first_index = 0, sfn_lba = 0;
	uint16_t sfn_offset = 0;
	enum zedbsd_fs_result result;

	if (!fat_utf8_to_utf16(component->text, units, &unit_count))
		return ZEDBSD_FS_INVALID_PATH;
	for (serial = 1; serial <= 999999U; serial++) {
		if (!fat_sfn_make_alias(component->text, serial, sfn))
			return ZEDBSD_FS_INVALID_PATH;
		result = fat16_sfn_in_use(filesystem, parent, sfn);
		if (result == ZEDBSD_FS_NOT_FOUND)
			break;
		if (result != ZEDBSD_FS_OK)
			return result;
	}
	if (serial > 999999U)
		return ZEDBSD_FS_NO_SPACE;
	lfn_count = (unit_count + 12U) / 13U;
	result = fat16_find_free_run(filesystem, parent, lfn_count + 1U,
				     &first_index);
	if (result != ZEDBSD_FS_OK)
		return result;
	for (i = 0; i < lfn_count; i++) {
		unsigned ordinal = lfn_count - i;
		fat_lfn_build_entry(raw, units, unit_count, ordinal,
				    fat_lfn_checksum(sfn));
		written++;
		result = fat16_write_directory_entry(filesystem, parent,
			first_index + i, raw, 0, 0);
		if (result != ZEDBSD_FS_OK)
			goto rollback;
	}
	clear_bytes(raw, sizeof(raw));
	copy_bytes(raw, sfn, 11);
	raw[11] = attributes;
	fat16_put_dir_cluster(zedbsd_fat_state(filesystem), raw, first_cluster);
	put32(raw + 28, size);
	written++;
	result = fat16_write_directory_entry(filesystem, parent,
		first_index + lfn_count, raw, &sfn_lba, &sfn_offset);
	if (result != ZEDBSD_FS_OK)
		goto rollback;
	if (entry_lba != 0)
		*entry_lba = sfn_lba;
	if (entry_offset != 0)
		*entry_offset = sfn_offset;
	return ZEDBSD_FS_OK;
rollback:
	fat16_rollback_directory_entries(filesystem, parent, first_index,
				 written);
	return result;
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_insert_entry(struct zedbsd_filesystem *filesystem,
		   const struct fat16_directory *parent,
		   const struct fat_component *component, uint8_t attributes,
		   uint32_t first_cluster, uint32_t size, uint32_t *entry_lba,
		   uint16_t *entry_offset)
{
	uint32_t lba = 0, free_lba = 0;
	uint16_t offset = 0, free_offset = 0;
	uint8_t *sector;
	enum zedbsd_fs_result result;

	result = fat16_find_entry(filesystem, parent, component, FAT_NAME_EXACT,
		&lba, &offset, &free_lba, &free_offset, 0);
	if (result == ZEDBSD_FS_OK)
		return ZEDBSD_FS_EXISTS;
	if (result != ZEDBSD_FS_NOT_FOUND && result != ZEDBSD_FS_NO_SPACE)
		return result;
	if (zedbsd_fat_state(filesystem)->type == ZEDBSD_FAT32) {
		result = fat16_find_entry(filesystem, parent, component,
			FAT_NAME_CASEFOLD, &lba, &offset, &free_lba,
			&free_offset, 0);
		if (result == ZEDBSD_FS_OK)
			return ZEDBSD_FS_EXISTS;
		if (result != ZEDBSD_FS_NOT_FOUND && result != ZEDBSD_FS_NO_SPACE)
			return result;
		return fat32_create_entry(filesystem, parent, component,
			attributes, first_cluster, size, entry_lba, entry_offset);
	}
	if (result == ZEDBSD_FS_NO_SPACE)
		return result;
	result = zedbsd_fat_write_sector_result(filesystem, free_lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	clear_bytes(sector + free_offset, 32);
	copy_bytes(sector + free_offset, component->sfn, 11);
	sector[free_offset + 11] = attributes;
	fat16_put_dir_cluster(zedbsd_fat_state(filesystem),
		sector + free_offset, first_cluster);
	put32(sector + free_offset + 28, size);
	result = zedbsd_fat_mark_sector_dirty(filesystem);
	if (result == ZEDBSD_FS_OK)
		result = zedbsd_fat_flush(filesystem);
	if (result == ZEDBSD_FS_OK) {
		if (entry_lba != 0)
			*entry_lba = free_lba;
		if (entry_offset != 0)
			*entry_offset = free_offset;
	}
	return result;
}

static FAT_MUTATION enum zedbsd_fs_result fat16_create(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct zedbsd_file *file)
{
	struct fat16_directory parent;
	struct fat_component component;
	uint32_t lba = 0, free_lba = 0;
	uint16_t offset = 0, free_offset = 0;
	const uint8_t *sector;
	enum zedbsd_fs_result result;

	if (!filesystem->volume.write)
		return ZEDBSD_FS_READ_ONLY;
	result = fat16_resolve_parent(filesystem, path, &parent, &component);
	if (result != ZEDBSD_FS_OK)
		return result;
	result = fat16_find_entry(filesystem, &parent, &component,
				  FAT_NAME_EXACT, &lba, &offset,
				  &free_lba, &free_offset, 0);
	if (result == ZEDBSD_FS_OK) {
		result = zedbsd_fat_read_sector_result(filesystem, lba,
						       &sector);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (sector[offset + 11] & 0x10U)
			return ZEDBSD_FS_INVALID_PATH;
		result = fat16_populate_file(file, lba, offset,
					     sector + offset);
		if (result != ZEDBSD_FS_OK)
			return result;
		return fat16_truncate(file, 0);
	}
	if (result != ZEDBSD_FS_NOT_FOUND &&
	    !(zedbsd_fat_state(filesystem)->type == ZEDBSD_FAT32 &&
	      result == ZEDBSD_FS_NO_SPACE))
		return result;
	result = fat16_insert_entry(filesystem, &parent, &component, 0x20U,
		0, 0, &lba, &offset);
	if (result != ZEDBSD_FS_OK)
		return result;
	result = zedbsd_fat_read_sector_result(filesystem, lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	return fat16_populate_file(file, lba, offset, sector + offset);
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_mark_deleted(struct zedbsd_filesystem *filesystem, uint32_t lba,
		   uint16_t offset)
{
	uint8_t *sector;
	enum zedbsd_fs_result result;

	result = zedbsd_fat_write_sector_result(filesystem, lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	sector[offset] = 0xe5;
	result = zedbsd_fat_mark_sector_dirty(filesystem);
	return result == ZEDBSD_FS_OK ? zedbsd_fat_flush(filesystem) : result;
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_delete_location(struct zedbsd_filesystem *filesystem,
		      const struct fat16_directory *parent, uint32_t target_lba,
		      uint16_t target_offset)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	uint32_t limit = parent->first_cluster == 0 ? fat->root_entries :
		fat->cluster_count * (uint32_t)fat->sectors_per_cluster *
		FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;

	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		enum zedbsd_fs_result result = fat16_directory_entry(
			filesystem, parent, index, &lba, &offset, &raw);
		if (result != ZEDBSD_FS_OK)
			return result;
		if (lba != target_lba || offset != target_offset)
			continue;
		/* Remove the LFN run first.  A crash during this phase leaves the
		 * file reachable through its short alias; the cluster chain is not
		 * released until the VFS inode reaches its final reference. */
		while (index != 0) {
			uint32_t previous_lba;
			uint16_t previous_offset;
			const uint8_t *previous;
			result = fat16_directory_entry(filesystem, parent, index - 1U,
				&previous_lba, &previous_offset, &previous);
			if (result != ZEDBSD_FS_OK || previous[11] != 0x0fU ||
			    previous[0] == 0xe5)
				break;
			result = fat16_mark_deleted(filesystem, previous_lba,
				previous_offset);
			if (result != ZEDBSD_FS_OK)
				return result;
			index--;
		}
		return fat16_mark_deleted(filesystem, target_lba, target_offset);
	}
	return ZEDBSD_FS_NOT_FOUND;
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_directory_empty(struct zedbsd_filesystem *filesystem,
		      uint32_t first_cluster)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	struct fat16_directory directory = { .first_cluster = first_cluster };
	uint32_t limit = fat->cluster_count *
		(uint32_t)fat->sectors_per_cluster * FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;

	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		enum zedbsd_fs_result result = fat16_directory_entry(
			filesystem, &directory, index, &lba, &offset, &raw);
		(void)lba;
		(void)offset;
		if (result == ZEDBSD_FS_NOT_FOUND)
			return ZEDBSD_FS_OK;
		if (result != ZEDBSD_FS_OK)
			return result;
		if (raw[0] == 0)
			return ZEDBSD_FS_OK;
		if (raw[0] == 0xe5 || raw[11] == 0x0fU ||
		    (raw[11] & 0x08U) != 0 || raw[0] == '.')
			continue;
		return ZEDBSD_FS_NOT_EMPTY;
	}
	return ZEDBSD_FS_CORRUPT;
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_initialize_directory(struct zedbsd_filesystem *filesystem,
			   uint32_t cluster, uint32_t parent_cluster)
{
	uint32_t lba;
	uint8_t *sector;
	uint8_t *raw;
	enum zedbsd_fs_result result;

	result = zedbsd_fat_cluster_lba(filesystem, cluster, 0, &lba);
	if (result != ZEDBSD_FS_OK)
		return result;
	result = zedbsd_fat_write_sector_result(filesystem, lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	clear_bytes(sector, 512);
	raw = sector;
	for (unsigned i = 0; i < 11; i++)
		raw[i] = ' ';
	raw[0] = '.';
	raw[11] = 0x10U;
	fat16_put_dir_cluster(zedbsd_fat_state(filesystem), raw, cluster);
	raw += 32;
	for (unsigned i = 0; i < 11; i++)
		raw[i] = ' ';
	raw[0] = raw[1] = '.';
	raw[11] = 0x10U;
	fat16_put_dir_cluster(zedbsd_fat_state(filesystem), raw, parent_cluster);
	result = zedbsd_fat_mark_sector_dirty(filesystem);
	return result == ZEDBSD_FS_OK ? zedbsd_fat_flush(filesystem) : result;
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_mkdir(struct zedbsd_filesystem *filesystem, const char *path)
{
	struct fat16_directory parent;
	struct fat_component component;
	uint32_t cluster, lba;
	uint16_t offset;
	enum zedbsd_fs_result result;

	if (!filesystem->volume.write)
		return ZEDBSD_FS_READ_ONLY;
	result = fat16_resolve_parent(filesystem, path, &parent, &component);
	if (result != ZEDBSD_FS_OK)
		return result;
	result = fat16_allocate_cluster(filesystem, &cluster);
	if (result != ZEDBSD_FS_OK)
		return result;
	result = fat16_initialize_directory(filesystem, cluster,
		parent.first_cluster);
	if (result == ZEDBSD_FS_OK)
		result = fat16_insert_entry(filesystem, &parent, &component,
			0x10U, cluster, 0, &lba, &offset);
	if (result != ZEDBSD_FS_OK)
		(void)fat16_free_chain(filesystem, cluster);
	return result;
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_remove(struct zedbsd_filesystem *filesystem, const char *path,
	     int directory)
{
	struct fat16_directory parent;
	struct fat_component component;
	uint32_t lba = 0, free_lba = 0, cluster;
	uint16_t offset = 0, free_offset = 0;
	uint8_t raw[32];
	const uint8_t *sector;
	enum zedbsd_fs_result result;

	if (!filesystem->volume.write)
		return ZEDBSD_FS_READ_ONLY;
	result = fat16_resolve_parent(filesystem, path, &parent, &component);
	if (result != ZEDBSD_FS_OK)
		return result;
	result = fat16_find_entry(filesystem, &parent, &component,
		FAT_NAME_EXACT, &lba, &offset, &free_lba, &free_offset, 0);
	if (result != ZEDBSD_FS_OK)
		return result == ZEDBSD_FS_NO_SPACE ? ZEDBSD_FS_NOT_FOUND : result;
	result = zedbsd_fat_read_sector_result(filesystem, lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	copy_bytes(raw, sector + offset, sizeof(raw));
	if (directory != ((raw[11] & 0x10U) != 0))
		return directory ? ZEDBSD_FS_INVALID_PATH :
			ZEDBSD_FS_IS_DIRECTORY;
	cluster = fat16_dir_cluster(zedbsd_fat_state(filesystem), raw);
	if (directory) {
		if (!fat16_valid_cluster(zedbsd_fat_state(filesystem), cluster))
			return ZEDBSD_FS_CORRUPT;
		result = fat16_directory_empty(filesystem, cluster);
		if (result != ZEDBSD_FS_OK)
			return result;
	}
	return fat16_delete_location(filesystem, &parent, lba, offset);
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_unlink(struct zedbsd_filesystem *filesystem, const char *path)
{
	return fat16_remove(filesystem, path, 0);
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_rmdir(struct zedbsd_filesystem *filesystem, const char *path)
{
	return fat16_remove(filesystem, path, 1);
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_update_dotdot(struct zedbsd_filesystem *filesystem,
		    uint32_t directory_cluster, uint32_t parent_cluster)
{
	struct fat16_directory directory = { .first_cluster = directory_cluster };
	uint32_t lba;
	uint16_t offset;
	const uint8_t *raw;
	uint8_t *sector;
	enum zedbsd_fs_result result = fat16_directory_entry(
		filesystem, &directory, 1, &lba, &offset, &raw);
	(void)raw;
	if (result != ZEDBSD_FS_OK)
		return result;
	result = zedbsd_fat_write_sector_result(filesystem, lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	fat16_put_dir_cluster(zedbsd_fat_state(filesystem), sector + offset,
		parent_cluster);
	result = zedbsd_fat_mark_sector_dirty(filesystem);
	return result == ZEDBSD_FS_OK ? zedbsd_fat_flush(filesystem) : result;
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_restore_entry_payload(struct zedbsd_filesystem *filesystem,
			    uint32_t lba, uint16_t offset,
			    const uint8_t raw[32])
{
	uint8_t *sector;
	enum zedbsd_fs_result result;

	result = zedbsd_fat_write_sector_result(filesystem, lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	sector[offset + 11] = raw[11];
	fat16_put_dir_cluster(zedbsd_fat_state(filesystem), sector + offset,
		fat16_dir_cluster(zedbsd_fat_state(filesystem), raw));
	put32(sector + offset + 28, zedbsd_fat_get32(raw + 28));
	result = zedbsd_fat_mark_sector_dirty(filesystem);
	return result == ZEDBSD_FS_OK ? zedbsd_fat_flush(filesystem) : result;
}

static FAT_MUTATION void
fat16_rename_rollback_destination(struct zedbsd_filesystem *filesystem,
				  const struct fat16_directory *parent,
				  uint32_t lba, uint16_t offset,
				  int replacing, const uint8_t target[32])
{
	if (replacing)
		(void)fat16_restore_entry_payload(filesystem, lba, offset, target);
	else
		(void)fat16_delete_location(filesystem, parent, lba, offset);
}

static FAT_MUTATION enum zedbsd_fs_result
fat16_rename(struct zedbsd_filesystem *filesystem, const char *old_path,
	     const char *new_path)
{
	struct fat16_directory old_parent, new_parent;
	struct fat_component old_component, new_component;
	uint32_t old_lba = 0, old_free_lba = 0, new_lba = 0, new_free_lba = 0;
	uint16_t old_offset = 0, old_free_offset = 0;
	uint16_t new_offset = 0, new_free_offset = 0;
	uint8_t source[32], target[32];
	const uint8_t *sector;
	uint8_t *write_sector;
	uint32_t source_cluster;
	int replacing = 0;
	enum zedbsd_fs_result result, target_result;

	if (!filesystem->volume.write)
		return ZEDBSD_FS_READ_ONLY;
	result = fat16_resolve_parent(filesystem, old_path, &old_parent,
		&old_component);
	if (result != ZEDBSD_FS_OK)
		return result;
	result = fat16_find_entry(filesystem, &old_parent, &old_component,
		FAT_NAME_EXACT, &old_lba, &old_offset, &old_free_lba,
		&old_free_offset, 0);
	if (result != ZEDBSD_FS_OK)
		return result == ZEDBSD_FS_NO_SPACE ? ZEDBSD_FS_NOT_FOUND : result;
	result = zedbsd_fat_read_sector_result(filesystem, old_lba, &sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	copy_bytes(source, sector + old_offset, sizeof(source));
	result = fat16_resolve_parent(filesystem, new_path, &new_parent,
		&new_component);
	if (result != ZEDBSD_FS_OK)
		return result;
	target_result = fat16_find_entry(filesystem, &new_parent, &new_component,
		FAT_NAME_EXACT, &new_lba, &new_offset, &new_free_lba,
		&new_free_offset, 0);
	if (target_result == ZEDBSD_FS_OK) {
		if (old_lba == new_lba && old_offset == new_offset)
			return ZEDBSD_FS_OK;
		result = zedbsd_fat_read_sector_result(filesystem, new_lba,
			&sector);
		if (result != ZEDBSD_FS_OK)
			return result;
		copy_bytes(target, sector + new_offset, sizeof(target));
		if (((source[11] ^ target[11]) & 0x10U) != 0)
			return ZEDBSD_FS_INVALID_PATH;
		if ((target[11] & 0x10U) != 0) {
			result = fat16_directory_empty(filesystem,
				fat16_dir_cluster(zedbsd_fat_state(filesystem), target));
			if (result != ZEDBSD_FS_OK)
				return result;
		}
		/* Preserve the destination's spelling/LFN run and replace only its
		 * payload.  Open references to the old target keep their own cluster
		 * state and the VFS marks that inode orphaned after this succeeds. */
		result = zedbsd_fat_write_sector_result(filesystem, new_lba,
			&write_sector);
		if (result != ZEDBSD_FS_OK)
			return result;
		write_sector[new_offset + 11] = source[11];
		fat16_put_dir_cluster(zedbsd_fat_state(filesystem),
			write_sector + new_offset,
			fat16_dir_cluster(zedbsd_fat_state(filesystem), source));
		put32(write_sector + new_offset + 28,
			zedbsd_fat_get32(source + 28));
		result = zedbsd_fat_mark_sector_dirty(filesystem);
		if (result == ZEDBSD_FS_OK)
			result = zedbsd_fat_flush(filesystem);
		if (result != ZEDBSD_FS_OK)
			return result;
		replacing = 1;
	} else if (target_result != ZEDBSD_FS_NOT_FOUND &&
		   target_result != ZEDBSD_FS_NO_SPACE) {
		return target_result;
	}
	source_cluster = fat16_dir_cluster(zedbsd_fat_state(filesystem), source);
	if (!replacing) {
		result = fat16_insert_entry(filesystem, &new_parent, &new_component,
			source[11], source_cluster, zedbsd_fat_get32(source + 28),
			&new_lba, &new_offset);
		if (result != ZEDBSD_FS_OK)
			return result;
	}
	if ((source[11] & 0x10U) != 0 &&
	    old_parent.first_cluster != new_parent.first_cluster) {
		result = fat16_update_dotdot(filesystem, source_cluster,
			new_parent.first_cluster);
		if (result != ZEDBSD_FS_OK) {
			fat16_rename_rollback_destination(filesystem, &new_parent,
				new_lba, new_offset, replacing, target);
			return result;
		}
	}
	result = fat16_delete_location(filesystem, &old_parent,
		old_lba, old_offset);
	if (result != ZEDBSD_FS_OK) {
		if ((source[11] & 0x10U) != 0 &&
		    old_parent.first_cluster != new_parent.first_cluster)
			(void)fat16_update_dotdot(filesystem, source_cluster,
				old_parent.first_cluster);
		fat16_rename_rollback_destination(filesystem, &new_parent,
			new_lba, new_offset, replacing, target);
	}
	return result;
}

static enum zedbsd_fs_result fat16_read(
	struct zedbsd_file *file, uint64_t offset, void *buffer, uint32_t length,
	zedbsd_read_progress_t progress, void *progress_context)
{
	return zedbsd_fat_read_chain(
		file, offset, buffer, length, progress, progress_context,
		fat16_next_cluster,
		fat16_reserved_limit(zedbsd_fat_state(file->filesystem)));
}

static enum zedbsd_fs_result fat16_readdir(
	struct zedbsd_filesystem *filesystem, const char *path, unsigned wanted,
	struct zedbsd_dirent *entry)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	struct fat16_directory directory = {
		.first_cluster = fat16_root_cluster(fat),
	};
	struct fat_lfn_state lfn;
	uint32_t limit;
	unsigned visible = 0;
	uint32_t index;

	if (*path && !(path[0] == '/' && !path[1])) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		enum zedbsd_fs_result result = fat16_resolve_entry(
			filesystem, path, &lba, &offset, &raw,
			FAT_NAME_EXACT, 0);

		if (result != ZEDBSD_FS_OK)
			return result;
		if (!(raw[11] & 0x10U))
			return ZEDBSD_FS_INVALID_PATH;
		directory.first_cluster = fat16_dir_cluster(fat, raw);
		if (!fat16_valid_cluster(fat, directory.first_cluster))
			return ZEDBSD_FS_CORRUPT;
	}
	limit = directory.first_cluster == 0 ? fat->root_entries :
		fat->cluster_count * (uint32_t)fat->sectors_per_cluster *
		FAT16_ENTRIES_PER_SECTOR;
	fat_lfn_reset(&lfn);
	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		enum zedbsd_fs_result result;

		result = fat16_directory_entry(filesystem, &directory, index,
					       &lba, &offset, &raw);
		if (result == ZEDBSD_FS_NOT_FOUND)
			return result;
		if (result != ZEDBSD_FS_OK)
			return result;
		if (!raw[0]) {
			fat_lfn_reset(&lfn);
			return ZEDBSD_FS_NOT_FOUND;
		}
		if (raw[0] == 0xe5) {
			fat_lfn_reset(&lfn);
			continue;
		}
		if (raw[11] == 0x0f) {
			if (fat->type == ZEDBSD_FAT32)
				(void)fat_lfn_feed(&lfn, raw);
			continue;
		}
		if ((raw[11] & 0x08U) || raw[0] == '.') {
			fat_lfn_reset(&lfn);
			continue;
		}
		if (visible++ != wanted)
		{
			fat_lfn_reset(&lfn);
			continue;
		}
		if (fat->type != ZEDBSD_FAT32 ||
		    !fat_lfn_finish(&lfn, raw, entry->name,
				    sizeof(entry->name))) {
			if (fat->type == ZEDBSD_FAT32)
				fat_sfn_decode_preserve(raw, entry->name,
						      sizeof(entry->name));
			else
				fat_sfn_decode_lower(raw, entry);
		}
		entry->size = zedbsd_fat_get32(raw + 28);
		entry->attributes = raw[11];
		return ZEDBSD_FS_OK;
	}
	return ZEDBSD_FS_NOT_FOUND;
}

static enum zedbsd_fs_result fat16_stat(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct zedbsd_dirent *entry)
{
	uint32_t lba = 0;
	uint16_t offset = 0;
	const uint8_t *raw;
	char found_name[ZEDBSD_PATH_MAX];
	enum zedbsd_fs_result result;

	result = fat16_resolve_entry(filesystem, path, &lba, &offset, &raw,
				     FAT_NAME_EXACT, found_name);
	if (result != ZEDBSD_FS_OK)
		return result;
	if (zedbsd_fat_state(filesystem)->type == ZEDBSD_FAT32) {
		text_copy(entry->name, found_name, sizeof(entry->name));
		entry->size = zedbsd_fat_get32(raw + 28);
		entry->attributes = raw[11];
	} else {
		fat_sfn_decode_lower(raw, entry);
	}
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result fat_stat_location_mode(
	struct zedbsd_filesystem *filesystem, const char *path,
	struct zedbsd_dirent *entry, uint32_t *lba, uint16_t *offset,
	uint32_t *first_cluster, uint8_t *attributes,
	enum fat_name_match match)
{
	const uint8_t *raw;
	char found_name[ZEDBSD_PATH_MAX];
	enum zedbsd_fs_result result;

	if (!filesystem || !path || !entry || !lba || !offset ||
	    !first_cluster || !attributes)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	result = fat16_resolve_entry(filesystem, path, lba, offset, &raw,
				     match, found_name);
	if (result != ZEDBSD_FS_OK)
		return result;
	if (zedbsd_fat_state(filesystem)->type == ZEDBSD_FAT32) {
		text_copy(entry->name, found_name, sizeof(entry->name));
		entry->size = zedbsd_fat_get32(raw + 28);
		entry->attributes = raw[11];
	} else {
		fat_sfn_decode_lower(raw, entry);
	}
	*first_cluster = fat16_dir_cluster(zedbsd_fat_state(filesystem), raw);
	*attributes = raw[11];
	return ZEDBSD_FS_OK;
}

enum zedbsd_fs_result
zedbsd_fat_stat_location(struct zedbsd_filesystem *filesystem,
			  const char *path, struct zedbsd_dirent *entry,
			  uint32_t *lba, uint16_t *offset,
			  uint32_t *first_cluster, uint8_t *attributes)
{
	return fat_stat_location_mode(filesystem, path, entry, lba, offset,
				      first_cluster, attributes,
				      FAT_NAME_EXACT);
}

enum zedbsd_fs_result
zedbsd_fat_stat_location_casefold(struct zedbsd_filesystem *filesystem,
				  const char *path,
				  struct zedbsd_dirent *entry,
				  uint32_t *lba, uint16_t *offset,
				  uint32_t *first_cluster,
				  uint8_t *attributes)
{
	if (filesystem == 0 ||
	    zedbsd_fat_state(filesystem)->type != ZEDBSD_FAT32)
		return ZEDBSD_FS_UNSUPPORTED;
	return fat_stat_location_mode(filesystem, path, entry, lba, offset,
				      first_cluster, attributes,
				      FAT_NAME_CASEFOLD);
}

static enum zedbsd_fs_result fat16_contiguous_lba(
	struct zedbsd_file *file, uint32_t *absolute_lba)
{
	return zedbsd_fat_contiguous_lba(file, absolute_lba,
					 fat16_next_cluster);
}

int
zedbsd_fat_file_extents(struct zedbsd_file *file, zedbsd_fat_extent_cb callback,
			void *context)
{
	struct zedbsd_filesystem *filesystem;
	struct zedbsd_fat_state *fat;
	struct zedbsd_fat_file_state *state;
	uint64_t remaining, file_block = 0, run_file = 0, run_disk = 0;
	uint32_t run_count = 0, cluster, steps;

	if (file == 0 || callback == 0 || file->filesystem == 0)
		return -1;
	filesystem = file->filesystem;
	fat = zedbsd_fat_state(filesystem);
	state = zedbsd_fat_file_state(file);
	if (fat->type != ZEDBSD_FAT12 && fat->type != ZEDBSD_FAT16)
		return -1;
	remaining = file->size;
	if (remaining == 0)
		return state->first_cluster == 0 ? 0 : -1;
	cluster = state->first_cluster;
	if (!fat16_valid_cluster(fat, cluster))
		return -1;
	for (steps = 0; steps < fat->cluster_count && remaining != 0; steps++) {
		uint32_t disk_block, blocks = fat->sectors_per_cluster;
		uint32_t next;
		enum zedbsd_fs_result result;

		if ((uint64_t)blocks * 512U > remaining)
			blocks = (uint32_t)((remaining + 511U) / 512U);
		result = zedbsd_fat_cluster_lba(filesystem, cluster, 0,
					       &disk_block);
		if (result != ZEDBSD_FS_OK)
			return -1;
		if (run_count != 0 && run_disk + run_count == disk_block &&
		    run_file + run_count == file_block) {
			run_count += blocks;
		} else {
			if (run_count != 0 &&
			    callback(run_file, run_disk, run_count, context) != 0)
				return -1;
			run_file = file_block;
			run_disk = disk_block;
			run_count = blocks;
		}
		file_block += blocks;
		remaining -= remaining > (uint64_t)blocks * 512U ?
			(uint64_t)blocks * 512U : remaining;
		result = fat16_next_cluster(filesystem, cluster, &next);
		if (result != ZEDBSD_FS_OK)
			return -1;
		if (remaining == 0) {
			if (!fat16_is_end(fat, next))
				return -1;
			break;
		}
		if (!fat16_valid_cluster(fat, next))
			return -1;
		cluster = next;
	}
	if (remaining != 0 || run_count == 0)
		return -1;
	return callback(run_file, run_disk, run_count, context) == 0 ? 0 : -1;
}

enum zedbsd_fs_result
zedbsd_fat_discard_chain_result(struct zedbsd_filesystem *filesystem,
				uint32_t first_cluster)
{
	if (filesystem == NULL)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	return fat16_free_chain(filesystem, first_cluster);
}

static enum zedbsd_fs_result fat12_probe(const struct zedbsd_volume *volume)
{
	return zedbsd_fat_probe(volume, ZEDBSD_FAT12);
}

static enum zedbsd_fs_result fat12_mount(
	struct zedbsd_filesystem *filesystem)
{
	struct zedbsd_fat_state *fat;
	enum zedbsd_fs_result result;
	uint32_t fat_entries;

	result = zedbsd_fat_mount(filesystem, ZEDBSD_FAT12);
	if (result != ZEDBSD_FS_OK)
		return result;
	fat = zedbsd_fat_state(filesystem);
	if (!fat->root_entries || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U)
		return ZEDBSD_FS_CORRUPT;
	/* Three bytes hold two packed 12-bit entries. */
	fat_entries = fat->fat_sectors * 512U / 3U * 2U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT12_RESERVED_CLUSTER)
		return ZEDBSD_FS_CORRUPT;
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result fat32_probe(const struct zedbsd_volume *volume)
{
	return zedbsd_fat_probe(volume, ZEDBSD_FAT32);
}

static enum zedbsd_fs_result fat32_mount(
	struct zedbsd_filesystem *filesystem)
{
	struct zedbsd_fat_state *fat;
	enum zedbsd_fs_result result;
	uint32_t fat_entries;

	result = zedbsd_fat_mount(filesystem, ZEDBSD_FAT32);
	if (result != ZEDBSD_FS_OK)
		return result;
	fat = zedbsd_fat_state(filesystem);
	if (!fat->fat32_layout || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U)
		return ZEDBSD_FS_CORRUPT;
	fat_entries = fat->fat_sectors * 512U / 4U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT32_RESERVED_CLUSTER)
		return ZEDBSD_FS_CORRUPT;
	return fat16_valid_cluster(fat, fat->root_cluster) ?
		ZEDBSD_FS_OK : ZEDBSD_FS_CORRUPT;
}

const struct zedbsd_filesystem_driver zedbsd_fat12_driver = {
	.name = "fat12",
	.probe = fat12_probe,
	.mount = fat12_mount,
	.create = fat16_create,
	.mkdir = fat16_mkdir,
	.unlink = fat16_unlink,
	.rmdir = fat16_rmdir,
	.rename = fat16_rename,
	.open = fat16_open,
	.read = fat16_read,
	.write = fat16_write,
	.truncate = fat16_truncate,
	.flush = fat16_flush_file,
	.readdir = fat16_readdir,
	.stat = fat16_stat,
	.contiguous_lba = fat16_contiguous_lba,
};

const struct zedbsd_filesystem_driver zedbsd_fat16_driver = {
	.name = "fat16",
	.probe = fat16_probe,
	.mount = fat16_mount,
	.create = fat16_create,
	.mkdir = fat16_mkdir,
	.unlink = fat16_unlink,
	.rmdir = fat16_rmdir,
	.rename = fat16_rename,
	.open = fat16_open,
	.read = fat16_read,
	.write = fat16_write,
	.truncate = fat16_truncate,
	.flush = fat16_flush_file,
	.readdir = fat16_readdir,
	.stat = fat16_stat,
	.contiguous_lba = fat16_contiguous_lba,
};

const struct zedbsd_filesystem_driver zedbsd_fat32_driver = {
	.name = "fat32",
	.probe = fat32_probe,
	.mount = fat32_mount,
	.create = fat16_create,
	.mkdir = fat16_mkdir,
	.unlink = fat16_unlink,
	.rmdir = fat16_rmdir,
	.rename = fat16_rename,
	.open = fat16_open,
	.read = fat16_read,
	.write = fat16_write,
	.truncate = fat16_truncate,
	.flush = fat16_flush_file,
	.readdir = fat16_readdir,
	.stat = fat16_stat,
	.contiguous_lba = fat16_contiguous_lba,
};
