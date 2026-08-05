/*
 * Boots FAT family support
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_FAT_H
#define BOOTS_FAT_H

#include "core/fs.h"

enum boots_fat_type {
	BOOTS_FAT12 = 12,
	BOOTS_FAT16 = 16,
	BOOTS_FAT32 = 32,
};

/* All sector addresses below use the filesystem layer's physical 512-byte
 * units. bytes_per_sector and sector_scale retain the logical BPB geometry. */
struct boots_fat_state {
	uint32_t fat_start;
	uint32_t root_start;
	uint32_t data_start;
	uint32_t total_sectors;
	uint32_t cluster_count;
	uint32_t fat_sectors;
	uint32_t allocation_hint;
	uint16_t bytes_per_sector;
	uint16_t root_entries;
	uint16_t sectors_per_cluster;
	uint8_t sector_scale;
	uint8_t number_of_fats;
	uint8_t type;
	uint8_t fat16_layout;
};

struct boots_fat_file_state {
	uint32_t first_cluster;
	uint32_t directory_lba;
	uint16_t directory_offset;
	uint8_t directory_dirty;
	uint8_t reserved;
};

typedef enum boots_fs_result (*boots_fat_next_cluster_t)(
	struct boots_filesystem *filesystem, uint32_t cluster,
	uint32_t *next_cluster);

struct boots_fat_state *boots_fat_state(
	struct boots_filesystem *filesystem);
struct boots_fat_file_state *boots_fat_file_state(
	struct boots_file *file);

enum boots_fs_result boots_fat_probe(
	const struct boots_volume *volume,
	enum boots_fat_type required_type);
enum boots_fs_result boots_fat_mount(
	struct boots_filesystem *filesystem,
	enum boots_fat_type required_type);

const uint8_t *boots_fat_read_sector(struct boots_filesystem *filesystem,
				      uint32_t lba);
enum boots_fs_result boots_fat_read_sector_result(
	struct boots_filesystem *filesystem, uint32_t lba,
	const uint8_t **sector);
enum boots_fs_result boots_fat_write_sector_result(
	struct boots_filesystem *filesystem, uint32_t lba, uint8_t **sector);
enum boots_fs_result boots_fat_mark_sector_dirty(
	struct boots_filesystem *filesystem);
enum boots_fs_result boots_fat_flush(
	struct boots_filesystem *filesystem);
void boots_fat_invalidate(struct boots_filesystem *filesystem);
enum boots_fs_result boots_fat_cluster_lba(
	struct boots_filesystem *filesystem, uint32_t cluster,
	uint32_t sector_in_cluster, uint32_t *lba);
uint16_t boots_fat_get16(const uint8_t *bytes);
uint32_t boots_fat_get32(const uint8_t *bytes);

int boots_fat_short_name(const char *path, char output[11]);
int boots_fat_name_matches(const uint8_t entry[32], const char name[11]);
void boots_fat_decode_dirent(const uint8_t raw[32],
			      struct boots_dirent *entry);

enum boots_fs_result boots_fat_read_chain(
	struct boots_file *file, uint64_t offset, void *buffer, uint32_t length,
	boots_read_progress_t progress, void *progress_context,
	boots_fat_next_cluster_t next_cluster, uint32_t end_of_chain);
enum boots_fs_result boots_fat_contiguous_lba(
	struct boots_file *file, uint32_t *absolute_lba,
	boots_fat_next_cluster_t next_cluster);

#endif
