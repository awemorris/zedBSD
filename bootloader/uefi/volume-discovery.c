/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bounded helpers for UEFI zedbsd.cfg volume discovery.
 */

#include "volume-discovery.h"

/* The device path node header, and the node types and subtypes the parser knows. */
#define DEVICE_PATH_HEADER_SIZE 4U
#define DEVICE_PATH_TYPE_HARDWARE 0x01U
#define DEVICE_PATH_TYPE_ACPI 0x02U
#define DEVICE_PATH_TYPE_MESSAGING 0x03U
#define DEVICE_PATH_TYPE_MEDIA 0x04U
#define DEVICE_PATH_TYPE_END 0x7fU
#define DEVICE_PATH_SUBTYPE_HARD_DRIVE 0x01U
#define DEVICE_PATH_SUBTYPE_END_INSTANCE 0x01U
#define DEVICE_PATH_SUBTYPE_END_ENTIRE 0xffU

/* The layout of the HD() media node and the partition styles it names. */
#define HARD_DRIVE_DEVICE_PATH_SIZE 42U
#define HARD_DRIVE_MBR_TYPE_OFFSET 40U
#define HARD_DRIVE_SIGNATURE_TYPE_OFFSET 41U
#define HARD_DRIVE_MBR_TYPE_PCAT 0x01U
#define HARD_DRIVE_MBR_TYPE_GPT 0x02U
#define HARD_DRIVE_SIGNATURE_MBR 0x01U
#define HARD_DRIVE_SIGNATURE_GUID 0x02U

/* The FAT boot sector fields and the cluster counts that tell the FAT types apart. */
#define FAT_BOOT_SIGNATURE_OFFSET 510U
#define FAT16_EXTENDED_SIGNATURE_OFFSET 38U
#define FAT16_SERIAL_OFFSET 39U
#define FAT32_EXTENDED_SIGNATURE_OFFSET 66U
#define FAT32_SERIAL_OFFSET 67U
#define FAT_EXTENDED_SIGNATURE 0x29U
#define FAT12_CLUSTER_LIMIT 4085U
#define FAT16_CLUSTER_LIMIT 65525U
#define FAT32_CLUSTER_MAX 0x0ffffff4U

static uint16_t read_le16(const uint8_t *bytes);
static uint32_t read_le32(const uint8_t *bytes);
static uint64_t read_le64(const uint8_t *bytes);
static void byte_zero(void *pointer, size_t size);
static int byte_equal(const uint8_t *left, const uint8_t *right, size_t size);
static enum zbl_uefi_device_path_result path_boundary_result(size_t available_bytes);
static int supported_fat_block_size(uint32_t size);
static int fat_media_byte(uint8_t value);
static int fat_capacity_ok(uint64_t fat_bytes, uint64_t clusters, unsigned bits);

/*
 * Parses a firmware device path down to its one partition node.
 *
 * The path must be a single instance of hardware, ACPI or messaging nodes,
 * one HD() node, and only media nodes after it.  The view borrows the path.
 */
enum zbl_uefi_device_path_result
zbl_uefi_partition_path_parse(
	const void *path,
	size_t available_bytes,
	struct zbl_uefi_partition_path *view)
{
	const uint8_t *bytes;
	size_t limit;
	size_t position;
	size_t nodes;
	size_t partition_offset;
	size_t node_size;
	uint32_t partition_number;
	uint64_t partition_start;
	uint64_t partition_size;
	uint8_t type;
	uint8_t subtype;
	uint8_t mbr_type;
	uint8_t signature_type;
	enum zbl_uefi_partition_style style;
	enum zbl_uefi_device_path_result boundary;
	int found_partition;

	/* Starts at the first node with no partition found. */
	bytes = path;
	position = 0U;
	nodes = 0U;
	partition_offset = 0U;
	partition_number = 0U;
	partition_start = 0U;
	partition_size = 0U;
	style = ZBL_UEFI_PARTITION_MBR;
	found_partition = 0;

