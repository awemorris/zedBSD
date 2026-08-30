/*
 * WS004 HW-T20 strict GPT parser regression fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#include <drivers/disklabel.h>
#include <kern/disk.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_BLOCKS 4096U
#define TEST_CAPACITY 16U
#define GPT_HEADER_SIZE 92U

struct memory_medium {
	uint8_t *bytes;
	size_t size;
	uint8_t *sparse_bytes;
	uint64_t sparse_first;
	uint32_t sparse_count;
	uint64_t fail_lba;
	int fail_reads;
};

struct gpt_layout {
	uint32_t entry_count;
	uint32_t entry_size;
	uint64_t table_bytes;
	uint64_t table_blocks;
	uint64_t primary_table;
	uint64_t backup_table;
	uint64_t first_usable;
	uint64_t last_usable;
	uint64_t logical_last;
};

static struct memory_medium medium;
static struct disk disk;
static struct gpt_layout layout;
static char diagnostics[4096];
static size_t diagnostic_length;
static unsigned failures;

#define CHECK(expression) do { \
	if (!(expression)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

void *
kern_malloc(size_t size)
{
	return malloc(size);
}

void *
kern_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

int
hal_printf(const char *format, ...)
{
	va_list arguments;
	int amount;

	if (diagnostic_length >= sizeof(diagnostics))
		return 0;
	va_start(arguments, format);
	amount = vsnprintf(diagnostics + diagnostic_length,
	    sizeof(diagnostics) - diagnostic_length, format, arguments);
	va_end(arguments);
	if (amount > 0) {
		size_t written = (size_t)amount;

		if (written >= sizeof(diagnostics) - diagnostic_length)
			diagnostic_length = sizeof(diagnostics) - 1U;
		else
			diagnostic_length += written;
	}
	return amount;
}

int
disk_read(struct disk *target, uint64_t block, uint32_t count, void *data)
{
	struct memory_medium *source;
	uint8_t *output = data;
	uint32_t index;

	if (target == NULL || data == NULL || target->d_data == NULL ||
	    count == 0U)
		return EINVAL;
	source = target->d_data;
	if (source->fail_reads && block <= source->fail_lba &&
	    (uint64_t)count > source->fail_lba - block)
		return EIO;
	if (block >= target->d_block_count ||
	    (uint64_t)count > target->d_block_count - block ||
	    (uint64_t)target->d_block_size * count > SIZE_MAX)
		return EOVERFLOW;
	for (index = 0U; index < count; index++) {
		uint64_t lba = block + index;
		const uint8_t *input;

		if (lba < source->size / target->d_block_size) {
			input = source->bytes + (size_t)lba * target->d_block_size;
		} else if (source->sparse_bytes != NULL &&
		    lba >= source->sparse_first &&
		    lba - source->sparse_first < source->sparse_count) {
			input = source->sparse_bytes +
			    (size_t)(lba - source->sparse_first) *
			    target->d_block_size;
		} else {
			return EIO;
		}
		memcpy(output + (size_t)index * target->d_block_size, input,
		    target->d_block_size);
	}
	return 0;
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

static uint32_t
crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
	while (size-- != 0U) {
		unsigned bit;

		crc ^= *data++;
		for (bit = 0U; bit < 8U; bit++)
			crc = (crc >> 1) ^
			    (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
	}
	return crc;
}

static uint32_t
crc32(const uint8_t *data, size_t size)
{
	return ~crc32_update(UINT32_MAX, data, size);
}

static uint8_t *
block_at(uint64_t lba)
{
	uint8_t *result = NULL;

	if (disk.d_block_size == 0U || lba >= disk.d_block_count) {
		fputs("fixture block address outside medium\n", stderr);
		exit(2);
	}
	if (lba < medium.size / disk.d_block_size) {
		result = medium.bytes + (size_t)lba * disk.d_block_size;
	} else if (medium.sparse_bytes != NULL &&
	    lba >= medium.sparse_first &&
	    lba - medium.sparse_first < medium.sparse_count) {
		result = medium.sparse_bytes +
		    (size_t)(lba - medium.sparse_first) * disk.d_block_size;
	}
	if (result == NULL) {
		fputs("fixture block is not materialized\n", stderr);
		exit(2);
	}
	return result;
}

static uint8_t *
header_at(int primary)
{
	return block_at(primary ? 1U : layout.logical_last);
}

static uint8_t *
table_at(int primary)
{
	return block_at(primary ? layout.primary_table : layout.backup_table);
}

static void
reset_medium(uint32_t block_size)
{
	free(medium.bytes);
	free(medium.sparse_bytes);
	memset(&medium, 0, sizeof(medium));
	memset(&disk, 0, sizeof(disk));
	memset(&layout, 0, sizeof(layout));
	memset(diagnostics, 0, sizeof(diagnostics));
	diagnostic_length = 0U;
	medium.size = (size_t)TEST_BLOCKS * block_size;
	medium.bytes = calloc(1U, medium.size);
	if (medium.bytes == NULL) {
		fputs("fixture allocation failed\n", stderr);
		exit(2);
	}
	disk.d_block_size = block_size;
	disk.d_block_count = TEST_BLOCKS;
	disk.d_data = &medium;
	strcpy(disk.d_name, "nvme0n1");
}

static void
map_sparse_blocks(uint64_t first, uint32_t count)
{
	if (medium.sparse_bytes != NULL || count == 0U) {
		fputs("invalid sparse fixture mapping\n", stderr);
		exit(2);
	}
	medium.sparse_bytes = calloc(count, disk.d_block_size);
	if (medium.sparse_bytes == NULL) {
		fputs("sparse fixture allocation failed\n", stderr);
		exit(2);
	}
	medium.sparse_first = first;
	medium.sparse_count = count;
}

static void
extend_sparse_blocks(uint64_t last)
{
	uint8_t *resized;
	uint64_t count;
	size_t old_size, new_size;

	if (medium.sparse_bytes == NULL || last < medium.sparse_first) {
		fputs("invalid sparse fixture extension\n", stderr);
		exit(2);
	}
	count = last - medium.sparse_first + 1U;
	if (count > UINT32_MAX ||
	    count > SIZE_MAX / disk.d_block_size) {
		fputs("sparse fixture extension too large\n", stderr);
		exit(2);
	}
	if (count <= medium.sparse_count)
		return;
	old_size = (size_t)medium.sparse_count * disk.d_block_size;
	new_size = (size_t)count * disk.d_block_size;
	resized = realloc(medium.sparse_bytes, new_size);
	if (resized == NULL) {
		fputs("sparse fixture extension failed\n", stderr);
		exit(2);
	}
	memset(resized + old_size, 0, new_size - old_size);
	medium.sparse_bytes = resized;
	medium.sparse_count = (uint32_t)count;
}

static void
mbr_entry(unsigned slot, uint8_t status, uint8_t type, uint32_t start,
	uint32_t blocks)
{
	uint8_t *entry = block_at(0U) + 0x1beU + slot * 16U;

	entry[0] = status;
	entry[4] = type;
	put32(entry + 8U, start);
	put32(entry + 12U, blocks);
}

static void
protective_mbr(void)
{
	uint32_t blocks = layout.logical_last > UINT32_MAX ? UINT32_MAX :
	    (uint32_t)layout.logical_last;
	uint8_t *mbr = block_at(0U);

	mbr[510U] = 0x55U;
	mbr[511U] = 0xaaU;
	mbr_entry(0U, 0U, 0xeeU, 1U, blocks);
}

static void
refresh_header_crc(int primary)
{
	uint8_t *header = header_at(primary);
	uint32_t size = (uint32_t)header[12U] |
	    ((uint32_t)header[13U] << 8) | ((uint32_t)header[14U] << 16) |
	    ((uint32_t)header[15U] << 24);

	put32(header + 16U, 0U);
	if (size <= disk.d_block_size)
		put32(header + 16U, crc32(header, size));
}

static void
write_header(int primary)
{
	uint8_t *header = header_at(primary);
	uint64_t here = primary ? 1U : layout.logical_last;
	uint64_t other = primary ? layout.logical_last : 1U;
	uint64_t table = primary ? layout.primary_table : layout.backup_table;
	static const uint8_t disk_guid[16U] = {
		0xefU, 0xcdU, 0xabU, 0x89U, 0x67U, 0x45U, 0x23U, 0x01U,
		0x80U, 0x81U, 0x82U, 0x83U, 0x84U, 0x85U, 0x86U, 0x87U
	};

	memset(header, 0, disk.d_block_size);
	memcpy(header, "EFI PART", 8U);
	put32(header + 8U, 0x00010000U);
	put32(header + 12U, GPT_HEADER_SIZE);
	put64(header + 24U, here);
	put64(header + 32U, other);
	put64(header + 40U, layout.first_usable);
	put64(header + 48U, layout.last_usable);
	memcpy(header + 56U, disk_guid, sizeof(disk_guid));
	put64(header + 72U, table);
	put32(header + 80U, layout.entry_count);
	put32(header + 84U, layout.entry_size);
	put32(header + 88U, crc32(table_at(primary),
	    (size_t)layout.table_bytes));
	refresh_header_crc(primary);
}

static void
refresh_table_and_header(int primary)
{
	uint8_t *header = header_at(primary);

	put32(header + 88U, crc32(table_at(primary),
	    (size_t)layout.table_bytes));
	refresh_header_crc(primary);
}

static void
begin_gpt_extent(uint32_t block_size, uint32_t entry_count,
	uint32_t entry_size, uint64_t logical_last, uint64_t physical_blocks)
{
	uint64_t reserved_blocks;

	reset_medium(block_size);
	if (logical_last < 3U || logical_last >= physical_blocks) {
		fputs("invalid GPT fixture extent\n", stderr);
		exit(2);
	}
	disk.d_block_count = physical_blocks;
	layout.entry_count = entry_count;
	layout.entry_size = entry_size;
	layout.table_bytes = (uint64_t)entry_count * entry_size;
	layout.table_blocks = (layout.table_bytes + block_size - 1U) /
	    block_size;
	reserved_blocks = (16384U + block_size - 1U) / block_size;
	if (reserved_blocks < layout.table_blocks)
		reserved_blocks = layout.table_blocks;
	layout.logical_last = logical_last;
	layout.primary_table = 2U;
	layout.backup_table = logical_last - reserved_blocks;
	if (layout.backup_table >= medium.size / block_size)
		map_sparse_blocks(layout.backup_table,
		    (uint32_t)reserved_blocks + 1U);
	layout.first_usable = layout.primary_table + reserved_blocks;
	layout.last_usable = layout.backup_table - 1U;
	protective_mbr();
}

static void
begin_gpt(uint32_t block_size, uint32_t entry_count, uint32_t entry_size)
{
	begin_gpt_extent(block_size, entry_count, entry_size, TEST_BLOCKS - 1U,
	    TEST_BLOCKS);
}

static void
set_name(uint8_t *entry, const uint16_t *name, unsigned units)
{
	unsigned index;

	memset(entry + 56U, 0, 72U);
	if (units > 36U)
		units = 36U;
	for (index = 0U; index < units; index++) {
		entry[56U + index * 2U] = (uint8_t)name[index];
		entry[57U + index * 2U] = (uint8_t)(name[index] >> 8);
	}
}

static void
write_entry(unsigned slot, uint8_t unique_seed, uint64_t first,
	uint64_t last, const uint16_t *name, unsigned name_units)
{
	uint8_t *entry;
	unsigned index;

	CHECK(slot < layout.entry_count);
	entry = table_at(1) + (size_t)slot * layout.entry_size;
	memset(entry, 0, layout.entry_size);
	entry[0] = 0x28U;
	entry[1] = 0x73U;
	for (index = 0U; index < 16U; index++)
		entry[16U + index] = (uint8_t)(unique_seed + index);
	put64(entry + 32U, first);
	put64(entry + 40U, last);
	set_name(entry, name, name_units);
	memcpy(table_at(0) + (size_t)slot * layout.entry_size, entry,
	    layout.entry_size);
}

static void
build_gpt_extent(uint32_t block_size, uint64_t logical_last,
	uint64_t physical_blocks)
{
	static const uint16_t name[] = {
		'z', 'e', 'd', 'B', 'S', 'D'
	};

	begin_gpt_extent(block_size, 8U, 128U, logical_last, physical_blocks);
	write_entry(3U, 0x10U, layout.first_usable + 8U,
	    layout.first_usable + 71U, name,
	    (unsigned)(sizeof(name) / sizeof(name[0])));
	write_header(1);
	write_header(0);
}

static void
build_gpt(uint32_t block_size)
{
	build_gpt_extent(block_size, TEST_BLOCKS - 1U, TEST_BLOCKS);
}

static void
add_valid_stale_physical_backup(void)
{
	uint64_t saved_logical_last = layout.logical_last;
	uint64_t saved_backup_table = layout.backup_table;
	uint64_t saved_last_usable = layout.last_usable;
	uint64_t physical_last = disk.d_block_count - 1U;
	uint64_t reserve_blocks = saved_logical_last - saved_backup_table;
	uint64_t stale_table = physical_last - reserve_blocks;
	uint8_t *stale_entry;
	unsigned index;

	CHECK(physical_last > saved_logical_last);
	if (stale_table >= medium.size / disk.d_block_size) {
		if (medium.sparse_bytes == NULL)
			map_sparse_blocks(stale_table,
			    (uint32_t)(physical_last - stale_table + 1U));
		else
			extend_sparse_blocks(physical_last);
	}
	memcpy(block_at(stale_table), table_at(1), (size_t)layout.table_bytes);
	/* Give the stale whole-device copy a distinct partition wholly inside
	 * the destination tail, so accidental publication changes the result. */
	stale_entry = block_at(stale_table) + 5U * layout.entry_size;
	memset(stale_entry, 0, layout.entry_size);
	stale_entry[0U] = 0x28U;
	stale_entry[1U] = 0x73U;
	for (index = 0U; index < 16U; index++)
		stale_entry[16U + index] = (uint8_t)(0x90U + index);
	put64(stale_entry + 32U, saved_logical_last + 8U);
	put64(stale_entry + 40U, saved_logical_last + 15U);
	layout.logical_last = physical_last;
	layout.backup_table = stale_table;
	layout.last_usable = stale_table - 1U;
	write_header(0);
	layout.logical_last = saved_logical_last;
	layout.backup_table = saved_backup_table;
	layout.last_usable = saved_last_usable;
}

