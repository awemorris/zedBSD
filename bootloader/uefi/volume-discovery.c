/* Bounded helpers for UEFI zedbsd.cfg volume discovery. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "volume-discovery.h"

#define DEVICE_PATH_HEADER_SIZE 4U
#define DEVICE_PATH_TYPE_HARDWARE 0x01U
#define DEVICE_PATH_TYPE_ACPI 0x02U
#define DEVICE_PATH_TYPE_MESSAGING 0x03U
#define DEVICE_PATH_TYPE_MEDIA 0x04U
#define DEVICE_PATH_TYPE_END 0x7fU
#define DEVICE_PATH_SUBTYPE_HARD_DRIVE 0x01U
#define DEVICE_PATH_SUBTYPE_END_INSTANCE 0x01U
#define DEVICE_PATH_SUBTYPE_END_ENTIRE 0xffU
#define HARD_DRIVE_DEVICE_PATH_SIZE 42U
#define HARD_DRIVE_MBR_TYPE_OFFSET 40U
#define HARD_DRIVE_SIGNATURE_TYPE_OFFSET 41U
#define HARD_DRIVE_MBR_TYPE_PCAT 0x01U
#define HARD_DRIVE_MBR_TYPE_GPT 0x02U
#define HARD_DRIVE_SIGNATURE_MBR 0x01U
#define HARD_DRIVE_SIGNATURE_GUID 0x02U

#define FAT_BOOT_SIGNATURE_OFFSET 510U
#define FAT16_EXTENDED_SIGNATURE_OFFSET 38U
#define FAT16_SERIAL_OFFSET 39U
#define FAT32_EXTENDED_SIGNATURE_OFFSET 66U
#define FAT32_SERIAL_OFFSET 67U
#define FAT_EXTENDED_SIGNATURE 0x29U
#define FAT12_CLUSTER_LIMIT 4085U
#define FAT16_CLUSTER_LIMIT 65525U
#define FAT32_CLUSTER_MAX 0x0ffffff4U

static uint16_t
read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t
read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t
read_le64(const uint8_t *bytes)
{
	return (uint64_t)read_le32(bytes) |
	       ((uint64_t)read_le32(bytes + 4U) << 32);
}

static void
byte_zero(void *pointer, size_t size)
{
	uint8_t *bytes = pointer;

	while (size-- != 0U)
		*bytes++ = 0U;
}

static int
byte_equal(const uint8_t *left, const uint8_t *right, size_t size)
{
	while (size-- != 0U)
		if (*left++ != *right++)
			return 0;
	return 1;
}

static enum zbl_uefi_device_path_result
path_boundary_result(size_t available_bytes)
{
	return available_bytes > ZBL_UEFI_DEVICE_PATH_MAX_BYTES ?
	    ZBL_UEFI_DEVICE_PATH_LIMIT : ZBL_UEFI_DEVICE_PATH_TRUNCATED;
}

enum zbl_uefi_device_path_result
zbl_uefi_partition_path_parse(const void *path, size_t available_bytes,
	struct zbl_uefi_partition_path *view)
{
	const uint8_t *bytes = path;
	size_t limit;
	size_t position = 0U;
	size_t nodes = 0U;
	size_t partition_offset = 0U;
	uint32_t partition_number = 0U;
	uint64_t partition_start = 0U;
	uint64_t partition_size = 0U;
	enum zbl_uefi_partition_style style = ZBL_UEFI_PARTITION_MBR;
	int found_partition = 0;

	if (path == NULL || view == NULL || available_bytes == 0U)
		return ZBL_UEFI_DEVICE_PATH_INVALID_ARGUMENT;
	byte_zero(view, sizeof(*view));
	limit = available_bytes;
	if (limit > ZBL_UEFI_DEVICE_PATH_MAX_BYTES)
		limit = ZBL_UEFI_DEVICE_PATH_MAX_BYTES;
	while (position < limit) {
		uint8_t type;
		uint8_t subtype;
		size_t node_size;

		if (limit - position < DEVICE_PATH_HEADER_SIZE)
			return path_boundary_result(available_bytes);
		type = bytes[position];
		subtype = bytes[position + 1U];
		node_size = read_le16(bytes + position + 2U);
		if (node_size < DEVICE_PATH_HEADER_SIZE)
			return ZBL_UEFI_DEVICE_PATH_MALFORMED;
		if (node_size > limit - position)
			return path_boundary_result(available_bytes);
		if (type == DEVICE_PATH_TYPE_END) {
			if (node_size != DEVICE_PATH_HEADER_SIZE)
				return ZBL_UEFI_DEVICE_PATH_MALFORMED;
			if (subtype == DEVICE_PATH_SUBTYPE_END_INSTANCE)
				return ZBL_UEFI_DEVICE_PATH_UNSUPPORTED;
			if (subtype != DEVICE_PATH_SUBTYPE_END_ENTIRE)
				return ZBL_UEFI_DEVICE_PATH_MALFORMED;
			if (!found_partition)
				return ZBL_UEFI_DEVICE_PATH_NO_PARTITION;
			view->bytes = bytes;
			view->path_size = position + node_size;
			view->disk_prefix_size = partition_offset;
			view->partition_offset = partition_offset;
			view->partition_number = partition_number;
			view->partition_start = partition_start;
			view->partition_size = partition_size;
			view->style = style;
			return ZBL_UEFI_DEVICE_PATH_OK;
		}
		if (nodes == ZBL_UEFI_DEVICE_PATH_MAX_NODES)
			return ZBL_UEFI_DEVICE_PATH_LIMIT;
		nodes++;
		if (type == DEVICE_PATH_TYPE_MEDIA &&
		    subtype == DEVICE_PATH_SUBTYPE_HARD_DRIVE) {
			uint8_t mbr_type;
			uint8_t signature_type;

			if (found_partition || position == 0U ||
			    node_size != HARD_DRIVE_DEVICE_PATH_SIZE)
				return ZBL_UEFI_DEVICE_PATH_MALFORMED;
			mbr_type = bytes[position + HARD_DRIVE_MBR_TYPE_OFFSET];
			signature_type =
			    bytes[position + HARD_DRIVE_SIGNATURE_TYPE_OFFSET];
			if (mbr_type == HARD_DRIVE_MBR_TYPE_PCAT &&
			    signature_type == HARD_DRIVE_SIGNATURE_MBR) {
				style = ZBL_UEFI_PARTITION_MBR;
			} else if (mbr_type == HARD_DRIVE_MBR_TYPE_GPT &&
			    signature_type == HARD_DRIVE_SIGNATURE_GUID) {
				style = ZBL_UEFI_PARTITION_GPT;
			} else {
				return ZBL_UEFI_DEVICE_PATH_UNSUPPORTED;
			}
			partition_number = read_le32(bytes + position + 4U);
			partition_start = read_le64(bytes + position + 8U);
			partition_size = read_le64(bytes + position + 16U);
			if (partition_number == 0U || partition_size == 0U ||
			    partition_start > UINT64_MAX - partition_size)
				return ZBL_UEFI_DEVICE_PATH_MALFORMED;
			partition_offset = position;
			found_partition = 1;
		} else if (!found_partition) {
			if (type < DEVICE_PATH_TYPE_HARDWARE ||
			    type > DEVICE_PATH_TYPE_MESSAGING)
				return ZBL_UEFI_DEVICE_PATH_UNSUPPORTED;
		} else if (type != DEVICE_PATH_TYPE_MEDIA) {
			return ZBL_UEFI_DEVICE_PATH_UNSUPPORTED;
		}
		position += node_size;
	}
	return path_boundary_result(available_bytes);
}

int
zbl_uefi_partition_paths_same_disk(
	const struct zbl_uefi_partition_path *left,
	const struct zbl_uefi_partition_path *right)
{
	if (left == NULL || right == NULL || left->bytes == NULL ||
	    right->bytes == NULL || left->disk_prefix_size == 0U ||
	    left->disk_prefix_size != right->disk_prefix_size ||
	    left->partition_offset != left->disk_prefix_size ||
	    right->partition_offset != right->disk_prefix_size ||
	    left->disk_prefix_size > left->path_size ||
	    right->disk_prefix_size > right->path_size)
		return 0;
	return byte_equal(left->bytes, right->bytes,
	    left->disk_prefix_size);
}

const char *
zbl_uefi_device_path_result_name(enum zbl_uefi_device_path_result result)
{
	switch (result) {
	case ZBL_UEFI_DEVICE_PATH_OK:
		return "ok";
	case ZBL_UEFI_DEVICE_PATH_INVALID_ARGUMENT:
		return "invalid-argument";
	case ZBL_UEFI_DEVICE_PATH_TRUNCATED:
		return "truncated";
	case ZBL_UEFI_DEVICE_PATH_LIMIT:
		return "limit";
	case ZBL_UEFI_DEVICE_PATH_MALFORMED:
		return "malformed";
	case ZBL_UEFI_DEVICE_PATH_UNSUPPORTED:
		return "unsupported";
	case ZBL_UEFI_DEVICE_PATH_NO_PARTITION:
		return "no-partition";
	default:
		return "unknown";
	}
}

enum zbl_uefi_handle_order_result
zbl_uefi_volume_order_handles(void *loaded_handle,
	void *const *firmware_handles, size_t firmware_handle_count,
	void **ordered_handles, size_t ordered_capacity, size_t *ordered_count)
{
	size_t used = 1U;

	if (ordered_count != NULL)
		*ordered_count = 0U;
	if (loaded_handle == NULL || ordered_handles == NULL ||
	    ordered_count == NULL ||
	    (firmware_handles == NULL && firmware_handle_count != 0U))
		return ZBL_UEFI_HANDLE_ORDER_INVALID_ARGUMENT;
	if (firmware_handle_count > ZBL_UEFI_VOLUME_MAX_HANDLES)
		return ZBL_UEFI_HANDLE_ORDER_LIMIT;
	if (ordered_capacity == 0U)
		return ZBL_UEFI_HANDLE_ORDER_OUTPUT_TOO_SMALL;
	for (size_t index = 0U; index < firmware_handle_count; index++)
		if (firmware_handles[index] == NULL)
			return ZBL_UEFI_HANDLE_ORDER_INVALID_ARGUMENT;

	ordered_handles[0] = loaded_handle;
	for (size_t index = 0U; index < firmware_handle_count; index++) {
		void *handle = firmware_handles[index];
		int duplicate = 0;

		for (size_t previous = 0U; previous < used; previous++)
			if (ordered_handles[previous] == handle) {
				duplicate = 1;
				break;
			}
		if (duplicate)
			continue;
		if (used == ZBL_UEFI_VOLUME_MAX_HANDLES)
			return ZBL_UEFI_HANDLE_ORDER_LIMIT;
		if (used == ordered_capacity)
			return ZBL_UEFI_HANDLE_ORDER_OUTPUT_TOO_SMALL;
		ordered_handles[used++] = handle;
	}
	*ordered_count = used;
	return ZBL_UEFI_HANDLE_ORDER_OK;
}

static int
supported_fat_block_size(uint32_t size)
{
	/* Keep discovery aligned with the kernel FAT driver's disk contract. */
	return size == 512U;
}

