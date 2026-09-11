/* WS013 p002 bounded UEFI volume-discovery regression fixture. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "bootloader/uefi/volume-discovery.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE 512U
#define PATH_CAPACITY 512U

static unsigned failures;

#define CHECK(expression) do { \
	if (!(expression)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

static void
put16(uint8_t *output, uint16_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
}

static void
put32(uint8_t *output, uint32_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
	output[2] = (uint8_t)(value >> 16);
	output[3] = (uint8_t)(value >> 24);
}

static void
put64(uint8_t *output, uint64_t value)
{
	put32(output, (uint32_t)value);
	put32(output + 4U, (uint32_t)(value >> 32));
}

static size_t
path_node(uint8_t *path, size_t position, uint8_t type, uint8_t subtype,
	uint16_t size)
{
	path[position] = type;
	path[position + 1U] = subtype;
	put16(path + position + 2U, size);
	return position + size;
}

static size_t
make_partition_path(uint8_t *path, size_t capacity, uint32_t disk_id,
	uint32_t partition, enum zbl_uefi_partition_style style,
	int append_file_path)
{
	size_t position = 0U;
	size_t hard_drive;

	CHECK(capacity >= 68U);
	memset(path, 0, capacity);
	position = path_node(path, position, 1U, 1U, 6U);
	path[4U] = 1U;
	path[5U] = 2U;
	position = path_node(path, position, 3U, 0x12U, 8U);
	put32(path + position - 4U, disk_id);
	hard_drive = position;
	position = path_node(path, position, 4U, 1U, 42U);
	put32(path + hard_drive + 4U, partition);
	put64(path + hard_drive + 8U, 2048U * partition);
	put64(path + hard_drive + 16U, 131072U);
	for (unsigned index = 0U; index < 16U; index++)
		path[hard_drive + 24U + index] =
		    (uint8_t)(disk_id + partition + index);
	if (style == ZBL_UEFI_PARTITION_GPT) {
		path[hard_drive + 40U] = 2U;
		path[hard_drive + 41U] = 2U;
	} else {
		path[hard_drive + 40U] = 1U;
		path[hard_drive + 41U] = 1U;
	}
	if (append_file_path) {
		position = path_node(path, position, 4U, 4U, 8U);
		path[position - 4U] = 'x';
	}
	position = path_node(path, position, 0x7fU, 0xffU, 4U);
	return position;
}

static size_t
make_sized_partition_path(uint8_t *path, size_t physical_node_size)
{
	size_t position;

	memset(path, 0, physical_node_size + 46U);
	position = path_node(path, 0U, 3U, 0x12U,
	    (uint16_t)physical_node_size);
	position = path_node(path, position, 4U, 1U, 42U);
	put32(path + physical_node_size + 4U, 1U);
	put64(path + physical_node_size + 8U, 2048U);
	put64(path + physical_node_size + 16U, 4096U);
	path[physical_node_size + 40U] = 2U;
	path[physical_node_size + 41U] = 2U;
	return path_node(path, position, 0x7fU, 0xffU, 4U);
}

static void
test_device_paths(void)
{
	uint8_t gpt_a[PATH_CAPACITY], gpt_b[PATH_CAPACITY];
	uint8_t gpt_other[PATH_CAPACITY], gpt_file[PATH_CAPACITY];
	uint8_t mbr_a[PATH_CAPACITY], mbr_b[PATH_CAPACITY];
	struct zbl_uefi_partition_path a, b, other, file, ma, mb;
	size_t gpt_a_size, gpt_b_size, other_size, file_size;
	size_t mbr_a_size, mbr_b_size;
	enum zbl_uefi_device_path_result result;

	gpt_a_size = make_partition_path(gpt_a, sizeof(gpt_a), 7U, 1U,
	    ZBL_UEFI_PARTITION_GPT, 0);
	gpt_b_size = make_partition_path(gpt_b, sizeof(gpt_b), 7U, 3U,
	    ZBL_UEFI_PARTITION_GPT, 0);
	other_size = make_partition_path(gpt_other, sizeof(gpt_other), 8U, 1U,
	    ZBL_UEFI_PARTITION_GPT, 0);
	file_size = make_partition_path(gpt_file, sizeof(gpt_file), 7U, 2U,
	    ZBL_UEFI_PARTITION_GPT, 1);
	mbr_a_size = make_partition_path(mbr_a, sizeof(mbr_a), 9U, 1U,
	    ZBL_UEFI_PARTITION_MBR, 0);
	mbr_b_size = make_partition_path(mbr_b, sizeof(mbr_b), 9U, 4U,
	    ZBL_UEFI_PARTITION_MBR, 0);

	CHECK(zbl_uefi_partition_path_parse(gpt_a, gpt_a_size, &a) ==
	    ZBL_UEFI_DEVICE_PATH_OK);
	CHECK(zbl_uefi_partition_path_parse(gpt_b, gpt_b_size, &b) ==
	    ZBL_UEFI_DEVICE_PATH_OK);
	CHECK(zbl_uefi_partition_path_parse(gpt_other, other_size, &other) ==
	    ZBL_UEFI_DEVICE_PATH_OK);
	CHECK(zbl_uefi_partition_path_parse(gpt_file, file_size, &file) ==
	    ZBL_UEFI_DEVICE_PATH_OK);
	CHECK(a.style == ZBL_UEFI_PARTITION_GPT);
	CHECK(a.partition_number == 1U);
	CHECK(b.partition_number == 3U);
	CHECK(b.partition_start == 6144U);
	CHECK(b.partition_size == 131072U);
	CHECK(a.disk_prefix_size == 14U);
	CHECK(a.partition_offset == a.disk_prefix_size);
	CHECK(zbl_uefi_partition_paths_same_disk(&a, &b));
	CHECK(zbl_uefi_partition_paths_same_disk(&a, &file));
	CHECK(!zbl_uefi_partition_paths_same_disk(&a, &other));

	CHECK(zbl_uefi_partition_path_parse(mbr_a, mbr_a_size, &ma) ==
	    ZBL_UEFI_DEVICE_PATH_OK);
	CHECK(zbl_uefi_partition_path_parse(mbr_b, mbr_b_size, &mb) ==
	    ZBL_UEFI_DEVICE_PATH_OK);
	CHECK(ma.style == ZBL_UEFI_PARTITION_MBR);
	CHECK(zbl_uefi_partition_paths_same_disk(&ma, &mb));

	result = zbl_uefi_partition_path_parse(gpt_a, gpt_a_size - 1U, &a);
	CHECK(result == ZBL_UEFI_DEVICE_PATH_TRUNCATED);
	gpt_a[16U] = 3U;
	gpt_a[17U] = 0U;
	CHECK(zbl_uefi_partition_path_parse(gpt_a, gpt_a_size, &a) ==
	    ZBL_UEFI_DEVICE_PATH_MALFORMED);
	gpt_a_size = make_partition_path(gpt_a, sizeof(gpt_a), 7U, 1U,
	    ZBL_UEFI_PARTITION_GPT, 0);
	gpt_a[14U + 41U] = 1U;
	CHECK(zbl_uefi_partition_path_parse(gpt_a, gpt_a_size, &a) ==
	    ZBL_UEFI_DEVICE_PATH_UNSUPPORTED);
	gpt_a_size = make_partition_path(gpt_a, sizeof(gpt_a), 7U, 1U,
	    ZBL_UEFI_PARTITION_GPT, 0);
	gpt_a[gpt_a_size - 3U] = 1U;
	CHECK(zbl_uefi_partition_path_parse(gpt_a, gpt_a_size, &a) ==
	    ZBL_UEFI_DEVICE_PATH_UNSUPPORTED);
	CHECK(strcmp(zbl_uefi_device_path_result_name(
	    ZBL_UEFI_DEVICE_PATH_TRUNCATED), "truncated") == 0);
	CHECK(strcmp(zbl_uefi_device_path_result_name(
	    (enum zbl_uefi_device_path_result)99), "unknown") == 0);
}

static void
test_device_path_bounds(void)
{
	uint8_t path[ZBL_UEFI_DEVICE_PATH_MAX_BYTES + 1U];
	struct zbl_uefi_partition_path view;
	size_t size;

	size = make_sized_partition_path(path,
	    ZBL_UEFI_DEVICE_PATH_MAX_BYTES - 46U);
	CHECK(size == ZBL_UEFI_DEVICE_PATH_MAX_BYTES);
	CHECK(zbl_uefi_partition_path_parse(path, size, &view) ==
	    ZBL_UEFI_DEVICE_PATH_OK);
	size = make_sized_partition_path(path,
	    ZBL_UEFI_DEVICE_PATH_MAX_BYTES - 45U);
	CHECK(size == ZBL_UEFI_DEVICE_PATH_MAX_BYTES + 1U);
	CHECK(zbl_uefi_partition_path_parse(path, size, &view) ==
	    ZBL_UEFI_DEVICE_PATH_LIMIT);

	memset(path, 0, sizeof(path));
	size_t position = 0U;
	for (unsigned index = 0U;
	     index < ZBL_UEFI_DEVICE_PATH_MAX_NODES; index++)
		position = path_node(path, position, 1U, 1U, 4U);
	position = path_node(path, position, 4U, 1U, 42U);
	put32(path + position - 38U, 1U);
	put64(path + position - 34U, 1U);
	put64(path + position - 26U, 2U);
	path[position - 2U] = 2U;
	path[position - 1U] = 2U;
	position = path_node(path, position, 0x7fU, 0xffU, 4U);
	CHECK(zbl_uefi_partition_path_parse(path, position, &view) ==
	    ZBL_UEFI_DEVICE_PATH_LIMIT);

	memset(path, 0, sizeof(path));
	position = path_node(path, 0U, 1U, 1U, 4U);
	position = path_node(path, position, 0x7fU, 0xffU, 4U);
	CHECK(zbl_uefi_partition_path_parse(path, position, &view) ==
	    ZBL_UEFI_DEVICE_PATH_NO_PARTITION);
	CHECK(zbl_uefi_partition_path_parse(NULL, position, &view) ==
	    ZBL_UEFI_DEVICE_PATH_INVALID_ARGUMENT);
}

static void
test_handle_order(void)
{
	int tokens[ZBL_UEFI_VOLUME_MAX_HANDLES + 1U];
	void *firmware[ZBL_UEFI_VOLUME_MAX_HANDLES + 1U];
	void *ordered[ZBL_UEFI_VOLUME_MAX_HANDLES];
	size_t count = 99U;

	void *short_firmware[] = {
	    &tokens[1], &tokens[0], &tokens[2], &tokens[1], &tokens[3]
	};
	CHECK(zbl_uefi_volume_order_handles(&tokens[0], short_firmware,
	    sizeof(short_firmware) / sizeof(short_firmware[0]), ordered,
	    ZBL_UEFI_VOLUME_MAX_HANDLES, &count) ==
	    ZBL_UEFI_HANDLE_ORDER_OK);
	CHECK(count == 4U);
	CHECK(ordered[0] == &tokens[0]);
	CHECK(ordered[1] == &tokens[1]);
	CHECK(ordered[2] == &tokens[2]);
	CHECK(ordered[3] == &tokens[3]);

	CHECK(zbl_uefi_volume_order_handles(&tokens[0], NULL, 0U, ordered,
	    ZBL_UEFI_VOLUME_MAX_HANDLES, &count) ==
	    ZBL_UEFI_HANDLE_ORDER_OK);
	CHECK(count == 1U && ordered[0] == &tokens[0]);
	CHECK(zbl_uefi_volume_order_handles(&tokens[0], short_firmware,
	    5U, ordered, 3U, &count) ==
	    ZBL_UEFI_HANDLE_ORDER_OUTPUT_TOO_SMALL);
	CHECK(count == 0U);
	short_firmware[2] = NULL;
	CHECK(zbl_uefi_volume_order_handles(&tokens[0], short_firmware,
	    5U, ordered, ZBL_UEFI_VOLUME_MAX_HANDLES, &count) ==
	    ZBL_UEFI_HANDLE_ORDER_INVALID_ARGUMENT);
	CHECK(count == 0U);

	firmware[0] = &tokens[0];
	for (size_t index = 1U; index < ZBL_UEFI_VOLUME_MAX_HANDLES; index++)
		firmware[index] = &tokens[index];
	CHECK(zbl_uefi_volume_order_handles(&tokens[0], firmware,
	    ZBL_UEFI_VOLUME_MAX_HANDLES, ordered,
	    ZBL_UEFI_VOLUME_MAX_HANDLES, &count) ==
	    ZBL_UEFI_HANDLE_ORDER_OK);
	CHECK(count == ZBL_UEFI_VOLUME_MAX_HANDLES);
	for (size_t index = 0U; index < ZBL_UEFI_VOLUME_MAX_HANDLES; index++)
		firmware[index] = &tokens[index + 1U];
	CHECK(zbl_uefi_volume_order_handles(&tokens[0], firmware,
	    ZBL_UEFI_VOLUME_MAX_HANDLES, ordered,
	    ZBL_UEFI_VOLUME_MAX_HANDLES, &count) ==
	    ZBL_UEFI_HANDLE_ORDER_LIMIT);
	firmware[ZBL_UEFI_VOLUME_MAX_HANDLES] = &tokens[0];
	CHECK(zbl_uefi_volume_order_handles(&tokens[0], firmware,
	    ZBL_UEFI_VOLUME_MAX_HANDLES + 1U, ordered,
	    ZBL_UEFI_VOLUME_MAX_HANDLES, &count) ==
	    ZBL_UEFI_HANDLE_ORDER_LIMIT);
}

static void
fat_common(uint8_t *boot, uint16_t bytes_per_sector,
	uint8_t sectors_per_cluster, uint16_t reserved, uint8_t fats,
	uint16_t root_entries, uint32_t total_sectors)
{
	memset(boot, 0, ZBL_UEFI_FAT_MAX_BLOCK_SIZE);
	boot[0] = 0xebU;
	boot[1] = 0x3cU;
	boot[2] = 0x90U;
	memcpy(boot + 3U, "MSDOS5.0", 8U);
	put16(boot + 11U, bytes_per_sector);
	boot[13U] = sectors_per_cluster;
	put16(boot + 14U, reserved);
	boot[16U] = fats;
	put16(boot + 17U, root_entries);
	if (total_sectors <= UINT16_MAX)
		put16(boot + 19U, (uint16_t)total_sectors);
	else
		put32(boot + 32U, total_sectors);
	boot[21U] = 0xf8U;
	boot[510U] = 0x55U;
	boot[511U] = 0xaaU;
}

static void
make_fat12(uint8_t *boot, uint32_t serial)
{
	fat_common(boot, SECTOR_SIZE, 1U, 1U, 2U, 224U, 2880U);
	put16(boot + 22U, 9U);
	boot[38U] = 0x29U;
	put32(boot + 39U, serial);
}

static void
make_fat16(uint8_t *boot, uint16_t bytes_per_sector, uint32_t serial)
{
	uint32_t total = bytes_per_sector == 512U ? 32768U : 8192U;
	uint16_t fat_sectors = bytes_per_sector == 512U ? 128U : 20U;

	fat_common(boot, bytes_per_sector, 1U, 1U, 2U, 512U, total);
	put16(boot + 22U, fat_sectors);
	boot[38U] = 0x29U;
	put32(boot + 39U, serial);
	memcpy(boot + 54U, "FAT16   ", 8U);
}

static void
make_fat32(uint8_t *boot, uint32_t serial)
{
	fat_common(boot, SECTOR_SIZE, 1U, 32U, 2U, 0U, 131072U);
	put32(boot + 36U, 1009U);
	put16(boot + 42U, 0U);
	put32(boot + 44U, 2U);
	put16(boot + 48U, 1U);
	put16(boot + 50U, 6U);
	boot[66U] = 0x29U;
	put32(boot + 67U, serial);
	memcpy(boot + 82U, "FAT32   ", 8U);
}

static void
test_fat_bpb(void)
{
	uint8_t boot[ZBL_UEFI_FAT_MAX_BLOCK_SIZE];
	struct zbl_uefi_fat_info info;
	char uuid[ZBL_UEFI_FAT_UUID_SIZE];

	make_fat16(boot, SECTOR_SIZE, 0x6740911dU);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE,
	    32768U, &info) == ZBL_UEFI_FAT_OK);
	CHECK(info.type == ZBL_UEFI_FAT16);
	CHECK(info.cluster_count == 32479U);
	CHECK(info.volume_serial == 0x6740911dU);
	zbl_uefi_fat_uuid(info.volume_serial, uuid);
	CHECK(strcmp(uuid, "6740-911D") == 0);

	make_fat32(boot, 0xa1b2c3d4U);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE,
	    131072U, &info) == ZBL_UEFI_FAT_OK);
	CHECK(info.type == ZBL_UEFI_FAT32);
	CHECK(info.cluster_count == 129022U);
	CHECK(info.volume_serial == 0xa1b2c3d4U);
	zbl_uefi_fat_uuid(info.volume_serial, uuid);
	CHECK(strcmp(uuid, "A1B2-C3D4") == 0);

	make_fat16(boot, 1024U, 1U);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 16384U,
	    &info) == ZBL_UEFI_FAT_OK);
	CHECK(info.type == ZBL_UEFI_FAT16);
	CHECK(info.bytes_per_sector == 1024U);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 16383U,
	    &info) == ZBL_UEFI_FAT_MALFORMED);
	CHECK(zbl_uefi_fat_bpb_parse(boot, 1024U, 1024U, 8192U,
	    &info) == ZBL_UEFI_FAT_UNSUPPORTED_BLOCK_SIZE);
	make_fat16(boot, 2048U, 1U);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 32768U,
	    &info) == ZBL_UEFI_FAT_MALFORMED);

	make_fat12(boot, 1U);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 2880U,
	    &info) == ZBL_UEFI_FAT12);
	memcpy(boot + 3U, "EXFAT   ", 8U);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 2880U,
	    &info) == ZBL_UEFI_FAT_EXFAT);

	memset(boot, 0, sizeof(boot));
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 2880U,
	    &info) == ZBL_UEFI_FAT_NON_FAT);
	make_fat16(boot, SECTOR_SIZE, 1U);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE - 1U, SECTOR_SIZE,
	    32768U, &info) == ZBL_UEFI_FAT_TRUNCATED);
	CHECK(zbl_uefi_fat_bpb_parse(boot, sizeof(boot), 8192U, 32768U,
	    &info) == ZBL_UEFI_FAT_UNSUPPORTED_BLOCK_SIZE);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 0U,
	    &info) == ZBL_UEFI_FAT_INVALID_ARGUMENT);

	make_fat16(boot, SECTOR_SIZE, 1U);
	boot[13U] = 3U;
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 32768U,
	    &info) == ZBL_UEFI_FAT_MALFORMED);
	make_fat16(boot, SECTOR_SIZE, 1U);
	put32(boot + 32U, 32768U);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 32768U,
	    &info) == ZBL_UEFI_FAT_MALFORMED);
	make_fat16(boot, SECTOR_SIZE, 1U);
	boot[38U] = 0U;
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 32768U,
	    &info) == ZBL_UEFI_FAT_NO_SERIAL);
	make_fat32(boot, 1U);
	put32(boot + 44U, 200000U);
	CHECK(zbl_uefi_fat_bpb_parse(boot, SECTOR_SIZE, SECTOR_SIZE, 131072U,
	    &info) == ZBL_UEFI_FAT_MALFORMED);
	CHECK(strcmp(zbl_uefi_fat_result_name(ZBL_UEFI_FAT12), "fat12") == 0);
}

static void
test_selection(void)
{
	struct zbl_uefi_volume_selection selection;
	struct zbl_uefi_volume_match selected;
	struct zbl_uefi_fat_info first = {
	    .type = ZBL_UEFI_FAT16,
	    .volume_serial = 0x6740911dU,
	};
	struct zbl_uefi_fat_info second = {
	    .type = ZBL_UEFI_FAT32,
	    .volume_serial = 0xa1b2c3d4U,
	};
	int handles[2], roots[2], configs[2];

	zbl_uefi_volume_selection_init(&selection);
	CHECK(zbl_uefi_volume_selection_finish(&selection) ==
	    ZBL_UEFI_VOLUME_SELECTION_NOT_FOUND);
	CHECK(zbl_uefi_volume_selection_record_match(&selection, &handles[0],
	    0U, &first, &roots[0], &configs[0]) ==
	    ZBL_UEFI_VOLUME_MATCH_KEEP);
	CHECK(zbl_uefi_volume_selection_finish(&selection) ==
	    ZBL_UEFI_VOLUME_SELECTION_ONE);
	CHECK(selection.selected.handle == &handles[0]);
	CHECK(selection.selected.root == &roots[0]);
	CHECK(strcmp(selection.selected.uuid, "6740-911D") == 0);
	CHECK(zbl_uefi_volume_selection_record_match(&selection, &handles[1],
	    3U, &second, &roots[1], &configs[1]) ==
	    ZBL_UEFI_VOLUME_MATCH_RELEASE);
	CHECK(zbl_uefi_volume_selection_finish(&selection) ==
	    ZBL_UEFI_VOLUME_SELECTION_MULTIPLE);
	CHECK(selection.match_count == 2U);
	CHECK(selection.selected.handle == &handles[0]);
	CHECK(strcmp(selection.selected.uuid, "6740-911D") == 0);
	CHECK(zbl_uefi_volume_selection_record_match(&selection, &handles[1],
	    2U, &second, &roots[1], &configs[1]) ==
	    ZBL_UEFI_VOLUME_MATCH_INVALID);
	CHECK(selection.match_count == 2U);
	CHECK(zbl_uefi_volume_selection_take(&selection, &selected));
	CHECK(selected.handle == &handles[0]);
	CHECK(selected.root == &roots[0]);
	CHECK(selected.config == &configs[0]);
	CHECK(!zbl_uefi_volume_selection_take(&selection, &selected));
	CHECK(zbl_uefi_volume_selection_finish(&selection) ==
	    ZBL_UEFI_VOLUME_SELECTION_NOT_FOUND);
}

enum marker_state {
	MARKER_MISSING,
	MARKER_UNREADABLE,
	MARKER_READABLE
};

struct fake_resource {
	unsigned opens;
	unsigned closes;
};

struct fake_volume {
	void *handle;
	uint8_t path[PATH_CAPACITY];
	size_t path_size;
	uint8_t boot[ZBL_UEFI_FAT_MAX_BLOCK_SIZE];
	uint64_t blocks;
	int media_present;
	int logical_partition;
	int bpb_readable;
	int media_changed;
	enum marker_state marker;
	struct fake_resource root;
	struct fake_resource config;
};

struct fake_scan {
	unsigned pool_allocations;
	unsigned pool_frees;
	unsigned writes;
	struct zbl_uefi_volume_selection selection;
};

static void
resource_open(struct fake_resource *resource)
{
	resource->opens++;
}

static void
resource_close(struct fake_resource *resource)
{
	CHECK(resource->opens > resource->closes);
	resource->closes++;
}

static struct fake_volume *
find_volume(struct fake_volume *volumes, size_t count, void *handle)
{
	for (size_t index = 0U; index < count; index++)
		if (volumes[index].handle == handle)
			return &volumes[index];
	return NULL;
}

static enum zbl_uefi_volume_selection_result
fake_discover(struct fake_scan *scan, struct fake_volume *volumes,
	size_t volume_count, void *loaded_handle, void *const *firmware,
	size_t firmware_count)
{
	void *ordered[ZBL_UEFI_VOLUME_MAX_HANDLES];
	size_t ordered_count;
	struct zbl_uefi_partition_path loaded_path;
	enum zbl_uefi_handle_order_result order_result;

	memset(scan, 0, sizeof(*scan));
	zbl_uefi_volume_selection_init(&scan->selection);
	scan->pool_allocations++;
	order_result = zbl_uefi_volume_order_handles(loaded_handle, firmware,
	    firmware_count, ordered, ZBL_UEFI_VOLUME_MAX_HANDLES,
	    &ordered_count);
	if (order_result != ZBL_UEFI_HANDLE_ORDER_OK)
		goto done;
	struct fake_volume *loaded = find_volume(volumes, volume_count,
	    loaded_handle);
	if (loaded == NULL || zbl_uefi_partition_path_parse(loaded->path,
	    loaded->path_size, &loaded_path) != ZBL_UEFI_DEVICE_PATH_OK)
		goto done;
	for (size_t order = 0U; order < ordered_count; order++) {
		struct fake_volume *volume = find_volume(volumes, volume_count,
		    ordered[order]);
		struct zbl_uefi_partition_path candidate_path;
		struct zbl_uefi_fat_info fat;
		enum zbl_uefi_volume_match_action action;

		if (volume == NULL || !volume->media_present ||
		    !volume->logical_partition || !volume->bpb_readable ||
		    volume->media_changed)
			continue;
		if (zbl_uefi_partition_path_parse(volume->path,
		    volume->path_size, &candidate_path) !=
		    ZBL_UEFI_DEVICE_PATH_OK ||
		    !zbl_uefi_partition_paths_same_disk(&loaded_path,
		    &candidate_path))
			continue;
		if (zbl_uefi_fat_bpb_parse(volume->boot, SECTOR_SIZE,
		    SECTOR_SIZE, volume->blocks, &fat) != ZBL_UEFI_FAT_OK)
			continue;
		resource_open(&volume->root);
		if (volume->marker == MARKER_MISSING) {
			resource_close(&volume->root);
			continue;
		}
		resource_open(&volume->config);
		if (volume->marker == MARKER_UNREADABLE) {
			resource_close(&volume->config);
			resource_close(&volume->root);
			continue;
		}
		action = zbl_uefi_volume_selection_record_match(
		    &scan->selection, volume->handle, order, &fat,
		    &volume->root, &volume->config);
		if (action != ZBL_UEFI_VOLUME_MATCH_KEEP) {
			resource_close(&volume->config);
			resource_close(&volume->root);
		}
		if (action == ZBL_UEFI_VOLUME_MATCH_INVALID)
			goto done;
	}

done:
	scan->pool_frees++;
	return zbl_uefi_volume_selection_finish(&scan->selection);
}

static void
init_fake_volume(struct fake_volume *volume, void *handle, uint32_t disk,
	uint32_t partition, uint32_t serial, enum marker_state marker)
{
	memset(volume, 0, sizeof(*volume));
	volume->handle = handle;
	volume->path_size = make_partition_path(volume->path,
	    sizeof(volume->path), disk, partition, ZBL_UEFI_PARTITION_GPT, 0);
	make_fat16(volume->boot, SECTOR_SIZE, serial);
	volume->blocks = 32768U;
	volume->media_present = 1;
	volume->logical_partition = 1;
	volume->bpb_readable = 1;
	volume->marker = marker;
}

static void
close_selected(struct fake_scan *scan, struct fake_volume *volumes,
	size_t volume_count)
{
	struct zbl_uefi_volume_match selected;
	struct fake_volume *volume;

	if (!zbl_uefi_volume_selection_take(&scan->selection, &selected))
		return;
	volume = find_volume(volumes, volume_count, selected.handle);
	CHECK(volume != NULL);
	CHECK(selected.root == &volume->root);
	CHECK(selected.config == &volume->config);
	resource_close(&volume->config);
	resource_close(&volume->root);
}

static void
check_resources_closed(const struct fake_volume *volumes, size_t count)
{
	for (size_t index = 0U; index < count; index++) {
		CHECK(volumes[index].root.opens == volumes[index].root.closes);
		CHECK(volumes[index].config.opens ==
		    volumes[index].config.closes);
	}
}

static void
test_scan_contract(void)
{
	int handles[7];
	struct fake_volume volumes[7];
	struct fake_scan scan;
	void *firmware[] = {
	    &handles[2], &handles[0], &handles[1], &handles[2], &handles[3],
	    &handles[4], &handles[5], &handles[6]
	};
	enum zbl_uefi_volume_selection_result result;

	init_fake_volume(&volumes[0], &handles[0], 10U, 1U, 0x11112222U,
	    MARKER_READABLE);
	init_fake_volume(&volumes[1], &handles[1], 10U, 2U, 0x33334444U,
	    MARKER_READABLE);
	init_fake_volume(&volumes[2], &handles[2], 10U, 3U, 0x55556666U,
	    MARKER_READABLE);
	init_fake_volume(&volumes[3], &handles[3], 11U, 1U, 0x77778888U,
	    MARKER_READABLE);
	init_fake_volume(&volumes[4], &handles[4], 10U, 4U, 1U,
	    MARKER_READABLE);
	volumes[4].media_present = 0;
	init_fake_volume(&volumes[5], &handles[5], 10U, 5U, 1U,
	    MARKER_UNREADABLE);
	init_fake_volume(&volumes[6], &handles[6], 10U, 6U, 1U,
	    MARKER_READABLE);
	volumes[6].media_changed = 1;

	result = fake_discover(&scan, volumes,
	    sizeof(volumes) / sizeof(volumes[0]), &handles[0], firmware,
	    sizeof(firmware) / sizeof(firmware[0]));
	CHECK(result == ZBL_UEFI_VOLUME_SELECTION_MULTIPLE);
	CHECK(scan.selection.match_count == 3U);
	CHECK(scan.selection.selected.handle == &handles[0]);
	CHECK(scan.selection.selected.order == 0U);
	CHECK(strcmp(scan.selection.selected.uuid, "1111-2222") == 0);
	CHECK(scan.pool_allocations == scan.pool_frees);
	CHECK(scan.writes == 0U);
	close_selected(&scan, volumes,
	    sizeof(volumes) / sizeof(volumes[0]));
	check_resources_closed(volumes,
	    sizeof(volumes) / sizeof(volumes[0]));

	for (size_t index = 0U; index < sizeof(volumes) / sizeof(volumes[0]);
	     index++) {
		volumes[index].root.opens = 0U;
		volumes[index].root.closes = 0U;
		volumes[index].config.opens = 0U;
		volumes[index].config.closes = 0U;
		volumes[index].marker = MARKER_MISSING;
	}
	volumes[1].marker = MARKER_READABLE;
	volumes[2].marker = MARKER_READABLE;
	result = fake_discover(&scan, volumes,
	    sizeof(volumes) / sizeof(volumes[0]), &handles[0], firmware,
	    sizeof(firmware) / sizeof(firmware[0]));
	CHECK(result == ZBL_UEFI_VOLUME_SELECTION_MULTIPLE);
	CHECK(scan.selection.selected.handle == &handles[2]);
	CHECK(scan.selection.selected.order == 1U);
	CHECK(strcmp(scan.selection.selected.uuid, "5555-6666") == 0);
	close_selected(&scan, volumes,
	    sizeof(volumes) / sizeof(volumes[0]));
	check_resources_closed(volumes,
	    sizeof(volumes) / sizeof(volumes[0]));

	for (size_t index = 0U; index < sizeof(volumes) / sizeof(volumes[0]);
	     index++) {
		volumes[index].root.opens = 0U;
		volumes[index].root.closes = 0U;
		volumes[index].config.opens = 0U;
		volumes[index].config.closes = 0U;
		volumes[index].marker = MARKER_MISSING;
	}
	volumes[3].marker = MARKER_READABLE;
	make_fat12(volumes[1].boot, 1U);
	volumes[2].marker = MARKER_UNREADABLE;
	result = fake_discover(&scan, volumes,
	    sizeof(volumes) / sizeof(volumes[0]), &handles[0], firmware,
	    sizeof(firmware) / sizeof(firmware[0]));
	CHECK(result == ZBL_UEFI_VOLUME_SELECTION_NOT_FOUND);
	CHECK(scan.pool_allocations == scan.pool_frees);
	CHECK(scan.writes == 0U);
	check_resources_closed(volumes,
	    sizeof(volumes) / sizeof(volumes[0]));
}

int
main(void)
{
	test_device_paths();
	test_device_path_bounds();
	test_handle_order();
	test_fat_bpb();
	test_selection();
	test_scan_contract();
	if (failures != 0U) {
		printf("WS013 p002 UEFI volume discovery: %u failure(s)\n",
		    failures);
		return 1;
	}
	puts("WS013 p002 UEFI volume discovery: PASS");
	return 0;
}