static int
scan(struct partition *entries, unsigned capacity)
{
	memset(diagnostics, 0, sizeof(diagnostics));
	diagnostic_length = 0U;
	return partition_scheme_pcat_auto.scan(&partition_scheme_pcat_auto,
	    &disk, entries, capacity);
}

static void
check_basic_entry(const struct partition *entry, const char *label)
{
	CHECK(entry->p_parent == &disk);
	CHECK(entry->p_index == 3U);
	CHECK(entry->p_start_block == layout.first_usable + 8U);
	CHECK(entry->p_data_block == layout.first_usable + 8U);
	CHECK(entry->p_block_count == 64U);
	CHECK(entry->p_flags == (PARTITION_HAS_LABEL | PARTITION_HAS_UUID));
	CHECK(strcmp(entry->p_label, label) == 0);
	CHECK(strcmp(entry->p_uuid,
	    "13121110-1514-1716-1819-1a1b1c1d1e1f") == 0);
}

static void
expect_rejected(const char *name)
{
	struct partition entries[TEST_CAPACITY], before[TEST_CAPACITY];
	int result;

	memset(entries, 0xa5, sizeof(entries));
	memcpy(before, entries, sizeof(before));
	result = scan(entries, TEST_CAPACITY);
	if (result >= 0 || memcmp(entries, before, sizeof(entries)) != 0) {
		printf("FAIL %s: result=%d output_changed=%s diagnostics=%s\n",
		    name, result,
		    memcmp(entries, before, sizeof(entries)) == 0 ? "no" : "yes",
		    diagnostics);
		failures++;
	}
}