static int
fat_media_byte(uint8_t value)
{
	return value == 0xf0U || value >= 0xf8U;
}

static int
fat_capacity_ok(uint64_t fat_bytes, uint64_t clusters, unsigned bits)
{
	uint64_t entries = clusters + 2U;
	uint64_t required;

	if (bits == 12U) {
		if (entries > (UINT64_MAX - 1U) / 3U)
			return 0;
		required = (entries * 3U + 1U) / 2U;
	} else {
		if (entries > UINT64_MAX / (bits / 8U))
			return 0;
		required = entries * (bits / 8U);
	}
	return required <= fat_bytes;
}

enum zbl_uefi_fat_result
zbl_uefi_fat_bpb_parse(const void *boot_sector, size_t boot_sector_bytes,
	uint32_t media_block_size, uint64_t media_block_count,
	struct zbl_uefi_fat_info *info)
{
	const uint8_t *boot = boot_sector;
	uint16_t bytes_per_sector;
	uint8_t sectors_per_cluster;
	uint16_t reserved_sectors;
	uint8_t fat_count;
	uint16_t root_entries;
	uint16_t total16;
	uint32_t total32;
	uint32_t total_sectors;
	uint16_t fat16_sectors;
	uint32_t fat_sectors;
	uint64_t root_sectors;
	uint64_t overhead;
	uint64_t data_sectors;
	uint64_t clusters;
	uint64_t fat_bytes;
	uint32_t sector_scale;
	unsigned serial_offset;
	enum zbl_uefi_fat_type type;