	/* Refuses a missing path or view, and an empty path. */
	if (path == NULL || view == NULL || available_bytes == 0U)
		return ZBL_UEFI_DEVICE_PATH_INVALID_ARGUMENT;

	/* Clears the view and bounds the walk. */
	byte_zero(view, sizeof(*view));
	limit = available_bytes;
	if (limit > ZBL_UEFI_DEVICE_PATH_MAX_BYTES)
		limit = ZBL_UEFI_DEVICE_PATH_MAX_BYTES;

	/* Walks the nodes until the end of the path. */
	while (position < limit) {
		/* A node header that does not fit ends the walk early. */
		if (limit - position < DEVICE_PATH_HEADER_SIZE) {
			boundary = path_boundary_result(available_bytes);
			return boundary;
		}

		/* Reads the node's type, subtype and size. */
		type = bytes[position];
		subtype = bytes[position + 1U];
		node_size = read_le16(bytes + position + 2U);
		if (node_size < DEVICE_PATH_HEADER_SIZE)
			return ZBL_UEFI_DEVICE_PATH_MALFORMED;

		/* A node that runs past the bound ends the walk early. */
		if (node_size > limit - position) {
			boundary = path_boundary_result(available_bytes);
			return boundary;
		}

		/* The end node finishes the path. */
		if (type == DEVICE_PATH_TYPE_END) {
			/* Refuses an end node of another size. */
			if (node_size != DEVICE_PATH_HEADER_SIZE)
				return ZBL_UEFI_DEVICE_PATH_MALFORMED;

			/* Refuses a path of several instances. */
			if (subtype == DEVICE_PATH_SUBTYPE_END_INSTANCE)
				return ZBL_UEFI_DEVICE_PATH_UNSUPPORTED;

			/* Refuses an end node of an unknown subtype. */
			if (subtype != DEVICE_PATH_SUBTYPE_END_ENTIRE)
				return ZBL_UEFI_DEVICE_PATH_MALFORMED;

			/* Refuses a path without a partition. */
			if (!found_partition)
				return ZBL_UEFI_DEVICE_PATH_NO_PARTITION;

			/* Publishes the view of the whole path. */
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

		/* Refuses a path of more nodes than the bound. */
		if (nodes == ZBL_UEFI_DEVICE_PATH_MAX_NODES)
			return ZBL_UEFI_DEVICE_PATH_LIMIT;

		nodes++;

		/* The HD() node names the partition; other nodes must lie on the right side of it. */
		if (type == DEVICE_PATH_TYPE_MEDIA &&
		    subtype == DEVICE_PATH_SUBTYPE_HARD_DRIVE) {
			/* Refuses a second HD() node, one with no device before it, and one of another size. */
			if (found_partition ||
			    position == 0U ||
			    node_size != HARD_DRIVE_DEVICE_PATH_SIZE)
				return ZBL_UEFI_DEVICE_PATH_MALFORMED;

			/* Tells an MBR partition from a GPT one by the node's two type bytes. */
			mbr_type = bytes[position + HARD_DRIVE_MBR_TYPE_OFFSET];
			signature_type = bytes[position + HARD_DRIVE_SIGNATURE_TYPE_OFFSET];
			if (mbr_type == HARD_DRIVE_MBR_TYPE_PCAT &&
			    signature_type == HARD_DRIVE_SIGNATURE_MBR) {
				style = ZBL_UEFI_PARTITION_MBR;
			} else if (mbr_type == HARD_DRIVE_MBR_TYPE_GPT &&
				   signature_type == HARD_DRIVE_SIGNATURE_GUID) {
				style = ZBL_UEFI_PARTITION_GPT;
			} else {
				return ZBL_UEFI_DEVICE_PATH_UNSUPPORTED;
			}

			/* Reads the partition's number, start and size. */
			partition_number = read_le32(bytes + position + 4U);
			partition_start = read_le64(bytes + position + 8U);
			partition_size = read_le64(bytes + position + 16U);

			/* Refuses partition zero, an empty partition and one whose end does not fit. */
			if (partition_number == 0U ||
			    partition_size == 0U ||
			    partition_start > UINT64_MAX - partition_size)
				return ZBL_UEFI_DEVICE_PATH_MALFORMED;

			/* The disk's own path ends where the partition node starts. */
			partition_offset = position;
			found_partition = 1;
		} else if (!found_partition) {
			/* Before the partition only hardware, ACPI and messaging nodes may come. */
			if (type < DEVICE_PATH_TYPE_HARDWARE ||
			    type > DEVICE_PATH_TYPE_MESSAGING)
				return ZBL_UEFI_DEVICE_PATH_UNSUPPORTED;
		} else if (type != DEVICE_PATH_TYPE_MEDIA) {
			/* After the partition only media nodes may come. */
			return ZBL_UEFI_DEVICE_PATH_UNSUPPORTED;
		}

		/* Moves to the next node. */
		position += node_size;
	}

	/* A path without an end node within the bound is truncated or too long. */
	boundary = path_boundary_result(available_bytes);
	return boundary;
}

/* Reports whether two partition paths lie on the same disk, by their disks' own paths. */
int
zbl_uefi_partition_paths_same_disk(
	const struct zbl_uefi_partition_path *left,
	const struct zbl_uefi_partition_path *right)
{
	int equal;

	/* Refuses views that zbl_uefi_partition_path_parse() did not produce, or of different prefix sizes. */
	if (left == NULL ||
	    right == NULL ||
	    left->bytes == NULL ||
	    right->bytes == NULL ||
	    left->disk_prefix_size == 0U ||
	    left->disk_prefix_size != right->disk_prefix_size ||
	    left->partition_offset != left->disk_prefix_size ||
	    right->partition_offset != right->disk_prefix_size ||
	    left->disk_prefix_size > left->path_size ||
	    right->disk_prefix_size > right->path_size)
		return 0;

	/* Compares the two disks' paths. */
	equal = byte_equal(left->bytes, right->bytes, left->disk_prefix_size);
	if (!equal)
		return 0;

	/* Succeeded: the partitions are on the same disk. */
	return 1;
}

/* Names a device path result for a loader message. */
const char *
zbl_uefi_device_path_result_name(
	enum zbl_uefi_device_path_result result)
{
	/* Names each result; anything else is unknown. */
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

/*
 * Orders the volume handles to search: the loader's own first, then each
 * firmware handle once in the firmware's order.
 */
enum zbl_uefi_handle_order_result
zbl_uefi_volume_order_handles(
	void *loaded_handle,
	void *const *firmware_handles,
	size_t firmware_handle_count,
	void **ordered_handles,
	size_t ordered_capacity,
	size_t *ordered_count)
{
	void *handle;
	size_t used;
	size_t index;
	size_t previous;
	int duplicate;

	/* Clears the published count first. */
	used = 1U;
	if (ordered_count != NULL)
		*ordered_count = 0U;

	/* Refuses missing handles and buffers. */
	if (loaded_handle == NULL ||
	    ordered_handles == NULL ||
	    ordered_count == NULL ||
	    (firmware_handles == NULL && firmware_handle_count != 0U))
		return ZBL_UEFI_HANDLE_ORDER_INVALID_ARGUMENT;

	/* Refuses more firmware handles than the bound. */
	if (firmware_handle_count > ZBL_UEFI_VOLUME_MAX_HANDLES)
		return ZBL_UEFI_HANDLE_ORDER_LIMIT;

	/* Refuses an output with no room for the loader's own handle. */
	if (ordered_capacity == 0U)
		return ZBL_UEFI_HANDLE_ORDER_OUTPUT_TOO_SMALL;

	/* Refuses a firmware list with a null handle in it. */
	for (index = 0U; index < firmware_handle_count; index++) {
		if (firmware_handles[index] == NULL)
			return ZBL_UEFI_HANDLE_ORDER_INVALID_ARGUMENT;
	}

	/* The loader's own handle comes first. */
	ordered_handles[0] = loaded_handle;

	/* Appends each firmware handle not already in the order. */
	for (index = 0U; index < firmware_handle_count; index++) {
		/* Looks for the handle among those already ordered. */
		handle = firmware_handles[index];
		duplicate = 0;
		for (previous = 0U; previous < used; previous++) {
			if (ordered_handles[previous] == handle) {
				duplicate = 1;
				break;
			}
		}

		/* A handle seen before is not searched twice. */
		if (duplicate)
			continue;

		/* Refuses an order longer than the bound. */
		if (used == ZBL_UEFI_VOLUME_MAX_HANDLES)
			return ZBL_UEFI_HANDLE_ORDER_LIMIT;

		/* Refuses an order longer than the output. */
		if (used == ordered_capacity)
			return ZBL_UEFI_HANDLE_ORDER_OUTPUT_TOO_SMALL;

		/* Appends the handle. */
		ordered_handles[used] = handle;
		used++;
	}

	/* Publishes the count. */
	*ordered_count = used;

	/* Succeeded: the output holds the search order. */
	return ZBL_UEFI_HANDLE_ORDER_OK;
}

/*
 * Parses a FAT boot sector the way the kernel FAT driver will.
 *
 * FAT12, exFAT and volumes without a serial are told apart from malformed
 * ones, so the loader can say why a volume was passed over.
 */
enum zbl_uefi_fat_result
zbl_uefi_fat_bpb_parse(
	const void *boot_sector,
	size_t boot_sector_bytes,
	uint32_t media_block_size,
	uint64_t media_block_count,
	struct zbl_uefi_fat_info *info)
{
	const uint8_t *boot;
	uint16_t bytes_per_sector;
	uint8_t sectors_per_cluster;
	uint16_t reserved_sectors;
	uint8_t fat_count;
	uint16_t root_entries;
	uint16_t total16;
	uint32_t total32;
	uint32_t total_sectors;
	uint16_t fat16_sectors;
	uint16_t version;
	uint32_t fat_sectors;
	uint32_t root_cluster;
	uint64_t root_sectors;
	uint64_t overhead;
	uint64_t data_sectors;
	uint64_t clusters;
	uint64_t fat_bytes;
	uint32_t sector_scale;
	unsigned serial_offset;
	enum zbl_uefi_fat_type type;
	int supported;
	int exfat;
	int jump;
	int media;
	int capacity;