static void
test_valid_geometry(uint32_t block_size)
{
	struct partition entries[TEST_CAPACITY];
	int result;

	build_gpt(block_size);
	memset(entries, 0xa5, sizeof(entries));
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
}

static void
test_bounded_large_physical_media(void)
{
	const uint64_t logical_last = TEST_BLOCKS / 2U - 1U;
	struct partition entries[TEST_CAPACITY];
	int result;

	/* A small copied GPT remains non-saturated even when a 512-byte-sector
	 * destination itself has more than UINT32_MAX LBAs. */
	build_gpt_extent(512U, logical_last, (uint64_t)UINT32_MAX + 129U);
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");

	/* Saturation is based on the protective entry's LBA count, not bytes.
	 * This 4096-byte-sector destination is over 2 TiB but below UINT32_MAX
	 * sectors, so the small logical extent remains exact. */
	build_gpt_extent(4096U, logical_last, UINT64_C(600000001));
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
}

static void
test_bounded_extent(uint32_t block_size)
{
	const uint64_t logical_last = TEST_BLOCKS / 2U - 1U;
	struct partition entries[TEST_CAPACITY];
	char expected[256];
	int result;

	build_gpt_extent(block_size, logical_last, TEST_BLOCKS);
	memset(entries, 0xa5, sizeof(entries));
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
	(void)snprintf(expected, sizeof(expected),
	    "gpt: nvme0n1 bounded extent accepted: logical-last=%llu "
	    "physical-last=%u declared-sectors=%llu physical-sectors=%u "
	    "ignored-tail-sectors=%llu\n",
	    (unsigned long long)logical_last, TEST_BLOCKS - 1U,
	    (unsigned long long)(logical_last + 1U), TEST_BLOCKS,
	    (unsigned long long)(TEST_BLOCKS - 1U - logical_last));
	CHECK(strcmp(diagnostics, expected) == 0);

	/* Neither arbitrary tail contents nor an unreadable physical last block
	 * are part of the bounded GPT extent. */
	memset(block_at(disk.d_block_count - 1U), 0x5a, disk.d_block_size);
	medium.fail_lba = disk.d_block_count - 1U;
	medium.fail_reads = 1;
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");

	/* A complete but stale GPT backup at the destination's physical end is
	 * ignored in favor of the mutually valid bounded pair. */
	build_gpt_extent(block_size, logical_last, TEST_BLOCKS);
	add_valid_stale_physical_backup();
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
}

