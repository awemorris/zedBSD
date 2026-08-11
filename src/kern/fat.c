/*
 * Boots FAT family support
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/fat.h"

#define FAT_PROGRESS_INTERVAL (64U * 1024U)

/* Boots keeps one filesystem mounted, so one shared physical-sector cache is
 * sufficient and avoids consuming stack or private-handle space. */
static uint8_t sector_cache[512];
static uint8_t bpb_cache[512];
static struct boots_filesystem *sector_cache_owner;
static uint32_t sector_cache_lba;
static int sector_cache_valid;
static int sector_cache_dirty;

_Static_assert(sizeof(struct boots_fat_state) <=
	       sizeof(((struct boots_filesystem *)0)->private_data),
	       "FAT state exceeds generic filesystem storage");
_Static_assert(sizeof(struct boots_fat_file_state) <=
	       sizeof(((struct boots_file *)0)->private_data),
	       "FAT file state exceeds generic file storage");

static void copy_bytes(void *destination, const void *source, uint32_t length)
{
	uint8_t *output = destination;
	const uint8_t *input = source;

	while (length--)
		*output++ = *input++;
}

uint16_t boots_fat_get16(const uint8_t *bytes)
{
	return bytes[0] | ((uint16_t)bytes[1] << 8);
}

uint32_t boots_fat_get32(const uint8_t *bytes)
{
	return boots_fat_get16(bytes) |
	       ((uint32_t)boots_fat_get16(bytes + 2) << 16);
}

struct boots_fat_state *boots_fat_state(
	struct boots_filesystem *filesystem)
{
	return (struct boots_fat_state *)filesystem->private_data;
}

struct boots_fat_file_state *boots_fat_file_state(
	struct boots_file *file)
{
	return (struct boots_fat_file_state *)file->private_data;
}

const uint8_t *boots_fat_read_sector(struct boots_filesystem *filesystem,
				      uint32_t lba)
{
	const uint8_t *sector;

	if (boots_fat_read_sector_result(filesystem, lba, &sector) !=
	    BOOTS_FS_OK)
		return 0;
	return sector;
}

enum boots_fs_result boots_fat_flush(
	struct boots_filesystem *filesystem)
{
	enum boots_fs_result result;

	if (!sector_cache_dirty || sector_cache_owner != filesystem)
		return BOOTS_FS_OK;
	result = boots_volume_write_result(&filesystem->volume,
					    sector_cache_lba, sector_cache);
	if (result == BOOTS_FS_OK)
		sector_cache_dirty = 0;
	return result;
}

void boots_fat_invalidate(struct boots_filesystem *filesystem)
{
	if (sector_cache_owner == filesystem) {
		sector_cache_owner = 0;
		sector_cache_valid = 0;
		sector_cache_dirty = 0;
	}
}

enum boots_fs_result boots_fat_read_sector_result(
	struct boots_filesystem *filesystem, uint32_t lba,
	const uint8_t **sector)
{
	enum boots_fs_result result;
	struct boots_fat_state *fat;

	if (!filesystem || !sector)
		return BOOTS_FS_INVALID_ARGUMENT;
	fat = boots_fat_state(filesystem);
	if (lba >= fat->total_sectors)
		return BOOTS_FS_CORRUPT;
	if (sector_cache_owner == filesystem && sector_cache_valid &&
	    sector_cache_lba == lba) {
		*sector = sector_cache;
		return BOOTS_FS_OK;
	}
	if (sector_cache_dirty && sector_cache_owner != 0) {
		result = boots_fat_flush(sector_cache_owner);
		if (result != BOOTS_FS_OK)
			return result;
	}
	sector_cache_owner = filesystem;
	sector_cache_valid = 0;
	sector_cache_dirty = 0;
	result = boots_volume_read_result(&filesystem->volume, lba,
					  sector_cache);
	if (result != BOOTS_FS_OK)
		return result;
	sector_cache_lba = lba;
	sector_cache_valid = 1;
	*sector = sector_cache;
	return BOOTS_FS_OK;
}