	/* Views the sector as bytes. */
	boot = boot_sector;

	/* Refuses a missing sector or result, and an empty medium. */
	if (boot_sector == NULL || info == NULL || media_block_count == 0U)
		return ZBL_UEFI_FAT_INVALID_ARGUMENT;

	/* Clears the result. */
	byte_zero(info, sizeof(*info));

	/* Refuses a medium whose block size the kernel FAT driver does not take. */
	supported = supported_fat_block_size(media_block_size);
	if (!supported || media_block_size > ZBL_UEFI_FAT_MAX_BLOCK_SIZE)
		return ZBL_UEFI_FAT_UNSUPPORTED_BLOCK_SIZE;

	/* Refuses a sector buffer too short for the boot signature. */
	if (boot_sector_bytes < media_block_size ||
	    boot_sector_bytes <= FAT_BOOT_SIGNATURE_OFFSET + 1U)
		return ZBL_UEFI_FAT_TRUNCATED;

	/* Tells an exFAT volume apart by its file system name. */
	exfat = byte_equal(boot + 3U, (const uint8_t *)"EXFAT   ", 8U);
	if (exfat)
		return ZBL_UEFI_FAT_EXFAT;

	/* A FAT boot sector starts with a jump instruction. */
	jump = 0;
	if (boot[0] == 0xebU && boot[2] == 0x90U)
		jump = 1;
	else if (boot[0] == 0xe9U)
		jump = 1;