static void
test_bounded_extent_rejection(void)
{
	const uint64_t logical_last = TEST_BLOCKS / 2U - 1U;
	uint8_t *entry;
	uint64_t tail_table;

	build_gpt_extent(512U, logical_last, TEST_BLOCKS);
	header_at(0)[16U] ^= 1U;
	expect_rejected("bounded-damaged-backup");

	build_gpt_extent(512U, logical_last, TEST_BLOCKS);
	header_at(1)[16U] ^= 1U;
	expect_rejected("bounded-damaged-primary");

	build_gpt_extent(512U, logical_last, TEST_BLOCKS);
	put64(header_at(1) + 32U, disk.d_block_count);
	refresh_header_crc(1);
	expect_rejected("bounded-primary-alternate-after-physical-end");

	build_gpt_extent(512U, logical_last, TEST_BLOCKS);
	put64(header_at(0) + 32U, 2U);
	refresh_header_crc(0);
	expect_rejected("bounded-backup-alternate-not-primary");

	build_gpt_extent(512U, logical_last, TEST_BLOCKS);
	medium.fail_lba = logical_last;
	medium.fail_reads = 1;
	expect_rejected("bounded-unreadable-backup");

	build_gpt_extent(512U, logical_last, TEST_BLOCKS);
	table_at(0)[3U * layout.entry_size + 56U] = 'X';
	refresh_table_and_header(0);
	expect_rejected("bounded-contradictory-entry-arrays");

	/* A valid-CRC backup array in the ignored tail is still outside the
	 * logical GPT extent and cannot be used as metadata. */
	build_gpt_extent(512U, logical_last, TEST_BLOCKS);
	tail_table = logical_last + 16U;
	memcpy(block_at(tail_table), table_at(0), (size_t)layout.table_bytes);
	put64(header_at(0) + 72U, tail_table);
	refresh_header_crc(0);
	expect_rejected("bounded-backup-table-in-physical-tail");

	/* A partition in physically present tail blocks remains out of range. */
	build_gpt_extent(512U, logical_last, TEST_BLOCKS);
	entry = table_at(1) + 3U * layout.entry_size;
	put64(entry + 32U, logical_last + 8U);
	put64(entry + 40U, logical_last + 15U);
	memcpy(table_at(0) + 3U * layout.entry_size, entry,
	    layout.entry_size);
	refresh_table_and_header(1);
	refresh_table_and_header(0);
	expect_rejected("bounded-partition-in-physical-tail");

	/* The protective entry describes the copied GPT extent, not the larger
	 * destination medium. */
	build_gpt_extent(512U, logical_last, TEST_BLOCKS);
	put32(block_at(0U) + 0x1beU + 12U, TEST_BLOCKS - 1U);
	expect_rejected("bounded-pmbr-uses-physical-size");

	/* Truncating even one block below the advertised logical end is fatal. */
	build_gpt_extent(512U, 3071U, TEST_BLOCKS);
	disk.d_block_count = 3071U;
	expect_rejected("bounded-physical-medium-too-small");
}