	if (boot_sector == NULL || info == NULL || media_block_count == 0U)
		return ZBL_UEFI_FAT_INVALID_ARGUMENT;
	byte_zero(info, sizeof(*info));
	if (!supported_fat_block_size(media_block_size) ||
	    media_block_size > ZBL_UEFI_FAT_MAX_BLOCK_SIZE)
		return ZBL_UEFI_FAT_UNSUPPORTED_BLOCK_SIZE;
	if (boot_sector_bytes < media_block_size ||
	    boot_sector_bytes <= FAT_BOOT_SIGNATURE_OFFSET + 1U)
		return ZBL_UEFI_FAT_TRUNCATED;
	if (byte_equal(boot + 3U, (const uint8_t *)"EXFAT   ", 8U))
		return ZBL_UEFI_FAT_EXFAT;
	if (!((boot[0] == 0xebU && boot[2] == 0x90U) ||
	      boot[0] == 0xe9U) ||
	    boot[FAT_BOOT_SIGNATURE_OFFSET] != 0x55U ||
	    boot[FAT_BOOT_SIGNATURE_OFFSET + 1U] != 0xaaU)
		return ZBL_UEFI_FAT_NON_FAT;

	bytes_per_sector = read_le16(boot + 11U);
	sectors_per_cluster = boot[13U];
	reserved_sectors = read_le16(boot + 14U);
	fat_count = boot[16U];
	root_entries = read_le16(boot + 17U);
	total16 = read_le16(boot + 19U);
	total32 = read_le32(boot + 32U);
	fat16_sectors = read_le16(boot + 22U);
	sector_scale = bytes_per_sector == 512U ? 1U :
	    bytes_per_sector == 1024U ? 2U : 0U;
	if (sector_scale == 0U ||
	    sectors_per_cluster == 0U ||
	    (sectors_per_cluster & (sectors_per_cluster - 1U)) != 0U ||
	    (uint64_t)bytes_per_sector * sectors_per_cluster > 65536U ||
	    reserved_sectors == 0U || fat_count == 0U || fat_count > 4U ||
	    !fat_media_byte(boot[21U]) ||
	    (total16 == 0U) == (total32 == 0U))
		return ZBL_UEFI_FAT_MALFORMED;
	total_sectors = total16 != 0U ? total16 : total32;
	/* BPB sectors may be 1024 bytes on a 512-byte kernel disk. */
	if ((uint64_t)total_sectors > media_block_count / sector_scale)
		return ZBL_UEFI_FAT_MALFORMED;
	fat_sectors = fat16_sectors != 0U ? fat16_sectors :
	    read_le32(boot + 36U);
	if (fat_sectors == 0U)
		return ZBL_UEFI_FAT_MALFORMED;
	root_sectors = ((uint64_t)root_entries * 32U +
	    bytes_per_sector - 1U) / bytes_per_sector;
	overhead = (uint64_t)reserved_sectors +
	    (uint64_t)fat_count * fat_sectors + root_sectors;
	if (overhead >= total_sectors)
		return ZBL_UEFI_FAT_MALFORMED;
	data_sectors = total_sectors - overhead;
	clusters = data_sectors / sectors_per_cluster;
	if (clusters == 0U)
		return ZBL_UEFI_FAT_MALFORMED;
	fat_bytes = (uint64_t)fat_sectors * bytes_per_sector;