	/* Refuses a sector without the jump or the boot signature. */
	if (!jump ||
	    boot[FAT_BOOT_SIGNATURE_OFFSET] != 0x55U ||
	    boot[FAT_BOOT_SIGNATURE_OFFSET + 1U] != 0xaaU)
		return ZBL_UEFI_FAT_NON_FAT;

	/* Reads the BPB's geometry. */
	bytes_per_sector = read_le16(boot + 11U);
	sectors_per_cluster = boot[13U];
	reserved_sectors = read_le16(boot + 14U);
	fat_count = boot[16U];
	root_entries = read_le16(boot + 17U);
	total16 = read_le16(boot + 19U);
	total32 = read_le32(boot + 32U);
	fat16_sectors = read_le16(boot + 22U);

	/*
	 * Takes BPB sectors of 512 through 4096 bytes that are whole media
	 * blocks, as the kernel FAT driver does: it never writes part of a
	 * disk block for a volume.
	 */
	sector_scale = 0U;
	if (bytes_per_sector == 512U)
		sector_scale = 1U;
	else if (bytes_per_sector == 1024U)
		sector_scale = 1U;
	else if (bytes_per_sector == 2048U)
		sector_scale = 1U;
	else if (bytes_per_sector == 4096U)
		sector_scale = 1U;
	if (sector_scale != 0U && bytes_per_sector >= media_block_size)
		sector_scale = bytes_per_sector / media_block_size;
	else
		sector_scale = 0U;