static void
test_bounded_saturated_protective_mbr(void)
{
	uint64_t logical_last = UINT32_MAX;
	uint64_t physical_blocks = logical_last + 129U;
	struct partition entries[TEST_CAPACITY];
	int result;

	/* Only the low primary area and the high backup area are materialized by
	 * this sparse fixture.  Any accidental physical-tail read returns EIO. */
	build_gpt_extent(512U, logical_last, physical_blocks);
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
	CHECK(strstr(diagnostics, "bounded extent accepted") != NULL);

	logical_last = (uint64_t)UINT32_MAX + 64U;
	physical_blocks = logical_last + 129U;
	build_gpt_extent(512U, logical_last, physical_blocks);
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
	CHECK(strstr(diagnostics, "bounded extent accepted") != NULL);

	build_gpt_extent(512U, logical_last, physical_blocks);
	put32(block_at(0U) + 0x1beU + 12U, UINT32_MAX - 1U);
	expect_rejected("bounded-saturated-pmbr-must-be-ffffffff");
}

static void
test_saturated_extent_ambiguity_rejection(void)
{
	const uint64_t logical_last = (uint64_t)UINT32_MAX + 64U;
	const uint64_t physical_blocks = logical_last + 129U;

	/* Each case has a fully valid but stale whole-device backup at the
	 * physical end.  Saturation must not let primary damage select it. */
	build_gpt_extent(512U, logical_last, physical_blocks);
	add_valid_stale_physical_backup();
	table_at(1)[0U] ^= 1U;
	expect_rejected("saturated-primary-table-damaged-with-stale-tail");

	build_gpt_extent(512U, logical_last, physical_blocks);
	add_valid_stale_physical_backup();
	header_at(1)[16U] ^= 1U;
	expect_rejected("saturated-primary-header-damaged-with-stale-tail");

	build_gpt_extent(512U, logical_last, physical_blocks);
	add_valid_stale_physical_backup();
	put64(header_at(1) + 32U, disk.d_block_count);
	refresh_header_crc(1);
	expect_rejected("saturated-primary-alternate-after-physical-end");

	build_gpt_extent(512U, logical_last, physical_blocks);
	add_valid_stale_physical_backup();
	medium.fail_lba = 1U;
	medium.fail_reads = 1;
	expect_rejected("saturated-primary-unreadable-with-stale-tail");
}

static void
test_saturated_canonical_recovery(void)
{
	uint64_t physical_last = UINT32_MAX;
	struct partition entries[TEST_CAPACITY];
	int result;

	/* At the exact saturation boundary the PMBR still identifies the whole
	 * device uniquely, so ordinary backup-only recovery remains available. */
	build_gpt_extent(512U, physical_last, physical_last + 1U);
	header_at(1)[16U] ^= 1U;
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
	CHECK(strstr(diagnostics, "primary damaged") != NULL);

	/* Above the boundary, a CRC-valid primary header still establishes a
	 * canonical extent even if only its entry array is damaged. */
	physical_last = (uint64_t)UINT32_MAX + 128U;
	build_gpt_extent(512U, physical_last, physical_last + 1U);
	table_at(1)[0U] ^= 1U;
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
	CHECK(strstr(diagnostics, "primary damaged") != NULL);
}

static void
test_hybrid_is_gpt_authoritative(void)
{
	struct partition entries[TEST_CAPACITY];
	int result;

	build_gpt(512U);
	/* A compatibility entry must never be merged into GPT output. */
	mbr_entry(2U, 0x80U, 0xefU, 2000U, 100U);
	memset(entries, 0xa5, sizeof(entries));
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");

	/* Once an EE entry selected GPT, damaged GPT cannot fall back to slot 2. */
	header_at(1)[0U] ^= 0xffU;
	header_at(0)[0U] ^= 0xffU;
	expect_rejected("hybrid-broken-gpt-no-mbr-fallback");
}

static void
test_signature_without_ee_never_falls_back(void)
{
	build_gpt(512U);
	memset(block_at(0U) + 0x1beU, 0, 4U * 16U);
	mbr_entry(2U, 0x80U, 0x83U, 2000U, 100U);
	expect_rejected("gpt-signature-without-ee-no-fallback");
}

