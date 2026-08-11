/*
 * zedBSD FAT family support
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_FAT_H
#define ZEDBSD_FAT_H

#include "kern/fs.h"

enum zedbsd_fat_type {
	ZEDBSD_FAT12 = 12,
	ZEDBSD_FAT16 = 16,
	ZEDBSD_FAT32 = 32,
};

/* All sector addresses below use the filesystem layer's physical 512-byte
 * units. bytes_per_sector and sector_scale retain the logical BPB geometry. */
struct zedbsd_fat_state {
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

struct zedbsd_fat_file_state {
	uint32_t first_cluster;
	uint32_t directory_lba;
	uint16_t directory_offset;
	uint8_t directory_dirty;
	uint8_t reserved;
};

typedef enum zedbsd_fs_result (*zedbsd_fat_next_cluster_t)(
	struct zedbsd_filesystem *filesystem, uint32_t cluster,
	uint32_t *next_cluster);

struct zedbsd_fat_state *zedbsd_fat_state(
	struct zedbsd_filesystem *filesystem);
struct zedbsd_fat_file_state *zedbsd_fat_file_state(
	struct zedbsd_file *file);

enum zedbsd_fs_result zedbsd_fat_probe(
	const struct zedbsd_volume *volume,
	enum zedbsd_fat_type required_type);
enum zedbsd_fs_result zedbsd_fat_mount(
	struct zedbsd_filesystem *filesystem,
	enum zedbsd_fat_type required_type);

const uint8_t *zedbsd_fat_read_sector(struct zedbsd_filesystem *filesystem,
				      uint32_t lba);
enum zedbsd_fs_result zedbsd_fat_read_sector_result(
	struct zedbsd_filesystem *filesystem, uint32_t lba,
	const uint8_t **sector);
enum zedbsd_fs_result zedbsd_fat_write_sector_result(
	struct zedbsd_filesystem *filesystem, uint32_t lba, uint8_t **sector);
enum zedbsd_fs_result zedbsd_fat_mark_sector_dirty(
	struct zedbsd_filesystem *filesystem);
enum zedbsd_fs_result zedbsd_fat_flush(
	struct zedbsd_filesystem *filesystem);
void zedbsd_fat_invalidate(struct zedbsd_filesystem *filesystem);
enum zedbsd_fs_result zedbsd_fat_cluster_lba(
	struct zedbsd_filesystem *filesystem, uint32_t cluster,
	uint32_t sector_in_cluster, uint32_t *lba);
uint16_t zedbsd_fat_get16(const uint8_t *bytes);
uint32_t zedbsd_fat_get32(const uint8_t *bytes);

int fat_sfn_encode(const char *path, char output[11]);
int fat_sfn_equal(const uint8_t entry[32], const char name[11]);
void fat_sfn_decode_lower(const uint8_t raw[32],
			      struct zedbsd_dirent *entry);

enum zedbsd_fs_result zedbsd_fat_read_chain(
	struct zedbsd_file *file, uint64_t offset, void *buffer, uint32_t length,
	zedbsd_read_progress_t progress, void *progress_context,
	zedbsd_fat_next_cluster_t next_cluster, uint32_t end_of_chain);
enum zedbsd_fs_result zedbsd_fat_contiguous_lba(
	struct zedbsd_file *file, uint32_t *absolute_lba,
	zedbsd_fat_next_cluster_t next_cluster);

#endif