	/* Refuses an unsupported sector size and impossible cluster, reserved and FAT counts. */
	if (sector_scale == 0U ||
	    sectors_per_cluster == 0U ||
	    (sectors_per_cluster & (sectors_per_cluster - 1U)) != 0U ||
	    (uint64_t)bytes_per_sector * sectors_per_cluster > 65536U ||
	    reserved_sectors == 0U ||
	    fat_count == 0U ||
	    fat_count > 4U)
		return ZBL_UEFI_FAT_MALFORMED;

	/* Refuses a media byte FAT does not use. */
	media = fat_media_byte(boot[21U]);
	if (!media)
		return ZBL_UEFI_FAT_MALFORMED;

	/* Refuses a BPB that gives both sector counts or neither. */
	if ((total16 == 0U) == (total32 == 0U))
		return ZBL_UEFI_FAT_MALFORMED;

	/* The volume's size is whichever count it gives. */
	if (total16 != 0U)
		total_sectors = total16;
	else
		total_sectors = total32;

	/* A BPB sector may span several media blocks. */
	if ((uint64_t)total_sectors > media_block_count / sector_scale)
		return ZBL_UEFI_FAT_MALFORMED;

	/* The FAT's size is the 16-bit count, or the FAT32 count when that is zero. */
	if (fat16_sectors != 0U)
		fat_sectors = fat16_sectors;
	else
		fat_sectors = read_le32(boot + 36U);

	/* Refuses a volume without a FAT. */
	if (fat_sectors == 0U)
		return ZBL_UEFI_FAT_MALFORMED;

	/* Refuses a volume whose reserved area, FATs and root directory fill it. */
	root_sectors = ((uint64_t)root_entries * 32U + bytes_per_sector - 1U) / bytes_per_sector;
	overhead = (uint64_t)reserved_sectors + (uint64_t)fat_count * fat_sectors + root_sectors;
	if (overhead >= total_sectors)
		return ZBL_UEFI_FAT_MALFORMED;

	/* Refuses a volume without one whole cluster of data. */
	data_sectors = total_sectors - overhead;
	clusters = data_sectors / sectors_per_cluster;
	if (clusters == 0U)
		return ZBL_UEFI_FAT_MALFORMED;

	/* The bytes one FAT holds. */
	fat_bytes = (uint64_t)fat_sectors * bytes_per_sector;

	/* The cluster count decides the FAT type. */
	if (clusters < FAT12_CLUSTER_LIMIT) {
		/* Refuses a FAT12 volume without a fixed root directory. */
		if (fat16_sectors == 0U || root_entries == 0U)
			return ZBL_UEFI_FAT_MALFORMED;

		/* Refuses a FAT too small for the clusters. */
		capacity = fat_capacity_ok(fat_bytes, clusters, 12U);
		if (!capacity)
			return ZBL_UEFI_FAT_MALFORMED;

		/* Reports a well-formed FAT12 volume, which the kernel does not mount. */
		return ZBL_UEFI_FAT12;
	}

