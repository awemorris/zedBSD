/*
 * zedBSD FAT12/FAT16/FAT32 filesystem driver
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 *
 * FAT is implemented directly behind the ordinary VFS objects.  Its mount,
 * inode and file state are private to this translation unit.
 */

#include "kern/fat.h"
#include "kern/block-identity.h"
#include "kern/kmem.h"
#include "kern/namecache.h"
#include "kern/namei.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/statvfs.h>

/* Sector, BPB, cache, and shared FAT primitives. */

#define FAT_PROGRESS_INTERVAL (64U * 1024U)
#define FAT_MOUNT_MAX MOUNT_MAX
#define FAT_INODE_MAX 256U
#define FAT_FILE_MAX 96U
#define FAT_ATTRIBUTE_READ_ONLY 0x01U
#define FAT_ATTRIBUTE_DIRECTORY 0x10U
#define FAT_INODE_ORPHANED 0x01U
#define FAT_EPOCH_1980 315532800L
#define FAT_METADATA_MAX 32U
#define FAT_LFN_MAX_UNITS 255U
#define FAT_LFN_MAX_ENTRIES ((FAT_LFN_MAX_UNITS + 12U) / 13U)

typedef void (*fat_read_progress_fn)(void *, uint32_t);

struct fat_dir_entry {
	char name[ZEDBSD_PATH_MAX];
	uint64_t size;
	uint8_t attributes;
};

struct fat_metadata {
	char path[ZEDBSD_PATH_MAX];
	mode_t mode;
	uid_t uid;
	gid_t gid;
};

struct fat_metadata_table {
	struct fat_metadata entries[FAT_METADATA_MAX];
	unsigned count;
};

struct fat_mount_state {
	struct disk *disk;
	struct mutex lock;
	struct fat_metadata_table *metadata;
	uint32_t fat_start;
	uint32_t root_start;
	uint32_t data_start;
	uint32_t total_sectors;
	uint32_t cluster_count;
	uint32_t fat_sectors;
	uint32_t allocation_hint;
	uint32_t root_cluster;
	uint32_t pending_orphans[FAT_INODE_MAX];
	uint16_t pending_orphan_count;
	uint16_t bytes_per_sector;
	uint16_t root_entries;
	uint16_t sectors_per_cluster;
	uint16_t fsinfo_sector;
	uint8_t sector_scale;
	uint8_t number_of_fats;
	uint8_t type;
	uint8_t fat16_layout;
	uint8_t fat32_layout;
	uint8_t sector_cache[512];
	uint32_t sector_cache_lba;
	uint8_t sector_cache_valid;
	uint8_t sector_cache_dirty;
	uint8_t read_only;
	uint8_t direct_io;
	uint8_t used;
};

struct fat_file_state {
	struct fat_mount_state *mount;
	struct inode *owner;
	uint64_t size;
	uint32_t first_cluster;
	uint32_t directory_lba;
	uint16_t directory_offset;
	uint8_t directory_dirty;
	uint8_t pending_close;
	uint8_t used;
};

struct fat_inode_info {
	struct inode fi_inode;
	uint32_t fi_first_cluster;
	uint32_t fi_dirent_lba;
	uint16_t fi_dirent_offset;
	uint8_t fi_attributes;
	uint8_t fi_flags;
};

static struct fat_inode_info *
fat_inode(struct inode *inode)
{
	return (struct fat_inode_info *)inode;
}

struct fat_lfn_state {
	uint16_t units[FAT_LFN_MAX_UNITS + 1U];
	uint16_t unit_limit;
	uint8_t expected;
	uint8_t checksum;
	uint8_t active;
};

typedef int (*fat_next_cluster_fn)(struct fat_mount_state *, uint32_t,
				   uint32_t *);

static void fat_engine_copy_bytes(void *destination, const void *source, uint32_t length)
{
	uint8_t *output = destination;
	const uint8_t *input = source;

	while (length--)
		*output++ = *input++;
}

static uint16_t fat_engine_get16(const uint8_t *bytes)
{
	return bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t fat_engine_get32(const uint8_t *bytes)
{
	return fat_engine_get16(bytes) |
	       ((uint32_t)fat_engine_get16(bytes + 2) << 16);
}

static int
fat_sector_read(struct fat_mount_state *state, uint32_t lba, void *buffer)
{
	if (state == NULL || state->disk == NULL || buffer == NULL)
		return EINVAL;
	return state->direct_io
		   ? disk_read_direct(state->disk, lba, 1, buffer)
		   : disk_read(state->disk, lba, 1, buffer);
}

static int
fat_sector_write(struct fat_mount_state *state, uint32_t lba,
		 const void *buffer)
{
	if (state == NULL || state->disk == NULL || buffer == NULL)
		return EINVAL;
	if (state->read_only)
		return EROFS;
	return disk_write_filesystem(state->disk, lba, 1, buffer);
}

static int fat_engine_flush(
	struct fat_mount_state *filesystem)
{
	int result;

	if (!filesystem)
		return EINVAL;
	if (!filesystem->sector_cache_dirty)
		return 0;
	result = fat_sector_write(filesystem, filesystem->sector_cache_lba,
				  filesystem->sector_cache);
	if (result == 0)
		filesystem->sector_cache_dirty = 0;
	return result;
}

static void fat_engine_invalidate(struct fat_mount_state *filesystem)
{
	if (!filesystem)
		return;
	filesystem->sector_cache_valid = 0;
	filesystem->sector_cache_dirty = 0;
}

static int fat_engine_read_sector_result(
	struct fat_mount_state *filesystem, uint32_t lba,
	const uint8_t **sector)
{
	int result;

	if (!filesystem || !sector)
		return EINVAL;
	if (lba >= filesystem->total_sectors)
		return EIO;
	if (filesystem->sector_cache_valid &&
	    filesystem->sector_cache_lba == lba) {
		*sector = filesystem->sector_cache;
		return 0;
	}
	if (filesystem->sector_cache_dirty) {
		result = fat_engine_flush(filesystem);
		if (result != 0)
			return result;
	}
	filesystem->sector_cache_valid = 0;
	filesystem->sector_cache_dirty = 0;
	result = fat_sector_read(filesystem, lba, filesystem->sector_cache);
	if (result != 0)
		return result;
	filesystem->sector_cache_lba = lba;
	filesystem->sector_cache_valid = 1;
	*sector = filesystem->sector_cache;
	return 0;
}

static int fat_engine_write_sector_result(
	struct fat_mount_state *filesystem, uint32_t lba, uint8_t **sector)
{
	const uint8_t *read_sector;
	int result;

	if (!filesystem || !sector)
		return EINVAL;
	if (filesystem->read_only)
		return EROFS;
	result = fat_engine_read_sector_result(filesystem, lba, &read_sector);
	if (result != 0)
		return result;
	*sector = (uint8_t *)read_sector;
	return 0;
}

static int fat_engine_mark_sector_dirty(
	struct fat_mount_state *filesystem)
{
	if (!filesystem || filesystem->read_only)
		return EROFS;
	if (!filesystem->sector_cache_valid)
		return EIO;
	filesystem->sector_cache_dirty = 1;
	return 0;
}

static int fat_engine_cluster_lba(
	struct fat_mount_state *filesystem, uint32_t cluster,
	uint32_t sector_in_cluster, uint32_t *lba)
{
	struct fat_mount_state *fat;
	uint32_t cluster_offset;

	if (!filesystem || !lba)
		return EINVAL;
	fat = filesystem;
	if (cluster < 2 || cluster >= fat->cluster_count + 2 ||
	    sector_in_cluster >= fat->sectors_per_cluster)
		return EIO;
	if (cluster - 2 >
	    (0xffffffffU - fat->data_start) / fat->sectors_per_cluster)
		return EIO;
	cluster_offset = fat->data_start +
			 (cluster - 2) * fat->sectors_per_cluster;
	if (sector_in_cluster > 0xffffffffU - cluster_offset)
		return EIO;
	*lba = cluster_offset + sector_in_cluster;
	if (*lba >= fat->total_sectors)
		return EIO;
	return 0;
}

static int parse_bpb(struct fat_mount_state *fat)
{
	uint8_t bpb[512];
	uint32_t reserved, fat_sectors, fat32_sectors, root_sectors, metadata;
	uint32_t total, total_physical, data_sectors;
	uint16_t bytes, fat16_sectors;
	uint8_t sectors_per_cluster;

	if (fat_sector_read(fat, 0, bpb) != 0)
		return EIO;
	bytes = fat_engine_get16(bpb + 11);
	fat->sector_scale = bytes == 512 ? 1 : bytes == 1024 ? 2 : 0;
	sectors_per_cluster = bpb[13];
	reserved = fat_engine_get16(bpb + 14);
	fat->number_of_fats = bpb[16];
	fat->root_entries = fat_engine_get16(bpb + 17);
	total = fat_engine_get16(bpb + 19);
	if (!total)
		total = fat_engine_get32(bpb + 32);
	fat16_sectors = fat_engine_get16(bpb + 22);
	fat32_sectors = fat_engine_get32(bpb + 36);
	fat_sectors = fat16_sectors;
	if (!fat_sectors)
		fat_sectors = fat32_sectors;
	if (!fat->sector_scale || !sectors_per_cluster || !reserved ||
	    !fat->number_of_fats || !fat_sectors || !total)
		return EIO;
	if (total > 0xffffffffU / fat->sector_scale ||
	    reserved > 0xffffffffU / fat->sector_scale ||
	    fat_sectors > 0xffffffffU / fat->sector_scale)
		return EIO;
	total_physical = total * fat->sector_scale;
	if (fat->disk == NULL || total_physical > fat->disk->d_block_count)
		return EIO;
	reserved *= fat->sector_scale;
	fat_sectors *= fat->sector_scale;
	fat->sectors_per_cluster = sectors_per_cluster * fat->sector_scale;
	root_sectors = ((uint32_t)fat->root_entries * 32 + 511) >> 9;
	if (fat_sectors > (0xffffffffU - reserved) / fat->number_of_fats)
		return EIO;
	metadata = reserved + fat_sectors * fat->number_of_fats;
	if (root_sectors > 0xffffffffU - metadata)
		return EIO;
	metadata += root_sectors;
	if (metadata >= total_physical)
		return EIO;
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
	fat->root_cluster = fat_engine_get32(bpb + 44) & 0x0fffffffU;
	fat->fsinfo_sector = fat_engine_get16(bpb + 48);
	if (fat->type == ZEDBSD_FAT32 &&
	    (!fat->fat32_layout || fat->root_cluster < 2U ||
	     fat->root_cluster >= fat->cluster_count + 2U))
		return EIO;
	return 0;
}

static int fat_engine_mount(
	struct fat_mount_state *filesystem, enum bootfat_type required_type)
{
	struct fat_mount_state *fat = filesystem;
	int result = parse_bpb(fat);

	if (result != 0)
		return result;
	if (fat->type != required_type ||
	    (required_type == ZEDBSD_FAT16 && !fat->fat16_layout) ||
	    (required_type == ZEDBSD_FAT32 && !fat->fat32_layout))
		return EOPNOTSUPP;
	fat->allocation_hint = 2;
	fat_engine_invalidate(filesystem);
	return 0;
}

static int fat_sfn_encode(const char *path, char output[11])
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

static int fat_sfn_equal(const uint8_t entry[32], const char name[11])
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

static void fat_sfn_decode_lower(const uint8_t raw[32],
			      struct fat_dir_entry *entry)
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
	entry->size = fat_engine_get32(raw + 28);
	entry->attributes = raw[11];
}

static int valid_cluster(uint32_t cluster, uint32_t end_of_chain)
{
	return cluster >= 2 && cluster < end_of_chain;
}

static int fat_engine_read_chain(
	struct fat_file_state *file, uint64_t offset, void *buffer, uint32_t length,
	fat_read_progress_fn progress, void *progress_context,
	fat_next_cluster_fn next_cluster, uint32_t end_of_chain)
{
	struct fat_mount_state *filesystem = file->mount;
	struct fat_mount_state *fat = filesystem;
	struct fat_file_state *fat_file = file;
	uint32_t cluster = fat_file->first_cluster;
	uint32_t position, skip, within, since_update = 0;
	uint8_t *output = buffer;

	if (offset > 0xffffffffU || !next_cluster)
		return EINVAL;
	if (!valid_cluster(cluster, end_of_chain))
		return EIO;
	position = (uint32_t)offset;
	skip = position / 512;
	within = position & 511;
	while (skip >= fat->sectors_per_cluster) {
		int result = next_cluster(filesystem, cluster,
							     &cluster);

		if (result != 0)
			return result;
		if (!valid_cluster(cluster, end_of_chain))
			return EIO;
		skip -= fat->sectors_per_cluster;
	}
	while (length) {
		uint32_t lba;
		uint32_t chunk = 512 - within;
		const uint8_t *input;
		int result;

		result = fat_engine_cluster_lba(filesystem, cluster, skip, &lba);
		if (result != 0)
			return result;
		result = fat_engine_read_sector_result(filesystem, lba, &input);
		if (result != 0)
			return result;
		if (chunk > length)
			chunk = length;
		fat_engine_copy_bytes(output, input + within, chunk);
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
			int result;

			skip = 0;
			result = next_cluster(filesystem, cluster, &cluster);
			if (result != 0)
				return result;
			if (!valid_cluster(cluster, end_of_chain))
				return EIO;
		}
	}
	return 0;
}

/* VFAT long-file-name and Unicode case-fold helpers. */

static const uint8_t lfn_offsets[13] = {
	1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
};

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void fat_lfn_reset(struct fat_lfn_state *state)
{
	unsigned i;

	state->unit_limit = 0;
	state->expected = 0;
	state->checksum = 0;
	state->active = 0;
	for (i = 0; i <= FAT_LFN_MAX_UNITS; i++)
		state->units[i] = 0xffffU;
}

static uint8_t fat_lfn_checksum(const uint8_t sfn[11])
{
	uint8_t sum = 0;
	unsigned i;

	for (i = 0; i < 11; i++)
		sum = (uint8_t)(((sum & 1U) << 7) | (sum >> 1)) + sfn[i];
	return sum;
}

static int fat_lfn_feed(struct fat_lfn_state *state, const uint8_t raw[32])
{
	unsigned ordinal = raw[0] & 0x1fU;
	unsigned i;

	if (raw[11] != 0x0fU || raw[12] != 0 || get16(raw + 26) != 0 ||
	    ordinal == 0 || ordinal > 20U) {
		fat_lfn_reset(state);
		return 0;
	}
	if (raw[0] & 0x40U) {
		fat_lfn_reset(state);
		state->active = 1;
		state->expected = (uint8_t)ordinal;
		state->checksum = raw[13];
		state->unit_limit = (uint16_t)(ordinal * 13U);
		if (state->unit_limit > FAT_LFN_MAX_UNITS + 1U)
			state->unit_limit = FAT_LFN_MAX_UNITS + 1U;
	}
	if (!state->active || ordinal != state->expected ||
	    raw[13] != state->checksum || (raw[0] & 0x80U)) {
		fat_lfn_reset(state);
		return 0;
	}
	for (i = 0; i < 13; i++) {
		unsigned index = (ordinal - 1U) * 13U + i;
		if (index <= FAT_LFN_MAX_UNITS)
			state->units[index] = get16(raw + lfn_offsets[i]);
	}
	state->expected--;
	return 1;
}

static int append_utf8(char *output, size_t capacity, size_t *used,
		       uint32_t scalar)
{
	uint8_t bytes[4];
	unsigned count, i;

	if (scalar <= 0x7fU) {
		bytes[0] = (uint8_t)scalar; count = 1;
	} else if (scalar <= 0x7ffU) {
		bytes[0] = (uint8_t)(0xc0U | (scalar >> 6));
		bytes[1] = (uint8_t)(0x80U | (scalar & 0x3fU)); count = 2;
	} else if (scalar <= 0xffffU) {
		bytes[0] = (uint8_t)(0xe0U | (scalar >> 12));
		bytes[1] = (uint8_t)(0x80U | ((scalar >> 6) & 0x3fU));
		bytes[2] = (uint8_t)(0x80U | (scalar & 0x3fU)); count = 3;
	} else {
		bytes[0] = (uint8_t)(0xf0U | (scalar >> 18));
		bytes[1] = (uint8_t)(0x80U | ((scalar >> 12) & 0x3fU));
		bytes[2] = (uint8_t)(0x80U | ((scalar >> 6) & 0x3fU));
		bytes[3] = (uint8_t)(0x80U | (scalar & 0x3fU)); count = 4;
	}
	if (*used + count >= capacity)
		return 0;
	for (i = 0; i < count; i++)
		output[(*used)++] = (char)bytes[i];
	return 1;
}

static int fat_lfn_finish(struct fat_lfn_state *state, const uint8_t sfn[32],
		   char *output, size_t capacity)
{
	size_t used = 0;
	unsigned i;
	int terminated = 0;

	if (!state->active || state->expected != 0 ||
	    state->checksum != fat_lfn_checksum(sfn) || capacity == 0)
		goto invalid;
	for (i = 0; i < state->unit_limit; i++) {
		uint32_t scalar = state->units[i];
		if (scalar == 0) {
			terminated = 1;
			break;
		}
		if (scalar == 0xffffU)
			goto invalid;
		if (scalar >= 0xd800U && scalar <= 0xdbffU) {
			uint32_t low;
			if (++i >= state->unit_limit)
				goto invalid;
			low = state->units[i];
			if (low < 0xdc00U || low > 0xdfffU)
				goto invalid;
			scalar = 0x10000U + ((scalar - 0xd800U) << 10) +
				(low - 0xdc00U);
		} else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
			goto invalid;
		}
		if (scalar == '/' || scalar == 0 ||
		    !append_utf8(output, capacity, &used, scalar))
			goto invalid;
	}
	if (used == 0 || (!terminated && state->unit_limit > FAT_LFN_MAX_UNITS))
		goto invalid;
	if (terminated) {
		for (; i < state->unit_limit; i++)
			if (state->units[i] != 0 && state->units[i] != 0xffffU)
				goto invalid;
	}
	output[used] = '\0';
	fat_lfn_reset(state);
	return 1;
invalid:
	fat_lfn_reset(state);
	if (capacity)
		output[0] = '\0';
	return 0;
}

static void fat_sfn_decode_preserve(const uint8_t raw[32], char *output,
			     size_t capacity)
{
	size_t used = 0;
	unsigned i;
	int lower_base = (raw[12] & 0x08U) != 0;
	int lower_ext = (raw[12] & 0x10U) != 0;

	if (capacity == 0)
		return;
	for (i = 0; i < 8 && raw[i] != ' ' && used + 1U < capacity; i++) {
		uint8_t c = raw[i];
		if (lower_base && c >= 'A' && c <= 'Z') c += 'a' - 'A';
		output[used++] = (char)c;
	}
	if (raw[8] != ' ' && used + 1U < capacity) {
		output[used++] = '.';
		for (i = 8; i < 11 && raw[i] != ' ' && used + 1U < capacity; i++) {
			uint8_t c = raw[i];
			if (lower_ext && c >= 'A' && c <= 'Z') c += 'a' - 'A';
			output[used++] = (char)c;
		}
	}
	output[used] = '\0';
}

struct fat_casefold_range {
	uint32_t start;
	/* Bit 31 marks a stride-two range; Unicode scalar values never use it. */
	uint32_t encoded_end;
	int32_t delta;
};

/* Generated by scripts/generate-unicode-casefold.py from
 * Unicode CaseFolding-17.0.0.txt; statuses C and S only.  Do not edit. */