enum boots_fs_result boots_fat_write_sector_result(
	struct boots_filesystem *filesystem, uint32_t lba, uint8_t **sector)
{
	const uint8_t *read_sector;
	enum boots_fs_result result;

	if (!filesystem || !sector)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!filesystem->volume.write)
		return BOOTS_FS_READ_ONLY;
	result = boots_fat_read_sector_result(filesystem, lba, &read_sector);
	if (result != BOOTS_FS_OK)
		return result;
	*sector = (uint8_t *)read_sector;
	return BOOTS_FS_OK;
}

enum boots_fs_result boots_fat_mark_sector_dirty(
	struct boots_filesystem *filesystem)
{
	if (!filesystem || !filesystem->volume.write)
		return BOOTS_FS_READ_ONLY;
	if (sector_cache_owner != filesystem || !sector_cache_valid)
		return BOOTS_FS_CORRUPT;
	sector_cache_dirty = 1;
	return BOOTS_FS_OK;
}

enum boots_fs_result boots_fat_cluster_lba(
	struct boots_filesystem *filesystem, uint32_t cluster,
	uint32_t sector_in_cluster, uint32_t *lba)
{
	struct boots_fat_state *fat;
	uint32_t cluster_offset;

	if (!filesystem || !lba)
		return BOOTS_FS_INVALID_ARGUMENT;
	fat = boots_fat_state(filesystem);
	if (cluster < 2 || cluster >= fat->cluster_count + 2 ||
	    sector_in_cluster >= fat->sectors_per_cluster)
		return BOOTS_FS_CORRUPT;
	if (cluster - 2 >
	    (0xffffffffU - fat->data_start) / fat->sectors_per_cluster)
		return BOOTS_FS_CORRUPT;
	cluster_offset = fat->data_start +
			 (cluster - 2) * fat->sectors_per_cluster;
	if (sector_in_cluster > 0xffffffffU - cluster_offset)
		return BOOTS_FS_CORRUPT;
	*lba = cluster_offset + sector_in_cluster;
	if (*lba >= fat->total_sectors)
		return BOOTS_FS_CORRUPT;
	return BOOTS_FS_OK;
}

static enum boots_fs_result parse_bpb(const struct boots_volume *volume,
				       struct boots_fat_state *fat)
{
	uint32_t reserved, fat_sectors, root_sectors, metadata;
	uint32_t total, total_physical, data_sectors;
	uint16_t bytes, fat16_sectors;
	uint8_t sectors_per_cluster;

	if (!boots_volume_read(volume, 0, bpb_cache))
		return BOOTS_FS_IO_ERROR;
	bytes = boots_fat_get16(bpb_cache + 11);
	fat->sector_scale = bytes == 512 ? 1 : bytes == 1024 ? 2 : 0;
	sectors_per_cluster = bpb_cache[13];
	reserved = boots_fat_get16(bpb_cache + 14);
	fat->number_of_fats = bpb_cache[16];
	fat->root_entries = boots_fat_get16(bpb_cache + 17);
	total = boots_fat_get16(bpb_cache + 19);
	if (!total)
		total = boots_fat_get32(bpb_cache + 32);
	fat16_sectors = boots_fat_get16(bpb_cache + 22);
	fat_sectors = fat16_sectors;
	if (!fat_sectors)
		fat_sectors = boots_fat_get32(bpb_cache + 36);
	if (!fat->sector_scale || !sectors_per_cluster || !reserved ||
	    !fat->number_of_fats || !fat_sectors || !total)
		return BOOTS_FS_CORRUPT;
	if (total > 0xffffffffU / fat->sector_scale ||
	    reserved > 0xffffffffU / fat->sector_scale ||
	    fat_sectors > 0xffffffffU / fat->sector_scale)
		return BOOTS_FS_CORRUPT;
	total_physical = total * fat->sector_scale;
	reserved *= fat->sector_scale;
	fat_sectors *= fat->sector_scale;
	fat->sectors_per_cluster = sectors_per_cluster * fat->sector_scale;
	root_sectors = ((uint32_t)fat->root_entries * 32 + 511) >> 9;
	if (fat_sectors > (0xffffffffU - reserved) / fat->number_of_fats)
		return BOOTS_FS_CORRUPT;
	metadata = reserved + fat_sectors * fat->number_of_fats;
	if (root_sectors > 0xffffffffU - metadata)
		return BOOTS_FS_CORRUPT;
	metadata += root_sectors;
	if (metadata >= total_physical)
		return BOOTS_FS_CORRUPT;
	data_sectors = total_physical - metadata;
	fat->cluster_count = data_sectors / fat->sectors_per_cluster;
	fat->type = fat->cluster_count < 4085 ? BOOTS_FAT12 :
	            fat->cluster_count < 65525 ? BOOTS_FAT16 : BOOTS_FAT32;
	fat->fat_start = reserved;
	fat->fat_sectors = fat_sectors;
	fat->root_start = reserved + fat_sectors * fat->number_of_fats;
	fat->data_start = fat->root_start + root_sectors;
	fat->total_sectors = total_physical;
	fat->bytes_per_sector = bytes;
	fat->fat16_layout = fat16_sectors != 0 && fat->root_entries != 0;
	return BOOTS_FS_OK;
}

