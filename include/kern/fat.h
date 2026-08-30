/*
 * zedBSD FAT family interface
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_FAT_H
#define ZEDBSD_FAT_H

#include "kern/fs.h"
#include "kern/file.h"
#include "kern/mount.h"

#include <stddef.h>
#include <stdint.h>

/* FAT family support. */

enum bootfat_type {
	ZEDBSD_FAT12 = 12,
	ZEDBSD_FAT16 = 16,
	ZEDBSD_FAT32 = 32,
};

/*
 * All sector addresses below use the filesystem layer's physical 512-byte
 * units. bytes_per_sector and sector_scale retain the logical BPB geometry.
 */
struct bootfat_state {
	uint32_t fat_start;
	uint32_t root_start;
	uint32_t data_start;
	uint32_t total_sectors;
	uint32_t cluster_count;
	uint32_t fat_sectors;
	uint32_t allocation_hint;
	uint32_t root_cluster;
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
};

struct bootfat_file_state {
	uint32_t first_cluster;
	uint32_t directory_lba;
	uint16_t directory_offset;
	uint8_t directory_dirty;
	uint8_t reserved;
};

typedef enum bootfs_result (
	*bootfat_next_cluster_fn)(
	struct bootfs *filesystem,
	uint32_t cluster,
	uint32_t *next_cluster);

struct bootfat_state *
bootfat_state(
	struct bootfs *filesystem);
struct bootfat_file_state *
bootfat_file_state(
	struct bootfs_file *file);

enum bootfs_result
bootfat_probe(
	const struct boot_volume *volume,
	enum bootfat_type required_type);
enum bootfs_result
bootfat_mount(
	struct bootfs *filesystem,
	enum bootfat_type required_type);

const uint8_t *
bootfat_read_sector(
	struct bootfs *filesystem,
	uint32_t lba);
enum bootfs_result
bootfat_read_sector_result(
	struct bootfs *filesystem,
	uint32_t lba,
	const uint8_t **sector);
enum bootfs_result
bootfat_write_sector_result(
	struct bootfs *filesystem,
	uint32_t lba,
	uint8_t **sector);
enum bootfs_result
bootfat_mark_sector_dirty(
	struct bootfs *filesystem);
enum bootfs_result
bootfat_flush(
	struct bootfs *filesystem);
enum bootfs_result
bootfat_count_free_clusters(
	struct bootfs *filesystem,
	uint32_t *free_clusters);
void
bootfat_invalidate(
	struct bootfs *filesystem);
enum bootfs_result
bootfat_cluster_lba(
	struct bootfs *filesystem,
	uint32_t cluster,
	uint32_t sector_in_cluster,
	uint32_t *lba);
uint16_t
bootfat_get16(
	const uint8_t *bytes);
uint32_t
bootfat_get32(
	const uint8_t *bytes);

int
fat_sfn_encode(
	const char *path,
	char output[11]);
int
fat_sfn_equal(
	const uint8_t entry[32],
	const char name[11]);
void
fat_sfn_decode_lower(
	const uint8_t raw[32],
	struct bootfs_dirent *entry);

enum bootfs_result
bootfat_read_chain(
	struct bootfs_file *file,
	uint64_t offset,
	void *buffer,
	uint32_t length,
	bootfs_read_progress_fn progress,
	void *progress_context,
	bootfat_next_cluster_fn next_cluster,
	uint32_t end_of_chain);
enum bootfs_result
bootfat_contiguous_lba(
	struct bootfs_file *file,
	uint32_t *absolute_lba,
	bootfat_next_cluster_fn next_cluster);

/* VFAT long-file-name helpers. */

#define FAT_LFN_MAX_UNITS	255U

struct fat_lfn_state {
	uint16_t units[FAT_LFN_MAX_UNITS + 1U];
	uint16_t unit_limit;
	uint8_t expected;
	uint8_t checksum;
	uint8_t active;
};

void
fat_lfn_reset(
	struct fat_lfn_state *state);
int
fat_lfn_feed(
	struct fat_lfn_state *state,
	const uint8_t raw[32]);
int
fat_lfn_finish(
	struct fat_lfn_state *state,
	const uint8_t sfn[32],
	char *output,
	size_t capacity);
uint8_t
fat_lfn_checksum(
	const uint8_t sfn[11]);
int
fat_utf8_casefold_equal(
	const char *left,
	const char *right);
void
fat_sfn_decode_preserve(
	const uint8_t raw[32],
	char *output,
	size_t capacity);
int
fat_utf8_to_utf16(
	const char *name,
	uint16_t units[FAT_LFN_MAX_UNITS],
	unsigned *unit_count);
void
fat_lfn_build_entry(
	uint8_t raw[32],
	const uint16_t *units,
	unsigned unit_count,
	unsigned ordinal,
	uint8_t checksum);
int
fat_sfn_make_alias(
	const char *name,
	unsigned serial,
	uint8_t sfn[11]);

/* FAT12/FAT16 driver contract. */

extern const struct bootfs_driver bootfat12_driver;
extern const struct bootfs_driver bootfat16_driver;

typedef int (*bootfat_extent_fn)(uint64_t, uint64_t, uint32_t, void *);

int
bootfat_file_extents(
	struct bootfs_file *file,
	bootfat_extent_fn callback,
	void *context);

enum bootfs_result
bootfat_stat_location(
	struct bootfs *filesystem,
	const char *path,
	struct bootfs_dirent *entry,
	uint32_t *lba,
	uint16_t *offset,
	uint32_t *first_cluster,
	uint8_t *attributes);

enum bootfs_result
bootfat_stat_location_casefold(
	struct bootfs *filesystem,
	const char *path,
	struct bootfs_dirent *entry,
	uint32_t *lba,
	uint16_t *offset,
	uint32_t *first_cluster,
	uint8_t *attributes);

enum bootfs_result
bootfat_discard_chain_result(
	struct bootfs *filesystem,
	uint32_t first_cluster);

/* FAT32 driver contract. */

extern const struct bootfs_driver bootfat32_driver;

/* Native VFS adapter contract. */

struct fat_inode_info {
	struct inode fi_inode;
	uint32_t fi_first_cluster;
	uint32_t fi_dirent_lba;
	uint16_t fi_dirent_offset;
	uint8_t fi_attributes;
	uint8_t fi_flags;
};

static inline struct fat_inode_info *
fat_inode(struct inode *inode)
{
	return (struct fat_inode_info *)inode;
}

extern const struct filesystem_type fat_filesystem_type;

int fat_probe_type(struct disk *disk, enum bootfat_type *type);
typedef int (*fat_extent_cb)(uint64_t, uint64_t, uint32_t, void *);

int fat_file_extents(struct file *file, fat_extent_cb callback, void *context);

int fat_file_contiguous_block(struct file *file, struct disk **disk,
			      uint64_t *block);

int fat_file_backing_identity(struct inode *inode, struct disk **disk,
			      uint64_t *object);

#endif