static const struct fat_casefold_range fat_casefold_ranges[] = {
	{ 0x000041U, 0x0000005aU, 32 },
	{ 0x0000b5U, 0x000000b5U, 775 },
	{ 0x0000c0U, 0x000000d6U, 32 },
	{ 0x0000d8U, 0x000000deU, 32 },
	{ 0x000100U, 0x8000012eU, 1 },
	{ 0x000132U, 0x80000136U, 1 },
	{ 0x000139U, 0x80000147U, 1 },
	{ 0x00014aU, 0x80000176U, 1 },
	{ 0x000178U, 0x00000178U, -121 },
	{ 0x000179U, 0x8000017dU, 1 },
	{ 0x00017fU, 0x0000017fU, -268 },
	{ 0x000181U, 0x00000181U, 210 },
	{ 0x000182U, 0x80000184U, 1 },
	{ 0x000186U, 0x00000186U, 206 },
	{ 0x000187U, 0x00000187U, 1 },
	{ 0x000189U, 0x0000018aU, 205 },
	{ 0x00018bU, 0x0000018bU, 1 },
	{ 0x00018eU, 0x0000018eU, 79 },
	{ 0x00018fU, 0x0000018fU, 202 },
	{ 0x000190U, 0x00000190U, 203 },
	{ 0x000191U, 0x00000191U, 1 },
	{ 0x000193U, 0x00000193U, 205 },
	{ 0x000194U, 0x00000194U, 207 },
	{ 0x000196U, 0x00000196U, 211 },
	{ 0x000197U, 0x00000197U, 209 },
	{ 0x000198U, 0x00000198U, 1 },
	{ 0x00019cU, 0x0000019cU, 211 },
	{ 0x00019dU, 0x0000019dU, 213 },
	{ 0x00019fU, 0x0000019fU, 214 },
	{ 0x0001a0U, 0x800001a4U, 1 },
	{ 0x0001a6U, 0x000001a6U, 218 },
	{ 0x0001a7U, 0x000001a7U, 1 },
	{ 0x0001a9U, 0x000001a9U, 218 },
	{ 0x0001acU, 0x000001acU, 1 },
	{ 0x0001aeU, 0x000001aeU, 218 },
	{ 0x0001afU, 0x000001afU, 1 },
	{ 0x0001b1U, 0x000001b2U, 217 },
	{ 0x0001b3U, 0x800001b5U, 1 },
	{ 0x0001b7U, 0x000001b7U, 219 },
	{ 0x0001b8U, 0x000001b8U, 1 },
	{ 0x0001bcU, 0x000001bcU, 1 },
	{ 0x0001c4U, 0x000001c4U, 2 },
	{ 0x0001c5U, 0x000001c5U, 1 },
	{ 0x0001c7U, 0x000001c7U, 2 },
	{ 0x0001c8U, 0x000001c8U, 1 },
	{ 0x0001caU, 0x000001caU, 2 },
	{ 0x0001cbU, 0x800001dbU, 1 },
	{ 0x0001deU, 0x800001eeU, 1 },
	{ 0x0001f1U, 0x000001f1U, 2 },
	{ 0x0001f2U, 0x800001f4U, 1 },
	{ 0x0001f6U, 0x000001f6U, -97 },
	{ 0x0001f7U, 0x000001f7U, -56 },
	{ 0x0001f8U, 0x8000021eU, 1 },
	{ 0x000220U, 0x00000220U, -130 },
	{ 0x000222U, 0x80000232U, 1 },
	{ 0x00023aU, 0x0000023aU, 10795 },
	{ 0x00023bU, 0x0000023bU, 1 },
	{ 0x00023dU, 0x0000023dU, -163 },
	{ 0x00023eU, 0x0000023eU, 10792 },
	{ 0x000241U, 0x00000241U, 1 },
	{ 0x000243U, 0x00000243U, -195 },
	{ 0x000244U, 0x00000244U, 69 },
	{ 0x000245U, 0x00000245U, 71 },
	{ 0x000246U, 0x8000024eU, 1 },
	{ 0x000345U, 0x00000345U, 116 },
	{ 0x000370U, 0x80000372U, 1 },
	{ 0x000376U, 0x00000376U, 1 },
	{ 0x00037fU, 0x0000037fU, 116 },
	{ 0x000386U, 0x00000386U, 38 },
	{ 0x000388U, 0x0000038aU, 37 },
	{ 0x00038cU, 0x0000038cU, 64 },
	{ 0x00038eU, 0x0000038fU, 63 },
	{ 0x000391U, 0x000003a1U, 32 },
	{ 0x0003a3U, 0x000003abU, 32 },
	{ 0x0003c2U, 0x000003c2U, 1 },
	{ 0x0003cfU, 0x000003cfU, 8 },
	{ 0x0003d0U, 0x000003d0U, -30 },
	{ 0x0003d1U, 0x000003d1U, -25 },
	{ 0x0003d5U, 0x000003d5U, -15 },
	{ 0x0003d6U, 0x000003d6U, -22 },
	{ 0x0003d8U, 0x800003eeU, 1 },
	{ 0x0003f0U, 0x000003f0U, -54 },
	{ 0x0003f1U, 0x000003f1U, -48 },
	{ 0x0003f4U, 0x000003f4U, -60 },
	{ 0x0003f5U, 0x000003f5U, -64 },
	{ 0x0003f7U, 0x000003f7U, 1 },
	{ 0x0003f9U, 0x000003f9U, -7 },
	{ 0x0003faU, 0x000003faU, 1 },
	{ 0x0003fdU, 0x000003ffU, -130 },
	{ 0x000400U, 0x0000040fU, 80 },
	{ 0x000410U, 0x0000042fU, 32 },
	{ 0x000460U, 0x80000480U, 1 },
	{ 0x00048aU, 0x800004beU, 1 },
	{ 0x0004c0U, 0x000004c0U, 15 },
	{ 0x0004c1U, 0x800004cdU, 1 },
	{ 0x0004d0U, 0x8000052eU, 1 },
	{ 0x000531U, 0x00000556U, 48 },
	{ 0x0010a0U, 0x000010c5U, 7264 },
	{ 0x0010c7U, 0x000010c7U, 7264 },
	{ 0x0010cdU, 0x000010cdU, 7264 },
	{ 0x0013f8U, 0x000013fdU, -8 },
	{ 0x001c80U, 0x00001c80U, -6222 },
	{ 0x001c81U, 0x00001c81U, -6221 },
	{ 0x001c82U, 0x00001c82U, -6212 },
	{ 0x001c83U, 0x00001c84U, -6210 },
	{ 0x001c85U, 0x00001c85U, -6211 },
	{ 0x001c86U, 0x00001c86U, -6204 },
	{ 0x001c87U, 0x00001c87U, -6180 },
	{ 0x001c88U, 0x00001c88U, 35267 },
	{ 0x001c89U, 0x00001c89U, 1 },
	{ 0x001c90U, 0x00001cbaU, -3008 },
	{ 0x001cbdU, 0x00001cbfU, -3008 },
	{ 0x001e00U, 0x80001e94U, 1 },
	{ 0x001e9bU, 0x00001e9bU, -58 },
	{ 0x001e9eU, 0x00001e9eU, -7615 },
	{ 0x001ea0U, 0x80001efeU, 1 },
	{ 0x001f08U, 0x00001f0fU, -8 },
	{ 0x001f18U, 0x00001f1dU, -8 },
	{ 0x001f28U, 0x00001f2fU, -8 },
	{ 0x001f38U, 0x00001f3fU, -8 },
	{ 0x001f48U, 0x00001f4dU, -8 },
	{ 0x001f59U, 0x80001f5fU, -8 },
	{ 0x001f68U, 0x00001f6fU, -8 },
	{ 0x001f88U, 0x00001f8fU, -8 },
	{ 0x001f98U, 0x00001f9fU, -8 },
	{ 0x001fa8U, 0x00001fafU, -8 },
	{ 0x001fb8U, 0x00001fb9U, -8 },
	{ 0x001fbaU, 0x00001fbbU, -74 },
	{ 0x001fbcU, 0x00001fbcU, -9 },
	{ 0x001fbeU, 0x00001fbeU, -7173 },
	{ 0x001fc8U, 0x00001fcbU, -86 },
	{ 0x001fccU, 0x00001fccU, -9 },
	{ 0x001fd3U, 0x00001fd3U, -7235 },
	{ 0x001fd8U, 0x00001fd9U, -8 },
	{ 0x001fdaU, 0x00001fdbU, -100 },
	{ 0x001fe3U, 0x00001fe3U, -7219 },
	{ 0x001fe8U, 0x00001fe9U, -8 },
	{ 0x001feaU, 0x00001febU, -112 },
	{ 0x001fecU, 0x00001fecU, -7 },
	{ 0x001ff8U, 0x00001ff9U, -128 },
	{ 0x001ffaU, 0x00001ffbU, -126 },
	{ 0x001ffcU, 0x00001ffcU, -9 },
	{ 0x002126U, 0x00002126U, -7517 },
	{ 0x00212aU, 0x0000212aU, -8383 },
	{ 0x00212bU, 0x0000212bU, -8262 },
	{ 0x002132U, 0x00002132U, 28 },
	{ 0x002160U, 0x0000216fU, 16 },
	{ 0x002183U, 0x00002183U, 1 },
	{ 0x0024b6U, 0x000024cfU, 26 },
	{ 0x002c00U, 0x00002c2fU, 48 },
	{ 0x002c60U, 0x00002c60U, 1 },
	{ 0x002c62U, 0x00002c62U, -10743 },
	{ 0x002c63U, 0x00002c63U, -3814 },
	{ 0x002c64U, 0x00002c64U, -10727 },
	{ 0x002c67U, 0x80002c6bU, 1 },
	{ 0x002c6dU, 0x00002c6dU, -10780 },
	{ 0x002c6eU, 0x00002c6eU, -10749 },
	{ 0x002c6fU, 0x00002c6fU, -10783 },
	{ 0x002c70U, 0x00002c70U, -10782 },
	{ 0x002c72U, 0x00002c72U, 1 },
	{ 0x002c75U, 0x00002c75U, 1 },
	{ 0x002c7eU, 0x00002c7fU, -10815 },
	{ 0x002c80U, 0x80002ce2U, 1 },
	{ 0x002cebU, 0x80002cedU, 1 },
	{ 0x002cf2U, 0x00002cf2U, 1 },
	{ 0x00a640U, 0x8000a66cU, 1 },
	{ 0x00a680U, 0x8000a69aU, 1 },
	{ 0x00a722U, 0x8000a72eU, 1 },
	{ 0x00a732U, 0x8000a76eU, 1 },
	{ 0x00a779U, 0x8000a77bU, 1 },
	{ 0x00a77dU, 0x0000a77dU, -35332 },
	{ 0x00a77eU, 0x8000a786U, 1 },
	{ 0x00a78bU, 0x0000a78bU, 1 },
	{ 0x00a78dU, 0x0000a78dU, -42280 },
	{ 0x00a790U, 0x8000a792U, 1 },
	{ 0x00a796U, 0x8000a7a8U, 1 },
	{ 0x00a7aaU, 0x0000a7aaU, -42308 },
	{ 0x00a7abU, 0x0000a7abU, -42319 },
	{ 0x00a7acU, 0x0000a7acU, -42315 },
	{ 0x00a7adU, 0x0000a7adU, -42305 },
	{ 0x00a7aeU, 0x0000a7aeU, -42308 },
	{ 0x00a7b0U, 0x0000a7b0U, -42258 },
	{ 0x00a7b1U, 0x0000a7b1U, -42282 },
	{ 0x00a7b2U, 0x0000a7b2U, -42261 },
	{ 0x00a7b3U, 0x0000a7b3U, 928 },
	{ 0x00a7b4U, 0x8000a7c2U, 1 },
	{ 0x00a7c4U, 0x0000a7c4U, -48 },
	{ 0x00a7c5U, 0x0000a7c5U, -42307 },
	{ 0x00a7c6U, 0x0000a7c6U, -35384 },
	{ 0x00a7c7U, 0x8000a7c9U, 1 },
	{ 0x00a7cbU, 0x0000a7cbU, -42343 },
	{ 0x00a7ccU, 0x8000a7daU, 1 },
	{ 0x00a7dcU, 0x0000a7dcU, -42561 },
	{ 0x00a7f5U, 0x0000a7f5U, 1 },
	{ 0x00ab70U, 0x0000abbfU, -38864 },
	{ 0x00fb05U, 0x0000fb05U, 1 },
	{ 0x00ff21U, 0x0000ff3aU, 32 },
	{ 0x010400U, 0x00010427U, 40 },
	{ 0x0104b0U, 0x000104d3U, 40 },
	{ 0x010570U, 0x0001057aU, 39 },
	{ 0x01057cU, 0x0001058aU, 39 },
	{ 0x01058cU, 0x00010592U, 39 },
	{ 0x010594U, 0x00010595U, 39 },
	{ 0x010c80U, 0x00010cb2U, 64 },
	{ 0x010d50U, 0x00010d65U, 32 },
	{ 0x0118a0U, 0x000118bfU, 32 },
	{ 0x016e40U, 0x00016e5fU, 32 },
	{ 0x016ea0U, 0x00016eb8U, 27 },
	{ 0x01e900U, 0x0001e921U, 34 },
};

static uint32_t fold_scalar(uint32_t scalar)
{
	size_t low = 0;
	size_t high = sizeof(fat_casefold_ranges) /
		sizeof(fat_casefold_ranges[0]);

	while (low < high) {
		size_t middle = low + (high - low) / 2U;
		if (fat_casefold_ranges[middle].start <= scalar)
			low = middle + 1U;
		else
			high = middle;
	}
	if (low != 0) {
		const struct fat_casefold_range *range =
			&fat_casefold_ranges[low - 1U];
		uint32_t end = range->encoded_end & 0x7fffffffU;
		if (scalar <= end &&
		    (!(range->encoded_end & 0x80000000U) ||
		     ((scalar - range->start) & 1U) == 0))
			return (uint32_t)((int32_t)scalar + range->delta);
	}
	return scalar;
}

static int decode_utf8(const uint8_t **cursor, uint32_t *scalar)
{
	const uint8_t *p = *cursor;
	uint32_t value;
	unsigned count, i;

	if (*p < 0x80U) {
		*scalar = *p;
		*cursor = p + 1;
		return *p != 0;
	}
	if (*p >= 0xc2U && *p <= 0xdfU) {
		value = *p & 0x1fU; count = 1;
	} else if (*p >= 0xe0U && *p <= 0xefU) {
		value = *p & 0x0fU; count = 2;
	} else if (*p >= 0xf0U && *p <= 0xf4U) {
		value = *p & 0x07U; count = 3;
	} else {
		return 0;
	}
	for (i = 0; i < count; i++) {
		uint8_t next = p[i + 1U];
		if ((next & 0xc0U) != 0x80U)
			return 0;
		value = (value << 6) | (next & 0x3fU);
	}
	if ((count == 2 && value < 0x800U) ||
	    (count == 3 && value < 0x10000U) || value > 0x10ffffU ||
	    (value >= 0xd800U && value <= 0xdfffU))
		return 0;
	*scalar = value;
	*cursor = p + count + 1U;
	return 1;
}

static int fat_utf8_to_utf16(const char *name,
		      uint16_t units[FAT_LFN_MAX_UNITS], unsigned *unit_count)
{
	const uint8_t *cursor = (const uint8_t *)name;
	unsigned count = 0;

	if (name == 0 || unit_count == 0 || !*cursor)
		return 0;
	while (*cursor) {
		uint32_t scalar;

		if (!decode_utf8(&cursor, &scalar) || scalar == '/' ||
		    scalar < 0x20U || scalar == 0x7fU)
			return 0;
		if (scalar <= 0xffffU) {
			if (count >= FAT_LFN_MAX_UNITS)
				return 0;
			units[count++] = (uint16_t)scalar;
		} else {
			if (count + 2U > FAT_LFN_MAX_UNITS)
				return 0;
			scalar -= 0x10000U;
			units[count++] = (uint16_t)(0xd800U | (scalar >> 10));
			units[count++] = (uint16_t)(0xdc00U | (scalar & 0x3ffU));
		}
	}
	/* VFAT forbids trailing dot/space and these punctuation characters. */
	if (count == 0 || units[count - 1U] == '.' || units[count - 1U] == ' ')
		return 0;
	*unit_count = count;
	return 1;
}

static void fat_lfn_build_entry(uint8_t raw[32], const uint16_t *units,
			 unsigned unit_count, unsigned ordinal,
			 uint8_t checksum)
{
	unsigned total = (unit_count + 12U) / 13U;
	unsigned i;

	for (i = 0; i < 32; i++)
		raw[i] = 0xffU;
	raw[0] = (uint8_t)ordinal;
	if (ordinal == total)
		raw[0] |= 0x40U;
	raw[11] = 0x0fU;
	raw[12] = 0;
	raw[13] = checksum;
	raw[26] = raw[27] = 0;
	for (i = 0; i < 13; i++) {
		unsigned index = (ordinal - 1U) * 13U + i;
		uint16_t value = index < unit_count ? units[index] :
			(index == unit_count ? 0 : 0xffffU);
		raw[lfn_offsets[i]] = (uint8_t)value;
		raw[lfn_offsets[i] + 1U] = (uint8_t)(value >> 8);
	}
}

static int sfn_character(uint8_t c)
{
	if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
		c == '@' || c == '~' || c == '`' || c == '!' ||
		c == '(' || c == ')' || c == '{' || c == '}' || c == '^' ||
		c == '#' || c == '&';
}

static int fat_sfn_make_alias(const char *name, unsigned serial, uint8_t sfn[11])
{
	const uint8_t *p = (const uint8_t *)name;
	const uint8_t *dot = 0, *q;
	uint8_t base[8], extension[3], digits[6];
	unsigned base_count = 0, extension_count = 0, digit_count = 0, i;

	if (name == 0 || sfn == 0 || serial == 0 || serial > 999999U)
		return 0;
	for (q = p; *q; q++)
		if (*q == '.') dot = q;
	for (q = p; *q && q != dot; q++) {
		uint8_t c = *q;
		if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
		if (sfn_character(c) && base_count < sizeof(base))
			base[base_count++] = c;
	}
	if (dot != 0) {
		for (q = dot + 1; *q && extension_count < sizeof(extension); q++) {
			uint8_t c = *q;
			if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
			if (sfn_character(c)) extension[extension_count++] = c;
		}
	}
	if (base_count == 0) {
		base[0] = 'F'; base[1] = 'I'; base[2] = 'L'; base[3] = 'E';
		base_count = 4;
	}
	while (serial) {
		digits[digit_count++] = (uint8_t)('0' + serial % 10U);
		serial /= 10U;
	}
	for (i = 0; i < 11; i++) sfn[i] = ' ';
	if (base_count > 7U - digit_count) base_count = 7U - digit_count;
	for (i = 0; i < base_count; i++) sfn[i] = base[i];
	sfn[base_count++] = '~';
	while (digit_count) sfn[base_count++] = digits[--digit_count];
	for (i = 0; i < extension_count; i++) sfn[8U + i] = extension[i];
	return 1;
}

static int fat_utf8_casefold_equal(const char *left, const char *right)
{
	const uint8_t *a = (const uint8_t *)left;
	const uint8_t *b = (const uint8_t *)right;

	if (left == NULL || right == NULL)
		return 0;
	while (*a && *b) {
		uint32_t left_scalar, right_scalar;
		if (!decode_utf8(&a, &left_scalar) ||
		    !decode_utf8(&b, &right_scalar) ||
		    fold_scalar(left_scalar) != fold_scalar(right_scalar))
			return 0;
	}
	return *a == 0 && *b == 0;
}


/*
 * FAT12/FAT16/FAT32 cluster and directory engine.  The directory, chain,
 * and write logic is shared; the mounted type selects the FAT entry width.
 */

#define FAT16_RESERVED_CLUSTER 0xfff0U
#define FAT16_END_OF_CHAIN 0xffffU
#define FAT12_RESERVED_CLUSTER 0xff0U
#define FAT12_END_OF_CHAIN 0xfffU
#define FAT32_RESERVED_CLUSTER 0x0ffffff0U
#define FAT32_END_OF_CHAIN 0x0fffffffU
#define FAT16_DIRECTORY_ENTRY_SIZE 32U
#define FAT16_ENTRIES_PER_SECTOR (512U / FAT16_DIRECTORY_ENTRY_SIZE)
#define FAT_MUTATION __attribute__((section(".hightext")))

struct fat_directory {
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

static int fat16_mount(
	struct fat_mount_state *filesystem)
{
	struct fat_mount_state *fat;
	int result;
	uint32_t fat_entries;

	result = fat_engine_mount(filesystem, ZEDBSD_FAT16);
	if (result != 0)
		return result;
	fat = filesystem;
	if (!fat->root_entries || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U)
		return EIO;
	fat_entries = fat->fat_sectors * 512U / 2U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT16_RESERVED_CLUSTER)
		return EIO;
	return 0;
}

static int fat_raw_valid_cluster(const struct fat_mount_state *fat,
			       uint32_t cluster)
{
	return cluster >= 2U && cluster < fat->cluster_count + 2U;
}

static int fat_raw_is_end(const struct fat_mount_state *fat, uint32_t cluster)
{
	return cluster >= (fat->type == ZEDBSD_FAT12 ? 0xff8U :
		fat->type == ZEDBSD_FAT16 ? 0xfff8U : 0x0ffffff8U);
}

static uint32_t fat_raw_reserved_limit(const struct fat_mount_state *fat)
{
	return fat->type == ZEDBSD_FAT12 ? FAT12_RESERVED_CLUSTER :
		fat->type == ZEDBSD_FAT16 ? FAT16_RESERVED_CLUSTER :
		FAT32_RESERVED_CLUSTER;
}

static uint32_t fat_raw_end_of_chain(const struct fat_mount_state *fat)
{
	return fat->type == ZEDBSD_FAT12 ? FAT12_END_OF_CHAIN :
		fat->type == ZEDBSD_FAT16 ? FAT16_END_OF_CHAIN :
		FAT32_END_OF_CHAIN;
}

static uint32_t fat_raw_entry_offset(const struct fat_mount_state *fat,
				   uint32_t cluster)
{
	return fat->type == ZEDBSD_FAT12 ? cluster + cluster / 2U :
		fat->type == ZEDBSD_FAT16 ? cluster * 2U : cluster * 4U;
}

static int fat_raw_next_cluster(
	struct fat_mount_state *filesystem, uint32_t cluster,
	uint32_t *next_cluster)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t offset;
	const uint8_t *sector;
	int result;

	if (!next_cluster || !fat_raw_valid_cluster(fat, cluster))
		return EIO;
	offset = fat_raw_entry_offset(fat, cluster);
	result = fat_engine_read_sector_result(filesystem,
					       fat->fat_start + (offset >> 9),
					       &sector);
	if (result != 0)
		return result;
	if (fat->type == ZEDBSD_FAT32) {
		*next_cluster = fat_engine_get32(sector + (offset & 511U)) &
			0x0fffffffU;
		return 0;
	}
	if (fat->type == ZEDBSD_FAT16) {
		*next_cluster = fat_engine_get16(sector + (offset & 511U));
		return 0;
	}
	{
		/* A 12-bit entry may straddle a sector boundary, and the
		 * sector cache holds one sector, so latch the first byte
		 * before a second read can evict it. */
		uint8_t low = sector[offset & 511U];
		uint8_t high;
		uint32_t value;

		if ((offset & 511U) == 511U) {
			result = fat_engine_read_sector_result(
				filesystem,
				fat->fat_start + (offset >> 9) + 1U, &sector);
			if (result != 0)
				return result;
			high = sector[0];
		} else {
			high = sector[(offset & 511U) + 1U];
		}
		value = (uint32_t)low | ((uint32_t)high << 8);
		*next_cluster = (cluster & 1U) ? value >> 4 : value & 0xfffU;
	}
	return 0;
}