static void
test_pure_mbr_fallback(void)
{
	struct partition entries[TEST_CAPACITY];
	uint8_t *mbr;
	int result;

	reset_medium(512U);
	mbr = block_at(0U);
	mbr[510U] = 0x55U;
	mbr[511U] = 0xaaU;
	put32(mbr + 0x1b8U, 0x1234abcdU);
	mbr_entry(1U, 0x80U, 0x83U, 64U, 32U);
	memset(entries, 0xa5, sizeof(entries));
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 4);
	CHECK(entries[1].p_index == 1U);
	CHECK(entries[1].p_start_block == 64U);
	CHECK(entries[1].p_block_count == 32U);
	CHECK((entries[1].p_flags & PARTITION_BOOTABLE) != 0U);
	CHECK(strcmp(entries[1].p_uuid, "1234abcd-02") == 0);
}

static void
test_degraded_copy_selection(void)
{
	struct partition entries[TEST_CAPACITY];
	int result;

	build_gpt(512U);
	header_at(1)[16U] ^= 1U;
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
	CHECK(strstr(diagnostics, "primary damaged") != NULL);

	build_gpt(512U);
	table_at(0)[0U] ^= 1U;
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
	CHECK(strstr(diagnostics, "backup damaged") != NULL);

	build_gpt(512U);
	medium.fail_lba = 1U;
	medium.fail_reads = 1;
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
	CHECK(strstr(diagnostics, "primary damaged") != NULL);

	build_gpt(512U);
	medium.fail_lba = disk.d_block_count - 1U;
	medium.fail_reads = 1;
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
	CHECK(strstr(diagnostics, "backup damaged") != NULL);

	build_gpt(512U);
	header_at(1)[0U] ^= 1U;
	header_at(0)[0U] ^= 1U;
	expect_rejected("both-gpt-copies-damaged");
}

static void
test_contradictory_valid_copies(void)
{
	build_gpt(512U);
	table_at(0)[3U * layout.entry_size + 56U] = 'X';
	refresh_table_and_header(0);
	expect_rejected("contradictory-entry-arrays");

	build_gpt(512U);
	header_at(0)[56U] ^= 1U;
	refresh_header_crc(0);
	expect_rejected("contradictory-disk-guid");

	build_gpt(512U);
	put64(header_at(0) + 40U, layout.first_usable + 1U);
	refresh_header_crc(0);
	expect_rejected("contradictory-usable-range");

	build_gpt(512U);
	put32(header_at(0) + 12U, 96U);
	refresh_header_crc(0);
	expect_rejected("contradictory-header-size");
}

static void
invalidate_both_u32(unsigned offset, uint32_t value, const char *name)
{
	build_gpt(512U);
	put32(header_at(1) + offset, value);
	put32(header_at(0) + offset, value);
	refresh_header_crc(1);
	refresh_header_crc(0);
	expect_rejected(name);
}

static void
invalidate_both_u64(unsigned offset, uint64_t primary_value,
	uint64_t backup_value, const char *name)
{
	build_gpt(512U);
	put64(header_at(1) + offset, primary_value);
	put64(header_at(0) + offset, backup_value);
	refresh_header_crc(1);
	refresh_header_crc(0);
	expect_rejected(name);
}

static void
test_header_and_geometry_rejection(void)
{
	invalidate_both_u32(8U, 0x00010001U, "revision");
	invalidate_both_u32(12U, 91U, "short-header");
	invalidate_both_u32(12U, 513U, "oversized-header");
	invalidate_both_u32(20U, 1U, "reserved-field");
	invalidate_both_u32(80U, 0U, "zero-entry-count");
	invalidate_both_u32(80U, 4097U, "entry-count-limit");
	invalidate_both_u32(84U, 64U, "short-entry-size");
	invalidate_both_u32(84U, 384U, "non-power-of-two-entry-size");
	invalidate_both_u64(24U, 2U, TEST_BLOCKS - 2U, "my-lba");
	invalidate_both_u64(32U, TEST_BLOCKS - 2U, 2U, "alternate-lba");
	invalidate_both_u64(40U, TEST_BLOCKS - 2U, TEST_BLOCKS - 2U,
	    "usable-range");
	invalidate_both_u64(48U, TEST_BLOCKS, TEST_BLOCKS, "last-usable");
	invalidate_both_u64(72U, 1U, TEST_BLOCKS - 1U,
	    "entry-array-overlaps-header");
	invalidate_both_u64(72U, UINT64_MAX, UINT64_MAX,
	    "entry-array-lba-overflow");

	build_gpt(512U);
	put64(header_at(1) + 72U, layout.first_usable - 31U);
	put64(header_at(0) + 72U, disk.d_block_count - 1U - 31U);
	refresh_header_crc(1);
	refresh_header_crc(0);
	expect_rejected("entry-array-minimum-reservation");

	build_gpt(512U);
	header_at(1)[92U] = 1U;
	header_at(0)[92U] = 1U;
	expect_rejected("header-reserved-tail");

	build_gpt(512U);
	memset(header_at(1) + 56U, 0, 16U);
	memset(header_at(0) + 56U, 0, 16U);
	refresh_header_crc(1);
	refresh_header_crc(0);
	expect_rejected("zero-disk-guid");

	build_gpt(512U);
	header_at(1)[16U] ^= 1U;
	header_at(0)[16U] ^= 1U;
	expect_rejected("header-crc");

	build_gpt(512U);
	table_at(1)[0U] ^= 1U;
	table_at(0)[0U] ^= 1U;
	expect_rejected("entry-array-crc");
}