	/* FAT16 and FAT32 have their own checks and serial offsets. */
	if (clusters < FAT16_CLUSTER_LIMIT) {
		/* Refuses a FAT16 volume without a fixed root directory. */
		if (fat16_sectors == 0U || root_entries == 0U)
			return ZBL_UEFI_FAT_MALFORMED;

		/* Refuses a FAT too small for the clusters. */
		capacity = fat_capacity_ok(fat_bytes, clusters, 16U);
		if (!capacity)
			return ZBL_UEFI_FAT_MALFORMED;

		type = ZBL_UEFI_FAT16;
		serial_offset = FAT16_SERIAL_OFFSET;
	} else {
		/* Refuses a FAT32 volume with too many clusters or with FAT16 fields. */
		if (clusters > FAT32_CLUSTER_MAX ||
		    fat16_sectors != 0U ||
		    root_entries != 0U)
			return ZBL_UEFI_FAT_MALFORMED;

		/* Refuses a FAT32 version other than 0.0. */
		version = read_le16(boot + 42U);
		if (version != 0U)
			return ZBL_UEFI_FAT_MALFORMED;

		/* Refuses a FAT too small for the clusters. */
		capacity = fat_capacity_ok(fat_bytes, clusters, 32U);
		if (!capacity)
			return ZBL_UEFI_FAT_MALFORMED;

		/* Refuses a root directory cluster outside the volume. */
		root_cluster = read_le32(boot + 44U);
		if (root_cluster < 2U || (uint64_t)root_cluster >= clusters + 2U)
			return ZBL_UEFI_FAT_MALFORMED;

		type = ZBL_UEFI_FAT32;
		serial_offset = FAT32_SERIAL_OFFSET;
	}

	/* Refuses a volume without the extended boot signature that carries the serial. */
	if (boot[serial_offset - 1U] != FAT_EXTENDED_SIGNATURE)
		return ZBL_UEFI_FAT_NO_SERIAL;

	/* Publishes the volume's description. */
	info->type = type;
	info->bytes_per_sector = bytes_per_sector;
	info->sectors_per_cluster = sectors_per_cluster;
	info->total_sectors = total_sectors;
	info->fat_sectors = fat_sectors;
	info->cluster_count = (uint32_t)clusters;
	info->volume_serial = read_le32(boot + serial_offset);