static int
fat_engine_count_free_clusters(struct fat_mount_state *filesystem,
	uint32_t *free_clusters)
{
	struct fat_mount_state *fat;
	uint32_t cluster, count = 0;

	if (filesystem == NULL || free_clusters == NULL)
		return EINVAL;
	fat = filesystem;
	if (fat == NULL || fat->cluster_count == 0)
		return EIO;
	for (cluster = 2U; cluster < fat->cluster_count + 2U; cluster++) {
		uint32_t value;
		int result =
		    fat_raw_next_cluster(filesystem, cluster, &value);
		if (result != 0)
			return result;
		if (value == 0)
			count++;
	}
	*free_clusters = count;
	return 0;
}

static int fat_raw_set_entry_byte(
	struct fat_mount_state *filesystem, uint32_t copy_start,
	uint32_t offset, uint8_t keep_mask, uint8_t merge_value)
{
	uint8_t *sector;
	int result;

	if ((offset >> 9) > 0xffffffffU - copy_start)
		return EIO;
	result = fat_engine_write_sector_result(
		filesystem, copy_start + (offset >> 9), &sector);
	if (result != 0)
		return result;
	sector[offset & 511U] =
		(uint8_t)((sector[offset & 511U] & keep_mask) | merge_value);
	result = fat_engine_mark_sector_dirty(filesystem);
	if (result == 0)
		result = fat_engine_flush(filesystem);
	return result;
}

static int
fat_raw_set_cluster_copy(struct fat_mount_state *filesystem,
	uint32_t cluster, uint32_t value, unsigned copy)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t offset;
	uint32_t copy_start;
	int result;

	if (!fat_raw_valid_cluster(fat, cluster) || copy >= fat->number_of_fats)
		return EIO;
	offset = fat_raw_entry_offset(fat, cluster);
	if (copy > (0xffffffffU - fat->fat_start) / fat->fat_sectors)
		return EIO;
	copy_start = fat->fat_start + copy * fat->fat_sectors;
	if (fat->type != ZEDBSD_FAT12) {
		uint8_t *sector;

		if ((offset >> 9) > 0xffffffffU - copy_start)
			return EIO;
		result = fat_engine_write_sector_result(
			filesystem, copy_start + (offset >> 9), &sector);
		if (result != 0)
			return result;
		if (fat->type == ZEDBSD_FAT32) {
			uint8_t *entry = sector + (offset & 511U);
			uint32_t old = fat_engine_get32(entry);
			put32(entry, (old & 0xf0000000U) |
			      (value & 0x0fffffffU));
		} else {
			put16(sector + (offset & 511U), (uint16_t)value);
		}
		result = fat_engine_mark_sector_dirty(filesystem);
		if (result == 0)
			result = fat_engine_flush(filesystem);
		return result;
	}
	/* Read-modify-write both bytes of the packed 12-bit entry; they may
	 * live in different sectors. */
	if (cluster & 1U) {
		result = fat_raw_set_entry_byte(filesystem, copy_start, offset,
			0x0f, (uint8_t)((value << 4) & 0xf0));
		if (result == 0)
			result = fat_raw_set_entry_byte(filesystem, copy_start,
				offset + 1U, 0x00, (uint8_t)(value >> 4));
	} else {
		result = fat_raw_set_entry_byte(filesystem, copy_start, offset,
			0x00, (uint8_t)value);
		if (result == 0)
			result = fat_raw_set_entry_byte(filesystem, copy_start,
				offset + 1U, 0xf0,
				(uint8_t)((value >> 8) & 0x0f));
	}
	return result;
}

static int
fat_raw_set_cluster(struct fat_mount_state *filesystem, uint32_t cluster,
	uint32_t value)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t old_value;
	unsigned copy;
	int result, rollback = 0;

	if (!fat_raw_valid_cluster(fat, cluster))
		return EIO;
	result = fat_raw_next_cluster(filesystem, cluster, &old_value);
	if (result != 0)
		return result;
	for (copy = 0; copy < fat->number_of_fats; copy++) {
		result = fat_raw_set_cluster_copy(filesystem, cluster, value,
			copy);
		if (result != 0)
			break;
	}
	if (result == 0)
		return 0;

	/* The cache still contains an unflushed update after a failed write.
	 * Restore every copy before returning so retry cannot publish half of a
	 * mirrored FAT update as a side effect.  A healthy volume keeps all
	 * copies equal, so the primary value is also the rollback value for its
	 * mirrors. */
	for (copy = 0; copy < fat->number_of_fats; copy++) {
		int error = fat_raw_set_cluster_copy(filesystem, cluster,
			old_value, copy);

		if (rollback == 0 && error != 0)
			rollback = error;
	}
	if (rollback != 0)
		filesystem->read_only = 1;
	return rollback != 0 ? rollback : result;
}

static uint32_t fat_raw_dir_cluster(const struct fat_mount_state *fat,
				  const uint8_t raw[32])
{
	uint32_t cluster = fat_engine_get16(raw + 26);

	if (fat->type == ZEDBSD_FAT32)
		cluster |= (uint32_t)fat_engine_get16(raw + 20) << 16;
	return cluster & 0x0fffffffU;
}

static void fat_raw_put_dir_cluster(const struct fat_mount_state *fat,
				  uint8_t raw[32], uint32_t cluster)
{
	put16(raw + 26, (uint16_t)cluster);
	if (fat->type == ZEDBSD_FAT32)
		put16(raw + 20, (uint16_t)(cluster >> 16));
}

static uint32_t fat_raw_root_cluster(const struct fat_mount_state *fat)
{
	return fat->type == ZEDBSD_FAT32 ? fat->root_cluster : 0;
}

/* Per-operation position only: never retained after releasing the mount lock. */
struct fat_chain_cursor {
	uint32_t index;
	uint32_t cluster;
};

static int
fat_raw_validate_chain_at(struct fat_mount_state *filesystem,
	uint32_t first_cluster, uint32_t wanted_index,
	struct fat_chain_cursor *cursor, uint32_t *last_cluster)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t cluster = first_cluster, checkpoint = first_cluster;
	uint32_t steps, span = 0U, power = 1U;

	if (!fat_raw_valid_cluster(fat, first_cluster))
		return EIO;
	if (cursor != NULL)
		cursor->cluster = 0U;
	for (steps = 0; steps <= fat->cluster_count; steps++) {
		uint32_t next;
		int result;

		if (cursor != NULL && steps == wanted_index) {
			cursor->index = steps;
			cursor->cluster = cluster;
		}

		/* Brent's single forward cursor retains bounded cycle detection
		 * without alternating distant FAT sectors on every link.  That
		 * alternation defeats this mount's one-sector cache for large loop
		 * backing files, even when the block cache already holds the FAT. */
		result = fat_raw_next_cluster(filesystem, cluster, &next);
		if (result != 0)
			return result;
		if (fat_raw_is_end(fat, next)) {
			if (last_cluster != NULL)
				*last_cluster = cluster;
			/* A growing write may begin beyond the old EOF.  Its seek can
			 * continue from this validated tail instead of the first link. */
			if (cursor != NULL && cursor->cluster == 0U) {
				cursor->index = steps;
				cursor->cluster = cluster;
			}
			return 0;
		}
		if (!fat_raw_valid_cluster(fat, next))
			return EIO;
		cluster = next;
		span++;
		if (cluster == checkpoint)
			return EIO;
		if (span == power) {
			checkpoint = cluster;
			span = 0U;
			if (power <= UINT32_MAX / 2U)
				power *= 2U;
		}
	}
	return EIO;
}

static int
fat_raw_validate_chain(struct fat_mount_state *filesystem,
	uint32_t first_cluster)
{
	return fat_raw_validate_chain_at(filesystem, first_cluster, 0U,
		NULL, NULL);
}

static int fat_raw_free_chain(
	struct fat_mount_state *filesystem, uint32_t first_cluster)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t *clusters;
	uint32_t cluster = first_cluster, count = 0, index;
	int result;

	if (!first_cluster)
		return 0;
	result = fat_raw_validate_chain(filesystem, first_cluster);
	if (result != 0)
		return result;

	for (;;) {
		uint32_t next;

		result = fat_raw_next_cluster(filesystem, cluster, &next);
		if (result != 0)
			return result;
		if (++count > fat->cluster_count)
			return EIO;
		if (fat_raw_is_end(fat, next))
			break;
		cluster = next;
	}
#if SIZE_MAX <= UINT32_MAX
	if (count > (uint32_t)(SIZE_MAX / sizeof(*clusters)))
		return ENOMEM;
#endif
	clusters = kern_malloc((size_t)count * sizeof(*clusters));
	if (clusters == NULL)
		return ENOMEM;
	cluster = first_cluster;
	for (index = 0; index < count; index++) {
		uint32_t next;

		clusters[index] = cluster;
		result = fat_raw_next_cluster(filesystem, cluster, &next);
		if (result != 0)
			goto out;
		cluster = next;
	}
	for (index = 0; index < count; index++) {
		result = fat_raw_set_cluster(filesystem, clusters[index], 0);
		if (result != 0) {
			uint32_t restore;
			int rollback = 0;

			/* The failing entry restores itself.  Recreate every link
			 * cleared earlier so callers can also restore the directory
			 * entry and expose the complete old file after an error. */
			for (restore = 0; restore < index; restore++) {
				int restore_error = fat_raw_set_cluster(filesystem,
				    clusters[restore], restore + 1U < count ?
				    clusters[restore + 1U] :
				    fat_raw_end_of_chain(fat));

				if (rollback == 0 && restore_error != 0)
					rollback = restore_error;
			}
			if (rollback != 0) {
				filesystem->read_only = 1;
				result = rollback;
			}
			goto out;
		}
	}
	result = 0;
out:
	kern_free(clusters);
	return result;
}

static int
fat_drain_pending_orphans(struct fat_mount_state *filesystem)
{
	while (filesystem->pending_orphan_count != 0U) {
		unsigned index = filesystem->pending_orphan_count - 1U;
		int result = fat_raw_free_chain(filesystem,
			filesystem->pending_orphans[index]);

		if (result != 0)
			return result;
		filesystem->pending_orphans[index] = 0;
		filesystem->pending_orphan_count--;
	}
	return 0;
}

static int
fat_defer_orphan(struct fat_mount_state *filesystem, uint32_t first_cluster)
{
	unsigned index;

	if (filesystem == NULL || first_cluster == 0)
		return EINVAL;
	for (index = 0; index < filesystem->pending_orphan_count; index++)
		if (filesystem->pending_orphans[index] == first_cluster)
			return 0;
	if (filesystem->pending_orphan_count >= FAT_INODE_MAX)
		return ENOSPC;
	filesystem->pending_orphans[filesystem->pending_orphan_count++] =
		first_cluster;
	return 0;
}

static int fat_raw_find_free_cluster(
	struct fat_mount_state *filesystem, uint32_t *free_cluster)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t start = fat->allocation_hint;
	uint32_t index;

	if (!free_cluster || !fat->cluster_count)
		return EIO;
	if (!fat_raw_valid_cluster(fat, start))
		start = 2;
	for (index = 0; index < fat->cluster_count; index++) {
		uint32_t cluster = 2U +
			((start - 2U + index) % fat->cluster_count);
		uint32_t value;
		int result;

		result = fat_raw_next_cluster(filesystem, cluster, &value);
		if (result != 0)
			return result;
		if (!value) {
			*free_cluster = cluster;
			fat->allocation_hint = cluster + 1U;
			if (!fat_raw_valid_cluster(fat, fat->allocation_hint))
				fat->allocation_hint = 2;
			return 0;
		}
	}
	return ENOSPC;
}

static int fat_raw_zero_cluster(
	struct fat_mount_state *filesystem, uint32_t cluster)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t index;

	for (index = 0; index < fat->sectors_per_cluster; index++) {
		uint32_t lba;
		uint8_t *sector;
		int result;

		result = fat_engine_cluster_lba(filesystem, cluster, index, &lba);
		if (result != 0)
			return result;
		result = fat_engine_write_sector_result(filesystem, lba, &sector);
		if (result != 0)
			return result;
		clear_bytes(sector, 512);
		result = fat_engine_mark_sector_dirty(filesystem);
		if (result == 0)
			result = fat_engine_flush(filesystem);
		if (result != 0)
			return result;
	}
	return 0;
}

static int fat_raw_allocate_cluster(
	struct fat_mount_state *filesystem, uint32_t *cluster)
{
	int result;

	result = fat_raw_find_free_cluster(filesystem, cluster);
	if (result != 0)
		return result;
	result = fat_raw_zero_cluster(filesystem, *cluster);
	if (result != 0)
		return result;
	return fat_raw_set_cluster(filesystem, *cluster,
				 fat_raw_end_of_chain(filesystem));
}

static int fat_raw_directory_entry(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory, uint32_t index,
	uint32_t *entry_lba, uint16_t *entry_offset, const uint8_t **raw)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t lba;
	uint16_t offset;
	const uint8_t *sector;
	int result;

	if (directory->first_cluster == 0) {
		if (index >= fat->root_entries)
			return ENOENT;
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

		if (!fat_raw_valid_cluster(fat, cluster) || !entries_per_cluster)
			return EIO;
		cluster_index = index / entries_per_cluster;
		index %= entries_per_cluster;
		while (cluster_index--) {
			uint32_t next;

			result = fat_raw_next_cluster(filesystem, cluster, &next);
			if (result != 0)
				return result;
			if (fat_raw_is_end(fat, next))
				return ENOENT;
			if (!fat_raw_valid_cluster(fat, next))
				return EIO;
			cluster = next;
		}
		sector_index = index / FAT16_ENTRIES_PER_SECTOR;
		result = fat_engine_cluster_lba(filesystem, cluster,
					       sector_index, &lba);
		if (result != 0)
			return result;
		offset = (uint16_t)((index % FAT16_ENTRIES_PER_SECTOR) *
				    FAT16_DIRECTORY_ENTRY_SIZE);
	}
	result = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;
	*entry_lba = lba;
	*entry_offset = offset;
	*raw = sector + offset;
	return 0;
}

static int fat_raw_find_entry(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory,
	const struct fat_component *component, enum fat_name_match match,
	uint32_t *entry_lba, uint16_t *entry_offset,
	uint32_t *free_lba, uint16_t *free_offset,
	char found_name[ZEDBSD_PATH_MAX])
{
	struct fat_mount_state *fat = filesystem;
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
		int result;

		result = fat_raw_directory_entry(filesystem, directory, index,
					       &lba, &offset, &raw);
		if (result == ENOENT)
			break;
		if (result != 0)
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
		} else {
			struct fat_dir_entry decoded;

			if (!fat_sfn_equal(raw, component->sfn))
				continue;
			if (found_name != 0) {
				fat_sfn_decode_lower(raw, &decoded);
				text_copy(found_name, decoded.name,
					ZEDBSD_PATH_MAX);
			}
		}
		{
			*entry_lba = lba;
			*entry_offset = offset;
			return 0;
		}
	}
	return have_free ? ENOENT : ENOSPC;
}

static int fat_raw_resolve_parent(
	struct fat_mount_state *filesystem, const char *path,
	struct fat_directory *parent, struct fat_component *component)
{
	const char *cursor = path;

	parent->first_cluster = fat_raw_root_cluster(filesystem);
	if (*cursor == '/')
		cursor++;
	if (!*cursor)
		return EINVAL;
	for (;;) {
		unsigned length = 0;
		const char *separator;
		uint32_t lba = 0, free_lba = 0;
		uint16_t offset = 0, free_offset = 0;
		const uint8_t *sector;
		int result;

		separator = cursor;
		while (*separator && *separator != '/')
			separator++;
		length = (unsigned)(separator - cursor);
		if (!length || length >= sizeof(component->text))
			return EINVAL;
		copy_bytes(component->text, cursor, length);
		component->text[length] = '\0';
		if (filesystem->type != ZEDBSD_FAT32 &&
		    !fat_sfn_encode(component->text, component->sfn))
			return EINVAL;
		if (!*separator)
			return 0;
		cursor = separator + 1;
		if (!*cursor)
			return EINVAL;
		result = fat_raw_find_entry(filesystem, parent, component,
					  FAT_NAME_EXACT, &lba, &offset,
					  &free_lba, &free_offset, 0);
		if (result != 0)
			return result == ENOSPC ?
				ENOENT : result;
		result = fat_engine_read_sector_result(filesystem, lba, &sector);
		if (result != 0)
			return result;
		if (!(sector[offset + 11] & 0x10U))
			return ENOENT;
		parent->first_cluster = fat_raw_dir_cluster(
			filesystem, sector + offset);
		if (!fat_raw_valid_cluster(filesystem,
					 parent->first_cluster))
			return EIO;
	}
}

static int fat_raw_resolve_entry(
	struct fat_mount_state *filesystem, const char *path,
	uint32_t *lba, uint16_t *offset, const uint8_t **raw,
	enum fat_name_match match, char found_name[ZEDBSD_PATH_MAX])
{
	struct fat_directory parent;
	struct fat_component component;
	uint32_t free_lba = 0;
	uint16_t free_offset = 0;
	const uint8_t *sector;
	int result;

	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;
	result = fat_raw_find_entry(filesystem, &parent, &component, match,
				  lba, offset, &free_lba, &free_offset,
				  found_name);
	if (result != 0)
		return result == ENOSPC ? ENOENT : result;
	result = fat_engine_read_sector_result(filesystem, *lba, &sector);
	if (result != 0)
		return result;
	*raw = sector + *offset;
	return 0;
}

static int fat_raw_populate_file(
	struct fat_file_state *file, uint32_t lba, uint16_t offset,
	const uint8_t raw[32])
{
	struct fat_mount_state *fat = file->mount;
	struct fat_file_state *state = file;

	state->first_cluster = fat_raw_dir_cluster(fat, raw);
	state->directory_lba = lba;
	state->directory_offset = offset;
	state->directory_dirty = 0;
	file->size = fat_engine_get32(raw + 28);
	if (!file->size && !state->first_cluster)
		return 0;
	return fat_raw_valid_cluster(fat, state->first_cluster) ?
		0 : EIO;
}

static void
fat_file_bind(struct fat_file_state *file, struct fat_mount_state *mount)
{
	file->mount = mount;
	file->size = 0;
	file->first_cluster = 0;
	file->directory_lba = 0;
	file->directory_offset = 0;
	file->directory_dirty = 0;
}

static int fat_raw_open(
	struct fat_mount_state *filesystem, const char *path,
	struct fat_file_state *file)
{
	uint32_t lba = 0;
	uint16_t offset = 0;
	const uint8_t *raw;
	int result;

	if (filesystem == NULL || path == NULL || file == NULL)
		return EINVAL;
	fat_file_bind(file, filesystem);
	result = fat_raw_resolve_entry(filesystem, path, &lba, &offset, &raw,
				     FAT_NAME_EXACT, 0);
	if (result != 0)
		return result;
	if (raw[11] & 0x10U)
		return EINVAL;
	return fat_raw_populate_file(file, lba, offset, raw);
}

static int fat_raw_flush_file(struct fat_file_state *file)
{
	struct fat_file_state *state = file;
	uint8_t *sector;
	int result;

	if (file->mount->read_only)
		return EROFS;
	result = fat_engine_flush(file->mount);
	if (result != 0 || !state->directory_dirty)
		return result;
	/* An unlinked-but-open inode has no directory entry to update.  Its
	 * cluster state remains live until the final reference triggers reclaim. */
	if (state->owner != NULL &&
	    (state->owner->i_flags & INODE_DEAD) != 0) {
		state->directory_dirty = 0;
		return 0;
	}
	if (file->size > 0xffffffffU)
		return EINVAL;
	result = fat_engine_write_sector_result(file->mount,
						state->directory_lba, &sector);
	if (result != 0)
		return result;
	fat_raw_put_dir_cluster(file->mount,
			      sector + state->directory_offset,
			      state->first_cluster);
	put32(sector + state->directory_offset + 28, (uint32_t)file->size);
	result = fat_engine_mark_sector_dirty(file->mount);
	if (result == 0)
		result = fat_engine_flush(file->mount);
	if (result == 0)
		state->directory_dirty = 0;
	return result;
}

static int
fat_raw_advance_cluster(struct fat_file_state *file, uint32_t cluster,
	int allocate, uint32_t *next)
{
	int result = fat_raw_next_cluster(file->mount, cluster, next);

	if (result != 0)
		return result;
	if (fat_raw_is_end(file->mount, *next)) {
		if (!allocate)
			return EIO;
		result = fat_raw_allocate_cluster(file->mount, next);
		if (result != 0)
			return result;
		result = fat_raw_set_cluster(file->mount, cluster, *next);
		if (result != 0) {
			int rollback;

			/* The new cluster is not reachable until the tail link
			 * succeeds.  Return it to the free pool on failure. */
			rollback = fat_raw_set_cluster(file->mount, *next, 0);
			if (rollback != 0) {
				file->mount->read_only = 1;
				return rollback;
			}
			return result;
		}
	} else if (!fat_raw_valid_cluster(file->mount, *next)) {
		return EIO;
	}
	return 0;
}

static int fat_raw_cluster_at(
	struct fat_file_state *file, uint32_t cluster_index, int allocate,
	uint32_t *found_cluster, struct fat_chain_cursor *cursor)
{
	struct fat_mount_state *fat = file->mount;
	struct fat_file_state *state = file;
	uint32_t cluster = state->first_cluster;
	uint32_t index = 0U;
	int result;

	if (cursor != NULL && cursor->cluster != 0U &&
	    cursor->index <= cluster_index) {
		cluster = cursor->cluster;
		index = cursor->index;
	}
	if (!cluster) {
		if (!allocate)
			return EIO;
		result = fat_raw_allocate_cluster(file->mount, &cluster);
		if (result != 0)
			return result;
		state->first_cluster = cluster;
		state->directory_dirty = 1;
	}
	if (!fat_raw_valid_cluster(fat, cluster))
		return EIO;
	for (; index < cluster_index; index++) {
		uint32_t next;

		if (index >= fat->cluster_count)
			return EIO;
		result = fat_raw_advance_cluster(file, cluster, allocate, &next);
		if (result != 0)
			return result;
		cluster = next;
	}
	*found_cluster = cluster;
	if (cursor != NULL) {
		cursor->index = cluster_index;
		cursor->cluster = cluster;
	}
	return 0;
}