enum boots_fs_result boots_fat_probe(
	const struct boots_volume *volume, enum boots_fat_type required_type)
{
	struct boots_fat_state candidate = { 0 };
	enum boots_fs_result result = parse_bpb(volume, &candidate);

	if (result != BOOTS_FS_OK)
		return result;
	if (candidate.type != required_type ||
	    (required_type == BOOTS_FAT16 && !candidate.fat16_layout))
		return BOOTS_FS_UNSUPPORTED;
	return BOOTS_FS_OK;
}

enum boots_fs_result boots_fat_mount(
	struct boots_filesystem *filesystem, enum boots_fat_type required_type)
{
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	enum boots_fs_result result = parse_bpb(&filesystem->volume, fat);

	if (result != BOOTS_FS_OK)
		return result;
	if (fat->type != required_type ||
	    (required_type == BOOTS_FAT16 && !fat->fat16_layout))
		return BOOTS_FS_UNSUPPORTED;
	fat->allocation_hint = 2;
	boots_fat_invalidate(filesystem);
	return BOOTS_FS_OK;
}

int boots_fat_short_name(const char *path, char output[11])
{
	unsigned base = 0, extension = 0;

	for (unsigned i = 0; i < 11; i++)
		output[i] = ' ';
	if (*path == '/')
		path++;
	if (!*path)
		return 0;
	while (*path && *path != '.') {
		char character = *path++;

		if (character == '/' || base == 8)
			return 0;
		output[base++] = character >= 'a' && character <= 'z' ?
		                 character - 32 : character;
	}
	if (!base)
		return 0;
	if (*path == '.')
		path++;
	while (*path) {
		char character = *path++;

		if (character == '/' || character == '.' || extension == 3)
			return 0;
		output[8 + extension++] =
			character >= 'a' && character <= 'z' ?
			character - 32 : character;
	}
	return 1;
}

int boots_fat_name_matches(const uint8_t entry[32], const char name[11])
{
	for (unsigned i = 0; i < 11; i++)
		if (entry[i] != (uint8_t)name[i])
			return 0;
	return 1;
}

void boots_fat_decode_dirent(const uint8_t raw[32],
			      struct boots_dirent *entry)
{
	unsigned output = 0;

	for (unsigned i = 0; i < 8 && raw[i] != ' '; i++)
		entry->name[output++] = raw[i];
	if (raw[8] != ' ') {
		entry->name[output++] = '.';
		for (unsigned i = 8; i < 11 && raw[i] != ' '; i++)
			entry->name[output++] = raw[i];
	}
	entry->name[output] = 0;
	entry->size = boots_fat_get32(raw + 28);
	entry->attributes = raw[11];
}

