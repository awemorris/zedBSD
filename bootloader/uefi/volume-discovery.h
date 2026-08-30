/* Bounded helpers for UEFI zedbsd.cfg volume discovery. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UEFI_VOLUME_DISCOVERY_H
#define ZEDBSD_UEFI_VOLUME_DISCOVERY_H

#include <stddef.h>
#include <stdint.h>

#define ZBL_UEFI_VOLUME_MAX_HANDLES 128U
#define ZBL_UEFI_DEVICE_PATH_MAX_BYTES 4096U
#define ZBL_UEFI_DEVICE_PATH_MAX_NODES 128U
#define ZBL_UEFI_FAT_MAX_BLOCK_SIZE 4096U
#define ZBL_UEFI_FAT_UUID_SIZE 10U

/* Values match the ZBL6 handoff partition-scheme constants. */
enum zbl_uefi_partition_style {
	ZBL_UEFI_PARTITION_MBR = 1,
	ZBL_UEFI_PARTITION_GPT = 4
};

enum zbl_uefi_device_path_result {
	ZBL_UEFI_DEVICE_PATH_OK = 0,
	ZBL_UEFI_DEVICE_PATH_INVALID_ARGUMENT,
	ZBL_UEFI_DEVICE_PATH_TRUNCATED,
	ZBL_UEFI_DEVICE_PATH_LIMIT,
	ZBL_UEFI_DEVICE_PATH_MALFORMED,
	ZBL_UEFI_DEVICE_PATH_UNSUPPORTED,
	ZBL_UEFI_DEVICE_PATH_NO_PARTITION
};

/*
 * The view borrows the firmware-owned path.  disk_prefix_size excludes the
 * HD() node and every child node, so two partition paths compare only their
 * provable physical-device prefix.
 */
struct zbl_uefi_partition_path {
	const uint8_t *bytes;
	size_t path_size;
	size_t disk_prefix_size;
	size_t partition_offset;
	uint32_t partition_number;
	uint64_t partition_start;
	uint64_t partition_size;
	enum zbl_uefi_partition_style style;
};

enum zbl_uefi_device_path_result zbl_uefi_partition_path_parse(
	const void *path, size_t available_bytes,
	struct zbl_uefi_partition_path *view);

int zbl_uefi_partition_paths_same_disk(
	const struct zbl_uefi_partition_path *left,
	const struct zbl_uefi_partition_path *right);

const char *zbl_uefi_device_path_result_name(
	enum zbl_uefi_device_path_result result);

enum zbl_uefi_handle_order_result {
	ZBL_UEFI_HANDLE_ORDER_OK = 0,
	ZBL_UEFI_HANDLE_ORDER_INVALID_ARGUMENT,
	ZBL_UEFI_HANDLE_ORDER_LIMIT,
	ZBL_UEFI_HANDLE_ORDER_OUTPUT_TOO_SMALL
};

/*
 * Place loaded_handle first, then append the first occurrence of each
 * firmware handle without changing firmware order.  No allocation occurs.
 */
enum zbl_uefi_handle_order_result zbl_uefi_volume_order_handles(
	void *loaded_handle, void *const *firmware_handles,
	size_t firmware_handle_count, void **ordered_handles,
	size_t ordered_capacity, size_t *ordered_count);

enum zbl_uefi_fat_type {
	ZBL_UEFI_FAT16 = 16,
	ZBL_UEFI_FAT32 = 32
};

enum zbl_uefi_fat_result {
	ZBL_UEFI_FAT_OK = 0,
	ZBL_UEFI_FAT_INVALID_ARGUMENT,
	ZBL_UEFI_FAT_TRUNCATED,
	ZBL_UEFI_FAT_UNSUPPORTED_BLOCK_SIZE,
	ZBL_UEFI_FAT_EXFAT,
	ZBL_UEFI_FAT_NON_FAT,
	ZBL_UEFI_FAT_MALFORMED,
	ZBL_UEFI_FAT12,
	ZBL_UEFI_FAT_NO_SERIAL
};

struct zbl_uefi_fat_info {
	enum zbl_uefi_fat_type type;
	uint16_t bytes_per_sector;
	uint8_t sectors_per_cluster;
	uint32_t total_sectors;
	uint32_t fat_sectors;
	uint32_t cluster_count;
	uint32_t volume_serial;
};

/*
 * media_block_count is LastBlock + 1, in media_block_size units.  Discovery
 * intentionally matches the kernel FAT driver: 512-byte media blocks and a
 * FAT BPB bytes-per-sector value of either 512 or 1024.
 */
enum zbl_uefi_fat_result zbl_uefi_fat_bpb_parse(
	const void *boot_sector, size_t boot_sector_bytes,
	uint32_t media_block_size, uint64_t media_block_count,
	struct zbl_uefi_fat_info *info);

/* Format the canonical FAT UUID as XXXX-XXXX. */
void zbl_uefi_fat_uuid(uint32_t serial,
	char output[ZBL_UEFI_FAT_UUID_SIZE]);

const char *zbl_uefi_fat_result_name(enum zbl_uefi_fat_result result);

/*
 * A successful marker open owns two resources until p003 consumes them.
 * record_match() returns KEEP for the first match and transfers those opaque
 * resources to the state.  RELEASE leaves later resources with the caller,
 * which must close them immediately.  take_selected() moves the retained
 * resources back to the caller on success or error cleanup.
 */
struct zbl_uefi_volume_match {
	void *handle;
	void *root;
	void *config;
	size_t order;
	struct zbl_uefi_fat_info fat;
	char uuid[ZBL_UEFI_FAT_UUID_SIZE];
};

struct zbl_uefi_volume_selection {
	size_t match_count;
	size_t last_match_order;
	int has_selected;
	struct zbl_uefi_volume_match selected;
};

enum zbl_uefi_volume_match_action {
	ZBL_UEFI_VOLUME_MATCH_INVALID = 0,
	ZBL_UEFI_VOLUME_MATCH_KEEP,
	ZBL_UEFI_VOLUME_MATCH_RELEASE
};

enum zbl_uefi_volume_selection_result {
	ZBL_UEFI_VOLUME_SELECTION_INVALID_ARGUMENT = 0,
	ZBL_UEFI_VOLUME_SELECTION_NOT_FOUND,
	ZBL_UEFI_VOLUME_SELECTION_ONE,
	ZBL_UEFI_VOLUME_SELECTION_MULTIPLE
};

void zbl_uefi_volume_selection_init(
	struct zbl_uefi_volume_selection *selection);

enum zbl_uefi_volume_match_action zbl_uefi_volume_selection_record_match(
	struct zbl_uefi_volume_selection *selection, void *handle,
	size_t order, const struct zbl_uefi_fat_info *fat,
	void *root, void *config);

enum zbl_uefi_volume_selection_result zbl_uefi_volume_selection_finish(
	const struct zbl_uefi_volume_selection *selection);

int zbl_uefi_volume_selection_take(
	struct zbl_uefi_volume_selection *selection,
	struct zbl_uefi_volume_match *match);

#endif