static int fat_raw_write_bytes(
	struct fat_file_state *file, uint32_t offset, const uint8_t *input,
	uint32_t length, int zero, struct fat_chain_cursor *cursor)
{
	struct fat_mount_state *fat = file->mount;
	uint32_t cluster_bytes = (uint32_t)fat->sectors_per_cluster * 512U;
	uint32_t position = offset;
	uint32_t cluster;
	int result;

	if (length == 0U)
		return 0;
	result = fat_raw_cluster_at(file, position / cluster_bytes, 1, &cluster,
		cursor);
	if (result != 0)
		return result;

	while (length) {
		uint32_t in_cluster = position % cluster_bytes;
		uint32_t sector_index = in_cluster / 512U;
		uint32_t within = in_cluster & 511U;
		uint32_t chunk = 512U - within;
		uint32_t lba;
		uint8_t *sector;

		if (chunk > length)
			chunk = length;
		result = fat_engine_cluster_lba(file->mount, cluster,
						sector_index, &lba);
		if (result != 0)
			return result;
		result = fat_engine_write_sector_result(file->mount, lba,
						  &sector);
		if (result != 0)
			return result;
		if (zero)
			clear_bytes(sector + within, chunk);
		else
			copy_bytes(sector + within, input, chunk);
		result = fat_engine_mark_sector_dirty(file->mount);
		if (result == 0)
			result = fat_engine_flush(file->mount);
		if (result != 0)
			return result;
		if (!zero)
			input += chunk;
		position += chunk;
		length -= chunk;
		/* The mount lock keeps this operation's chain stable.  Reuse the
		 * current cluster within it and advance once at each boundary,
		 * instead of seeking again from the first cluster for every sector. */
		if (length != 0U && position % cluster_bytes == 0U) {
			uint32_t next;

			if (cursor->index >= fat->cluster_count)
				return EIO;
			result = fat_raw_advance_cluster(file, cluster, 1, &next);
			if (result != 0)
				return result;
			cluster = next;
			cursor->index++;
			cursor->cluster = cluster;
		}
	}
	return 0;
}

static int
fat_raw_rollback_growth(struct fat_file_state *file, uint32_t old_first,
	uint32_t old_last, uint64_t old_size, uint8_t old_directory_dirty)
{
	struct fat_mount_state *fat = file->mount;
	struct fat_file_state *state = file;
	uint32_t added = 0;
	int result = 0;

	if (old_first == 0) {
		added = state->first_cluster;
	} else if (old_last != 0) {
		result = fat_raw_next_cluster(fat, old_last, &added);
		if (result == 0 && fat_raw_is_end(fat, added))
			added = 0;
		else if (result == 0 && !fat_raw_valid_cluster(fat, added))
			result = EIO;
		if (result == 0 && added != 0)
			result = fat_raw_set_cluster(fat, old_last,
				fat_raw_end_of_chain(fat));
	}
	if (result == 0 && added != 0)
		result = fat_raw_free_chain(fat, added);
	state->first_cluster = old_first;
	file->size = old_size;
	state->directory_dirty = old_directory_dirty;
	if (result != 0)
		fat->read_only = 1;
	return result;
}

static int
fat_raw_restore_directory(struct fat_file_state *file, uint32_t first_cluster,
	uint64_t size, uint8_t directory_dirty)
{
	struct fat_file_state *state = file;
	int result;

	state->first_cluster = first_cluster;
	file->size = size;
	state->directory_dirty = 1;
	result = fat_raw_flush_file(file);
	if (result == 0)
		state->directory_dirty = directory_dirty;
	return result;
}

static int fat_raw_write(
	struct fat_file_state *file, uint64_t offset, const void *buffer,
	uint32_t length)
{
	struct fat_file_state *state = file;
	uint64_t end;
	uint64_t old_size;
	uint32_t old_first, old_last = 0;
	struct fat_chain_cursor cursor = {0U, 0U};
	uint8_t old_directory_dirty;
	int result;

	if (file->mount->read_only)
		return EROFS;
	if ((!buffer && length) || offset > 0xffffffffU ||
	    (uint64_t)length > 0xffffffffU - offset)
		return EINVAL;
	if (!length)
		return 0;
	end = offset + length;
	old_first = state->first_cluster;
	old_size = file->size;
	old_directory_dirty = state->directory_dirty;
	if (file->first_cluster) {
		uint32_t cluster_bytes =
			(uint32_t)file->mount->sectors_per_cluster * 512U;
		uint64_t first_offset = offset < old_size ? offset : old_size;

		/* Full validation precedes all writes, including corruption beyond
		 * the requested range.  Retain only this call's start and old tail
		 * so data/zero-fill/growth do not repeat the same validated walk. */
		result = fat_raw_validate_chain_at(file->mount,
			file->first_cluster, (uint32_t)first_offset / cluster_bytes,
			&cursor, end > old_size ? &old_last : NULL);
		if (result != 0)
			return result;
	}
	if (offset > file->size) {
		result = fat_raw_write_bytes(file, (uint32_t)file->size, 0,
					   (uint32_t)(offset - file->size), 1, &cursor);
		if (result != 0) {
			int rollback = fat_raw_rollback_growth(file, old_first,
				old_last, old_size, old_directory_dirty);
			if (rollback != 0)
				return rollback;
			return result;
		}
	}
	result = fat_raw_write_bytes(file, (uint32_t)offset, buffer, length, 0,
		&cursor);
	if (result != 0) {
		int rollback = fat_raw_rollback_growth(file, old_first, old_last,
			old_size, old_directory_dirty);
		if (rollback != 0)
			return rollback;
		return result;
	}
	if (end > file->size) {
		file->size = end;
		file->directory_dirty = 1;
	}
	return 0;
}

static int fat_raw_truncate(
	struct fat_file_state *file, uint64_t size)
{
	struct fat_mount_state *fat = file->mount;
	struct fat_file_state *state = file;
	uint32_t old_first = state->first_cluster;
	uint32_t old_last = 0U;
	struct fat_chain_cursor cursor = {0U, 0U};
	uint64_t old_size = file->size;
	uint8_t old_directory_dirty = state->directory_dirty;
	int result;

	if (file->mount->read_only)
		return EROFS;
	if (size > 0xffffffffU)
		return EINVAL;
	/* A zero-length file may still own a cluster chain. */
	if (size == file->size && (size || !state->first_cluster))
		return 0;
	if (state->first_cluster) {
		uint32_t cluster_bytes = (uint32_t)fat->sectors_per_cluster * 512U;
		uint64_t first_offset = size > old_size ? old_size :
			size != 0U ? size - 1U : 0U;

		result = fat_raw_validate_chain_at(file->mount, state->first_cluster,
			(uint32_t)first_offset / cluster_bytes, &cursor, &old_last);
		if (result != 0)
			return result;
	}
	if (size > file->size) {
		result = fat_raw_write_bytes(file, (uint32_t)file->size, 0,
					   (uint32_t)(size - file->size), 1, &cursor);
		if (result != 0) {
			int rollback = fat_raw_rollback_growth(file, old_first,
				old_last, old_size, old_directory_dirty);
			if (rollback != 0)
				return rollback;
			return result;
		}
		file->size = size;
		state->directory_dirty = 1;
		return 0;
	}
	if (!size) {
		state->first_cluster = 0;
		file->size = 0;
		state->directory_dirty = 1;
		result = fat_raw_flush_file(file);
		if (result != 0) {
			int rollback = fat_raw_restore_directory(file, old_first,
				old_size, old_directory_dirty);

			if (rollback != 0) {
				file->mount->read_only = 1;
				return rollback;
			}
			return result;
		}
		result = fat_raw_free_chain(file->mount, old_first);
		if (result != 0) {
			int rollback = fat_raw_restore_directory(file, old_first,
				old_size, old_directory_dirty);

			if (rollback != 0) {
				file->mount->read_only = 1;
				return rollback;
			}
			return result;
		}
		return 0;
	}
	{
		uint32_t cluster_bytes = (uint32_t)fat->sectors_per_cluster * 512U;
		uint32_t keep_index = ((uint32_t)size - 1U) / cluster_bytes;
		uint32_t keep, tail;

		result = fat_raw_cluster_at(file, keep_index, 0, &keep, &cursor);
		if (result != 0)
			return result;
		result = fat_raw_next_cluster(file->mount, keep, &tail);
		if (result != 0)
			return result;
		if (!fat_raw_is_end(fat, tail) &&
		    !fat_raw_valid_cluster(fat, tail))
			return EIO;
		if (!fat_raw_is_end(fat, tail)) {
			result = fat_raw_set_cluster(file->mount, keep,
				fat_raw_end_of_chain(fat));
			if (result != 0)
				return result;
		}
		file->size = size;
		state->directory_dirty = 1;
		result = fat_raw_flush_file(file);
		if (result != 0) {
			int rollback = 0, cleanup;

			if (!fat_raw_is_end(fat, tail)) {
				cleanup = fat_raw_set_cluster(file->mount, keep, tail);
				if (rollback == 0 && cleanup != 0)
					rollback = cleanup;
			}
			cleanup = fat_raw_restore_directory(file, old_first,
				old_size, old_directory_dirty);
			if (rollback == 0 && cleanup != 0)
				rollback = cleanup;
			if (rollback != 0) {
				file->mount->read_only = 1;
				return rollback;
			}
			return result;
		}
		if (fat_raw_is_end(fat, tail))
			return 0;
		result = fat_raw_free_chain(file->mount, tail);
		if (result != 0) {
			int rollback = 0, cleanup;

			cleanup = fat_raw_set_cluster(file->mount, keep, tail);
			if (cleanup != 0)
				rollback = cleanup;
			cleanup = fat_raw_restore_directory(file, old_first,
				old_size, old_directory_dirty);
			if (rollback == 0 && cleanup != 0)
				rollback = cleanup;
			if (rollback != 0) {
				file->mount->read_only = 1;
				return rollback;
			}
			return result;
		}
		return 0;
	}
}

static int fat_raw_sfn_in_use(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory, const uint8_t sfn[11])
{
	struct fat_mount_state *fat = filesystem;
	uint32_t limit = fat->cluster_count *
		(uint32_t)fat->sectors_per_cluster * FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;

	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		int result = fat_raw_directory_entry(
			filesystem, directory, index, &lba, &offset, &raw);
		(void)lba;
		(void)offset;
		if (result == ENOENT)
			return ENOENT;
		if (result != 0)
			return result;
		if (!raw[0])
			return ENOENT;
		if (raw[0] != 0xe5 && raw[11] != 0x0f &&
		    fat_sfn_equal(raw, (const char *)sfn))
			return 0;
	}
	return EIO;
}

static int fat_raw_extend_directory(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t last = directory->first_cluster;
	uint32_t steps, next, added;
	int result;

	if (fat->type != ZEDBSD_FAT32 || !fat_raw_valid_cluster(fat, last))
		return ENOSPC;
	for (steps = 0; steps < fat->cluster_count; steps++) {
		result = fat_raw_next_cluster(filesystem, last, &next);
		if (result != 0)
			return result;
		if (fat_raw_is_end(fat, next))
			break;
		if (!fat_raw_valid_cluster(fat, next))
			return EIO;
		last = next;
	}
	if (steps == fat->cluster_count)
		return EIO;
	result = fat_raw_allocate_cluster(filesystem, &added);
	if (result != 0)
		return result;
	result = fat_raw_set_cluster(filesystem, last, added);
	if (result != 0) {
		int rollback = fat_raw_set_cluster(filesystem, added, 0);

		if (rollback != 0) {
			filesystem->read_only = 1;
			return rollback;
		}
		return result;
	}
	return 0;
}

static int fat_raw_find_free_run(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory, unsigned needed,
	uint32_t *first_index)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t maximum = fat->cluster_count *
		(uint32_t)fat->sectors_per_cluster * FAT16_ENTRIES_PER_SECTOR;
	uint32_t index = 0, run_start = 0;
	unsigned run = 0;
	int after_end = 0;

	while (index < maximum) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		int result = fat_raw_directory_entry(
			filesystem, directory, index, &lba, &offset, &raw);
		(void)lba;
		(void)offset;
		if (result == ENOENT) {
			result = fat_raw_extend_directory(filesystem, directory);
			if (result != 0)
				return result;
			continue;
		}
		if (result != 0)
			return result;
		if (after_end || raw[0] == 0 || raw[0] == 0xe5) {
			if (run++ == 0)
				run_start = index;
			if (raw[0] == 0)
				after_end = 1;
			if (run == needed) {
				*first_index = run_start;
				return 0;
			}
		} else {
			run = 0;
		}
		index++;
	}
	return ENOSPC;
}

static FAT_MUTATION int
fat_raw_restore_directory_entry(struct fat_mount_state *filesystem,
	uint32_t lba, uint16_t offset, const uint8_t entry[32])
{
	uint8_t *sector;
	int result;

	result = fat_engine_write_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;
	copy_bytes(sector + offset, entry, 32U);
	result = fat_engine_mark_sector_dirty(filesystem);
	return result == 0 ? fat_engine_flush(filesystem) : result;
}

static int fat_raw_write_directory_entry(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory, uint32_t index,
	const uint8_t entry[32], uint32_t *written_lba,
	uint16_t *written_offset)
{
	uint32_t lba;
	uint16_t offset;
	const uint8_t *raw;
	uint8_t *sector;
	int result;

	result = fat_raw_directory_entry(
		filesystem, directory, index, &lba, &offset, &raw);
	if (result != 0)
		return result;
	(void)raw;
	result = fat_engine_write_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;
	copy_bytes(sector + offset, entry, 32);
	result = fat_engine_mark_sector_dirty(filesystem);
	if (result == 0)
		result = fat_engine_flush(filesystem);
	if (result == 0) {
		if (written_lba != 0) *written_lba = lba;
		if (written_offset != 0) *written_offset = offset;
	}
	return result;
}

static FAT_MUTATION int fat_raw_restore_directory_entries(
	struct fat_mount_state *filesystem, const uint32_t *lbas,
	const uint16_t *offsets, const uint8_t entries[][32], unsigned count);

static FAT_MUTATION int fat32_create_entry(
	struct fat_mount_state *filesystem,
	const struct fat_directory *parent,
	const struct fat_component *component, uint8_t attributes,
	uint32_t first_cluster, uint32_t size, uint32_t *entry_lba,
	uint16_t *entry_offset)
{
	uint16_t units[FAT_LFN_MAX_UNITS];
	uint8_t sfn[11], raw[32];
	uint8_t saved[FAT_LFN_MAX_ENTRIES + 1U][32];
	unsigned unit_count, lfn_count, serial, written = 0, i;
	uint32_t saved_lba[FAT_LFN_MAX_ENTRIES + 1U];
	uint16_t saved_offset[FAT_LFN_MAX_ENTRIES + 1U];
	uint32_t first_index = 0, sfn_lba = 0;
	uint16_t sfn_offset = 0;
	int result, rollback;

	if (!fat_utf8_to_utf16(component->text, units, &unit_count))
		return EINVAL;
	for (serial = 1; serial <= 999999U; serial++) {
		if (!fat_sfn_make_alias(component->text, serial, sfn))
			return EINVAL;
		result = fat_raw_sfn_in_use(filesystem, parent, sfn);
		if (result == ENOENT)
			break;
		if (result != 0)
			return result;
	}
	if (serial > 999999U)
		return ENOSPC;
	lfn_count = (unit_count + 12U) / 13U;
	result = fat_raw_find_free_run(filesystem, parent, lfn_count + 1U,
				     &first_index);
	if (result != 0)
		return result;
	/* A directory end marker is semantic state, not merely a free slot.
	 * Snapshot every slot before the first write so a failed multi-entry
	 * create restores 0x00 markers and any bytes hidden beyond them exactly.
	 */
	for (i = 0; i < lfn_count + 1U; i++) {
		const uint8_t *existing;

		result = fat_raw_directory_entry(filesystem, parent,
			first_index + i, &saved_lba[i], &saved_offset[i],
			&existing);
		if (result != 0)
			return result;
		copy_bytes(saved[i], existing, sizeof(saved[i]));
	}
	for (i = 0; i < lfn_count; i++) {
		unsigned ordinal = lfn_count - i;
		fat_lfn_build_entry(raw, units, unit_count, ordinal,
				    fat_lfn_checksum(sfn));
		written++;
		result = fat_raw_write_directory_entry(filesystem, parent,
			first_index + i, raw, 0, 0);
		if (result != 0)
			goto rollback;
	}
	clear_bytes(raw, sizeof(raw));
	copy_bytes(raw, sfn, 11);
	raw[11] = attributes;
	fat_raw_put_dir_cluster(filesystem, raw, first_cluster);
	put32(raw + 28, size);
	written++;
	result = fat_raw_write_directory_entry(filesystem, parent,
		first_index + lfn_count, raw, &sfn_lba, &sfn_offset);
	if (result != 0)
		goto rollback;
	if (entry_lba != 0)
		*entry_lba = sfn_lba;
	if (entry_offset != 0)
		*entry_offset = sfn_offset;
	return 0;
rollback:
	rollback = fat_raw_restore_directory_entries(filesystem, saved_lba,
		saved_offset, saved, written);
	return rollback != 0 ? rollback : result;
}

static FAT_MUTATION int
fat_raw_insert_entry(struct fat_mount_state *filesystem,
		   const struct fat_directory *parent,
		   const struct fat_component *component, uint8_t attributes,
		   uint32_t first_cluster, uint32_t size, uint32_t *entry_lba,
		   uint16_t *entry_offset)
{
	uint32_t lba = 0, free_lba = 0;
	uint16_t offset = 0, free_offset = 0;
	uint8_t saved[32];
	uint8_t *sector;
	int result, rollback;

	result = fat_raw_find_entry(filesystem, parent, component, FAT_NAME_EXACT,
		&lba, &offset, &free_lba, &free_offset, 0);
	if (result == 0)
		return EEXIST;
	if (result != ENOENT && result != ENOSPC)
		return result;
	if (filesystem->type == ZEDBSD_FAT32) {
		result = fat_raw_find_entry(filesystem, parent, component,
			FAT_NAME_CASEFOLD, &lba, &offset, &free_lba,
			&free_offset, 0);
		if (result == 0)
			return EEXIST;
		if (result != ENOENT && result != ENOSPC)
			return result;
		return fat32_create_entry(filesystem, parent, component,
			attributes, first_cluster, size, entry_lba, entry_offset);
	}
	if (result == ENOSPC)
		return result;
	result = fat_engine_write_sector_result(filesystem, free_lba, &sector);
	if (result != 0)
		return result;
	copy_bytes(saved, sector + free_offset, sizeof(saved));
	clear_bytes(sector + free_offset, 32);
	copy_bytes(sector + free_offset, component->sfn, 11);
	sector[free_offset + 11] = attributes;
	fat_raw_put_dir_cluster(filesystem,
		sector + free_offset, first_cluster);
	put32(sector + free_offset + 28, size);
	result = fat_engine_mark_sector_dirty(filesystem);
	if (result == 0)
		result = fat_engine_flush(filesystem);
	if (result != 0) {
		rollback = fat_raw_restore_directory_entry(filesystem, free_lba,
			free_offset, saved);
		if (rollback != 0)
			filesystem->read_only = 1;
		return rollback != 0 ? rollback : result;
	}
	if (result == 0) {
		if (entry_lba != 0)
			*entry_lba = free_lba;
		if (entry_offset != 0)
			*entry_offset = free_offset;
	}
	return result;
}

static FAT_MUTATION int fat_raw_delete_location(
	struct fat_mount_state *, const struct fat_directory *, uint32_t,
	uint16_t);

static FAT_MUTATION int fat_raw_create(
	struct fat_mount_state *filesystem, const char *path,
	struct fat_file_state *file)
{
	struct fat_directory parent;
	struct fat_component component;
	uint32_t lba = 0, free_lba = 0;
	uint16_t offset = 0, free_offset = 0;
	const uint8_t *sector;
	int result, rollback;

	if (filesystem == NULL || path == NULL || file == NULL)
		return EINVAL;
	fat_file_bind(file, filesystem);
	if (filesystem->read_only)
		return EROFS;
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;
	result = fat_raw_find_entry(filesystem, &parent, &component,
				  FAT_NAME_EXACT, &lba, &offset,
				  &free_lba, &free_offset, 0);
	if (result == 0)
		return EEXIST;
	if (result != ENOENT &&
	    !(filesystem->type == ZEDBSD_FAT32 &&
	      result == ENOSPC))
		return result;
	result = fat_raw_insert_entry(filesystem, &parent, &component, 0x20U,
		0, 0, &lba, &offset);
	if (result != 0)
		return result;
	result = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (result == 0)
		result = fat_raw_populate_file(file, lba, offset, sector + offset);
	if (result == 0)
		return 0;
	rollback = fat_raw_delete_location(filesystem, &parent, lba, offset);
	if (rollback != 0) {
		filesystem->read_only = 1;
		return rollback;
	}
	return result;
}

static FAT_MUTATION int
fat_raw_mark_deleted(struct fat_mount_state *filesystem, uint32_t lba,
		   uint16_t offset)
{
	uint8_t *sector;
	int result;

	result = fat_engine_write_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;
	sector[offset] = 0xe5;
	result = fat_engine_mark_sector_dirty(filesystem);
	return result == 0 ? fat_engine_flush(filesystem) : result;
}

