/*
 * zedBSD FAT family support
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/fat.h"

#define FAT_PROGRESS_INTERVAL (64U * 1024U)

static uint8_t bpb_cache[512];

_Static_assert(sizeof(struct zedbsd_fat_state) <=
	       sizeof(((struct zedbsd_filesystem *)0)->private_data),
	       "FAT state exceeds generic filesystem storage");
_Static_assert(sizeof(struct zedbsd_fat_file_state) <=
	       sizeof(((struct zedbsd_file *)0)->private_data),
	       "FAT file state exceeds generic file storage");

static void copy_bytes(void *destination, const void *source, uint32_t length)
{
	uint8_t *output = destination;
	const uint8_t *input = source;

	while (length--)
		*output++ = *input++;
}

uint16_t zedbsd_fat_get16(const uint8_t *bytes)
{
	return bytes[0] | ((uint16_t)bytes[1] << 8);
}

uint32_t zedbsd_fat_get32(const uint8_t *bytes)
{
	return zedbsd_fat_get16(bytes) |
	       ((uint32_t)zedbsd_fat_get16(bytes + 2) << 16);
}

struct zedbsd_fat_state *zedbsd_fat_state(
	struct zedbsd_filesystem *filesystem)
{
	return (struct zedbsd_fat_state *)filesystem->private_data;
}

struct zedbsd_fat_file_state *zedbsd_fat_file_state(
	struct zedbsd_file *file)
{
	return (struct zedbsd_fat_file_state *)file->private_data;
}

const uint8_t *zedbsd_fat_read_sector(struct zedbsd_filesystem *filesystem,
				      uint32_t lba)
{
	const uint8_t *sector;

	if (zedbsd_fat_read_sector_result(filesystem, lba, &sector) !=
	    ZEDBSD_FS_OK)
		return 0;
	return sector;
}

enum zedbsd_fs_result zedbsd_fat_flush(
	struct zedbsd_filesystem *filesystem)
{
	struct zedbsd_fat_state *fat;
	enum zedbsd_fs_result result;

	if (!filesystem)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	fat = zedbsd_fat_state(filesystem);
	if (!fat->sector_cache_dirty)
		return ZEDBSD_FS_OK;
	result = zedbsd_volume_write_result(&filesystem->volume,
					    fat->sector_cache_lba,
					    fat->sector_cache);
	if (result == ZEDBSD_FS_OK)
		fat->sector_cache_dirty = 0;
	return result;
}

void zedbsd_fat_invalidate(struct zedbsd_filesystem *filesystem)
{
	struct zedbsd_fat_state *fat;
	if (!filesystem)
		return;
	fat = zedbsd_fat_state(filesystem);
	fat->sector_cache_valid = 0;
	fat->sector_cache_dirty = 0;
}

enum zedbsd_fs_result zedbsd_fat_read_sector_result(
	struct zedbsd_filesystem *filesystem, uint32_t lba,
	const uint8_t **sector)
{
	enum zedbsd_fs_result result;
	struct zedbsd_fat_state *fat;

	if (!filesystem || !sector)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	fat = zedbsd_fat_state(filesystem);
	if (lba >= fat->total_sectors)
		return ZEDBSD_FS_CORRUPT;
	if (fat->sector_cache_valid && fat->sector_cache_lba == lba) {
		*sector = fat->sector_cache;
		return ZEDBSD_FS_OK;
	}
	if (fat->sector_cache_dirty) {
		result = zedbsd_fat_flush(filesystem);
		if (result != ZEDBSD_FS_OK)
			return result;
	}
	fat->sector_cache_valid = 0;
	fat->sector_cache_dirty = 0;
	result = zedbsd_volume_read_result(&filesystem->volume, lba,
					  fat->sector_cache);
	if (result != ZEDBSD_FS_OK)
		return result;
	fat->sector_cache_lba = lba;
	fat->sector_cache_valid = 1;
	*sector = fat->sector_cache;
	return ZEDBSD_FS_OK;
}

enum zedbsd_fs_result zedbsd_fat_write_sector_result(
	struct zedbsd_filesystem *filesystem, uint32_t lba, uint8_t **sector)
{
	const uint8_t *read_sector;
	enum zedbsd_fs_result result;

	if (!filesystem || !sector)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!filesystem->volume.write)
		return ZEDBSD_FS_READ_ONLY;
	result = zedbsd_fat_read_sector_result(filesystem, lba, &read_sector);
	if (result != ZEDBSD_FS_OK)
		return result;
	*sector = (uint8_t *)read_sector;
	return ZEDBSD_FS_OK;
}

enum zedbsd_fs_result zedbsd_fat_mark_sector_dirty(
	struct zedbsd_filesystem *filesystem)
{
	if (!filesystem || !filesystem->volume.write)
		return ZEDBSD_FS_READ_ONLY;
	if (!zedbsd_fat_state(filesystem)->sector_cache_valid)
		return ZEDBSD_FS_CORRUPT;
	zedbsd_fat_state(filesystem)->sector_cache_dirty = 1;
	return ZEDBSD_FS_OK;
}

enum zedbsd_fs_result zedbsd_fat_cluster_lba(
	struct zedbsd_filesystem *filesystem, uint32_t cluster,
	uint32_t sector_in_cluster, uint32_t *lba)
{
	struct zedbsd_fat_state *fat;
	uint32_t cluster_offset;

	if (!filesystem || !lba)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	fat = zedbsd_fat_state(filesystem);
	if (cluster < 2 || cluster >= fat->cluster_count + 2 ||
	    sector_in_cluster >= fat->sectors_per_cluster)
		return ZEDBSD_FS_CORRUPT;
	if (cluster - 2 >
	    (0xffffffffU - fat->data_start) / fat->sectors_per_cluster)
		return ZEDBSD_FS_CORRUPT;
	cluster_offset = fat->data_start +
			 (cluster - 2) * fat->sectors_per_cluster;
	if (sector_in_cluster > 0xffffffffU - cluster_offset)
		return ZEDBSD_FS_CORRUPT;
	*lba = cluster_offset + sector_in_cluster;
	if (*lba >= fat->total_sectors)
		return ZEDBSD_FS_CORRUPT;
	return ZEDBSD_FS_OK;
}

static enum zedbsd_fs_result parse_bpb(const struct zedbsd_volume *volume,
				       struct zedbsd_fat_state *fat)
{
	uint32_t reserved, fat_sectors, fat32_sectors, root_sectors, metadata;
	uint32_t total, total_physical, data_sectors;
	uint16_t bytes, fat16_sectors;
	uint8_t sectors_per_cluster;

	if (!zedbsd_volume_read(volume, 0, bpb_cache))
		return ZEDBSD_FS_IO_ERROR;
	bytes = zedbsd_fat_get16(bpb_cache + 11);
	fat->sector_scale = bytes == 512 ? 1 : bytes == 1024 ? 2 : 0;
	sectors_per_cluster = bpb_cache[13];
	reserved = zedbsd_fat_get16(bpb_cache + 14);
	fat->number_of_fats = bpb_cache[16];
	fat->root_entries = zedbsd_fat_get16(bpb_cache + 17);
	total = zedbsd_fat_get16(bpb_cache + 19);
	if (!total)
		total = zedbsd_fat_get32(bpb_cache + 32);
	fat16_sectors = zedbsd_fat_get16(bpb_cache + 22);
	fat32_sectors = zedbsd_fat_get32(bpb_cache + 36);
	fat_sectors = fat16_sectors;
	if (!fat_sectors)
		fat_sectors = fat32_sectors;
	if (!fat->sector_scale || !sectors_per_cluster || !reserved ||
	    !fat->number_of_fats || !fat_sectors || !total)
		return ZEDBSD_FS_CORRUPT;
	if (total > 0xffffffffU / fat->sector_scale ||
	    reserved > 0xffffffffU / fat->sector_scale ||
	    fat_sectors > 0xffffffffU / fat->sector_scale)
		return ZEDBSD_FS_CORRUPT;
	total_physical = total * fat->sector_scale;
	reserved *= fat->sector_scale;
	fat_sectors *= fat->sector_scale;
	fat->sectors_per_cluster = sectors_per_cluster * fat->sector_scale;
	root_sectors = ((uint32_t)fat->root_entries * 32 + 511) >> 9;
	if (fat_sectors > (0xffffffffU - reserved) / fat->number_of_fats)
		return ZEDBSD_FS_CORRUPT;
	metadata = reserved + fat_sectors * fat->number_of_fats;
	if (root_sectors > 0xffffffffU - metadata)
		return ZEDBSD_FS_CORRUPT;
	metadata += root_sectors;
	if (metadata >= total_physical)
		return ZEDBSD_FS_CORRUPT;
	data_sectors = total_physical - metadata;
	fat->cluster_count = data_sectors / fat->sectors_per_cluster;
	fat->type = fat->cluster_count < 4085 ? ZEDBSD_FAT12 :
	            fat->cluster_count < 65525 ? ZEDBSD_FAT16 : ZEDBSD_FAT32;
	fat->fat_start = reserved;
	fat->fat_sectors = fat_sectors;
	fat->root_start = reserved + fat_sectors * fat->number_of_fats;
	fat->data_start = fat->root_start + root_sectors;
	fat->total_sectors = total_physical;
	fat->bytes_per_sector = bytes;
	fat->fat16_layout = fat16_sectors != 0 && fat->root_entries != 0;
	fat->fat32_layout = fat16_sectors == 0 && fat32_sectors != 0 &&
		fat->root_entries == 0;
	fat->root_cluster = zedbsd_fat_get32(bpb_cache + 44) & 0x0fffffffU;
	fat->fsinfo_sector = zedbsd_fat_get16(bpb_cache + 48);
	if (fat->type == ZEDBSD_FAT32 &&
	    (!fat->fat32_layout || fat->root_cluster < 2U ||
	     fat->root_cluster >= fat->cluster_count + 2U))
		return ZEDBSD_FS_CORRUPT;
	return ZEDBSD_FS_OK;
}

enum zedbsd_fs_result zedbsd_fat_probe(
	const struct zedbsd_volume *volume, enum zedbsd_fat_type required_type)
{
	struct zedbsd_fat_state candidate = { 0 };
	enum zedbsd_fs_result result = parse_bpb(volume, &candidate);

	if (result != ZEDBSD_FS_OK)
		return result;
	if (candidate.type != required_type ||
	    (required_type == ZEDBSD_FAT16 && !candidate.fat16_layout) ||
	    (required_type == ZEDBSD_FAT32 && !candidate.fat32_layout))
		return ZEDBSD_FS_UNSUPPORTED;
	return ZEDBSD_FS_OK;
}

enum zedbsd_fs_result zedbsd_fat_mount(
	struct zedbsd_filesystem *filesystem, enum zedbsd_fat_type required_type)
{
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	enum zedbsd_fs_result result = parse_bpb(&filesystem->volume, fat);

	if (result != ZEDBSD_FS_OK)
		return result;
	if (fat->type != required_type ||
	    (required_type == ZEDBSD_FAT16 && !fat->fat16_layout) ||
	    (required_type == ZEDBSD_FAT32 && !fat->fat32_layout))
		return ZEDBSD_FS_UNSUPPORTED;
	fat->allocation_hint = 2;
	zedbsd_fat_invalidate(filesystem);
	return ZEDBSD_FS_OK;
}

int fat_sfn_encode(const char *path, char output[11])
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

int fat_sfn_equal(const uint8_t entry[32], const char name[11])
{
	for (unsigned i = 0; i < 11; i++) {
		uint8_t left = entry[i];
		uint8_t right = (uint8_t)name[i];

		if (left >= 'a' && left <= 'z')
			left -= 'a' - 'A';
		if (right >= 'a' && right <= 'z')
			right -= 'a' - 'A';
		if (left != right)
			return 0;
	}
	return 1;
}

void fat_sfn_decode_lower(const uint8_t raw[32],
			      struct zedbsd_dirent *entry)
{
	unsigned output = 0;

	for (unsigned i = 0; i < 8 && raw[i] != ' '; i++) {
		uint8_t character = raw[i];
		if (character >= 'A' && character <= 'Z')
			character += 'a' - 'A';
		entry->name[output++] = (char)character;
	}
	if (raw[8] != ' ') {
		entry->name[output++] = '.';
		for (unsigned i = 8; i < 11 && raw[i] != ' '; i++) {
			uint8_t character = raw[i];
			if (character >= 'A' && character <= 'Z')
				character += 'a' - 'A';
			entry->name[output++] = (char)character;
		}
	}
	entry->name[output] = 0;
	entry->size = zedbsd_fat_get32(raw + 28);
	entry->attributes = raw[11];
}

static int valid_cluster(uint32_t cluster, uint32_t end_of_chain)
{
	return cluster >= 2 && cluster < end_of_chain;
}

enum zedbsd_fs_result zedbsd_fat_read_chain(
	struct zedbsd_file *file, uint64_t offset, void *buffer, uint32_t length,
	zedbsd_read_progress_t progress, void *progress_context,
	zedbsd_fat_next_cluster_t next_cluster, uint32_t end_of_chain)
{
	struct zedbsd_filesystem *filesystem = file->filesystem;
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	struct zedbsd_fat_file_state *fat_file = zedbsd_fat_file_state(file);
	uint32_t cluster = fat_file->first_cluster;
	uint32_t position, skip, within, since_update = 0;
	uint8_t *output = buffer;

	if (offset > 0xffffffffU || !next_cluster)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (!valid_cluster(cluster, end_of_chain))
		return ZEDBSD_FS_CORRUPT;
	position = (uint32_t)offset;
	skip = position / 512;
	within = position & 511;
	while (skip >= fat->sectors_per_cluster) {
		enum zedbsd_fs_result result = next_cluster(filesystem, cluster,
							     &cluster);

		if (result != ZEDBSD_FS_OK)
			return result;
		if (!valid_cluster(cluster, end_of_chain))
			return ZEDBSD_FS_CORRUPT;
		skip -= fat->sectors_per_cluster;
	}
	while (length) {
		uint32_t lba;
		uint32_t chunk = 512 - within;
		const uint8_t *input;
		enum zedbsd_fs_result result;

		result = zedbsd_fat_cluster_lba(filesystem, cluster, skip, &lba);
		if (result != ZEDBSD_FS_OK)
			return result;
		result = zedbsd_fat_read_sector_result(filesystem, lba, &input);
		if (result != ZEDBSD_FS_OK)
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
			enum zedbsd_fs_result result;

			skip = 0;
			result = next_cluster(filesystem, cluster, &cluster);
			if (result != ZEDBSD_FS_OK)
				return result;
			if (!valid_cluster(cluster, end_of_chain))
				return ZEDBSD_FS_CORRUPT;
		}
	}
	return ZEDBSD_FS_OK;
}

enum zedbsd_fs_result zedbsd_fat_contiguous_lba(
	struct zedbsd_file *file, uint32_t *absolute_lba,
	zedbsd_fat_next_cluster_t next_cluster)
{
	struct zedbsd_filesystem *filesystem = file->filesystem;
	struct zedbsd_fat_state *fat = zedbsd_fat_state(filesystem);
	struct zedbsd_fat_file_state *fat_file = zedbsd_fat_file_state(file);
	uint32_t cluster = fat_file->first_cluster;
	uint32_t cluster_lba;
	uint64_t left = file->size;

	if (!next_cluster)
		return ZEDBSD_FS_INVALID_ARGUMENT;
	if (cluster < 2)
		return ZEDBSD_FS_CORRUPT;
	while (left > (uint32_t)fat->sectors_per_cluster * 512) {
		uint32_t next;
		enum zedbsd_fs_result result = next_cluster(filesystem, cluster,
							     &next);

		if (result != ZEDBSD_FS_OK)
			return result;
		if (next != cluster + 1)
			return ZEDBSD_FS_UNSUPPORTED;
		cluster = next;
		left -= (uint32_t)fat->sectors_per_cluster * 512;
	}
	if (zedbsd_fat_cluster_lba(filesystem, fat_file->first_cluster, 0,
				   &cluster_lba) != ZEDBSD_FS_OK)
		return ZEDBSD_FS_CORRUPT;
	if (cluster_lba > 0xffffffffU - filesystem->volume.start_lba)
		return ZEDBSD_FS_CORRUPT;
	*absolute_lba = filesystem->volume.start_lba + cluster_lba;
	return ZEDBSD_FS_OK;
}
