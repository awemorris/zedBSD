/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * FAT family support
 */

#ifndef ZEDBSD_FAT_H
#define ZEDBSD_FAT_H

#include "kern/fs.h"

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

#endif