static FAT_MUTATION int
fat_raw_restore_directory_entries(struct fat_mount_state *filesystem,
	const uint32_t *lbas, const uint16_t *offsets,
	const uint8_t entries[][32], unsigned count)
{
	unsigned i;
	int rollback = 0;

	for (i = 0; i < count; i++) {
		uint8_t *sector;
		int error;

		error = fat_engine_write_sector_result(filesystem, lbas[i],
		    &sector);
		if (error != 0) {
			if (rollback == 0)
				rollback = error;
			continue;
		}
		copy_bytes(sector + offsets[i], entries[i], 32U);
		error = fat_engine_mark_sector_dirty(filesystem);
		if (error == 0)
			error = fat_engine_flush(filesystem);
		if (rollback == 0 && error != 0)
			rollback = error;
	}
	if (rollback != 0)
		filesystem->read_only = 1;
	return rollback;
}

static FAT_MUTATION int
fat_raw_delete_location(struct fat_mount_state *filesystem,
		      const struct fat_directory *parent, uint32_t target_lba,
		      uint16_t target_offset)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t limit = parent->first_cluster == 0 ? fat->root_entries :
		fat->cluster_count * (uint32_t)fat->sectors_per_cluster *
		FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;

	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		int result = fat_raw_directory_entry(
			filesystem, parent, index, &lba, &offset, &raw);
		if (result != 0)
			return result;
		if (lba != target_lba || offset != target_offset)
			continue;
		/* Save the bounded LFN run and SFN before mutation.  A failed sector
		 * write may leave the cache dirty with the attempted deletion, so the
		 * failed entry itself is included in rollback. */
		{
			uint32_t saved_lba[FAT_LFN_MAX_ENTRIES + 1U];
			uint16_t saved_offset[FAT_LFN_MAX_ENTRIES + 1U];
			uint8_t saved[FAT_LFN_MAX_ENTRIES + 1U][32];
			uint8_t target[32];
			unsigned saved_count = 0, attempted = 0, i;

			copy_bytes(target, raw, sizeof(target));
			while (index != 0) {
				uint32_t previous_lba;
				uint16_t previous_offset;
				const uint8_t *previous;

				result = fat_raw_directory_entry(filesystem, parent,
					index - 1U, &previous_lba,
					&previous_offset, &previous);
				if (result != 0)
					return result;
				if (previous[11] != 0x0fU || previous[0] == 0xe5)
					break;
				if (saved_count >= FAT_LFN_MAX_ENTRIES)
					return EIO;
				saved_lba[saved_count] = previous_lba;
				saved_offset[saved_count] = previous_offset;
				copy_bytes(saved[saved_count], previous, 32U);
				saved_count++;
				index--;
			}
			if (saved_count >= FAT_LFN_MAX_ENTRIES + 1U)
				return EIO;
			saved_lba[saved_count] = target_lba;
			saved_offset[saved_count] = target_offset;
			copy_bytes(saved[saved_count], target, 32U);
			saved_count++;
			for (i = 0; i < saved_count; i++) {
				attempted = i + 1U;
				result = fat_raw_mark_deleted(filesystem,
					saved_lba[i], saved_offset[i]);
				if (result != 0) {
					int rollback =
					    fat_raw_restore_directory_entries(filesystem,
					    saved_lba, saved_offset, saved,
					    attempted);

					return rollback != 0 ? rollback : result;
				}
			}
			return 0;
		}
	}
	return ENOENT;
}

static FAT_MUTATION int
fat_raw_directory_empty(struct fat_mount_state *filesystem,
		      uint32_t first_cluster)
{
	struct fat_mount_state *fat = filesystem;
	struct fat_directory directory = { .first_cluster = first_cluster };
	uint32_t limit = fat->cluster_count *
		(uint32_t)fat->sectors_per_cluster * FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;

	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		int result = fat_raw_directory_entry(
			filesystem, &directory, index, &lba, &offset, &raw);
		(void)lba;
		(void)offset;
		if (result == ENOENT)
			return 0;
		if (result != 0)
			return result;
		if (raw[0] == 0)
			return 0;
		if (raw[0] == 0xe5 || raw[11] == 0x0fU ||
		    (raw[11] & 0x08U) != 0 || raw[0] == '.')
			continue;
		return ENOTEMPTY;
	}
	return EIO;
}

static FAT_MUTATION int
fat_raw_initialize_directory(struct fat_mount_state *filesystem,
			   uint32_t cluster, uint32_t parent_cluster)
{
	uint32_t lba;
	uint8_t *sector;
	uint8_t *raw;
	int result;

	result = fat_engine_cluster_lba(filesystem, cluster, 0, &lba);
	if (result != 0)
		return result;
	result = fat_engine_write_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;
	clear_bytes(sector, 512);
	raw = sector;
	for (unsigned i = 0; i < 11; i++)
		raw[i] = ' ';
	raw[0] = '.';
	raw[11] = 0x10U;
	fat_raw_put_dir_cluster(filesystem, raw, cluster);
	raw += 32;
	for (unsigned i = 0; i < 11; i++)
		raw[i] = ' ';
	raw[0] = raw[1] = '.';
	raw[11] = 0x10U;
	fat_raw_put_dir_cluster(filesystem, raw, parent_cluster);
	result = fat_engine_mark_sector_dirty(filesystem);
	return result == 0 ? fat_engine_flush(filesystem) : result;
}

static FAT_MUTATION int
fat_raw_mkdir(struct fat_mount_state *filesystem, const char *path,
	uint32_t *created_cluster)
{
	struct fat_directory parent;
	struct fat_component component;
	uint32_t cluster, lba;
	uint16_t offset;
	int result;

	if (created_cluster == NULL)
		return EINVAL;
	*created_cluster = 0;
	if (filesystem->read_only)
		return EROFS;
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;
	result = fat_raw_allocate_cluster(filesystem, &cluster);
	if (result != 0)
		return result;
	result = fat_raw_initialize_directory(filesystem, cluster,
		parent.first_cluster);
	if (result == 0)
		result = fat_raw_insert_entry(filesystem, &parent, &component,
			0x10U, cluster, 0, &lba, &offset);
	if (result != 0) {
		/* A failed entry rollback may have left a reachable reference to
		 * this cluster.  In that state leaking it is safer than freeing
		 * storage which an on-disk directory may still name. */
		if (filesystem->read_only)
			return result;
		int rollback = fat_raw_free_chain(filesystem, cluster);

		if (rollback != 0) {
			filesystem->read_only = 1;
			return rollback;
		}
	} else {
		*created_cluster = cluster;
	}
	return result;
}

static FAT_MUTATION int
fat_raw_remove(struct fat_mount_state *filesystem, const char *path,
	     int directory)
{
	struct fat_directory parent;
	struct fat_component component;
	uint32_t lba = 0, free_lba = 0, cluster;
	uint16_t offset = 0, free_offset = 0;
	uint8_t raw[32];
	const uint8_t *sector;
	int result;

	if (filesystem->read_only)
		return EROFS;
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;
	result = fat_raw_find_entry(filesystem, &parent, &component,
		FAT_NAME_EXACT, &lba, &offset, &free_lba, &free_offset, 0);
	if (result != 0)
		return result == ENOSPC ? ENOENT : result;
	result = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;
	copy_bytes(raw, sector + offset, sizeof(raw));
	if (directory != ((raw[11] & 0x10U) != 0))
		return directory ? EINVAL :
			EISDIR;
	cluster = fat_raw_dir_cluster(filesystem, raw);
	if (directory) {
		if (!fat_raw_valid_cluster(filesystem, cluster))
			return EIO;
		result = fat_raw_directory_empty(filesystem, cluster);
		if (result != 0)
			return result;
	}
	return fat_raw_delete_location(filesystem, &parent, lba, offset);
}

static FAT_MUTATION int
fat_raw_unlink(struct fat_mount_state *filesystem, const char *path)
{
	return fat_raw_remove(filesystem, path, 0);
}

static FAT_MUTATION int
fat_raw_rmdir(struct fat_mount_state *filesystem, const char *path)
{
	return fat_raw_remove(filesystem, path, 1);
}

static FAT_MUTATION int
fat_raw_update_dotdot(struct fat_mount_state *filesystem,
		    uint32_t directory_cluster, uint32_t parent_cluster)
{
	struct fat_directory directory = { .first_cluster = directory_cluster };
	uint32_t lba;
	uint16_t offset;
	uint8_t saved[32];
	const uint8_t *raw;
	uint8_t *sector;
	int result, rollback;

	result = fat_raw_directory_entry(
		filesystem, &directory, 1, &lba, &offset, &raw);
	if (result != 0)
		return result;
	copy_bytes(saved, raw, sizeof(saved));
	result = fat_engine_write_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;
	fat_raw_put_dir_cluster(filesystem, sector + offset,
		parent_cluster);
	result = fat_engine_mark_sector_dirty(filesystem);
	if (result == 0)
		result = fat_engine_flush(filesystem);
	if (result == 0)
		return 0;
	rollback = fat_raw_restore_directory_entry(filesystem, lba, offset,
		saved);
	if (rollback != 0)
		filesystem->read_only = 1;
	return rollback != 0 ? rollback : result;
}

static FAT_MUTATION int
fat_raw_restore_entry_payload(struct fat_mount_state *filesystem,
			    uint32_t lba, uint16_t offset,
			    const uint8_t raw[32])
{
	uint8_t restored[32];
	const uint8_t *current;
	int result;

	result = fat_engine_read_sector_result(filesystem, lba, &current);
	if (result != 0)
		return result;
	copy_bytes(restored, current + offset, sizeof(restored));
	restored[11] = raw[11];
	fat_raw_put_dir_cluster(filesystem, restored,
		fat_raw_dir_cluster(filesystem, raw));
	put32(restored + 28, fat_engine_get32(raw + 28));
	return fat_raw_restore_directory_entry(filesystem, lba, offset,
		restored);
}

static FAT_MUTATION void
fat_raw_rename_rollback_destination(struct fat_mount_state *filesystem,
				  const struct fat_directory *parent,
				  uint32_t lba, uint16_t offset,
				  int replacing, const uint8_t target[32])
{
	if (replacing)
		(void)fat_raw_restore_entry_payload(filesystem, lba, offset, target);
	else
		(void)fat_raw_delete_location(filesystem, parent, lba, offset);
}

struct fat_rename_result {
	uint32_t lba;
	uint16_t offset;
	uint8_t attributes;
};

static int
fat_raw_canonical_basename(struct fat_mount_state *filesystem,
	const char *path, char basename[ZEDBSD_PATH_MAX])
{
	struct fat_directory parent;
	struct fat_component component;
	uint8_t raw[32];
	int result;

	if (filesystem == NULL || path == NULL || basename == NULL)
		return EINVAL;
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;
	if (filesystem->type == ZEDBSD_FAT32) {
		text_copy(basename, component.text, ZEDBSD_PATH_MAX);
		return 0;
	}
	{
		struct fat_dir_entry decoded;

		clear_bytes(raw, sizeof(raw));
		copy_bytes(raw, component.sfn, sizeof(component.sfn));
		fat_sfn_decode_lower(raw, &decoded);
		text_copy(basename, decoded.name, ZEDBSD_PATH_MAX);
	}
	return 0;
}

static FAT_MUTATION int
fat_raw_rename(struct fat_mount_state *filesystem, const char *old_path,
	     const char *new_path, uint32_t authoritative_cluster,
	     uint32_t authoritative_size, struct fat_rename_result *renamed)
{
	struct fat_directory old_parent, new_parent;
	struct fat_component old_component, new_component;
	uint32_t old_lba = 0, old_free_lba = 0, new_lba = 0, new_free_lba = 0;
	uint16_t old_offset = 0, old_free_offset = 0;
	uint16_t new_offset = 0, new_free_offset = 0;
	uint8_t source[32], target[32];
	const uint8_t *sector;
	uint8_t *write_sector;
	uint32_t source_cluster;
	int replacing = 0;
	int result, target_result;

	if (filesystem->read_only)
		return EROFS;
	result = fat_raw_resolve_parent(filesystem, old_path, &old_parent,
		&old_component);
	if (result != 0)
		return result;
	result = fat_raw_find_entry(filesystem, &old_parent, &old_component,
		FAT_NAME_EXACT, &old_lba, &old_offset, &old_free_lba,
		&old_free_offset, 0);
	if (result != 0)
		return result == ENOSPC ? ENOENT : result;
	result = fat_engine_read_sector_result(filesystem, old_lba, &sector);
	if (result != 0)
		return result;
	copy_bytes(source, sector + old_offset, sizeof(source));
	/* The inode/open-file state is authoritative while a writer is open.
	 * Publish that state at the destination as part of the rename commit so
	 * no fallible repair read/write remains after the old name is removed. */
	fat_raw_put_dir_cluster(filesystem, source, authoritative_cluster);
	put32(source + 28, authoritative_size);
	result = fat_raw_resolve_parent(filesystem, new_path, &new_parent,
		&new_component);
	if (result != 0)
		return result;
	target_result = fat_raw_find_entry(filesystem, &new_parent, &new_component,
		FAT_NAME_EXACT, &new_lba, &new_offset, &new_free_lba,
		&new_free_offset, 0);
	if (target_result == 0) {
		if (old_lba == new_lba && old_offset == new_offset) {
			if (renamed != NULL) {
				renamed->lba = old_lba;
				renamed->offset = old_offset;
				renamed->attributes = source[11];
			}
			return 0;
		}
		result = fat_engine_read_sector_result(filesystem, new_lba,
			&sector);
		if (result != 0)
			return result;
		copy_bytes(target, sector + new_offset, sizeof(target));
		if (((source[11] ^ target[11]) & 0x10U) != 0)
			return EINVAL;
		if ((target[11] & 0x10U) != 0) {
			result = fat_raw_directory_empty(filesystem,
				fat_raw_dir_cluster(filesystem, target));
			if (result != 0)
				return result;
		}
		/* Preserve the destination's spelling/LFN run and replace only its
		 * payload.  Open references to the old target keep their own cluster
		 * state and the VFS marks that inode orphaned after this succeeds. */
		result = fat_engine_write_sector_result(filesystem, new_lba,
			&write_sector);
		if (result != 0)
			return result;
		write_sector[new_offset + 11] = source[11];
		fat_raw_put_dir_cluster(filesystem,
			write_sector + new_offset,
			fat_raw_dir_cluster(filesystem, source));
		put32(write_sector + new_offset + 28,
			fat_engine_get32(source + 28));
		result = fat_engine_mark_sector_dirty(filesystem);
		if (result == 0)
			result = fat_engine_flush(filesystem);
		if (result != 0) {
			int rollback = fat_raw_restore_entry_payload(filesystem,
				new_lba, new_offset, target);

			return rollback != 0 ? rollback : result;
		}
		replacing = 1;
	} else if (target_result != ENOENT &&
		   target_result != ENOSPC) {
		return target_result;
	}
	source_cluster = fat_raw_dir_cluster(filesystem, source);
	if (!replacing) {
		result = fat_raw_insert_entry(filesystem, &new_parent, &new_component,
			source[11], source_cluster, fat_engine_get32(source + 28),
			&new_lba, &new_offset);
		if (result != 0)
			return result;
	}
	if ((source[11] & 0x10U) != 0 &&
	    old_parent.first_cluster != new_parent.first_cluster) {
		result = fat_raw_update_dotdot(filesystem, source_cluster,
			new_parent.first_cluster);
		if (result != 0) {
			fat_raw_rename_rollback_destination(filesystem, &new_parent,
				new_lba, new_offset, replacing, target);
			return result;
		}
	}
	result = fat_raw_delete_location(filesystem, &old_parent,
		old_lba, old_offset);
	if (result != 0) {
		if ((source[11] & 0x10U) != 0 &&
		    old_parent.first_cluster != new_parent.first_cluster)
			(void)fat_raw_update_dotdot(filesystem, source_cluster,
				old_parent.first_cluster);
		fat_raw_rename_rollback_destination(filesystem, &new_parent,
			new_lba, new_offset, replacing, target);
	} else if (renamed != NULL) {
		renamed->lba = new_lba;
		renamed->offset = new_offset;
		renamed->attributes = source[11];
	}
	return result;
}

static int fat_raw_read(
	struct fat_file_state *file, uint64_t offset, void *buffer, uint32_t length,
	fat_read_progress_fn progress, void *progress_context)
{
	return fat_engine_read_chain(
		file, offset, buffer, length, progress, progress_context,
		fat_raw_next_cluster,
		fat_raw_reserved_limit(file->mount));
}

static int fat_raw_readdir(
	struct fat_mount_state *filesystem, const char *path, unsigned wanted,
	struct fat_dir_entry *entry)
{
	struct fat_mount_state *fat = filesystem;
	struct fat_directory directory = {
		.first_cluster = fat_raw_root_cluster(fat),
	};
	struct fat_lfn_state lfn;
	uint32_t limit;
	unsigned visible = 0;
	uint32_t index;

	if (*path && !(path[0] == '/' && !path[1])) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		int result = fat_raw_resolve_entry(
			filesystem, path, &lba, &offset, &raw,
			FAT_NAME_EXACT, 0);

		if (result != 0)
			return result;
		if (!(raw[11] & 0x10U))
			return EINVAL;
		directory.first_cluster = fat_raw_dir_cluster(fat, raw);
		if (!fat_raw_valid_cluster(fat, directory.first_cluster))
			return EIO;
	}
	limit = directory.first_cluster == 0 ? fat->root_entries :
		fat->cluster_count * (uint32_t)fat->sectors_per_cluster *
		FAT16_ENTRIES_PER_SECTOR;
	fat_lfn_reset(&lfn);
	for (index = 0; index < limit; index++) {
		uint32_t lba;
		uint16_t offset;
		const uint8_t *raw;
		int result;

		result = fat_raw_directory_entry(filesystem, &directory, index,
					       &lba, &offset, &raw);
		if (result == ENOENT)
			return result;
		if (result != 0)
			return result;
		if (!raw[0]) {
			fat_lfn_reset(&lfn);
			return ENOENT;
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
		entry->size = fat_engine_get32(raw + 28);
		entry->attributes = raw[11];
		return 0;
	}
	return ENOENT;
}

static int fat_stat_location_mode(
	struct fat_mount_state *filesystem, const char *path,
	struct fat_dir_entry *entry, uint32_t *lba, uint16_t *offset,
	uint32_t *first_cluster, uint8_t *attributes,
	enum fat_name_match match)
{
	const uint8_t *raw;
	char found_name[ZEDBSD_PATH_MAX];
	int result;

	if (!filesystem || !path || !entry || !lba || !offset ||
	    !first_cluster || !attributes)
		return EINVAL;
	result = fat_raw_resolve_entry(filesystem, path, lba, offset, &raw,
				     match, found_name);
	if (result != 0)
		return result;
	if (filesystem->type == ZEDBSD_FAT32) {
		text_copy(entry->name, found_name, sizeof(entry->name));
		entry->size = fat_engine_get32(raw + 28);
		entry->attributes = raw[11];
	} else {
		fat_sfn_decode_lower(raw, entry);
	}
	*first_cluster = fat_raw_dir_cluster(filesystem, raw);
	*attributes = raw[11];
	return 0;
}

static int
fat_engine_stat_location(struct fat_mount_state *filesystem,
			  const char *path, struct fat_dir_entry *entry,
			  uint32_t *lba, uint16_t *offset,
			  uint32_t *first_cluster, uint8_t *attributes)
{
	return fat_stat_location_mode(filesystem, path, entry, lba, offset,
				      first_cluster, attributes,
				      FAT_NAME_EXACT);
}

static int
fat_engine_stat_location_casefold(struct fat_mount_state *filesystem,
				  const char *path,
				  struct fat_dir_entry *entry,
				  uint32_t *lba, uint16_t *offset,
				  uint32_t *first_cluster,
				  uint8_t *attributes)
{
	if (filesystem == 0 ||
	    filesystem->type != ZEDBSD_FAT32)
		return EOPNOTSUPP;
	return fat_stat_location_mode(filesystem, path, entry, lba, offset,
				      first_cluster, attributes,
				      FAT_NAME_CASEFOLD);
}

static int
fat_engine_file_extents(struct fat_file_state *file, fat_extent_cb callback,
			void *context)
{
	struct fat_mount_state *filesystem;
	struct fat_mount_state *fat;
	struct fat_file_state *state;
	uint64_t remaining, file_block = 0, run_file = 0, run_disk = 0;
	uint32_t run_count = 0, cluster, steps;

	if (file == NULL || callback == NULL || file->mount == NULL)
		return EINVAL;
	filesystem = file->mount;
	fat = filesystem;
	state = file;
	if (fat->type != ZEDBSD_FAT12 && fat->type != ZEDBSD_FAT16 &&
	    fat->type != ZEDBSD_FAT32)
		return EIO;
	remaining = file->size;
	if (remaining == 0)
		return state->first_cluster == 0 ? 0 : EIO;
	cluster = state->first_cluster;
	if (!fat_raw_valid_cluster(fat, cluster))
		return EIO;
	for (steps = 0; steps < fat->cluster_count && remaining != 0; steps++) {
		uint32_t disk_block, blocks = fat->sectors_per_cluster;
		uint32_t next;
		int result;

		if ((uint64_t)blocks * 512U > remaining)
			blocks = (uint32_t)((remaining + 511U) / 512U);
		result = fat_engine_cluster_lba(filesystem, cluster, 0,
					       &disk_block);
		if (result != 0)
			return result;
		if (run_count != 0 && run_disk + run_count == disk_block &&
		    run_file + run_count == file_block) {
			run_count += blocks;
		} else {
			if (run_count != 0) {
				result = callback(run_file, run_disk, run_count,
						  context);
				if (result != 0)
					return result;
			}
			run_file = file_block;
			run_disk = disk_block;
			run_count = blocks;
		}
		file_block += blocks;
		remaining -= remaining > (uint64_t)blocks * 512U ?
			(uint64_t)blocks * 512U : remaining;
		result = fat_raw_next_cluster(filesystem, cluster, &next);
		if (result != 0)
			return result;
		if (remaining == 0) {
			if (!fat_raw_is_end(fat, next))
				return EIO;
			break;
		}
		if (!fat_raw_valid_cluster(fat, next))
			return EIO;
		cluster = next;
	}
	if (remaining != 0 || run_count == 0)
		return EIO;
	return callback(run_file, run_disk, run_count, context);
}