	/* Succeeded: the volume is FAT16 or FAT32 with a serial. */
	return ZBL_UEFI_FAT_OK;
}

/* Formats the canonical FAT UUID as XXXX-XXXX. */
void
zbl_uefi_fat_uuid(
	uint32_t serial,
	char output[ZBL_UEFI_FAT_UUID_SIZE])
{
	/* The upper-case hexadecimal digits. */
	static const char digits[] = "0123456789ABCDEF";
	unsigned index;

	/* There is nowhere to write. */
	if (output == NULL)
		return;

	/* Writes the high half, most significant digit first. */
	for (index = 0U; index < 4U; index++)
		output[index] = digits[(serial >> (28U - index * 4U)) & 15U];

	/* Writes the dash and the low half, then terminates. */
	output[4] = '-';
	for (index = 0U; index < 4U; index++)
		output[5U + index] = digits[(serial >> (12U - index * 4U)) & 15U];
	output[9] = '\0';
}

/* Names a FAT result for a loader message. */
const char *
zbl_uefi_fat_result_name(
	enum zbl_uefi_fat_result result)
{
	/* Names each result; anything else is unknown. */
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

/* Starts a volume selection with no match. */
void
zbl_uefi_volume_selection_init(
	struct zbl_uefi_volume_selection *selection)
{
	/* Clears the selection when there is one. */
	if (selection != NULL)
		byte_zero(selection, sizeof(*selection));
}

/*
 * Records a volume that holds zedbsd.cfg.
 *
 * The first match keeps the caller's open resources; a later one only counts
 * and leaves its resources for the caller to close.
 */
enum zbl_uefi_volume_match_action
zbl_uefi_volume_selection_record_match(
	struct zbl_uefi_volume_selection *selection,
	void *handle,
	size_t order,
	const struct zbl_uefi_fat_info *fat,
	void *root,
	void *config)
{
	/* Refuses missing parts, a non-FAT16/32 volume, too many matches and an order that does not ascend. */
	if (selection == NULL ||
	    handle == NULL ||
	    fat == NULL ||
	    root == NULL ||
	    config == NULL ||
	    order >= ZBL_UEFI_VOLUME_MAX_HANDLES ||
	    (fat->type != ZBL_UEFI_FAT16 && fat->type != ZBL_UEFI_FAT32) ||
	    selection->match_count >= ZBL_UEFI_VOLUME_MAX_HANDLES ||
	    (selection->match_count != 0U &&
	     order <= selection->last_match_order))
		return ZBL_UEFI_VOLUME_MATCH_INVALID;

	/* Counts the match. */
	selection->match_count++;
	selection->last_match_order = order;

	/* A later match leaves its resources with the caller. */
	if (selection->has_selected)
		return ZBL_UEFI_VOLUME_MATCH_RELEASE;

	/* The first match is kept with its resources and its UUID. */
	selection->selected.handle = handle;
	selection->selected.root = root;
	selection->selected.config = config;
	selection->selected.order = order;
	selection->selected.fat = *fat;
	zbl_uefi_fat_uuid(fat->volume_serial, selection->selected.uuid);
	selection->has_selected = 1;

	/* Succeeded: the selection owns the first match's resources. */
	return ZBL_UEFI_VOLUME_MATCH_KEEP;
}

/* Reports whether no volume, one volume or several volumes matched. */
enum zbl_uefi_volume_selection_result
zbl_uefi_volume_selection_finish(
	const struct zbl_uefi_volume_selection *selection)
{
	/* Refuses a missing or inconsistent selection. */
	if (selection == NULL ||
	    (selection->has_selected && selection->match_count == 0U) ||
	    (!selection->has_selected && selection->match_count != 0U))
		return ZBL_UEFI_VOLUME_SELECTION_INVALID_ARGUMENT;

	/* No volume matched. */
	if (selection->match_count == 0U)
		return ZBL_UEFI_VOLUME_SELECTION_NOT_FOUND;

	/* Exactly one volume matched. */
	if (selection->match_count == 1U)
		return ZBL_UEFI_VOLUME_SELECTION_ONE;

	/* Reports that several volumes matched. */
	return ZBL_UEFI_VOLUME_SELECTION_MULTIPLE;
}

/* Hands the selected match and its resources back to the caller. */
int
zbl_uefi_volume_selection_take(
	struct zbl_uefi_volume_selection *selection,
	struct zbl_uefi_volume_match *match)
{
	/* Refuses a selection with nothing selected. */
	if (selection == NULL ||
	    match == NULL ||
	    !selection->has_selected ||
	    selection->match_count == 0U)
		return 0;

	/* Moves the match out and empties the selection. */
	*match = selection->selected;
	zbl_uefi_volume_selection_init(selection);

	/* Succeeded: the caller owns the match's resources. */
	return 1;
}

/* Copies the parsed hard-drive signature before firmware storage expires. */
int
zbl_uefi_partition_identity_copy(
	const struct zbl_uefi_partition_path *path,
	struct kern_boot_partition_identity *identity)
{
	size_t i;

	/* Refuses a view without a whole HD() node. */
	if (path == 0 ||
	    identity == 0 ||
	    path->bytes == 0 ||
	    path->partition_offset > path->path_size ||
	    path->path_size - path->partition_offset < HARD_DRIVE_DEVICE_PATH_SIZE)
		return 0;

	/* Copies the partition's place on the disk. */
	identity->scheme = (uint32_t)path->style;
	identity->index = path->partition_number;
	identity->first_lba = path->partition_start;
	identity->block_count = path->partition_size;