	if (clusters < FAT12_CLUSTER_LIMIT) {
		if (fat16_sectors == 0U || root_entries == 0U ||
		    !fat_capacity_ok(fat_bytes, clusters, 12U))
			return ZBL_UEFI_FAT_MALFORMED;
		return ZBL_UEFI_FAT12;
	}
	if (clusters < FAT16_CLUSTER_LIMIT) {
		if (fat16_sectors == 0U || root_entries == 0U ||
		    !fat_capacity_ok(fat_bytes, clusters, 16U))
			return ZBL_UEFI_FAT_MALFORMED;
		type = ZBL_UEFI_FAT16;
		serial_offset = FAT16_SERIAL_OFFSET;
	} else {
		uint32_t root_cluster;

		if (clusters > FAT32_CLUSTER_MAX || fat16_sectors != 0U ||
		    root_entries != 0U || read_le16(boot + 42U) != 0U ||
		    !fat_capacity_ok(fat_bytes, clusters, 32U))
			return ZBL_UEFI_FAT_MALFORMED;
		root_cluster = read_le32(boot + 44U);
		if (root_cluster < 2U || (uint64_t)root_cluster >= clusters + 2U)
			return ZBL_UEFI_FAT_MALFORMED;
		type = ZBL_UEFI_FAT32;
		serial_offset = FAT32_SERIAL_OFFSET;
	}
	if (boot[serial_offset - 1U] != FAT_EXTENDED_SIGNATURE)
		return ZBL_UEFI_FAT_NO_SERIAL;
	info->type = type;
	info->bytes_per_sector = bytes_per_sector;
	info->sectors_per_cluster = sectors_per_cluster;
	info->total_sectors = total_sectors;
	info->fat_sectors = fat_sectors;
	info->cluster_count = (uint32_t)clusters;
	info->volume_serial = read_le32(boot + serial_offset);
	return ZBL_UEFI_FAT_OK;
}