static int
fat_engine_discard_chain_result(struct fat_mount_state *filesystem,
				uint32_t first_cluster)
{
	if (filesystem == NULL)
		return EINVAL;
	return fat_raw_free_chain(filesystem, first_cluster);
}

static int fat12_mount(
	struct fat_mount_state *filesystem)
{
	struct fat_mount_state *fat;
	int result;
	uint32_t fat_entries;

	result = fat_engine_mount(filesystem, ZEDBSD_FAT12);
	if (result != 0)
		return result;
	fat = filesystem;
	if (!fat->root_entries || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U)
		return EIO;
	/* Three bytes hold two packed 12-bit entries. */
	fat_entries = fat->fat_sectors * 512U / 3U * 2U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT12_RESERVED_CLUSTER)
		return EIO;
	return 0;
}

static int fat32_mount(
	struct fat_mount_state *filesystem)
{
	struct fat_mount_state *fat;
	int result;
	uint32_t fat_entries;

	result = fat_engine_mount(filesystem, ZEDBSD_FAT32);
	if (result != 0)
		return result;
	fat = filesystem;
	if (!fat->fat32_layout || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U)
		return EIO;
	fat_entries = fat->fat_sectors * 512U / 4U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT32_RESERVED_CLUSTER)
		return EIO;
	return fat_raw_valid_cluster(fat, fat->root_cluster) ?
		0 : EIO;
}

/* Native VFS operations over the private FAT engine. */

struct fat_inode_slot {
	struct fat_inode_info info;
	char path[ZEDBSD_PATH_MAX];
	uint8_t used;
};

static struct fat_mount_state fat_mounts[FAT_MOUNT_MAX]
    __attribute__((section(".vfs_bss")));
static struct fat_metadata_table fat_metadata_tables[FAT_MOUNT_MAX]
    __attribute__((section(".vfs_bss")));
static struct fat_inode_slot fat_inodes[FAT_INODE_MAX]
    __attribute__((section(".vfs_bss")));
static struct fat_file_state fat_files[FAT_FILE_MAX]
    __attribute__((section(".vfs_bss")));
static struct spinlock fat_pool_lock = {
    {0}, LOCK_RANK_INODE, "FAT object pools", 0, 0};

static struct fat_mount_state *
fat_mount_state(struct mount *mountp)
{
	return mountp != NULL ? mountp->m_data : NULL;
}

static int
fat_metadata_number(const char *text, unsigned base, uint32_t *value)
{
	uint32_t result = 0;
	if (*text == '\0')
		return EINVAL;
	while (*text != '\0') {
		unsigned digit = (unsigned)(*text++ - '0');
		if (digit >= base || result > (UINT32_MAX - digit) / base)
			return EINVAL;
		result = result * base + digit;
	}
	*value = result;
	return 0;
}

static void
fat_metadata_load(struct fat_mount_state *state)
{
	struct fat_file_state file = {0};
	char buffer[4096];
	uint32_t length, offset = 0;

	if (fat_raw_open(state, "etc/unixmode", &file) !=
	    0)
		return;
	length = file.size < sizeof(buffer) - 1U
		     ? (uint32_t)file.size
		     : (uint32_t)sizeof(buffer) - 1U;
	if (fat_raw_read(&file, 0, buffer, length, NULL, NULL) !=
	    0)
		return;
	buffer[length] = '\0';
	while (offset < length && state->metadata != NULL &&
	       state->metadata->count < FAT_METADATA_MAX) {
		struct fat_metadata *metadata =
		    &state->metadata->entries[state->metadata->count];
		char *line = buffer + offset, *mode, *uid, *gid, *end;
		uint32_t mode_value, uid_value, gid_value;

		end = strchr(line, '\n');
		if (end != NULL)
			*end = '\0';
		offset += (uint32_t)strlen(line) + (end != NULL ? 1U : 0U);
		mode = strchr(line, ':');
		if (mode == NULL)
			continue;
		*mode++ = '\0';
		uid = strchr(mode, ':');
		if (uid == NULL)
			continue;
		*uid++ = '\0';
		gid = strchr(uid, ':');
		if (gid == NULL)
			continue;
		*gid++ = '\0';
		if (strchr(gid, ':') != NULL || line[0] == '/' ||
		    line[0] == '\0' || strlen(line) >= sizeof(metadata->path) ||
		    fat_metadata_number(mode, 8, &mode_value) != 0 ||
		    fat_metadata_number(uid, 10, &uid_value) != 0 ||
		    fat_metadata_number(gid, 10, &gid_value) != 0 ||
		    mode_value > 07777U)
			continue;
		strcpy(metadata->path, line);
		metadata->mode = (mode_t)mode_value;
		metadata->uid = (uid_t)uid_value;
		metadata->gid = (gid_t)gid_value;
		state->metadata->count++;
	}
}

static const struct fat_metadata *
fat_metadata_find(const struct fat_mount_state *state, const char *path)
{
	unsigned i;

	for (i = 0; state != NULL && state->metadata != NULL &&
	     i < state->metadata->count; i++)
		if (!strcmp(state->metadata->entries[i].path, path))
			return &state->metadata->entries[i];
	return NULL;
}

static void
fat_metadata_apply(struct mount *mountp, const char *path, struct inode *inode)
{
	const struct fat_metadata *metadata =
	    fat_metadata_find(fat_mount_state(mountp), path);

	if (metadata == NULL)
		return;
	inode->i_mode = (inode->i_mode & S_IFMT) | metadata->mode;
	inode->i_uid = metadata->uid;
	inode->i_gid = metadata->gid;
}

static struct fat_inode_slot *
fat_slot(struct inode *inode)
{
	unsigned i;
	for (i = 0; i < FAT_INODE_MAX; i++)
		if (&fat_inodes[i].info.fi_inode == inode)
			return &fat_inodes[i];
	return NULL;
}

static const char *
fat_path(struct inode *inode)
{
	struct fat_inode_slot *slot = fat_slot(inode);
	return slot != NULL ? slot->path : NULL;
}

static struct inode *
fat_alloc_inode(struct mount *mountp)
{
	unsigned i;
	unsigned long irq;
	(void)mountp;
	irq = spin_lock_irqsave(&fat_pool_lock);
	for (i = 0; i < FAT_INODE_MAX; i++) {
		if (!fat_inodes[i].used) {
			fat_inodes[i].used = 1;
			memset(&fat_inodes[i].info, 0,
			       sizeof(fat_inodes[i].info));
			fat_inodes[i].path[0] = '\0';
			spin_unlock_irqrestore(&fat_pool_lock, irq);
			return &fat_inodes[i].info.fi_inode;
		}
	}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	return NULL;
}

static void
fat_free_inode(struct inode *inode)
{
	struct fat_inode_slot *slot = fat_slot(inode);
	unsigned long irq;
	if (slot != NULL) {
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(slot, 0, sizeof(*slot));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
	}
}

static int
join_path(const char *parent, const struct componentname *name,
	  char output[ZEDBSD_PATH_MAX])
{
	size_t parent_length = strlen(parent);
	if (name->cn_namelen == 0 || name->cn_namelen > NAME_MAX ||
	    parent_length + (parent_length != 0) + name->cn_namelen >=
		ZEDBSD_PATH_MAX)
		return ENAMETOOLONG;
	memcpy(output, parent, parent_length);
	if (parent_length != 0)
		output[parent_length++] = '/';
	memcpy(output + parent_length, name->cn_nameptr, name->cn_namelen);
	output[parent_length + name->cn_namelen] = '\0';
	return 0;
}

static int
fat_creation_collision(struct fat_mount_state *state, const char *path)
{
	struct fat_directory parent;
	struct fat_component component;
	uint32_t lba = 0, free_lba = 0;
	uint16_t offset = 0, free_offset = 0;
	int error;

	if (state == NULL || path == NULL)
		return EINVAL;
	error = fat_raw_resolve_parent(state, path, &parent, &component);
	if (error != 0)
		return error;
	error = fat_raw_find_entry(state, &parent, &component, FAT_NAME_EXACT,
	    &lba, &offset, &free_lba, &free_offset, 0);
	if (error == 0)
		return EEXIST;
	if (error != ENOENT && error != ENOSPC)
		return error;
	if (state->type != ZEDBSD_FAT32)
		return 0;
	error = fat_raw_find_entry(state, &parent, &component,
	    FAT_NAME_CASEFOLD, &lba, &offset, &free_lba, &free_offset, 0);
	if (error == 0)
		return EEXIST;
	return error == ENOENT || error == ENOSPC ? 0 : error;
}

static int
fat_creation_representation(const struct fat_mount_state *state,
	const char *path, mode_t *mode, uid_t *uid, gid_t *gid)
{
	const struct fat_metadata *metadata;

	if (state == NULL || path == NULL || mode == NULL || uid == NULL ||
	    gid == NULL)
		return EINVAL;
	metadata = fat_metadata_find(state, path);
	if (metadata != NULL) {
		*mode = metadata->mode;
		*uid = metadata->uid;
		*gid = metadata->gid;
	} else {
		*mode = 0755U;
		*uid = 0;
		*gid = 0;
	}
	return 0;
}

static int
fat_creation_representable(const struct fat_mount_state *state,
	const char *path, const struct inode_creation_request *request,
	enum inode_type type)
{
	mode_t mode;
	uid_t uid;
	gid_t gid;
	int error;

	if (request == NULL || request->origin < INODE_CREATION_USER ||
	    request->origin > INODE_CREATION_PRESERVE ||
	    request->type != type || (request->mode & S_IFMT) != 0 ||
	    request->special != NULL || request->rdev != 0)
		return EINVAL;
	error = fat_creation_representation(state, path, &mode, &uid, &gid);
	if (error != 0)
		return error;
	return mode == (request->mode & 07777U) && uid == request->uid &&
	    gid == request->gid ? 0 : EOPNOTSUPP;
}

static int
fat_created_inode_matches(const struct fat_mount_state *state,
	const char *path, const struct inode *inode)
{
	mode_t mode;
	uid_t uid;
	gid_t gid;
	int error = fat_creation_representation(state, path, &mode, &uid, &gid);

	if (error != 0)
		return error;
	return (inode->i_mode & 07777U) == mode && inode->i_uid == uid &&
	    inode->i_gid == gid ? 0 : EOPNOTSUPP;
}

static ino_t
fat_ino(uint32_t lba, uint16_t offset)
{
	return 2U + (ino_t)lba * 16U + offset / 32U;
}

static int
fat_leap_year(int year)
{
	return (year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0);
}

static int
fat_month_days(int year, int month)
{
	static const uint8_t days[] = {
	    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};
	return month == 2 && fat_leap_year(year) ? 29 : days[month - 1];
}

static time_t
fat_decode_time(uint16_t date, uint16_t time)
{
	int year, month, day, days = 0;
	int64_t seconds;

	if (date == 0)
		return 0;
	year = 1980 + ((date >> 9) & 0x7f);
	month = (date >> 5) & 0x0f;
	day = date & 0x1f;
	if (month < 1 || month > 12 || day < 1 ||
	    day > fat_month_days(year, month))
		return 0;
	for (int y = 1970; y < year; y++)
		days += fat_leap_year(y) ? 366 : 365;
	for (int m = 1; m < month; m++)
		days += fat_month_days(year, m);
	days += day - 1;
	seconds = (int64_t)days * 86400 + ((time >> 11) & 0x1f) * 3600 +
		  ((time >> 5) & 0x3f) * 60 + (time & 0x1f) * 2;
#ifdef ZEDBSD_USER_ABI_LP64
	return (time_t)seconds;
#else
	return seconds > INT32_MAX ? (time_t)INT32_MAX : (time_t)seconds;
#endif
}

static FAT_MUTATION int
fat_encode_time(time_t seconds, uint16_t *date, uint16_t *time)
{
	int64_t days, remainder;
	int year = 1970, month = 1;

	if (seconds < FAT_EPOCH_1980)
		return EOVERFLOW;
	days = seconds / 86400;
	remainder = seconds % 86400;
	while (days >= (fat_leap_year(year) ? 366 : 365)) {
		days -= fat_leap_year(year) ? 366 : 365;
		year++;
	}
	if (year > 2107)
		return EOVERFLOW;
	while (days >= fat_month_days(year, month)) {
		days -= fat_month_days(year, month);
		month++;
	}
	*date = (uint16_t)(((year - 1980) << 9) | (month << 5) | (days + 1));
	*time =
	    (uint16_t)(((remainder / 3600) << 11) |
		       (((remainder / 60) % 60) << 5) | ((remainder % 60) / 2));
	return 0;
}

static void
fat_load_inode_times(struct mount *mountp, struct inode *inode, uint32_t lba,
		     uint16_t offset)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	const uint8_t *sector;
	const uint8_t *raw;

	if (state == NULL || fat_engine_read_sector_result(
				 state, lba, &sector) != 0)
		return;
	raw = sector + offset;
	inode->i_atime.tv_sec = fat_decode_time(fat_engine_get16(raw + 18), 0);
	inode->i_mtime.tv_sec =
	    fat_decode_time(fat_engine_get16(raw + 24), fat_engine_get16(raw + 22));
	inode->i_ctime.tv_sec =
	    fat_decode_time(fat_engine_get16(raw + 16), fat_engine_get16(raw + 14));
}

static int
fat_make_inode(struct mount *mountp, const char *path,
	       const struct fat_dir_entry *entry, uint32_t lba, uint16_t offset,
	       uint32_t first_cluster, uint8_t attributes,
	       struct inode **result)
{
	struct fat_inode_info *info;
	struct fat_inode_slot *slot;
	struct inode *inode;
	ino_t ino = fat_ino(lba, offset);
	int error = inode_get(mountp, ino, result);
	if (error == 0)
		return 0;
	inode = inode_alloc(mountp);
	if (inode == NULL)
		return ENOSPC;
	info = fat_inode(inode);
	slot = fat_slot(inode);
	if (slot == NULL || strlen(path) >= ZEDBSD_PATH_MAX) {
		inode_release(inode);
		return EINVAL;
	}
	strcpy(slot->path, path);
	info->fi_first_cluster = first_cluster;
	info->fi_dirent_lba = lba;
	info->fi_dirent_offset = offset;
	info->fi_attributes = attributes;
	inode->i_ino = ino;
	inode->i_data = info;
	inode->i_linkcount = 1;
	inode->i_uid = inode->i_gid = 0;
	inode->i_size = (off_t)entry->size;
	if (attributes & FAT_ATTRIBUTE_DIRECTORY) {
		inode->i_type = INODE_DIR;
		inode->i_mode = S_IFDIR | 0755U;
		inode->i_size = 0;
	} else {
		inode->i_type = INODE_REG;
		/* FAT has no execute bit.  Mount regular files with the
		 * executable default expected by this boot/userland volume. */
		inode->i_mode = S_IFREG | 0755U;
	}
	if (attributes & FAT_ATTRIBUTE_READ_ONLY)
		inode->i_mode &= ~(mode_t)0222U;
	fat_metadata_apply(mountp, path, inode);
	fat_load_inode_times(mountp, inode, lba, offset);
	*result = inode;
	return 0;
}

static int fat_lookup(struct inode *, const struct componentname *,
		      struct inode **);
static int fat_lookup_casefold(struct inode *, const struct componentname *,
			       struct inode **);
static int fat_create(struct inode *, const struct componentname *,
		      const struct inode_creation_request *, struct inode **);
static int fat_mkdir(struct inode *, const struct componentname *,
		     const struct inode_creation_request *, struct inode **);
static int fat_unlink(struct inode *, const struct componentname *);
static int fat_rmdir(struct inode *, const struct componentname *);
static int fat_rename(struct inode *, const struct componentname *,
		      struct inode *, const struct componentname *, unsigned);
static int fat_truncate(struct inode *, off_t);
static int fat_getattr(struct inode *, struct stat *);
static int fat_setattr(struct inode *, const struct stat *, unsigned);
static void fat_reclaim(struct inode *);
static void fat_orphan(struct inode *);
static ssize_t fat_read_file(struct file *, void *, size_t);
static ssize_t fat_write_file(struct file *, const void *, size_t);
static ssize_t fat_pread_file(struct file *, void *, size_t, off_t);
static ssize_t fat_pwrite_file(struct file *, const void *, size_t, off_t);
static int fat_readdir(struct file *, struct dirent *, int *);
static int fat_open_file(struct file *);
static int fat_fsync(struct file *);
static int fat_close_file(struct file *);
static int fat_flush_pending_closes(struct fat_mount_state *);

static const struct inode_ops fat_inode_ops = {
    .lookup = fat_lookup,
    .lookup_casefold = fat_lookup_casefold,
    .create = fat_create,
    .mkdir = fat_mkdir,
    .unlink = fat_unlink,
    .rmdir = fat_rmdir,
    .rename = fat_rename,
    .getattr = fat_getattr,
    .setattr = fat_setattr,
    .truncate = fat_truncate,
    .sync = NULL,
    .reclaim = fat_reclaim,
};
static const struct file_ops fat_regular_ops = {
    .open = fat_open_file,
    .read = fat_read_file,
    .write = fat_write_file,
    .pread = fat_pread_file,
    .pwrite = fat_pwrite_file,
    .fsync = fat_fsync,
    .close = fat_close_file,
};
static const struct file_ops fat_directory_ops = {
    .readdir = fat_readdir,
    .close = fat_close_file,
};

static void
set_inode_ops(struct inode *inode)
{
	inode->i_op = &fat_inode_ops;
	inode->i_fop =
	    inode->i_type == INODE_DIR ? &fat_directory_ops : &fat_regular_ops;
}

int
fat_file_backing_identity(struct inode *inode, struct disk **disk,
			  uint64_t *object)
{
	struct fat_mount_state *state;
	struct fat_inode_info *info;

	if (inode == NULL || disk == NULL || object == NULL)
		return EINVAL;
	if (inode->i_type != INODE_REG || inode->i_mount == NULL ||
	    inode->i_mount->m_type != &fat_filesystem_type ||
	    inode->i_mount->m_disk == NULL)
		return EOPNOTSUPP;
	state = fat_mount_state(inode->i_mount);
	if (state == NULL)
		return EIO;
	mutex_lock(&state->lock);
	info = fat_inode(inode);
	*disk = inode->i_mount->m_disk;
	/* The directory-entry location is identical across separate mounts of
	 * the same FAT volume. Claimed rename is rejected before it can change
	 * these fields.
	 */
	*object = ((uint64_t)info->fi_dirent_lba << 16) |
		  (uint64_t)info->fi_dirent_offset;
	mutex_unlock(&state->lock);
	return 0;
}

static int
fat_stat_path(struct mount *mountp, const char *path, struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_dir_entry entry;
	char canonical[ZEDBSD_PATH_MAX];
	const char *slash;
	size_t prefix_length;
	uint32_t lba, first_cluster;
	uint16_t offset;
	uint8_t attributes;
	int error;
	int fsresult =
	    fat_engine_stat_location(state, path, &entry, &lba, &offset,
				  &first_cluster, &attributes);
	if (fsresult != 0)
		return fsresult;
	slash = strrchr(path, '/');
	prefix_length = slash != NULL ? (size_t)(slash - path + 1) : 0;
	if (prefix_length + strlen(entry.name) >= sizeof(canonical))
		return ENAMETOOLONG;
	memcpy(canonical, path, prefix_length);
	strcpy(canonical + prefix_length, entry.name);
	error = fat_make_inode(mountp, canonical, &entry, lba, offset,
			       first_cluster, attributes, result);
	if (error == 0)
		set_inode_ops(*result);
	return error;
}

static int
fat_stat_path_casefold(struct mount *mountp, const char *path,
		       struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_dir_entry entry;
	char canonical[ZEDBSD_PATH_MAX];
	const char *slash;
	size_t prefix_length;
	uint32_t lba, first_cluster;
	uint16_t offset;
	uint8_t attributes;
	int error;
	int fsresult = fat_engine_stat_location_casefold(
	    state, path, &entry, &lba, &offset, &first_cluster,
	    &attributes);
	if (fsresult != 0)
		return fsresult;
	slash = strrchr(path, '/');
	prefix_length = slash != NULL ? (size_t)(slash - path + 1) : 0;
	if (prefix_length + strlen(entry.name) >= sizeof(canonical))
		return ENAMETOOLONG;
	memcpy(canonical, path, prefix_length);
	strcpy(canonical + prefix_length, entry.name);
	error = fat_make_inode(mountp, canonical, &entry, lba, offset,
			       first_cluster, attributes, result);
	if (error == 0)
		set_inode_ops(*result);
	return error;
}

static int
fat_lookup_unlocked(struct inode *directory, const struct componentname *name,
		    struct inode **result)
{
	char path[ZEDBSD_PATH_MAX];
	const char *parent = fat_path(directory);
	int error;
	if (parent == NULL)
		return EIO;
	if (name->cn_namelen == 1 && name->cn_nameptr[0] == '.') {
		inode_ref(directory);
		*result = directory;
		return 0;
	}
	if (name->cn_namelen == 2 && name->cn_nameptr[0] == '.' &&
	    name->cn_nameptr[1] == '.') {
		char *slash;
		if (parent[0] == '\0') {
			inode_ref(directory);
			*result = directory;
			return 0;
		}
		strcpy(path, parent);
		slash = strrchr(path, '/');
		if (slash == NULL) {
			inode_ref(directory->i_mount->m_root);
			*result = directory->i_mount->m_root;
			return 0;
		}
		*slash = '\0';
		return fat_stat_path(directory->i_mount, path, result);
	}
	error = join_path(parent, name, path);
	return error != 0 ? error
			  : fat_stat_path(directory->i_mount, path, result);
}