static void
test_protective_mbr_rejection(void)
{
	build_gpt(512U);
	put32(block_at(0U) + 0x1beU + 12U, 0U);
	expect_rejected("protective-zero-size");

	build_gpt(512U);
	put32(block_at(0U) + 0x1beU + 12U, UINT32_MAX);
	expect_rejected("protective-saturated-size-on-small-medium");

	build_gpt(512U);
	block_at(0U)[0x1beU] = 0x80U;
	expect_rejected("protective-status");

	build_gpt(512U);
	put32(block_at(0U) + 0x1beU + 8U, 2U);
	expect_rejected("protective-start");

	build_gpt(512U);
	put32(block_at(0U) + 0x1beU + 12U, TEST_BLOCKS - 2U);
	expect_rejected("protective-size");

	build_gpt(512U);
	mbr_entry(1U, 0U, 0xeeU, 1U, TEST_BLOCKS - 1U);
	expect_rejected("multiple-protective-entries");
}

static void
test_entry_validation(void)
{
	static const uint16_t name[] = { 'd', 'a', 't', 'a' };
	struct partition entries[TEST_CAPACITY], before[TEST_CAPACITY];
	uint8_t *entry;
	int result;

	build_gpt(512U);
	write_entry(5U, 0x10U, layout.first_usable + 100U,
	    layout.first_usable + 120U, name, 4U);
	write_header(1);
	write_header(0);
	expect_rejected("duplicate-unique-guid");

	build_gpt(512U);
	write_entry(5U, 0x40U, layout.first_usable + 71U,
	    layout.first_usable + 90U, name, 4U);
	write_header(1);
	write_header(0);
	expect_rejected("inclusive-overlap");

	build_gpt(512U);
	entry = table_at(1) + 3U * layout.entry_size;
	put64(entry + 32U, layout.first_usable - 1U);
	memcpy(table_at(0) + 3U * layout.entry_size, entry,
	    layout.entry_size);
	write_header(1);
	write_header(0);
	expect_rejected("entry-before-usable");

	build_gpt(512U);
	entry = table_at(1) + 3U * layout.entry_size;
	memset(entry + 16U, 0, 16U);
	memcpy(table_at(0) + 3U * layout.entry_size, entry,
	    layout.entry_size);
	write_header(1);
	write_header(0);
	expect_rejected("zero-unique-guid");

	build_gpt(512U);
	entry = table_at(1) + 3U * layout.entry_size;
	put64(entry + 48U, UINT64_C(1) << 3);
	memcpy(table_at(0) + 3U * layout.entry_size, entry,
	    layout.entry_size);
	write_header(1);
	write_header(0);
	expect_rejected("reserved-attribute-bits");

	build_gpt(512U);
	entry = table_at(1) + 3U * layout.entry_size;
	put64(entry + 48U, (UINT64_C(1) << 2) | (UINT64_C(1) << 48));
	memcpy(table_at(0) + 3U * layout.entry_size, entry,
	    layout.entry_size);
	write_header(1);
	write_header(0);
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");

	build_gpt(512U);
	write_entry(5U, 0x40U, layout.first_usable + 100U,
	    layout.first_usable + 120U, name, 4U);
	write_header(1);
	write_header(0);
	memset(entries, 0x5a, sizeof(entries));
	memcpy(before, entries, sizeof(before));
	result = scan(entries, 1U);
	CHECK(result == -ENOSPC);
	CHECK(memcmp(entries, before, sizeof(entries)) == 0);

	/* An invalid active entry beyond capacity must beat ENOSPC. */
	build_gpt(512U);
	write_entry(5U, 0x40U, layout.first_usable + 100U,
	    layout.first_usable + 120U, name, 4U);
	memset(table_at(1) + 5U * layout.entry_size + 16U, 0, 16U);
	memset(table_at(0) + 5U * layout.entry_size + 16U, 0, 16U);
	write_header(1);
	write_header(0);
	memset(entries, 0x5a, sizeof(entries));
	memcpy(before, entries, sizeof(before));
	result = scan(entries, 1U);
	CHECK(result == -EINVAL);
	CHECK(memcmp(entries, before, sizeof(entries)) == 0);

	/* A zero type GUID marks an unused slot regardless of stale payload. */
	build_gpt(512U);
	entry = table_at(1) + 5U * layout.entry_size;
	memset(entry, 0, 16U);
	memset(entry + 16U, 0xff, layout.entry_size - 16U);
	memcpy(table_at(0) + 5U * layout.entry_size, entry,
	    layout.entry_size);
	write_header(1);
	write_header(0);
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	check_basic_entry(&entries[0], "zedBSD");
}

