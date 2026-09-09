/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_MKFS_FAT32_FORMAT_H
#define ZEDBSD_MKFS_FAT32_FORMAT_H

#include <stdint.h>

struct fat32_format_geometry {
	uint32_t sector_size;
	uint32_t sectors;
	uint32_t sectors_per_cluster;
	uint32_t fat_sectors;
	uint32_t clusters;
	uint32_t data_sector;
	uint32_t hidden_sectors;
	uint32_t volume_id;
};

int fat32_format_geometry(uint64_t, uint32_t, uint64_t, uint32_t,
    struct fat32_format_geometry *);
/* Caller retains exclusive mutation ownership throughout write and verify. */
int fat32_format_write(int, const struct fat32_format_geometry *);
int fat32_format_verify(int, const struct fat32_format_geometry *);

#endif