static int
fat_lookup(struct inode *directory, const struct componentname *name,
	   struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_lookup_unlocked(directory, name, result);
	mutex_unlock(&state->lock);
	return error;
}

static int
fat_lookup_casefold_unlocked(struct inode *directory,
			     const struct componentname *name,
			     struct inode **result)
{
	char path[ZEDBSD_PATH_MAX];
	const char *parent = fat_path(directory);
	int error;

	if (parent == NULL)
		return EIO;
	error = join_path(parent, name, path);
	return error != 0
		   ? error
		   : fat_stat_path_casefold(directory->i_mount, path, result);
}

static int
fat_getattr(struct inode *inode, struct stat *status)
{
	memset(status, 0, sizeof(*status));
	status->st_dev = inode->i_mount->m_disk->d_dev;
	status->st_ino = inode->i_ino;
	status->st_mode = inode->i_mode;
	status->st_nlink = inode->i_linkcount;
	status->st_uid = inode->i_uid;
	status->st_gid = inode->i_gid;
	status->st_size = inode->i_size;
	status->st_atime = inode->i_atime.tv_sec;
	status->st_mtime = inode->i_mtime.tv_sec;
	status->st_ctime = inode->i_ctime.tv_sec;
	status->st_blksize = 512;
	status->st_blocks =
	    inode->i_size > 0
		? (blkcnt_t)(((uint64_t)inode->i_size + 511U) / 512U)
		: 0;
	return 0;
}

static FAT_MUTATION void
fat_put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static FAT_MUTATION int
fat_setattr_unlocked(struct inode *inode, const struct stat *status,
		     unsigned mask)
{
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	struct fat_inode_info *info = fat_inode(inode);
	uint8_t *sector, saved[32];
	uint16_t atime_date = 0, atime_time = 0;
	uint16_t mtime_date = 0, mtime_time = 0;
	mode_t permissions;
	int result;
	int error;

	if (state == NULL || info == NULL || (inode->i_flags & INODE_ROOT) != 0)
		return EOPNOTSUPP;
	if ((mask & INODE_ATTR_SIZE) != 0)
		return EOPNOTSUPP;
	if ((mask & INODE_ATTR_UID) != 0 && status->st_uid != inode->i_uid)
		return EOPNOTSUPP;
	if ((mask & INODE_ATTR_GID) != 0 && status->st_gid != inode->i_gid)
		return EOPNOTSUPP;
	if (mask & INODE_ATTR_MODE) {
		permissions = status->st_mode & 07777U;
		if (permissions != 0755U && permissions != 0555U)
			return EOPNOTSUPP;
	}
	if (mask & INODE_ATTR_ATIME) {
		if (status->st_atim.tv_nsec < 0 ||
		    status->st_atim.tv_nsec >= 1000000000L)
			return EINVAL;
		error = fat_encode_time(status->st_atim.tv_sec, &atime_date,
					&atime_time);
		if (error != 0)
			return error;
	}
	if (mask & INODE_ATTR_MTIME) {
		if (status->st_mtim.tv_nsec < 0 ||
		    status->st_mtim.tv_nsec >= 1000000000L)
			return EINVAL;
		error = fat_encode_time(status->st_mtim.tv_sec, &mtime_date,
					&mtime_time);
		if (error != 0)
			return error;
	}
	/* The generic inode layer still applies attributes to an unlinked open
	 * inode, but its former FAT slot may already belong to another file. */
	if ((inode->i_flags & INODE_DEAD) != 0)
		return 0;
	result = fat_engine_write_sector_result(state,
					     info->fi_dirent_lba, &sector);
	if (result != 0)
		return result;
	memcpy(saved, sector + info->fi_dirent_offset, sizeof(saved));
	sector += info->fi_dirent_offset;
	if (mask & INODE_ATTR_MODE) {
		if ((status->st_mode & 0222U) == 0)
			sector[11] |= FAT_ATTRIBUTE_READ_ONLY;
		else
			sector[11] &= (uint8_t)~FAT_ATTRIBUTE_READ_ONLY;
	}
	if (mask & INODE_ATTR_ATIME)
		fat_put16(sector + 18, atime_date);
	if (mask & INODE_ATTR_MTIME) {
		fat_put16(sector + 22, mtime_time);
		fat_put16(sector + 24, mtime_date);
	}
	result = fat_engine_mark_sector_dirty(state);
	if (result == 0)
		result = fat_engine_flush(state);
	if (result != 0) {
		uint8_t *rollback;
		if (fat_engine_write_sector_result(state,
						info->fi_dirent_lba,
						&rollback) == 0) {
			memcpy(rollback + info->fi_dirent_offset, saved,
			       sizeof(saved));
			(void)fat_engine_mark_sector_dirty(state);
		}
		return result;
	}
	info->fi_attributes = sector[11];
	(void)atime_time;
	return 0;
}

static FAT_MUTATION int
fat_setattr(struct inode *inode, const struct stat *status, unsigned mask)
{
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_flush_pending_closes(state);
	if (error == 0)
		error = fat_setattr_unlocked(inode, status, mask);
	mutex_unlock(&state->lock);
	return error;
}

static struct fat_file_state *
fat_file_get(struct file *file)
{
	struct fat_file_state *slot = NULL;
	struct fat_mount_state *mount_state;
	unsigned i;
	unsigned long irq;
	if (file->f_data != NULL)
		return file->f_data;
	irq = spin_lock_irqsave(&fat_pool_lock);
	for (i = 0; i < FAT_FILE_MAX; i++) {
		if (!fat_files[i].used) {
			memset(&fat_files[i], 0, sizeof(fat_files[i]));
			fat_files[i].used = 1;
			fat_files[i].owner = file->f_inode;
			slot = &fat_files[i];
			break;
		}
	}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	if (slot == NULL)
		return NULL;
	mount_state = fat_mount_state(file->f_inode->i_mount);
	if (fat_raw_open(mount_state, fat_path(file->f_inode), slot) != 0) {
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(slot, 0, sizeof(*slot));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
		return NULL;
	}
	/*
	 * Another open file may have extended this inode without yet flushing
	 * its FAT directory entry.  The inode is the coherent in-memory
	 * size/cluster authority for every open description.
	 */
	slot->size = (uint64_t)file->f_inode->i_size;
	slot->first_cluster = fat_inode(file->f_inode)->fi_first_cluster;
	file->f_data = slot;
	return slot;
}

static int
fat_open_file(struct file *file)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_file_get(file) != NULL ? 0 : EIO;
	mutex_unlock(&state->lock);
	return error;
}

static ssize_t
fat_pread_file_unlocked(struct file *file, void *buffer, size_t length,
			off_t offset)
{
	struct fat_file_state *state = fat_file_get(file);
	uint32_t count;
	int result;
	if (state == NULL)
		return -EIO;
	if (offset >= file->f_inode->i_size)
		return 0;
	if (length > (size_t)(file->f_inode->i_size - offset))
		length = (size_t)(file->f_inode->i_size - offset);
	count = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
	result = fat_raw_read(state, (uint64_t)offset,
					 buffer, count, NULL, NULL);
	if (result != 0)
		return -result;
	return count;
}

static void
fat_sync_inode_state(struct inode *inode, const struct fat_file_state *file)
{
	struct fat_inode_info *info;
	const struct fat_file_state *state;
	unsigned i;
	unsigned long irq;

	if (inode == NULL || file == NULL)
		return;
	info = fat_inode(inode);
	state = file;
	info->fi_first_cluster = state->first_cluster;
	inode->i_size = (off_t)file->size;
	irq = spin_lock_irqsave(&fat_pool_lock);
	for (i = 0; i < FAT_FILE_MAX; i++) {
		struct fat_file_state *open_state;
		if (!fat_files[i].used || fat_files[i].owner != inode)
			continue;
		fat_files[i].size = file->size;
		open_state = &fat_files[i];
		open_state->first_cluster = state->first_cluster;
	}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
}

/* Called with the mount mutex held.  A failed close leaves a self-contained
 * directory-entry retry record; it deliberately owns no inode reference so
 * generic unmount busy checks can reach filesystem sync. */
static int
fat_flush_pending_closes(struct fat_mount_state *mount_state)
{
	unsigned i;

	for (i = 0; i < FAT_FILE_MAX; i++) {
		struct fat_file_state *state = &fat_files[i];
		struct inode *owner;
		unsigned long irq = spin_lock_irqsave(&fat_pool_lock);
		int error;

		if (!state->used || state->mount != mount_state ||
		    !state->pending_close) {
			spin_unlock_irqrestore(&fat_pool_lock, irq);
			continue;
		}
		owner = state->owner;
		spin_unlock_irqrestore(&fat_pool_lock, irq);
		error = fat_raw_flush_file(state);
		if (error != 0)
			return error;
		if (owner != NULL)
			fat_sync_inode_state(owner, state);
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(state, 0, sizeof(*state));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
	}
	return 0;
}

static ssize_t
fat_pread_file(struct file *file, void *buffer, size_t length, off_t offset)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	ssize_t count;
	mutex_lock(&state->lock);
	count = fat_pread_file_unlocked(file, buffer, length, offset);
	mutex_unlock(&state->lock);
	return count;
}

static ssize_t
fat_read_file(struct file *file, void *buffer, size_t length)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	ssize_t count;
	mutex_lock(&state->lock);
	count = fat_pread_file_unlocked(file, buffer, length, file->f_offset);
	if (count > 0)
		file->f_offset += count;
	mutex_unlock(&state->lock);
	return count;
}

static ssize_t
fat_pwrite_file_unlocked(struct file *file, const void *buffer, size_t length,
			 off_t offset)
{
	struct fat_file_state *state = fat_file_get(file);
	uint32_t count = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
	int result;
	if (state == NULL)
		return -EIO;
	if (offset < 0)
		return -EINVAL;
	if ((uint64_t)offset > UINT32_MAX ||
	    (uint64_t)count > UINT32_MAX - (uint64_t)offset)
		return -EFBIG;
	result = fat_raw_write(state, (uint64_t)offset,
					  buffer, count);
	if (result != 0)
		return -result;
	fat_sync_inode_state(file->f_inode, state);
	return count;
}

static ssize_t
fat_pwrite_file(struct file *file, const void *buffer, size_t length,
		off_t offset)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	ssize_t count;
	mutex_lock(&state->lock);
	count = fat_pwrite_file_unlocked(file, buffer, length, offset);
	mutex_unlock(&state->lock);
	return count;
}

static ssize_t
fat_write_file(struct file *file, const void *buffer, size_t length)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	off_t offset;
	ssize_t count;
	mutex_lock(&state->lock);
	offset = (file_status_flags_get(file) & O_APPEND) != 0
		     ? file->f_inode->i_size
		     : file->f_offset;
	count = fat_pwrite_file_unlocked(file, buffer, length, offset);
	if (count > 0)
		file->f_offset = offset + count;
	mutex_unlock(&state->lock);
	return count;
}

static int
fat_readdir_unlocked(struct file *file, struct dirent *entry, int *eof)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	struct fat_dir_entry decoded;
	char child_path[ZEDBSD_PATH_MAX];
	struct componentname component;
	struct inode *child;
	int result =
	    fat_raw_readdir(state, fat_path(file->f_inode),
				  (unsigned)file->f_offset, &decoded);
	if (result == ENOENT) {
		*eof = 1;
		return 0;
	}
	if (result != 0)
		return result;
	component.cn_nameptr = decoded.name;
	component.cn_namelen = strlen(decoded.name);
	component.cn_flags = COMPONENT_LAST;
	if (join_path(fat_path(file->f_inode), &component, child_path) != 0)
		return ENAMETOOLONG;
	if (fat_stat_path(file->f_inode->i_mount, child_path, &child) != 0)
		return EIO;
	memset(entry, 0, sizeof(*entry));
	entry->d_ino = child->i_ino;
	entry->d_type = child->i_type;
	strncpy(entry->d_name, decoded.name, NAME_MAX);
	entry->d_name[NAME_MAX] = '\0';
	inode_release(child);
	file->f_offset++;
	*eof = 0;
	return 0;
}

static int
fat_readdir(struct file *file, struct dirent *entry, int *eof)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_readdir_unlocked(file, entry, eof);
	mutex_unlock(&state->lock);
	return error;
}

static int
fat_close_file(struct file *file)
{
	struct fat_file_state *state = file->f_data;
	struct fat_mount_state *mount_state =
	    file->f_inode != NULL ? fat_mount_state(file->f_inode->i_mount)
				  : NULL;
	unsigned long irq;
	int error = 0;
	if (mount_state != NULL)
		mutex_lock(&mount_state->lock);
	if (state != NULL) {
		if ((file_status_flags_get(file) & O_ACCMODE) != O_RDONLY &&
		    file->f_inode != NULL &&
		    (file->f_inode->i_flags & INODE_DEAD) == 0) {
			error = fat_raw_flush_file(state);
			if (error == 0)
				fat_sync_inode_state(file->f_inode,
						     state);
		} else if (file->f_inode != NULL &&
			   (file->f_inode->i_flags & INODE_DEAD) != 0)
			error = fat_engine_flush(
			    fat_mount_state(file->f_inode->i_mount));
		irq = spin_lock_irqsave(&fat_pool_lock);
		if (error != 0 && state->directory_dirty &&
		    file->f_inode != NULL &&
		    (file->f_inode->i_flags & INODE_DEAD) == 0) {
			/* The struct file is closing, but mount sync still needs the
			 * authoritative directory-entry retry record. */
			state->pending_close = 1;
			state->owner = NULL;
		} else {
			memset(state, 0, sizeof(*state));
		}
		spin_unlock_irqrestore(&fat_pool_lock, irq);
		file->f_data = NULL;
	}
	if (mount_state != NULL)
		mutex_unlock(&mount_state->lock);
	return error;
}

static int
fat_fsync(struct file *file)
{
	struct fat_mount_state *mount_state =
	    fat_mount_state(file->f_inode->i_mount);
	struct fat_file_state *state;
	int error;
	mutex_lock(&mount_state->lock);
	state = fat_file_get(file);
	if (state == NULL)
		error = EIO;
	else if (file->f_inode != NULL &&
		 (file->f_inode->i_flags & INODE_DEAD) != 0)
		error = fat_engine_flush(mount_state);
	else
		error = fat_raw_flush_file(state);
	if (error == 0)
		error = disk_sync(file->f_inode->i_mount->m_disk);
	mutex_unlock(&mount_state->lock);
	return error;
}

static int
fat_lookup_casefold(struct inode *directory, const struct componentname *name,
		    struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	int error;
	mutex_lock(&state->lock);
	error = fat_lookup_casefold_unlocked(directory, name, result);
	mutex_unlock(&state->lock);
	return error;
}

static int
fat_truncate(struct inode *inode, off_t size)
{
	struct fat_file_state file = {0};
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	int result;
	if (size < 0)
		return EINVAL;
	if ((uint64_t)size > UINT32_MAX)
		return EFBIG;
	mutex_lock(&state->lock);
	result = fat_flush_pending_closes(state);
	if (result != 0) {
		mutex_unlock(&state->lock);
		return result;
	}
	if ((inode->i_flags & INODE_DEAD) != 0) {
		file.mount = state;
		file.owner = inode;
		file.size = (uint64_t)inode->i_size;
		file.first_cluster = fat_inode(inode)->fi_first_cluster;
		result = 0;
	} else {
		result = fat_raw_open(state, fat_path(inode), &file);
	}
	if (result == 0) {
		/* An open writer may own a newer size/chain than the directory
		 * entry.  Preserve its directory location but truncate the coherent
		 * inode state, then propagate the result to every open handle. */
		file.size = (uint64_t)inode->i_size;
		file.first_cluster = fat_inode(inode)->fi_first_cluster;
		result = fat_raw_truncate(&file, (uint64_t)size);
	}
	if (result == 0)
		result = fat_raw_flush_file(&file);
	if (result == 0)
		fat_sync_inode_state(inode, &file);
	mutex_unlock(&state->lock);
	return result;
}

static int
fat_create_unlocked(struct inode *directory, const struct componentname *name,
		    const struct inode_creation_request *request,
		    struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct fat_file_state file = {0};
	struct inode *created = NULL;
	char path[ZEDBSD_PATH_MAX];
	int error, rollback;

	*result = NULL;
	error = fat_flush_pending_closes(state);
	if (error != 0)
		return error;
	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;
	error = fat_creation_collision(state, path);
	if (error != 0)
		return error;
	error = fat_creation_representable(state, path, request, INODE_REG);
	if (error != 0)
		return error;
	error = fat_raw_create(state, path, &file);
	if (error != 0)
		return error;
	error = fat_raw_flush_file(&file);
	if (error != 0)
		goto rollback_raw;
	namecache_remove(directory, name);
	error = fat_stat_path(directory->i_mount, path, &created);
	if (error != 0)
		goto rollback_raw;
	fat_sync_inode_state(created, &file);
	error = inode_creation_prepare(directory, created, request);
	if (error == 0)
		error = fat_created_inode_matches(state, fat_path(created), created);
	if (error != 0)
		goto rollback_inode;
	*result = created;
	return 0;

rollback_inode:
	rollback = fat_raw_unlink(state, path);
	if (rollback == 0) {
		fat_orphan(created);
		namecache_remove(directory, name);
	} else {
		mode_t mode;
		uid_t uid;
		gid_t gid;

		if (fat_creation_representation(state, fat_path(created), &mode,
		    &uid, &gid) == 0) {
			created->i_mode = S_IFREG | mode;
			created->i_uid = uid;
			created->i_gid = gid;
			created->i_rdev = 0;
		}
		state->read_only = 1;
		error = rollback;
	}
	*result = created;
	return error;

rollback_raw:
	rollback = fat_raw_unlink(state, path);
	if (rollback != 0) {
		state->read_only = 1;
		return rollback;
	}
	namecache_remove(directory, name);
	return error;
}

static int
fat_create(struct inode *directory, const struct componentname *name,
	   const struct inode_creation_request *request, struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *created = NULL;
	int error;

	if (result == NULL)
		return EINVAL;
	*result = NULL;
	mutex_lock(&state->lock);
	error = fat_create_unlocked(directory, name, request, &created);
	mutex_unlock(&state->lock);
	if (error != 0) {
		if (created != NULL)
			inode_release(created);
		return error;
	}
	*result = created;
	return error;
}

static FAT_MUTATION void
fat_orphan(struct inode *inode)
{
	struct fat_inode_info *info;
	if (inode == NULL)
		return;
	info = fat_inode(inode);
	info->fi_flags |= FAT_INODE_ORPHANED;
	inode->i_flags |= INODE_DEAD;
	namecache_purge_inode(inode);
}

static FAT_MUTATION void
fat_release_orphan(struct inode *inode)
{
	if (inode == NULL)
		return;
	/* inode_release() owns the transition from the final external reference
	 * to cache-only DEAD state and performs reclaim/free synchronously. */
	inode_release(inode);
}

static FAT_MUTATION int
fat_mkdir_unlocked(struct inode *directory, const struct componentname *name,
		   const struct inode_creation_request *request,
		   struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *created = NULL;
	char path[ZEDBSD_PATH_MAX];
	uint32_t cluster = 0;
	int error, rollback;

	*result = NULL;
	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;
	error = fat_creation_collision(state, path);
	if (error != 0)
		return error;
	error = fat_creation_representable(state, path, request, INODE_DIR);
	if (error != 0)
		return error;
	error = fat_raw_mkdir(state, path, &cluster);
	if (error != 0)
		return error;
	namecache_remove(directory, name);
	error = fat_stat_path(directory->i_mount, path, &created);
	if (error != 0)
		goto rollback_raw;
	error = inode_creation_prepare(directory, created, request);
	if (error == 0)
		error = fat_created_inode_matches(state, fat_path(created), created);
	if (error != 0)
		goto rollback_inode;
	*result = created;
	return 0;

rollback_inode:
	rollback = fat_raw_rmdir(state, path);
	if (rollback != 0) {
		mode_t mode;
		uid_t uid;
		gid_t gid;

		if (fat_creation_representation(state, fat_path(created), &mode,
		    &uid, &gid) == 0) {
			created->i_mode = S_IFDIR | mode;
			created->i_uid = uid;
			created->i_gid = gid;
			created->i_rdev = 0;
		}
		state->read_only = 1;
		error = rollback;
		*result = created;
		return error;
	}
	/* The directory entry is gone, so no live inode may retain the cluster
	 * once reclamation starts.  A recoverable free failure is represented by
	 * pending_orphans instead of the now-dead inode. */
	fat_inode(created)->fi_first_cluster = 0;
	fat_orphan(created);
	namecache_remove(directory, name);
	rollback = fat_raw_free_chain(state, cluster);
	if (rollback != 0) {
		int deferred = state->read_only ? rollback :
		    fat_defer_orphan(state, cluster);

		if (deferred != 0)
			state->read_only = 1;
		*result = created;
		return deferred != 0 ? deferred : rollback;
	}
	*result = created;
	return error;

rollback_raw:
	rollback = fat_raw_rmdir(state, path);
	if (rollback != 0) {
		state->read_only = 1;
		return rollback;
	}
	rollback = fat_raw_free_chain(state, cluster);
	if (rollback != 0) {
		if (state->read_only || fat_defer_orphan(state, cluster) != 0)
			state->read_only = 1;
		return rollback;
	}
	namecache_remove(directory, name);
	return error;
}