void
zbl_uefi_fat_uuid(uint32_t serial,
	char output[ZBL_UEFI_FAT_UUID_SIZE])
{
	static const char digits[] = "0123456789ABCDEF";

	if (output == NULL)
		return;
	for (unsigned index = 0U; index < 4U; index++)
		output[index] =
		    digits[(serial >> (28U - index * 4U)) & 15U];
	output[4] = '-';
	for (unsigned index = 0U; index < 4U; index++)
		output[5U + index] =
		    digits[(serial >> (12U - index * 4U)) & 15U];
	output[9] = '\0';
}

const char *
zbl_uefi_fat_result_name(enum zbl_uefi_fat_result result)
{
	switch (result) {
	case ZBL_UEFI_FAT_OK:
		return "ok";
	case ZBL_UEFI_FAT_INVALID_ARGUMENT:
		return "invalid-argument";
	case ZBL_UEFI_FAT_TRUNCATED:
		return "truncated";
	case ZBL_UEFI_FAT_UNSUPPORTED_BLOCK_SIZE:
		return "unsupported-block-size";
	case ZBL_UEFI_FAT_EXFAT:
		return "exfat";
	case ZBL_UEFI_FAT_NON_FAT:
		return "non-fat";
	case ZBL_UEFI_FAT_MALFORMED:
		return "malformed";
	case ZBL_UEFI_FAT12:
		return "fat12";
	case ZBL_UEFI_FAT_NO_SERIAL:
		return "no-serial";
	default:
		return "unknown";
	}
}

void
zbl_uefi_volume_selection_init(
	struct zbl_uefi_volume_selection *selection)
{
	if (selection != NULL)
		byte_zero(selection, sizeof(*selection));
}

enum zbl_uefi_volume_match_action
zbl_uefi_volume_selection_record_match(
	struct zbl_uefi_volume_selection *selection, void *handle,
	size_t order, const struct zbl_uefi_fat_info *fat,
	void *root, void *config)
{
	if (selection == NULL || handle == NULL || fat == NULL || root == NULL ||
	    config == NULL || order >= ZBL_UEFI_VOLUME_MAX_HANDLES ||
	    (fat->type != ZBL_UEFI_FAT16 && fat->type != ZBL_UEFI_FAT32) ||
	    selection->match_count >= ZBL_UEFI_VOLUME_MAX_HANDLES ||
	    (selection->match_count != 0U &&
	     order <= selection->last_match_order))
		return ZBL_UEFI_VOLUME_MATCH_INVALID;
	selection->match_count++;
	selection->last_match_order = order;
	if (selection->has_selected)
		return ZBL_UEFI_VOLUME_MATCH_RELEASE;
	selection->selected.handle = handle;
	selection->selected.root = root;
	selection->selected.config = config;
	selection->selected.order = order;
	selection->selected.fat = *fat;
	zbl_uefi_fat_uuid(fat->volume_serial, selection->selected.uuid);
	selection->has_selected = 1;
	return ZBL_UEFI_VOLUME_MATCH_KEEP;
}

enum zbl_uefi_volume_selection_result
zbl_uefi_volume_selection_finish(
	const struct zbl_uefi_volume_selection *selection)
{
	if (selection == NULL ||
	    (selection->has_selected && selection->match_count == 0U) ||
	    (!selection->has_selected && selection->match_count != 0U))
		return ZBL_UEFI_VOLUME_SELECTION_INVALID_ARGUMENT;
	if (selection->match_count == 0U)
		return ZBL_UEFI_VOLUME_SELECTION_NOT_FOUND;
	if (selection->match_count == 1U)
		return ZBL_UEFI_VOLUME_SELECTION_ONE;
	return ZBL_UEFI_VOLUME_SELECTION_MULTIPLE;
}

int
zbl_uefi_volume_selection_take(
	struct zbl_uefi_volume_selection *selection,
	struct zbl_uefi_volume_match *match)
{
	if (selection == NULL || match == NULL || !selection->has_selected ||
	    selection->match_count == 0U)
		return 0;
	*match = selection->selected;
	zbl_uefi_volume_selection_init(selection);
	return 1;
}