static int valid_cluster(uint32_t cluster, uint32_t end_of_chain)
{
	return cluster >= 2 && cluster < end_of_chain;
}

enum boots_fs_result boots_fat_read_chain(
	struct boots_file *file, uint64_t offset, void *buffer, uint32_t length,
	boots_read_progress_t progress, void *progress_context,
	boots_fat_next_cluster_t next_cluster, uint32_t end_of_chain)
{
	struct boots_filesystem *filesystem = file->filesystem;
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	struct boots_fat_file_state *fat_file = boots_fat_file_state(file);
	uint32_t cluster = fat_file->first_cluster;
	uint32_t position, skip, within, since_update = 0;
	uint8_t *output = buffer;

	if (offset > 0xffffffffU || !next_cluster)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (!valid_cluster(cluster, end_of_chain))
		return BOOTS_FS_CORRUPT;
	position = (uint32_t)offset;
	skip = position / 512;
	within = position & 511;
	while (skip >= fat->sectors_per_cluster) {
		enum boots_fs_result result = next_cluster(filesystem, cluster,
							     &cluster);

		if (result != BOOTS_FS_OK)
			return result;
		if (!valid_cluster(cluster, end_of_chain))
			return BOOTS_FS_CORRUPT;
		skip -= fat->sectors_per_cluster;
	}
	while (length) {
		uint32_t lba;
		uint32_t chunk = 512 - within;
		const uint8_t *input;
		enum boots_fs_result result;

		result = boots_fat_cluster_lba(filesystem, cluster, skip, &lba);
		if (result != BOOTS_FS_OK)
			return result;
		result = boots_fat_read_sector_result(filesystem, lba, &input);
		if (result != BOOTS_FS_OK)
			return result;
		if (chunk > length)
			chunk = length;
		copy_bytes(output, input + within, chunk);
		output += chunk;
		length -= chunk;
		if (progress) {
			since_update += chunk;
			if (since_update >= FAT_PROGRESS_INTERVAL || !length) {
				progress(progress_context, since_update);
				since_update = 0;
			}
		}
		within = 0;
		if (++skip >= fat->sectors_per_cluster && length) {
			enum boots_fs_result result;

			skip = 0;
			result = next_cluster(filesystem, cluster, &cluster);
			if (result != BOOTS_FS_OK)
				return result;
			if (!valid_cluster(cluster, end_of_chain))
				return BOOTS_FS_CORRUPT;
		}
	}
	return BOOTS_FS_OK;
}

enum boots_fs_result boots_fat_contiguous_lba(
	struct boots_file *file, uint32_t *absolute_lba,
	boots_fat_next_cluster_t next_cluster)
{
	struct boots_filesystem *filesystem = file->filesystem;
	struct boots_fat_state *fat = boots_fat_state(filesystem);
	struct boots_fat_file_state *fat_file = boots_fat_file_state(file);
	uint32_t cluster = fat_file->first_cluster;
	uint32_t cluster_lba;
	uint64_t left = file->size;

	if (!next_cluster)
		return BOOTS_FS_INVALID_ARGUMENT;
	if (cluster < 2)
		return BOOTS_FS_CORRUPT;
	while (left > (uint32_t)fat->sectors_per_cluster * 512) {
		uint32_t next;
		enum boots_fs_result result = next_cluster(filesystem, cluster,
							     &next);

		if (result != BOOTS_FS_OK)
			return result;
		if (next != cluster + 1)
			return BOOTS_FS_UNSUPPORTED;
		cluster = next;
		left -= (uint32_t)fat->sectors_per_cluster * 512;
	}
	if (boots_fat_cluster_lba(filesystem, fat_file->first_cluster, 0,
				   &cluster_lba) != BOOTS_FS_OK)
		return BOOTS_FS_CORRUPT;
	if (cluster_lba > 0xffffffffU - filesystem->volume.start_lba)
		return BOOTS_FS_CORRUPT;
	*absolute_lba = filesystem->volume.start_lba + cluster_lba;
	return BOOTS_FS_OK;
}