static FAT_MUTATION int
fat_mkdir(struct inode *directory, const struct componentname *name,
	  const struct inode_creation_request *request, struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *created = NULL;
	int error;

	if (result == NULL)
		return EINVAL;
	*result = NULL;
	mutex_lock(&state->lock);
	error = fat_mkdir_unlocked(directory, name, request, &created);
	mutex_unlock(&state->lock);
	if (error != 0) {
		if (created != NULL)
			inode_release(created);
		return error;
	}
	*result = created;
	return error;
}

static FAT_MUTATION int
fat_remove_inode_unlocked(struct inode *directory,
			  const struct componentname *name,
			  int remove_directory, struct inode **orphaned)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *victim = NULL;
	char path[ZEDBSD_PATH_MAX];
	int error;

	*orphaned = NULL;
	error = fat_flush_pending_closes(state);
	if (error == 0)
		error = fat_drain_pending_orphans(state);
	if (error != 0)
		return error;
	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;
	error = fat_lookup_unlocked(directory, name, &victim);
	if (error != 0)
		return error;
	error = remove_directory ? fat_raw_rmdir(state, path)
				 : fat_raw_unlink(state, path);
	if (error == 0) {
		namecache_remove(directory, name);
		fat_orphan(victim);
		*orphaned = victim;
	}
	if (error != 0)
		inode_release(victim);
	return error;
}

static FAT_MUTATION int
fat_unlink(struct inode *directory, const struct componentname *name)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *orphaned = NULL;
	int error;
	mutex_lock(&state->lock);
	error = fat_remove_inode_unlocked(directory, name, 0, &orphaned);
	mutex_unlock(&state->lock);
	if (orphaned != NULL)
		fat_release_orphan(orphaned);
	return error;
}

static FAT_MUTATION int
fat_rmdir(struct inode *directory, const struct componentname *name)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *orphaned = NULL;
	int error;
	mutex_lock(&state->lock);
	error = fat_remove_inode_unlocked(directory, name, 1, &orphaned);
	mutex_unlock(&state->lock);
	if (orphaned != NULL)
		fat_release_orphan(orphaned);
	return error;
}

static FAT_MUTATION int
fat_path_descendant(const char *parent, const char *path)
{
	size_t length = strlen(parent);
	return length != 0 && !memcmp(parent, path, length) &&
	       path[length] == '/';
}

static FAT_MUTATION void
fat_repath_descendants(struct mount *mountp, const char *old_path,
		       const char *new_path)
{
	size_t old_length = strlen(old_path), new_length = strlen(new_path);
	unsigned i;
	unsigned long irq = spin_lock_irqsave(&fat_pool_lock);

	for (i = 0; i < FAT_INODE_MAX; i++) {
		char replacement[ZEDBSD_PATH_MAX];
		size_t suffix;
		if (!fat_inodes[i].used ||
		    fat_inodes[i].info.fi_inode.i_mount != mountp ||
		    !fat_path_descendant(old_path, fat_inodes[i].path))
			continue;
		suffix = strlen(fat_inodes[i].path + old_length);
		memcpy(replacement, new_path, new_length);
		memcpy(replacement + new_length,
		       fat_inodes[i].path + old_length, suffix + 1U);
		strcpy(fat_inodes[i].path, replacement);
	}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
}

static FAT_MUTATION int
fat_repath_descendants_possible(struct mount *mountp, const char *old_path,
	const char *new_path)
{
	size_t old_length = strlen(old_path), new_length = strlen(new_path);
	unsigned i;
	unsigned long irq = spin_lock_irqsave(&fat_pool_lock);
	int result = 0;

	for (i = 0; i < FAT_INODE_MAX; i++) {
		size_t suffix;

		if (!fat_inodes[i].used ||
		    fat_inodes[i].info.fi_inode.i_mount != mountp ||
		    !fat_path_descendant(old_path, fat_inodes[i].path))
			continue;
		suffix = strlen(fat_inodes[i].path + old_length);
		if (new_length + suffix >= ZEDBSD_PATH_MAX) {
			result = ENAMETOOLONG;
			break;
		}
	}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	return result;
}

static FAT_MUTATION int
fat_rename_unlocked(struct inode *old_directory,
		    const struct componentname *old_name,
		    struct inode *new_directory,
		    const struct componentname *new_name, unsigned flags,
		    struct inode **orphaned)
{
	struct fat_mount_state *state = fat_mount_state(old_directory->i_mount);
	struct inode *source = NULL, *target = NULL;
	struct fat_inode_info *info;
	struct fat_rename_result renamed = {0};
	struct componentname canonical_name;
	char old_path[ZEDBSD_PATH_MAX], new_path[ZEDBSD_PATH_MAX];
	char canonical_basename[ZEDBSD_PATH_MAX];
	char old_canonical[ZEDBSD_PATH_MAX], new_canonical[ZEDBSD_PATH_MAX];
	unsigned i;
	unsigned long irq;
	int error, target_error;

	*orphaned = NULL;
	if (flags != 0)
		return EINVAL;
	error = fat_flush_pending_closes(state);
	if (error != 0)
		return error;
	error = join_path(fat_path(old_directory), old_name, old_path);
	if (error == 0)
		error = join_path(fat_path(new_directory), new_name, new_path);
	if (error != 0)
		return error;
	error = fat_lookup_unlocked(old_directory, old_name, &source);
	if (error != 0)
		return error;
	if (fat_path(source) == NULL) {
		inode_release(source);
		return EIO;
	}
	strcpy(old_canonical, fat_path(source));
	target_error = fat_lookup_unlocked(new_directory, new_name, &target);
	if (target_error == 0 && target == source) {
		inode_release(target);
		inode_release(source);
		return 0;
	}
	if (target_error != 0 && target_error != ENOENT) {
		inode_release(source);
		return target_error;
	}
	if (target != NULL) {
		error = fat_drain_pending_orphans(state);
		if (error != 0) {
			inode_release(target);
			inode_release(source);
			return error;
		}
	}
	if (target != NULL) {
		if (fat_path(target) == NULL) {
			inode_release(target);
			inode_release(source);
			return EIO;
		}
		strcpy(new_canonical, fat_path(target));
	} else {
		error = fat_raw_canonical_basename(state, new_path,
			canonical_basename);
		if (error != 0) {
			inode_release(source);
			return error;
		}
		canonical_name.cn_nameptr = canonical_basename;
		canonical_name.cn_namelen = strlen(canonical_basename);
		canonical_name.cn_flags = COMPONENT_LAST;
		error = join_path(fat_path(new_directory), &canonical_name,
			new_canonical);
		if (error != 0) {
			inode_release(source);
			return error;
		}
	}
	if (source->i_type == INODE_DIR) {
		error = fat_repath_descendants_possible(old_directory->i_mount,
			old_canonical, new_canonical);
		if (error != 0) {
			if (target != NULL)
				inode_release(target);
			inode_release(source);
			return error;
		}
	}
	info = fat_inode(source);
	if (source->i_size < 0 || (uint64_t)source->i_size > UINT32_MAX) {
		if (target != NULL)
			inode_release(target);
		inode_release(source);
		return EFBIG;
	}
	error = fat_raw_rename(state, old_path, new_path,
		info->fi_first_cluster, (uint32_t)source->i_size, &renamed);
	if (error != 0) {
		if (target != NULL)
			inode_release(target);
		inode_release(source);
		return error;
	}
	if (target != NULL)
		fat_orphan(target);
	info->fi_dirent_lba = renamed.lba;
	info->fi_dirent_offset = renamed.offset;
	info->fi_attributes = renamed.attributes;
	source->i_ino = fat_ino(renamed.lba, renamed.offset);
	irq = spin_lock_irqsave(&fat_pool_lock);
	for (i = 0; i < FAT_FILE_MAX; i++) {
		struct fat_file_state *open_state;

		if (!fat_files[i].used || fat_files[i].owner != source)
			continue;
		open_state = &fat_files[i];
		open_state->directory_lba = renamed.lba;
		open_state->directory_offset = renamed.offset;
		open_state->directory_dirty = 0;
	}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	if (source->i_type == INODE_DIR)
		fat_repath_descendants(old_directory->i_mount, old_canonical,
			new_canonical);
	irq = spin_lock_irqsave(&fat_pool_lock);
	strcpy(fat_slot(source)->path, new_canonical);
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	namecache_remove(old_directory, old_name);
	namecache_remove(new_directory, new_name);
	if (target != NULL)
		*orphaned = target;
	inode_release(source);
	return 0;
}

static FAT_MUTATION int
fat_rename(struct inode *old_directory, const struct componentname *old_name,
	   struct inode *new_directory, const struct componentname *new_name,
	   unsigned flags)
{
	struct fat_mount_state *state = fat_mount_state(old_directory->i_mount);
	struct inode *orphaned = NULL;
	int error;
	mutex_lock(&state->lock);
	error = fat_rename_unlocked(old_directory, old_name, new_directory,
				    new_name, flags, &orphaned);
	mutex_unlock(&state->lock);
	if (orphaned != NULL)
		fat_release_orphan(orphaned);
	return error;
}

static void
fat_reclaim_unlocked(struct inode *inode)
{
	struct fat_inode_info *info = fat_inode(inode);
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	if ((info->fi_flags & FAT_INODE_ORPHANED) != 0 &&
	    info->fi_first_cluster != 0 && state != NULL) {
		int result = fat_engine_discard_chain_result(state,
			info->fi_first_cluster);

		if (result == 0 ||
		    fat_defer_orphan(state, info->fi_first_cluster) == 0)
			info->fi_first_cluster = 0;
	}
}

static void
fat_reclaim(struct inode *inode)
{
	struct fat_inode_info *info = fat_inode(inode);
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	if (state == NULL || info == NULL ||
	    (info->fi_flags & FAT_INODE_ORPHANED) == 0 ||
	    info->fi_first_cluster == 0)
		return;
	mutex_lock(&state->lock);
	fat_reclaim_unlocked(inode);
	mutex_unlock(&state->lock);
}

static int
fat_probe_volume(struct disk *disk, int direct_io, enum bootfat_type *type)
{
	struct fat_mount_state candidate = {
		.disk = disk,
		.read_only = 1,
		.direct_io = direct_io != 0,
	};
	int result;

	if (disk == NULL || type == NULL)
		return EOPNOTSUPP;
	result = parse_bpb(&candidate);
	if (result != 0)
		return result;
	*type = (enum bootfat_type)candidate.type;
	return 0;
}

int
fat_probe_type(struct disk *disk, enum bootfat_type *type)
{
	if (disk == NULL || type == NULL || disk->d_block_size != 512)
		return EOPNOTSUPP;
	return fat_probe_volume(disk, 0, type);
}

static char
fat_hex_digit(unsigned value)
{
	return (char)(value < 10U ? '0' + value : 'A' + value - 10U);
}

static void
fat_hex32(char output[9], uint32_t value)
{
	unsigned i;

	for (i = 0; i < 8U; i++)
		output[i] = fat_hex_digit((value >> (28U - i * 4U)) & 15U);
	output[8] = '\0';
}

static void
fat_copy_label(char *output, size_t capacity, const uint8_t *input,
	size_t length)
{
	size_t end = length;
	size_t i;

	while (end != 0U && (input[end - 1U] == ' ' || input[end - 1U] == 0U))
		end--;
	if (end >= capacity)
		end = capacity - 1U;
	for (i = 0; i < end; i++)
		output[i] = input[i] >= 0x20U && input[i] <= 0x7eU ?
		    (char)input[i] : '_';
	output[end] = '\0';
}

static int
fat_identify(struct disk *disk, struct block_identity *identity)
{
	enum bootfat_type type;
	uint8_t boot[512];
	uint32_t declared_sectors;
	uint32_t fat_sectors;
	uint32_t serial;
	uint32_t sector_scale;
	uint16_t sector_bytes;
	unsigned label_offset;
	unsigned serial_offset;
	int error;

	if (disk == NULL || identity == NULL)
		return EINVAL;
	if (disk->d_block_size != 512U || disk->d_block_count == 0U)
		return EOPNOTSUPP;
	if (disk_read_direct(disk, 0, 1, boot) != 0)
		return EIO;
	if (boot[510] != 0x55U || boot[511] != 0xaaU)
		return EOPNOTSUPP;
	sector_bytes = fat_engine_get16(boot + 11U);
	sector_scale = sector_bytes == 512U ? 1U :
	    sector_bytes == 1024U ? 2U : 0U;
	declared_sectors = fat_engine_get16(boot + 19U);
	if (declared_sectors == 0U)
		declared_sectors = fat_engine_get32(boot + 32U);
	fat_sectors = fat_engine_get16(boot + 22U);
	if (fat_sectors == 0U)
		fat_sectors = fat_engine_get32(boot + 36U);
	/* An MBR has the same trailing signature.  Require a credible FAT BPB
	 * before treating decoder failures as filesystem corruption. */
	if ((sector_scale != 1U && sector_scale != 2U) || boot[13U] == 0U ||
	    fat_engine_get16(boot + 14U) == 0U || boot[16U] == 0U ||
	    declared_sectors == 0U || fat_sectors == 0U)
		return EOPNOTSUPP;
	if (declared_sectors > UINT32_MAX / sector_scale ||
	    (uint64_t)declared_sectors * sector_scale > disk->d_block_count)
		return EINVAL;
	error = fat_probe_volume(disk, 1, &type);
	if (error != 0)
		return error;

	memset(identity, 0, sizeof(*identity));
	strcpy(identity->type, "vfat");
	identity->flags = ZEDBSD_BLKID_TYPE;
	serial_offset = type == ZEDBSD_FAT32 ? 67U : 39U;
	label_offset = type == ZEDBSD_FAT32 ? 71U : 43U;
	if (boot[serial_offset - 1U] != 0x29U)
		return 0;
	serial = fat_engine_get32(boot + serial_offset);
	fat_hex32(identity->uuid, serial);
	memmove(identity->uuid + 5, identity->uuid + 4, 4U);
	identity->uuid[4] = '-';
	identity->uuid[9] = '\0';
	identity->flags |= ZEDBSD_BLKID_UUID;
	fat_copy_label(identity->label, sizeof(identity->label),
	    boot + label_offset, 11U);
	if (identity->label[0] != '\0' && strcmp(identity->label, "NO NAME") != 0)
		identity->flags |= ZEDBSD_BLKID_LABEL;
	return 0;
}

static int
fat_probe(struct disk *disk)
{
	enum bootfat_type type;

	return fat_probe_type(disk, &type);
}

static int
fat_mount_impl(struct mount *mountp)
{
	struct fat_mount_state *state = NULL;
	struct inode *root;
	struct fat_inode_info *info;
	enum bootfat_type type;
	unsigned i;
	unsigned long irq;
	int result;
	irq = spin_lock_irqsave(&fat_pool_lock);
	for (i = 0; i < FAT_MOUNT_MAX; i++)
		if (!fat_mounts[i].used) {
			state = &fat_mounts[i];
			memset(state, 0, sizeof(*state));
			state->used = 1;
			break;
		}
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	if (state == NULL)
		return ENOSPC;
	memset(&fat_metadata_tables[i], 0, sizeof(fat_metadata_tables[i]));
	state->metadata = &fat_metadata_tables[i];
	(void)mutex_init(&state->lock, LOCK_RANK_INODE, "FAT mount");
	state->disk = mountp->m_disk;
	state->read_only =
	    (mountp->m_flags & MOUNT_READ_ONLY) != 0 ||
	    (mountp->m_disk->d_flags & DISK_READ_ONLY) != 0;
	result = fat_probe_volume(mountp->m_disk, 0, &type);
	if (result == 0) {
		switch (type) {
		case ZEDBSD_FAT12:
			result = fat12_mount(state);
			break;
		case ZEDBSD_FAT16:
			result = fat16_mount(state);
			break;
		case ZEDBSD_FAT32:
			result = fat32_mount(state);
			break;
		default:
			result = EOPNOTSUPP;
			break;
		}
	}
	if (result != 0) {
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(&fat_metadata_tables[i], 0,
		       sizeof(fat_metadata_tables[i]));
		memset(state, 0, sizeof(*state));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
		return result;
	}
	fat_metadata_load(state);
	mountp->m_data = state;
	root = inode_alloc(mountp);
	if (root == NULL) {
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(&fat_metadata_tables[i], 0,
		       sizeof(fat_metadata_tables[i]));
		memset(state, 0, sizeof(*state));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
		mountp->m_data = NULL;
		return ENOSPC;
	}
	info = fat_inode(root);
	root->i_type = INODE_DIR;
	root->i_ino = 1;
	root->i_mode = S_IFDIR | 0755U;
	root->i_linkcount = 1;
	root->i_flags = INODE_ROOT;
	root->i_data = info;
	set_inode_ops(root);
	mountp->m_root = root;
	return 0;
}

static int
fat_sync_mount(struct mount *mountp)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	unsigned i;
	int error;
	if (state == NULL)
		return EINVAL;
	mutex_lock(&state->lock);
	error = fat_drain_pending_orphans(state);
	if (error == 0)
		error = fat_flush_pending_closes(state);
	/* A successful filesystem sync includes directory-entry size/cluster
	 * state held by live open descriptions, not only the sector currently
	 * resident in the mount cache. */
	for (i = 0; i < FAT_FILE_MAX && error == 0; i++) {
		struct fat_file_state *file = &fat_files[i];
		struct inode *owner;
		unsigned long irq = spin_lock_irqsave(&fat_pool_lock);

		if (!file->used || file->mount != state ||
		    !file->directory_dirty) {
			spin_unlock_irqrestore(&fat_pool_lock, irq);
			continue;
		}
		owner = file->owner;
		spin_unlock_irqrestore(&fat_pool_lock, irq);
		if (owner == NULL || (owner->i_flags & INODE_DEAD) != 0)
			continue;
		error = fat_raw_flush_file(file);
		if (error == 0)
			fat_sync_inode_state(owner, file);
	}
	if (error == 0)
		error = fat_engine_flush(state);
	if (error == 0)
		error = disk_sync(mountp->m_disk);
	mutex_unlock(&state->lock);
	return error;
}

static void
fat_unmount_impl(struct mount *mountp)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_metadata_table *metadata;
	unsigned long irq;
	if (state == NULL)
		return;
	mutex_lock(&state->lock);
	fat_engine_invalidate(state);
	mutex_unlock(&state->lock);
	metadata = state->metadata;
	irq = spin_lock_irqsave(&fat_pool_lock);
	memset(state, 0, sizeof(*state));
	if (metadata != NULL)
		memset(metadata, 0, sizeof(*metadata));
	spin_unlock_irqrestore(&fat_pool_lock, irq);
	mountp->m_data = NULL;
}

static int
fat_statvfs(struct mount *mountp, struct statvfs *result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_mount_state *fat;
	uint32_t free_clusters;
	int error;

	if (state == NULL || result == NULL)
		return EINVAL;
	mutex_lock(&state->lock);
	fat = state;
	error = fat_engine_count_free_clusters(state, &free_clusters);
	if (error == 0) {
		memset(result, 0, sizeof(*result));
		result->f_bsize = (uint64_t)fat->sectors_per_cluster * 512U;
		result->f_frsize = result->f_bsize;
		result->f_blocks = fat->cluster_count;
		result->f_bfree = free_clusters;
		result->f_bavail = free_clusters;
		/* FAT has no fixed inode table.  Use clusters as the capacity
		 * unit for the advisory file counts as well. */
		result->f_files = fat->cluster_count;
		result->f_ffree = free_clusters;
		result->f_favail = free_clusters;
		result->f_namemax = NAME_MAX;
	}
	mutex_unlock(&state->lock);
	return error;
}

const struct filesystem_type fat_filesystem_type = {
    .fs_name = "fat",
    .probe = fat_probe,
    .identify = fat_identify,
    .mount = fat_mount_impl,
    .sync = fat_sync_mount,
    .statvfs = fat_statvfs,
    .unmount = fat_unmount_impl,
    .alloc_inode = fat_alloc_inode,
    .free_inode = fat_free_inode,
};

int
fat_file_extents(struct file *file, fat_extent_cb callback, void *context)
{
	struct fat_mount_state *mount_state;
	struct fat_file_state *state;
	int error;
	if (file == NULL || callback == NULL || file->f_inode == NULL ||
	    file->f_inode->i_type != INODE_REG ||
	    file->f_inode->i_mount == NULL ||
	    file->f_inode->i_mount->m_type != &fat_filesystem_type)
		return EINVAL;
	mount_state = fat_mount_state(file->f_inode->i_mount);
	if (mount_state == NULL)
		return EIO;
	mutex_lock(&mount_state->lock);
	state = fat_file_get(file);
	if (state == NULL)
		error = EIO;
	else
		error = fat_engine_file_extents(state, callback, context);
	mutex_unlock(&mount_state->lock);
	return error;
}

struct contiguous_context {
	uint64_t block;
	uint64_t expected;
	int seen;
};

static int
contiguous_extent(uint64_t file_block, uint64_t disk_block, uint32_t count,
		  void *argument)
{
	struct contiguous_context *context = argument;
	if (!context->seen) {
		context->block = disk_block;
		context->expected = disk_block;
		context->seen = 1;
	}
	if (file_block + context->block != disk_block ||
	    disk_block != context->expected)
		return EOPNOTSUPP;
	context->expected += count;
	return 0;
}

int
fat_file_contiguous_block(struct file *file, struct disk **disk,
			  uint64_t *block)
{
	struct contiguous_context context = {0};
	int error;
	if (file == NULL || disk == NULL || block == NULL ||
	    file->f_inode == NULL || file->f_inode->i_mount == NULL ||
	    file->f_inode->i_mount->m_type != &fat_filesystem_type)
		return EINVAL;
	error = fat_file_extents(file, contiguous_extent, &context);
	if (error != 0 || !context.seen)
		return error != 0 ? error : EIO;
	*disk = file->f_inode->i_mount->m_disk;
	*block = context.block;
	return 0;
}
