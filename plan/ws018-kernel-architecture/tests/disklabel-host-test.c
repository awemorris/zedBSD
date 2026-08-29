/*
 * KA-T010 disk-label parser regression fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#include <drivers/disklabel.h>
#include <kern/disk.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SECTOR_SIZE 512U
#define DISK_SECTORS 1024U
#define ENTRY_CAPACITY 16U

struct memory_medium {
	uint8_t bytes[DISK_SECTORS * SECTOR_SIZE];
	struct disk_geometry geometry;
	int read_error;
	int geometry_error;
};

static struct memory_medium medium;
static struct disk disk;
static unsigned failures;

#define CHECK(expression) do { \
	if (!(expression)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

static void
put_le32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void
put_be16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static void
put_be24(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 16);
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)value;
}

static void
put_be32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

int
disk_read(struct disk *target, uint64_t block, uint32_t count, void *data)
{
	struct memory_medium *source;

	if (target == NULL || data == NULL || target->d_data == NULL)
		return EINVAL;
	source = target->d_data;
	if (source->read_error != 0)
		return source->read_error;
	if (target->d_block_size != SECTOR_SIZE || count == 0 ||
	    block >= target->d_block_count ||
	    count > target->d_block_count - block)
		return EOVERFLOW;
	memcpy(data, source->bytes + (size_t)block * SECTOR_SIZE,
	    (size_t)count * SECTOR_SIZE);
	return 0;
}

int
disk_ioctl(struct disk *target, unsigned long request, void *argument)
{
	struct memory_medium *source;

	if (target == NULL || argument == NULL || target->d_data == NULL ||
	    request != DISK_IOCTL_GET_GEOMETRY)
		return EINVAL;
	source = target->d_data;
	if (source->geometry_error != 0)
		return source->geometry_error;
	*(struct disk_geometry *)argument = source->geometry;
	return 0;
}

static void
reset_medium(void)
{
	memset(&medium, 0, sizeof(medium));
	memset(&disk, 0, sizeof(disk));
	disk.d_block_size = SECTOR_SIZE;
	disk.d_block_count = DISK_SECTORS;
	disk.d_data = &medium;
	medium.geometry.cylinders = 8U;
	medium.geometry.heads = 8U;
	medium.geometry.sectors_per_track = 17U;
}

static void
mbr_entry(unsigned slot, uint8_t active, uint8_t type, uint32_t start,
	uint32_t blocks)
{
	uint8_t *entry = medium.bytes + 0x1beU + slot * 16U;

	entry[0] = active;
	entry[4] = type;
	put_le32(entry + 8U, start);
	put_le32(entry + 12U, blocks);
}

static void
check_partition(const char *fixture, const struct partition *actual,
	unsigned index, uint64_t start, uint64_t data, uint64_t blocks,
	unsigned flags, const char *label, const char *uuid)
{
	if (actual->p_index != index || actual->p_start_block != start ||
	    actual->p_data_block != data || actual->p_block_count != blocks ||
	    actual->p_flags != flags || strcmp(actual->p_label, label) != 0 ||
	    strcmp(actual->p_uuid, uuid) != 0 || actual->p_parent != &disk) {
		printf("FAIL %s: index=%u start=%llu data=%llu blocks=%llu "
		    "flags=%#x label='%s' uuid='%s' parent=%s\n", fixture,
		    actual->p_index,
		    (unsigned long long)actual->p_start_block,
		    (unsigned long long)actual->p_data_block,
		    (unsigned long long)actual->p_block_count, actual->p_flags,
		    actual->p_label, actual->p_uuid,
		    actual->p_parent == &disk ? "disk" : "other");
		printf("  expected index=%u start=%llu data=%llu blocks=%llu "
		    "flags=%#x label='%s' uuid='%s' parent=disk\n", index,
		    (unsigned long long)start, (unsigned long long)data,
		    (unsigned long long)blocks, flags, label, uuid);
		failures++;
	}
}

static void
test_mbr(void)
{
	struct partition entries[ENTRY_CAPACITY];
	int count;

	reset_medium();
	medium.bytes[510] = 0x55U;
	medium.bytes[511] = 0xaaU;
	put_le32(medium.bytes + 0x1b8U, 0x1234abcdU);
	mbr_entry(0, 0x80U, 0x0eU, 63U, 100U);
	mbr_entry(1, 0x00U, 0x83U, 200U, 50U);
	mbr_entry(2, 0x00U, 0x0fU, 300U, 40U);
	mbr_entry(3, 0x00U, 0xa5U, 1000U, 40U);
	memset(entries, 0xa5, sizeof(entries));
	count = partition_scheme_mbr.scan(&partition_scheme_mbr, &disk, entries,
	    ENTRY_CAPACITY);
	CHECK(count == 4);
	check_partition("mbr-active", &entries[0], 0U, 63U, 63U, 100U,
	    PARTITION_BOOTABLE | PARTITION_HAS_UUID, "mbr1",
	    "1234abcd-01");
	check_partition("mbr-data", &entries[1], 1U, 200U, 200U, 50U,
	    PARTITION_HAS_UUID, "mbr2", "1234abcd-02");
	CHECK(entries[2].p_block_count == 0U);
	CHECK(entries[3].p_block_count == 0U);
	CHECK((entries[0].p_flags & PARTITION_HAS_LABEL) == 0U);
	count = partition_scheme_mbr.scan(&partition_scheme_mbr, &disk, entries,
	    2U);
	CHECK(count == 2);
}

static void
test_mbr_does_not_decode_gpt_identity(void)
{
	struct partition entries[ENTRY_CAPACITY];
	int count;

	reset_medium();
	medium.bytes[510] = 0x55U;
	medium.bytes[511] = 0xaaU;
	put_le32(medium.bytes + 0x1b8U, 0x01020304U);
	mbr_entry(0, 0x80U, 0xefU, 128U, 64U);
	mbr_entry(1, 0x00U, 0xeeU, 1U, DISK_SECTORS - 1U);
	memset(entries, 0xa5, sizeof(entries));
	count = partition_scheme_mbr.scan(&partition_scheme_mbr, &disk, entries,
	    ENTRY_CAPACITY);
	CHECK(count == 4);
	check_partition("mbr-no-gpt-identity", &entries[0], 0U, 128U, 128U,
	    64U, PARTITION_BOOTABLE | PARTITION_HAS_UUID, "mbr1",
	    "01020304-01");
	CHECK(entries[1].p_block_count == 0U);
}

static void
pc98_chs(uint8_t *target, uint32_t lba)
{
	uint32_t cylinder = lba /
	    (medium.geometry.heads * medium.geometry.sectors_per_track);
	uint32_t remainder = lba %
	    (medium.geometry.heads * medium.geometry.sectors_per_track);

	target[0] = (uint8_t)(remainder % medium.geometry.sectors_per_track);
	target[1] = (uint8_t)(remainder / medium.geometry.sectors_per_track);
	target[2] = (uint8_t)cylinder;
	target[3] = (uint8_t)(cylinder >> 8);
}

static void
pc98_entry(unsigned slot, uint8_t mid, uint8_t sid, uint32_t start,
	uint32_t data, uint32_t end, const char *label)
{
	uint8_t *entry = medium.bytes + SECTOR_SIZE + slot * 32U;
	size_t length = strlen(label);

	entry[0] = mid;
	entry[1] = sid;
	pc98_chs(entry + 4U, start);
	pc98_chs(entry + 8U, data);
	pc98_chs(entry + 12U, end);
	if (length > 16U)
		length = 16U;
	memset(entry + 16U, ' ', 16U);
	memcpy(entry + 16U, label, length);
}

static void
test_pc98_native_priority(void)
{
	struct partition entries[ENTRY_CAPACITY];
	int count;

	reset_medium();
	memcpy(medium.bytes + 4U, "IPL1", 4U);
	medium.bytes[510] = 0x55U;
	medium.bytes[511] = 0xaaU;
	mbr_entry(0, 0x80U, 0x0eU, 128U, 32U);
	pc98_entry(0, 0xa1U, 0x91U, 34U, 51U, 100U, "BOOT ROOT");
	memset(entries, 0xa5, sizeof(entries));
	count = partition_scheme_pc98.scan(&partition_scheme_pc98, &disk,
	    entries, ENTRY_CAPACITY);
	CHECK(count == 16);
	check_partition("pc98-native", &entries[0], 0U, 34U, 51U, 50U,
	    PARTITION_BOOTABLE | PARTITION_HAS_LABEL, "BOOT", "");
	memset(entries, 0xa5, sizeof(entries));
	count = partition_scheme_pc98_auto.scan(&partition_scheme_pc98_auto,
	    &disk, entries, ENTRY_CAPACITY);
	CHECK(count == 16);
	check_partition("pc98-ipl1-priority", &entries[0], 0U, 34U, 51U,
	    50U, PARTITION_BOOTABLE | PARTITION_HAS_LABEL, "BOOT", "");
}

static void
test_pc98_mbr_fallback(void)
{
	struct partition entries[ENTRY_CAPACITY];
	int count;

	reset_medium();
	medium.geometry_error = EIO;
	medium.bytes[510] = 0x55U;
	medium.bytes[511] = 0xaaU;
	put_le32(medium.bytes + 0x1b8U, 0xa0b0c0d0U);
	mbr_entry(0, 0x80U, 0x0eU, 160U, 80U);
	memset(entries, 0xa5, sizeof(entries));
	count = partition_scheme_pc98_auto.scan(&partition_scheme_pc98_auto,
	    &disk, entries, ENTRY_CAPACITY);
	CHECK(count == 4);
	check_partition("pc98-auto-mbr", &entries[0], 0U, 160U, 160U, 80U,
	    PARTITION_BOOTABLE | PARTITION_HAS_UUID, "mbr1",
	    "a0b0c0d0-01");
}

static void
test_pc98_default_native_fallback(void)
{
	struct partition entries[ENTRY_CAPACITY];
	int count;

	reset_medium();
	pc98_entry(0, 0xa1U, 0x91U, 68U, 68U, 135U, "NATIVE");
	memset(entries, 0xa5, sizeof(entries));
	count = partition_scheme_pc98_auto.scan(&partition_scheme_pc98_auto,
	    &disk, entries, ENTRY_CAPACITY);
	CHECK(count == 16);
	check_partition("pc98-auto-native-default", &entries[0], 0U, 68U,
	    68U, 68U, PARTITION_BOOTABLE | PARTITION_HAS_LABEL, "NATIVE", "");
}

static void
sun_entry(unsigned slot, uint32_t cylinder, uint32_t blocks)
{
	uint8_t *entry = medium.bytes + 444U + slot * 8U;

	put_be32(entry, cylinder);
	put_be32(entry + 4U, blocks);
}

static void
sun_checksum(void)
{
	uint16_t checksum = 0;
	unsigned offset;

	for (offset = 0; offset < 510U; offset += 2U)
		checksum ^= (uint16_t)((uint16_t)medium.bytes[offset] << 8 |
		    medium.bytes[offset + 1U]);
	put_be16(medium.bytes + 510U, checksum);
}

static void
test_sun(void)
{
	struct partition entries[ENTRY_CAPACITY];
	int count;

	reset_medium();
	put_be16(medium.bytes + 436U, 4U);
	put_be16(medium.bytes + 438U, 16U);
	sun_entry(0, 2U, 50U);
	sun_entry(1, 4U, 100U);
	sun_entry(2, 16U, 1U);
	put_be16(medium.bytes + 508U, 0xdabeU);
	sun_checksum();
	memset(entries, 0xa5, sizeof(entries));
	count = partition_scheme_sun.scan(&partition_scheme_sun, &disk, entries,
	    ENTRY_CAPACITY);
	CHECK(count == 8);
	check_partition("sun-slice-a", &entries[0], 0U, 128U, 128U, 50U, 0U,
	    "slicea", "");
	check_partition("sun-slice-b", &entries[1], 1U, 256U, 256U, 100U,
	    PARTITION_BOOTABLE, "sliceb", "");
	CHECK(entries[2].p_block_count == 0U);
	CHECK((entries[0].p_flags & PARTITION_HAS_LABEL) == 0U);
}

static void
x68k_entry(unsigned slot, const char *label, uint8_t flags,
	uint32_t start1024, uint32_t blocks1024)
{
	uint8_t *entry = medium.bytes + 2048U + 16U + slot * 16U;
	size_t length = strlen(label);

	if (length > 8U)
		length = 8U;
	memset(entry, ' ', 8U);
	memcpy(entry, label, length);
	entry[8] = flags;
	put_be24(entry + 9U, start1024);
	put_be32(entry + 12U, blocks1024);
}

static void
test_x68k(void)
{
	struct partition entries[ENTRY_CAPACITY];
	int count;

	reset_medium();
	memcpy(medium.bytes, "X68SCSI1", 8U);
	memcpy(medium.bytes + 2048U, "X68K", 4U);
	put_be32(medium.bytes + 2048U + 8U, DISK_SECTORS / 2U);
	x68k_entry(0, "ZEDBSD", 0U, 16U, 50U);
	x68k_entry(1, "DATA", 2U, 80U, 20U);
	x68k_entry(2, "OLD", 1U, 120U, 10U);
	memset(entries, 0xa5, sizeof(entries));
	count = partition_scheme_x68k.scan(&partition_scheme_x68k, &disk,
	    entries, ENTRY_CAPACITY);
	CHECK(count == 8);
	check_partition("x68k-root", &entries[0], 0U, 32U, 32U, 100U,
	    PARTITION_BOOTABLE | PARTITION_HAS_LABEL, "ZEDBSD", "");
	check_partition("x68k-data", &entries[1], 1U, 160U, 160U, 40U,
	    PARTITION_HAS_LABEL, "DATA", "");
	check_partition("x68k-disabled", &entries[2], 2U, 240U, 240U, 0U,
	    PARTITION_HAS_LABEL, "OLD", "");
	medium.bytes[0] = 'x';
	CHECK(partition_scheme_x68k.scan(&partition_scheme_x68k, &disk, entries,
	    ENTRY_CAPACITY) == -1);
}

int
main(void)
{
	test_mbr();
	test_mbr_does_not_decode_gpt_identity();
	test_pc98_native_priority();
	test_pc98_mbr_fallback();
	test_pc98_default_native_fallback();
	test_sun();
	test_x68k();
	if (failures != 0U) {
		printf("KA-T010: %u failure(s)\n", failures);
		return 1;
	}
	puts("KA-T010: PASS (MBR, PC-98 native/auto, Sun, X68k)");
	return 0;
}