	/* Copies the partition's signature from the HD() node. */
	for (i = 0; i < sizeof(identity->signature); i++)
		identity->signature[i] = path->bytes[path->partition_offset + 24U + i];

	/* Succeeded: the identity outlives the firmware's path. */
	return 1;
}

/* Reads a little-endian 16-bit value. */
static uint16_t
read_le16(
	const uint8_t *bytes)
{
	/* Combines the low byte and the high byte. */
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

/* Reads a little-endian 32-bit value. */
static uint32_t
read_le32(
	const uint8_t *bytes)
{
	/* Combines the four bytes, the least significant first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Reads a little-endian 64-bit value. */
static uint64_t
read_le64(
	const uint8_t *bytes)
{
	uint64_t low;
	uint64_t high;

	/* Reads the two halves. */
	low = read_le32(bytes);
	high = read_le32(bytes + 4U);

	/* Reports the combined value. */
	return low | (high << 32);
}

/* Clears bytes without the C library. */
static void
byte_zero(
	void *pointer,
	size_t size)
{
	uint8_t *bytes;

	/* Clears the bytes one at a time. */
	bytes = pointer;
	while (size != 0U) {
		*bytes = 0U;
		bytes++;
		size--;
	}
}

/* Reports whether two byte ranges are equal, without the C library. */
static int
byte_equal(
	const uint8_t *left,
	const uint8_t *right,
	size_t size)
{
	/* Compares the bytes one at a time. */
	while (size != 0U) {
		if (*left != *right)
			return 0;

		left++;
		right++;
		size--;
	}

	/* Succeeded: the ranges are equal. */
	return 1;
}

/* Reports a path that ended without an end node as truncated, or as beyond the bound. */
static enum zbl_uefi_device_path_result
path_boundary_result(
	size_t available_bytes)
{
	/* A path longer than the bound may not be walked further. */
	if (available_bytes > ZBL_UEFI_DEVICE_PATH_MAX_BYTES)
		return ZBL_UEFI_DEVICE_PATH_LIMIT;

	/* Reports a path cut short. */
	return ZBL_UEFI_DEVICE_PATH_TRUNCATED;
}

/* Reports whether a medium's block size is one the kernel FAT driver takes. */
static int
supported_fat_block_size(
	uint32_t size)
{
	/*
	 * Keep discovery aligned with the kernel FAT driver's disk contract:
	 * blocks of a power of two from 512 bytes to a page.
	 */
	if (size == 512U)
		return 1;
	if (size == 1024U)
		return 1;
	if (size == 2048U)
		return 1;
	if (size == 4096U)
		return 1;

	/* Refuses any other block size. */
	return 0;
}

/* Reports whether a BPB media byte is one FAT uses. */
static int
fat_media_byte(
	uint8_t value)
{
	/* 0xf0 is removable media. */
	if (value == 0xf0U)
		return 1;

	/* 0xf8 to 0xff are the fixed and the older media bytes. */
	if (value >= 0xf8U)
		return 1;

	/* Refuses any other media byte. */
	return 0;
}

/* Reports whether one FAT of a size holds an entry for every cluster. */
static int
fat_capacity_ok(
	uint64_t fat_bytes,
	uint64_t clusters,
	unsigned bits)
{
	uint64_t entries;
	uint64_t required;

	/* The FAT has two reserved entries before the clusters'. */
	entries = clusters + 2U;

	/* FAT12 packs two entries in three bytes; the others use whole bytes. */
	if (bits == 12U) {
		/* Refuses a count whose size does not fit. */
		if (entries > (UINT64_MAX - 1U) / 3U)
			return 0;

		required = (entries * 3U + 1U) / 2U;
	} else {
		/* Refuses a count whose size does not fit. */
		if (entries > UINT64_MAX / (bits / 8U))
			return 0;

		required = entries * (bits / 8U);
	}

	/* Refuses a FAT smaller than its entries. */
	if (required > fat_bytes)
		return 0;

	/* Succeeded: the FAT holds every cluster. */
	return 1;
}