static void
test_utf16_names(void)
{
	static const uint16_t smiling[] = { 'A', 0xd83dU, 0xde00U };
	uint16_t boundary[36U];
	char expected[PARTITION_LABEL_MAX];
	struct partition entries[TEST_CAPACITY];
	uint8_t *entry;
	unsigned index;
	int result;

	build_gpt(512U);
	entry = table_at(1) + 3U * layout.entry_size;
	set_name(entry, smiling, 3U);
	memcpy(table_at(0) + 3U * layout.entry_size, entry,
	    layout.entry_size);
	write_header(1);
	write_header(0);
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	CHECK(strcmp(entries[0].p_label, "A\xf0\x9f\x98\x80") == 0);

	for (index = 0U; index < 36U; index++)
		boundary[index] = 0x4e00U;
	build_gpt(512U);
	entry = table_at(1) + 3U * layout.entry_size;
	set_name(entry, boundary, 36U);
	memcpy(table_at(0) + 3U * layout.entry_size, entry,
	    layout.entry_size);
	write_header(1);
	write_header(0);
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	CHECK(strlen(entries[0].p_label) == 108U);
	for (index = 0U; index < 36U; index++) {
		expected[index * 3U] = (char)0xe4U;
		expected[index * 3U + 1U] = (char)0xb8U;
		expected[index * 3U + 2U] = (char)0x80U;
	}
	expected[108U] = '\0';
	CHECK(memcmp(entries[0].p_label, expected, sizeof(expected)) == 0);

	build_gpt(512U);
	entry = table_at(1) + 3U * layout.entry_size;
	boundary[0] = 0xd83dU;
	boundary[1] = 'X';
	set_name(entry, boundary, 2U);
	memcpy(table_at(0) + 3U * layout.entry_size, entry,
	    layout.entry_size);
	write_header(1);
	write_header(0);
	expect_rejected("unpaired-high-surrogate");

	build_gpt(512U);
	entry = table_at(1) + 3U * layout.entry_size;
	boundary[0] = 0xde00U;
	set_name(entry, boundary, 1U);
	memcpy(table_at(0) + 3U * layout.entry_size, entry,
	    layout.entry_size);
	write_header(1);
	write_header(0);
	expect_rejected("unpaired-low-surrogate");

	build_gpt(512U);
	entry = table_at(1) + 3U * layout.entry_size;
	for (index = 0U; index < 35U; index++)
		boundary[index] = 'a';
	boundary[35U] = 0xd83dU;
	set_name(entry, boundary, 36U);
	memcpy(table_at(0) + 3U * layout.entry_size, entry,
	    layout.entry_size);
	write_header(1);
	write_header(0);
	expect_rejected("surrogate-crosses-name-boundary");
}

static void
test_entry_size_and_crc_extent(void)
{
	static const uint16_t name[] = { 'w', 'i', 'd', 'e' };
	struct partition entries[TEST_CAPACITY];
	uint8_t *primary_padding, *backup_padding;
	int result;

	begin_gpt(512U, 3U, 128U);
	write_entry(2U, 0x20U, layout.first_usable + 1U,
	    layout.first_usable + 8U, name, 4U);
	write_header(1);
	write_header(0);
	/* Bytes after count*size are sector padding and are outside the CRC. */
	primary_padding = table_at(1) + layout.table_bytes;
	backup_padding = table_at(0) + layout.table_bytes;
	*primary_padding = 0xa5U;
	*backup_padding = 0x5aU;
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	CHECK(entries[0].p_index == 2U);

	begin_gpt(512U, 4U, 256U);
	write_entry(1U, 0x30U, layout.first_usable + 1U,
	    layout.first_usable + 8U, name, 4U);
	write_header(1);
	write_header(0);
	result = scan(entries, TEST_CAPACITY);
	CHECK(result == 1);
	CHECK(entries[0].p_index == 1U);

	table_at(1)[1U * layout.entry_size + 200U] = 0x5aU;
	table_at(0)[1U * layout.entry_size + 200U] = 0x5aU;
	write_header(1);
	write_header(0);
	expect_rejected("entry-reserved-extension");
}

int
main(void)
{
	test_valid_geometry(512U);
	test_valid_geometry(4096U);
	test_bounded_extent(512U);
	test_bounded_extent(4096U);
	test_bounded_large_physical_media();
	test_bounded_extent_rejection();
	test_bounded_saturated_protective_mbr();
	test_saturated_extent_ambiguity_rejection();
	test_saturated_canonical_recovery();
	test_hybrid_is_gpt_authoritative();
	test_signature_without_ee_never_falls_back();
	test_pure_mbr_fallback();
	test_degraded_copy_selection();
	test_contradictory_valid_copies();
	test_header_and_geometry_rejection();
	test_protective_mbr_rejection();
	test_entry_validation();
	test_utf16_names();
	test_entry_size_and_crc_extent();
	free(medium.bytes);
	free(medium.sparse_bytes);
	if (failures != 0U) {
		printf("HW-T20 strict GPT: %u failure(s)\n", failures);
		return 1;
	}
	puts("HW-T20 strict GPT: PASS (512/4096, copy recovery, validation, identity)");
	return 0;
}
