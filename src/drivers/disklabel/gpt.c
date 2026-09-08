/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Strict, read-only GUID Partition Table parser.
 */

#include <drivers/disklabel.h>

#include <errno.h>
#include <hal/hal.h>
#include <kern/kmem.h>
#include <stdint.h>
#include <string.h>

#define GPT_HEADER_MIN_SIZE 92U
#define GPT_ENTRY_MIN_SIZE 128U
#define GPT_ENTRY_MAX_SIZE 4096U
#define GPT_ENTRY_COUNT_LIMIT 4096U
#define GPT_ENTRY_ARRAY_RESERVE 16384U
#define GPT_MBR_TABLE 0x1beU
#define GPT_MBR_ENTRY_SIZE 16U

struct gpt_copy {
	uint64_t header_lba;
	uint64_t alternate_lba;
	uint64_t first_usable;
	uint64_t last_usable;
	uint64_t table_lba;
	uint64_t table_bytes;
	uint32_t entry_count;
	uint32_t entry_size;
	uint32_t header_size;
	uint8_t disk_guid[16U];
	unsigned active_count;
};

struct gpt_record {
	uint8_t unique_guid[16U];
	uint64_t first;
	uint64_t last;
};

static uint32_t get32(const uint8_t *p);
static uint64_t get64(const uint8_t *p);
static int all_zero(const uint8_t *p, size_t size);
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size);
static uint32_t crc32(const uint8_t *data, size_t size);
static void decimal_u64(char output[21U], uint64_t value);
static int canonical_protective_mbr(struct disk *disk, uint8_t *block, uint32_t *advertised_blocks);
static int protective_mbr_matches_extent(uint32_t advertised_blocks, uint64_t logical_last);
static int pure_protective_mbr(struct disk *disk, uint8_t *block);
static uint64_t conventional_reserve_blocks(const struct disk *disk);
static int zero_declared_backup_reservation(struct disk *disk, uint64_t logical_last, uint8_t *block);
static int read_table_bytes(struct disk *disk, uint64_t table_lba, uint64_t offset, uint8_t *output, size_t size, uint8_t *block);
static int table_crc(struct disk *disk, const struct gpt_copy *copy, uint8_t *block, uint32_t *result);
static char hex(unsigned value);
static void guid_text(char output[PARTITION_UUID_MAX], const uint8_t guid[16U]);
static void utf8_emit(char output[PARTITION_LABEL_MAX], unsigned *at, uint32_t value);
static int gpt_name(char output[PARTITION_LABEL_MAX], const uint8_t raw[72U]);
static int header_layout(struct disk *disk, struct gpt_copy *copy, int primary, uint64_t logical_last);
static int validate_entries(struct disk *disk, struct gpt_copy *copy, struct partition *entries, unsigned capacity, uint8_t *block);
static int read_header(struct disk *disk, uint64_t header_lba, struct gpt_copy *copy, uint32_t *expected_table_crc, uint8_t *block);
static int primary_header_extent(struct disk *disk, uint64_t *logical_last);
static int validate_copy(struct disk *disk, uint64_t header_lba, int primary, uint64_t logical_last, struct gpt_copy *copy, struct partition *entries, unsigned capacity);
static int copy_headers_equal(const struct gpt_copy *left, const struct gpt_copy *right);
static int copy_tables_equal(struct disk *disk, const struct gpt_copy *left, const struct gpt_copy *right);
static int recover_backup_candidates(struct disk *disk, uint32_t protective_blocks, uint64_t physical_last, int primary_error, struct gpt_copy *output_copy, struct partition *output_entries, unsigned capacity, uint64_t *output_logical_last);
static int intentional_primary_only(struct disk *disk, const struct gpt_copy *primary, uint64_t logical_last, uint8_t *block);
static int gpt_scan(const struct partition_scheme *scheme, struct disk *disk, struct partition *entries, unsigned capacity);

const struct partition_scheme drv_partition_scheme_gpt = {
	.name = "gpt",
	.scan = gpt_scan,
};

/* Reads a 32-bit field, least significant byte first. */
static uint32_t
get32(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* Reads a 64-bit field, least significant byte first. */
static uint64_t
get64(
	const uint8_t *p)
{
	uint64_t function_result;

	/* Computes the function result. */
	function_result = (uint64_t)get32(p) | ((uint64_t)get32(p + 4U) << 32);

	/* Returns the computed result. */
	return function_result;
}

/* Asks whether a run of bytes is entirely zero. */
static int
all_zero(
	const uint8_t *p,
	size_t size)
{
	/* Process each remaining element. */
	while (size-- != 0U) {
		/* Checks the current pointer. */
		if (*p++ != 0U)
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Folds more bytes into a running checksum. */
static uint32_t
crc32_update(
	uint32_t crc,
	const uint8_t *data,
	size_t size)
{
	unsigned bit;

	/* Process each remaining element. */
	while (size-- != 0U) {
		crc ^= *data++;
		/* Process each element required by the operation. */
		for (bit = 0; bit < 8U; bit++) {
			crc = (crc >> 1) ^
			      (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
		}
	}

	/* Returns the computed result. */
	return crc;
}

/* Computes the checksum of a run of bytes. */
static uint32_t
crc32(
	const uint8_t *data,
	size_t size)
{
	uint32_t function_result;

	/* Computes the function result. */
	function_result = ~crc32_update(UINT32_MAX, data, size);

	/* Returns the computed result. */
	return function_result;
}

/* Renders a value as decimal digits. */
static void
decimal_u64(
	char output[21U],
	uint64_t value)
{
	char reverse[20U];
	unsigned count = 0U, index;

	do {
		reverse[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0U);
	/* Process each remaining element. */
	for (index = 0U; index < count; index++)
		output[index] = reverse[count - index - 1U];
	output[count] = '\0';
}

/* Reports what the protective boot record ought to hold. */
static int
canonical_protective_mbr(
	struct disk *disk,
	uint8_t *block,
	uint32_t *advertised_blocks)
{
	const uint8_t *entry;
	unsigned protective_count = 0U, slot;

	/* Checks the disk read result. */
	if (disk_read(disk, 0U, 1U, block) != 0)
		return -EIO;

	/* Handles the block condition. */
	if (block[510U] != 0x55U || block[511U] != 0xaaU)
		return -EINVAL;

	/* Handles the disk condition. */
	if (disk->d_block_count < 2U)
		return -EINVAL;

	/*
	 * The shared BIOS/UEFI system image also has a BIOS boot entry.  GPT is
	 * authoritative: require exactly one canonical EE entry and ignore
	 * every non-EE compatibility entry rather than reconciling or
	 * publishing it. CHS is ignored because creators use both geometry and
	 * ff/ff/ff.
	 */
	for (slot = 0U; slot < 4U; slot++) {
		/* Handles the entry condition. */
		entry = block + GPT_MBR_TABLE + slot * GPT_MBR_ENTRY_SIZE;
		if (entry[4U] != 0xeeU)
			continue;
		protective_count++;

		/* Checks the get32 result. */
		if (entry[0U] != 0U || get32(entry + 8U) != 1U ||
		    get32(entry + 12U) == 0U) {
			/* Failed. */
			return -EINVAL;
		}
		*advertised_blocks = get32(entry + 12U);
	}

	/* Handles the protective count condition. */
	if (protective_count != 1U)
		return -EINVAL;

	/* Succeeded. */
	return 0;
}

/* Asks whether that record covers the whole disk. */
static int
protective_mbr_matches_extent(
	uint32_t advertised_blocks,
	uint64_t logical_last)
{
	uint32_t expected =
		logical_last > UINT32_MAX ? UINT32_MAX : (uint32_t)logical_last;

	/* Returns the computed result. */
	return advertised_blocks == expected ? 0 : -EINVAL;
}

/* Refuses a boot record that also holds real partitions. */
static int
pure_protective_mbr(
	struct disk *disk,
	uint8_t *block)
{
	const uint8_t *entry;

	/* Checks the disk read result. */
	if (disk_read(disk, 0U, 1U, block) != 0)
		return -EIO;

	/* Checks the all zero result. */
	entry = block + GPT_MBR_TABLE;
	if (!all_zero(block, GPT_MBR_TABLE) || entry[0U] != 0U ||
	    entry[1U] != 0U || entry[2U] != 2U || entry[3U] != 0U ||
	    entry[4U] != 0xeeU || entry[5U] != 0xffU || entry[6U] != 0xffU ||
	    entry[7U] != 0xffU || get32(entry + 8U) != 1U ||
	    get32(entry + 12U) == 0U ||
	    !all_zero(entry + GPT_MBR_ENTRY_SIZE, 3U * GPT_MBR_ENTRY_SIZE) ||
	    block[510U] != 0x55U || block[511U] != 0xaaU) {
		/* Succeeded. */
		return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Reports how many blocks the table conventionally reserves. */
static uint64_t
conventional_reserve_blocks(
	const struct disk *disk)
{
	uint64_t blocks = GPT_ENTRY_ARRAY_RESERVE / disk->d_block_size;

	/* Handles the disk condition. */
	if (GPT_ENTRY_ARRAY_RESERVE % disk->d_block_size != 0U)
		blocks++;

	/* Returns the computed result. */
	return blocks;
}

/* Asks whether the backup header declares no reservation. */
static int
zero_declared_backup_reservation(
	struct disk *disk,
	uint64_t logical_last,
	uint8_t *block)
{
	uint64_t reserve_blocks = conventional_reserve_blocks(disk);
	uint64_t first, lba;

	/* Handles the logical last condition. */
	if (logical_last <= reserve_blocks + 1U)
		return 0;
	first = logical_last - reserve_blocks;
	/* Process each element required by the operation. */
	for (lba = first; lba <= logical_last; lba++) {
		/* Checks the disk read result. */
		if (disk_read(disk, lba, 1U, block) != 0)
			return -EIO;

		/* Checks the all zero result. */
		if (!all_zero(block, disk->d_block_size))
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Reads the partition table off the disk. */
static int
read_table_bytes(
	struct disk *disk,
	uint64_t table_lba,
	uint64_t offset,
	uint8_t *output,
	size_t size,
	uint8_t *block)
{
	uint64_t lba;
	size_t within;
	size_t amount;
	uint32_t block_size = disk->d_block_size;

	/* Process each remaining element. */
	while (size != 0U) {
		lba = table_lba + offset / block_size;
		within = (size_t)(offset % block_size);

		/* Handles the amount condition. */
		amount = block_size - within;
		if (amount > size)
			amount = size;

		/* Checks the disk read result. */
		if (disk_read(disk, lba, 1U, block) != 0)
			return -EIO;
		memcpy(output, block + within, amount);
		output += amount;
		offset += amount;
		size -= amount;
	}

	/* Succeeded. */
	return 0;
}

/* Computes the checksum the table should carry. */
static int
table_crc(
	struct disk *disk,
	const struct gpt_copy *copy,
	uint8_t *block,
	uint32_t *result)
{
	size_t amount;
	uint64_t remaining = copy->table_bytes;
	uint64_t lba = copy->table_lba;
	uint32_t crc = UINT32_MAX;

	/* Continue while the operation condition remains true. */
	while (remaining != 0U) {
		/* Handles the uint64 t condition. */
		amount = disk->d_block_size;
		if ((uint64_t)amount > remaining)
			amount = (size_t)remaining;

		/* Checks the disk read result. */
		if (disk_read(disk, lba++, 1U, block) != 0)
			return -EIO;
		crc = crc32_update(crc, block, amount);
		remaining -= amount;
	}

	*result = ~crc;
	/* Succeeded. */
	return 0;
}

/* Renders one nibble as a hexadecimal character. */
static char
hex(
	unsigned value)
{
	/* Returns the computed result. */
	return (char)(value < 10U ? '0' + value : 'a' + value - 10U);
}

/* Renders a unique identifier in its usual written form. */
static void
guid_text(
	char output[PARTITION_UUID_MAX],
	const uint8_t guid[16U])
{
	static const uint8_t order[16U] = {3U,	2U,  1U,  0U, 5U,  4U,
					   7U,	6U,  8U,  9U, 10U, 11U,
					   12U, 13U, 14U, 15U};
	unsigned at = 0U, i;

	/* Process each element required by the operation. */
	for (i = 0U; i < 16U; i++) {
		/* Checks the current index. */
		if (i == 4U || i == 6U || i == 8U || i == 10U)
			output[at++] = '-';
		output[at++] = hex(guid[order[i]] >> 4);
		output[at++] = hex(guid[order[i]] & 15U);
	}

	output[at] = '\0';
}

/* Appends one code point to a UTF-8 string being built. */
static void
utf8_emit(
	char output[PARTITION_LABEL_MAX],
	unsigned *at,
	uint32_t value)
{
	/* Validates the current value. */
	if (value <= 0x7fU) {
		output[(*at)++] = (char)value;
	} else if (value <= 0x7ffU) {
		output[(*at)++] = (char)(0xc0U | (value >> 6));
		output[(*at)++] = (char)(0x80U | (value & 0x3fU));
	} else if (value <= 0xffffU) {
		output[(*at)++] = (char)(0xe0U | (value >> 12));
		output[(*at)++] = (char)(0x80U | ((value >> 6) & 0x3fU));
		output[(*at)++] = (char)(0x80U | (value & 0x3fU));
	} else {
		output[(*at)++] = (char)(0xf0U | (value >> 18));
		output[(*at)++] = (char)(0x80U | ((value >> 12) & 0x3fU));
		output[(*at)++] = (char)(0x80U | ((value >> 6) & 0x3fU));
		output[(*at)++] = (char)(0x80U | (value & 0x3fU));
	}
}

/* Renders a partition name as UTF-8. */
static int
gpt_name(
	char output[PARTITION_LABEL_MAX],
	const uint8_t raw[72U])
{
	uint16_t second;
	uint16_t first;
	uint32_t value;
	unsigned at = 0U, unit = 0U;

	memset(output, 0, PARTITION_LABEL_MAX);
	/* Continue while the operation condition remains true. */
	while (unit < 36U) {
		first = (uint16_t)raw[unit * 2U] |
			(uint16_t)((uint16_t)raw[unit * 2U + 1U] << 8);

		unit++;

		/* Handles the first condition. */
		if (first == 0U)
			break;

		/* Handles the first condition. */
		if (first >= 0xd800U && first <= 0xdbffU) {
			/* Handles the unit condition. */
			if (unit == 36U)
				return -EINVAL;

			/* Handles the second condition. */
			second = (uint16_t)raw[unit * 2U] |
				 (uint16_t)((uint16_t)raw[unit * 2U + 1U] << 8);
			if (second < 0xdc00U || second > 0xdfffU)
				return -EINVAL;
			unit++;
			value = 0x10000U + ((uint32_t)(first - 0xd800U) << 10) +
				(uint32_t)(second - 0xdc00U);
		} else {
			/* Handles the first condition. */
			if (first >= 0xdc00U && first <= 0xdfffU)
				return -EINVAL;
			value = first;
		}

		/* 36 code units occupy at most 108 UTF-8 bytes. */
		utf8_emit(output, &at, value);
	}

	output[at] = '\0';

	/* Succeeded. */
	return 0;
}

/* Reports where the header says its table lies. */
static int
header_layout(
	struct disk *disk,
	struct gpt_copy *copy,
	int primary,
	uint64_t logical_last)
{
	uint64_t reserve_blocks;
	uint64_t table_blocks;
	uint64_t table_end;

	/* Handles the copy condition. */
	if (copy->entry_count == 0U ||
	    copy->entry_count > GPT_ENTRY_COUNT_LIMIT ||
	    copy->entry_size < GPT_ENTRY_MIN_SIZE ||
	    copy->entry_size > GPT_ENTRY_MAX_SIZE ||
	    copy->entry_size % GPT_ENTRY_MIN_SIZE != 0U ||
	    ((copy->entry_size / GPT_ENTRY_MIN_SIZE) &
	     (copy->entry_size / GPT_ENTRY_MIN_SIZE - 1U)) != 0U) {
		/* Failed. */
		return -EINVAL;
	}

	/* Handles the uint64 t condition. */
	if ((uint64_t)copy->entry_count >
	    UINT64_MAX / (uint64_t)copy->entry_size) {
		/* Failed. */
		return -EOVERFLOW;
	}
	copy->table_bytes = (uint64_t)copy->entry_count * copy->entry_size;

	/* Handles the copy condition. */
	table_blocks = copy->table_bytes / disk->d_block_size;
	if (copy->table_bytes % disk->d_block_size != 0U)
		table_blocks++;

	/* Handles the disk condition. */
	reserve_blocks = GPT_ENTRY_ARRAY_RESERVE / disk->d_block_size;
	if (GPT_ENTRY_ARRAY_RESERVE % disk->d_block_size != 0U)
		reserve_blocks++;

	/* Handles the copy condition. */
	if (copy->table_lba > logical_last ||
	    table_blocks > logical_last - copy->table_lba + 1U) {
		/* Failed. */
		return -EINVAL;
	}
	table_end = copy->table_lba + table_blocks;

	/* Handles the copy condition. */
	if (copy->first_usable < 2U || copy->first_usable > copy->last_usable ||
	    copy->last_usable >= logical_last) {
		/* Failed. */
		return -EINVAL;
	}

	/* Handles the primary condition. */
	if (primary) {
		/* Handles the copy condition. */
		if (copy->table_lba < 2U || table_end > copy->first_usable ||
		    copy->first_usable - copy->table_lba < reserve_blocks) {
			/* Failed. */
			return -EINVAL;
		}
	} else if (copy->table_lba <= copy->last_usable ||
		   table_end > copy->header_lba ||
		   copy->header_lba - copy->table_lba < reserve_blocks) {
		/* Failed. */
		return -EINVAL;
	}

	/* Succeeded. */
	return 0;
}

/* Refuses entries that overlap or leave the disk. */
static int
validate_entries(
	struct disk *disk,
	struct gpt_copy *copy,
	struct partition *entries,
	unsigned capacity,
	uint8_t *block)
{
	struct partition *entry;
	struct gpt_record *record;
	uint64_t offset;
	uint64_t first, last;
	char label[PARTITION_LABEL_MAX];
	unsigned prior;
	struct gpt_record *records;
	uint8_t *raw;
	unsigned active = 0U, index;
	int error = 0;

	records = kern_calloc(copy->entry_count, sizeof(*records));

	/* Handles the records availability. */
	raw = kern_malloc(copy->entry_size);
	if (records == NULL || raw == NULL) {
		kern_free(records);
		kern_free(raw);

		/* Failed. */
		return -ENOMEM;
	}

	/* Process each remaining element. */
	for (index = 0U; index < copy->entry_count; index++) {
		offset = (uint64_t)index * copy->entry_size;

		/* Checks the operation status. */
		error = read_table_bytes(disk, copy->table_lba, offset, raw,
					 copy->entry_size, block);
		if (error != 0)
			break;

		/* Checks the all zero result. */
		if (!all_zero(raw + GPT_ENTRY_MIN_SIZE,
			      copy->entry_size - GPT_ENTRY_MIN_SIZE)) {
			error = -EINVAL;
			break;
		}

		/* Handles the all zero condition. */
		if (all_zero(raw, 16U))
			continue;

		/* Handles the all zero condition. */
		if (all_zero(raw + 16U, 16U)) {
			error = -EINVAL;
			break;
		}

		first = get64(raw + 32U);

		/* Checks the get64 result. */
		last = get64(raw + 40U);
		if ((get64(raw + 48U) & UINT64_C(0x0000fffffffffff8)) != 0U ||
		    first < copy->first_usable || last > copy->last_usable ||
		    first > last || gpt_name(label, raw + 56U) != 0) {
			error = -EINVAL;
			break;
		}

		/* Process each element required by the operation. */
		for (prior = 0U; prior < active; prior++) {
			/* Handles the memcmp condition. */
			if (memcmp(records[prior].unique_guid, raw + 16U,
				   16U) == 0 ||
			    (first <= records[prior].last &&
			     records[prior].first <= last)) {
				error = -EINVAL;
				break;
			}
		}

		/* Checks the operation status. */
		if (error != 0)
			break;
		record = &records[active];
		memcpy(record->unique_guid, raw + 16U, 16U);
		record->first = first;
		record->last = last;

		/* Handles the active condition. */
		if (active < capacity) {
			entry = &entries[active];

			/* Describes the partition this entry covers. */
			memset(entry, 0, sizeof(*entry));
			entry->p_parent = disk;
			entry->p_index = index;
			entry->p_start_block = first;
			entry->p_data_block = first;
			entry->p_block_count = last - first + 1U;
			guid_text(entry->p_uuid, raw + 16U);
			entry->p_flags = PARTITION_HAS_UUID;

			/* Handles the label condition. */
			if (label[0] != '\0') {
				memcpy(entry->p_label, label,
				       sizeof(entry->p_label));
				entry->p_flags |= PARTITION_HAS_LABEL;
			}
		}

		active++;
	}

	copy->active_count = active;
	kern_free(records);
	kern_free(raw);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads and checks one of the two table headers. */
static int
read_header(
	struct disk *disk,
	uint64_t header_lba,
	struct gpt_copy *copy,
	uint32_t *expected_table_crc,
	uint8_t *block)
{
	uint32_t expected_header_crc;
	uint32_t header_size;

	memset(copy, 0, sizeof(*copy));

	/* Checks the disk read result. */
	if (disk_read(disk, header_lba, 1U, block) != 0)
		return -EIO;

	/* Checks the get32 result. */
	if (memcmp(block, "EFI PART", 8U) != 0 ||
	    get32(block + 8U) != 0x00010000U) {
		/* Failed. */
		return -EINVAL;
	}

	/* Checks the get32 result. */
	header_size = get32(block + 12U);
	if (header_size < GPT_HEADER_MIN_SIZE ||
	    header_size > disk->d_block_size || get32(block + 20U) != 0U ||
	    !all_zero(block + GPT_HEADER_MIN_SIZE,
		      disk->d_block_size - GPT_HEADER_MIN_SIZE)) {
		/* Failed. */
		return -EINVAL;
	}
	expected_header_crc = get32(block + 16U);
	memset(block + 16U, 0, 4U);

	/* Checks the crc32 result. */
	if (crc32(block, header_size) != expected_header_crc)
		return -EINVAL;
	copy->header_size = header_size;
	copy->header_lba = get64(block + 24U);
	copy->alternate_lba = get64(block + 32U);
	copy->first_usable = get64(block + 40U);
	copy->last_usable = get64(block + 48U);
	memcpy(copy->disk_guid, block + 56U, sizeof(copy->disk_guid));
	copy->table_lba = get64(block + 72U);
	copy->entry_count = get32(block + 80U);
	copy->entry_size = get32(block + 84U);
	*expected_table_crc = get32(block + 88U);
	/* Checks the all zero result. */
	if (copy->header_lba != header_lba ||
	    all_zero(copy->disk_guid, sizeof(copy->disk_guid))) {
		/* Failed. */
		return -EINVAL;
	}

	/* Succeeded. */
	return 0;
}

/* Reports the area the primary header describes. */
static int
primary_header_extent(
	struct disk *disk,
	uint64_t *logical_last)
{
	struct gpt_copy primary;
	uint8_t *block;
	uint32_t ignored_table_crc;
	int error;

	/* Handles the block availability. */
	block = kern_malloc(disk->d_block_size);
	if (block == NULL)
		return -ENOMEM;

	/* Checks the operation status. */
	error = read_header(disk, 1U, &primary, &ignored_table_crc, block);
	if (error == 0 && (primary.alternate_lba <= 1U ||
			   primary.alternate_lba >= disk->d_block_count))
		error = -EINVAL;

	/*
	 * A CRC-valid header is not yet a usable authority for an extent.  Its
	 * declared geometry must also be internally valid before its alternate
	 * LBA can suppress the two conventional backup candidates.  Table and
	 * entry validation remains in validate_copy(): damage there does not
	 * invalidate a structurally sound header's named-backup location.
	 */
	if (error == 0)
		error = header_layout(disk, &primary, 1, primary.alternate_lba);
	if (error == 0)
		*logical_last = primary.alternate_lba;
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Refuses a backup that does not agree with the primary. */
static int
validate_copy(
	struct disk *disk,
	uint64_t header_lba,
	int primary,
	uint64_t logical_last,
	struct gpt_copy *copy,
	struct partition *entries,
	unsigned capacity)
{
	uint8_t *block;
	uint32_t expected_table_crc, actual_table_crc;
	int error;

	/* Handles the block availability. */
	block = kern_malloc(disk->d_block_size);
	if (block == NULL)
		return -ENOMEM;

	/* Checks the operation status. */
	error = read_header(disk, header_lba, copy, &expected_table_crc, block);
	if (error != 0)
		goto out;

	/* Handles the copy condition. */
	if (copy->alternate_lba != (primary ? logical_last : 1U)) {
		error = -EINVAL;
		goto out;
	}

	/* Checks the operation status. */
	error = header_layout(disk, copy, primary, logical_last);
	if (error != 0)
		goto out;

	/* Checks the operation status. */
	error = table_crc(disk, copy, block, &actual_table_crc);
	if (error != 0)
		goto out;

	/* Handles the actual table crc condition. */
	if (actual_table_crc != expected_table_crc) {
		error = -EINVAL;
		goto out;
	}

	error = validate_entries(disk, copy, entries, capacity, block);
out:
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Asks whether the two headers agree. */
static int
copy_headers_equal(
	const struct gpt_copy *left,
	const struct gpt_copy *right)
{
	int error;

	/* Computes the function result. */
	error = left->first_usable == right->first_usable &&
			  left->last_usable == right->last_usable &&
			  left->header_size == right->header_size &&
			  left->entry_count == right->entry_count &&
			  left->entry_size == right->entry_size &&
			  left->table_bytes == right->table_bytes &&
			  left->active_count == right->active_count &&
			  memcmp(left->disk_guid, right->disk_guid,
				 sizeof(left->disk_guid)) == 0;

	/* Returns the computed result. */
	return error;
}

/* Asks whether the two tables agree. */
static int
copy_tables_equal(
	struct disk *disk,
	const struct gpt_copy *left,
	const struct gpt_copy *right)
{
	size_t amount;
	uint8_t *left_block, *right_block;
	uint64_t remaining = left->table_bytes;
	uint64_t left_lba = left->table_lba;
	uint64_t right_lba = right->table_lba;
	int equal = 0;

	left_block = kern_malloc(disk->d_block_size);

	/* Handles the left block availability. */
	right_block = kern_malloc(disk->d_block_size);
	if (left_block == NULL || right_block == NULL) {
		equal = -ENOMEM;
		goto out;
	}
	while (remaining != 0U) {
		/* Handles the uint64 t condition. */
		amount = disk->d_block_size;
		if ((uint64_t)amount > remaining)
			amount = (size_t)remaining;

		/* Checks the disk read result. */
		if (disk_read(disk, left_lba++, 1U, left_block) != 0 ||
		    disk_read(disk, right_lba++, 1U, right_block) != 0) {
			equal = -EIO;
			goto out;
		}

		/* Handles the memcmp condition. */
		if (memcmp(left_block, right_block, amount) != 0) {
			equal = 0;
			goto out;
		}

		remaining -= amount;
	}

	equal = 1;
out:
	kern_free(left_block);
	kern_free(right_block);

	/* Returns the computed result. */
	return equal;
}

/* A primary header that fails full structural validation cannot authenticate its alternate-LBA field.  The same bounded recovery is used after both copies at a structurally valid header's named extent fail full validation. Inspect only the two conventional, independently bounded backup locations: the Protective-MBR advertised end and the physical medium end.  Never search arbitrary LBAs.  If both locations contain valid copies they must describe exactly the same GPT; otherwise choosing either would turn stale tail metadata into an implicit repair policy. */
static int
recover_backup_candidates(
	struct disk *disk,
	uint32_t protective_blocks,
	uint64_t physical_last,
	int primary_error,
	struct gpt_copy *output_copy,
	struct partition *output_entries,
	unsigned capacity,
	uint64_t *output_logical_last)
{
	char pmbr_text_local[21U], physical_text_local[21U];
	char pmbr_text_local1[21U], physical_text_local2[21U];
	char lba_text[21U];
	struct gpt_copy copies[2U];
	struct partition *candidate_entries[2U] = {output_entries, NULL};
	uint64_t candidate_lbas[2U];
	int candidate_errors[2U] = {-EINVAL, -EINVAL};
	unsigned candidate_count = 0U, chosen, index;
	int equal, error = -EINVAL;

	/* Handles the protective blocks condition. */
	if (protective_blocks > 1U &&
	    (uint64_t)protective_blocks <= physical_last)
		candidate_lbas[candidate_count++] = protective_blocks;

	/* Handles the physical last condition. */
	if (physical_last > 1U &&
	    (candidate_count == 0U || candidate_lbas[0U] != physical_last))
		candidate_lbas[candidate_count++] = physical_last;

	/* Handles the candidate count condition. */
	if (candidate_count == 0U)
		return -EINVAL;

	/* Handles the candidate count condition. */
	if (candidate_count == 2U) {
		candidate_entries[1U] =
			kern_calloc(capacity, sizeof(*candidate_entries[1U]));

		/* Handles the candidate entries condition. */
		if (candidate_entries[1U] == NULL)
			return -ENOMEM;
	}

	/* Process each remaining element. */
	for (index = 0U; index < candidate_count; index++) {
		candidate_errors[index] = validate_copy(
			disk, candidate_lbas[index], 0, candidate_lbas[index],
			&copies[index], candidate_entries[index], capacity);

		/* Checks the operation status. */
		if (candidate_errors[index] == -ENOMEM) {
			error = -ENOMEM;
			goto out;
		}
	}

	/* Checks the operation status. */
	if (candidate_count == 2U && candidate_errors[0U] == 0 &&
	    candidate_errors[1U] == 0) {
		/* Checks the copy headers equal result. */
		if (!copy_headers_equal(&copies[0U], &copies[1U])) {
			decimal_u64(pmbr_text_local, candidate_lbas[0U]);
			decimal_u64(physical_text_local, candidate_lbas[1U]);
			hal_printf("gpt: %s rejected: contradictory backup "
				   "candidates pmbr-lba=%s physical-lba=%s\n",
				   disk->d_name, pmbr_text_local,
				   physical_text_local);
			goto out;
		}

		/* Handles the equal condition. */
		equal = copy_tables_equal(disk, &copies[0U], &copies[1U]);
		if (equal < 0) {
			error = equal;
			goto out;
		}

		/* Handles the equal condition. */
		if (equal == 0) {
			decimal_u64(pmbr_text_local1, candidate_lbas[0U]);
			decimal_u64(physical_text_local2, candidate_lbas[1U]);
			hal_printf("gpt: %s rejected: contradictory backup "
				   "entry arrays pmbr-lba=%s physical-lba=%s\n",
				   disk->d_name, pmbr_text_local1,
				   physical_text_local2);
			goto out;
		}

		/*
		 * Prefer the PMBR-bounded copy when both copies are identical.
		 */
		chosen = 0U;
	} else if (candidate_errors[0U] == 0) {
		chosen = 0U;
	} else if (candidate_count == 2U && candidate_errors[1U] == 0) {
		chosen = 1U;
	} else {
		/* Handles the candidate count condition. */
		if (candidate_count == 2U) {
			hal_printf(
				"gpt: %s rejected: primary=%d pmbr-backup=%d "
				"physical-backup=%d\n",
				disk->d_name, -primary_error,
				-candidate_errors[0U], -candidate_errors[1U]);
		} else {
			hal_printf("gpt: %s rejected: primary=%d backup=%d\n",
				   disk->d_name, -primary_error,
				   -candidate_errors[0U]);
		}

		goto out;
	}

	*output_copy = copies[chosen];
	/* Handles the chosen condition. */
	if (chosen != 0U) {
		memcpy(output_entries, candidate_entries[chosen],
		       capacity * sizeof(*output_entries));
	}

	*output_logical_last = candidate_lbas[chosen];

	/* Reports that the table came from the backup copy. */
	decimal_u64(lba_text, candidate_lbas[chosen]);
	hal_printf("gpt: %s primary damaged (%d), using backup at LBA %s "
		   "read-only\n",
		   disk->d_name, -primary_error, lba_text);
	error = 0;
out:
	kern_free(candidate_entries[1U]);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* The UEFI-only image deliberately omits the conventional backup array and header while reserving their logical final blocks as zero.  This strict shape remains self-contained when copied to a larger physical medium. Keep the probe independent of attacker-controlled usable-range values: it reads only the MBR and the 16-KiB array reservation plus one header block. A matching shape with nonzero reserved blocks is not classified as an intentional omission; exact-media degraded-copy recovery remains separate. */
static int
intentional_primary_only(
	struct disk *disk,
	const struct gpt_copy *primary,
	uint64_t logical_last,
	uint8_t *block)
{
	int error;
	uint64_t reserve_blocks;
	int result;

	/* Handles the logical last condition. */
	reserve_blocks = conventional_reserve_blocks(disk);
	if (logical_last <= reserve_blocks + 1U || primary->header_lba != 1U ||
	    primary->alternate_lba != logical_last ||
	    primary->table_lba != 2U || primary->entry_count != 128U ||
	    primary->entry_size != 128U ||
	    primary->table_bytes != GPT_ENTRY_ARRAY_RESERVE ||
	    primary->first_usable != 2U + reserve_blocks ||
	    primary->last_usable != logical_last - reserve_blocks - 1U) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the operation result. */
	result = pure_protective_mbr(disk, block);
	if (result != 1)
		return result;

	/* Obtains the zero declared backup reservation result. */
	error =
		zero_declared_backup_reservation(disk, logical_last, block);

	/* Returns the computed result. */
	return error;
}

/* Reads a disk's partition table and publishes what it holds. */
static int
gpt_scan(
	const struct partition_scheme *scheme,
	struct disk *disk,
	struct partition *entries,
	unsigned capacity)
{
	char advertised_last_text[21U], gpt_last_text[21U];
	char logical_last_text[21U], physical_last_text[21U];
	char declared_sectors_text[21U], physical_sectors_text[21U];
	char ignored_tail_text[21U];
	struct gpt_copy primary, backup;
	struct partition *primary_entries = NULL, *backup_entries = NULL;
	const struct gpt_copy *selected;
	const struct partition *selected_entries;
	uint8_t *block;
	uint64_t logical_last = 0U, physical_last;
	uint32_t protective_blocks = 0U;
	int bounded = 0;
	int protective_mismatch = 0;
	int primary_only = 0;
	int primary_error, backup_error, equal;
	int error = -EINVAL;

	(void)scheme;

	/* Handles the disk availability. */
	if (disk == NULL || entries == NULL || capacity == 0U ||
	    capacity > PARTITION_POOL_MAX ||
	    (disk->d_block_size != 512U && disk->d_block_size != 4096U) ||
	    disk->d_block_count < 4U) {
		/* Failed. */
		return -EINVAL;
	}
	block = kern_malloc(disk->d_block_size);
	primary_entries = kern_calloc(capacity, sizeof(*primary_entries));

	/* Handles the block availability. */
	backup_entries = kern_calloc(capacity, sizeof(*backup_entries));
	if (block == NULL || primary_entries == NULL ||
	    backup_entries == NULL) {
		error = -ENOMEM;
		goto out;
	}

	/* Checks the operation status. */
	error = canonical_protective_mbr(disk, block, &protective_blocks);
	if (error != 0) {
		hal_printf("gpt: %s rejected: invalid protective MBR (%d)\n",
			   disk->d_name, -error);
		goto out;
	}

	physical_last = disk->d_block_count - 1U;

	/* Checks the operation status. */
	error = primary_header_extent(disk, &logical_last);
	if (error == -ENOMEM)
		goto out;
	if (error != 0) {
		primary_error = error;

		/* Checks the operation status. */
		error = recover_backup_candidates(
			disk, protective_blocks, physical_last, primary_error,
			&backup, backup_entries, capacity, &logical_last);
		if (error != 0)
			goto out;
		selected = &backup;
		selected_entries = backup_entries;
		goto selected_copy;
	}

	/* Checks the operation status. */
	primary_error = validate_copy(disk, 1U, 1, logical_last, &primary,
				      primary_entries, capacity);
	if (primary_error == -ENOMEM) {
		error = -ENOMEM;
		goto out;
	}

	/* Checks the operation status. */
	backup_error = validate_copy(disk, logical_last, 0, logical_last,
				     &backup, backup_entries, capacity);
	if (backup_error == -ENOMEM) {
		error = -ENOMEM;
		goto out;
	}

	/* Checks the operation status. */
	if (primary_error == 0 && backup_error != 0 && backup_error != -EIO) {
		/* Handles the primary only condition. */
		primary_only = intentional_primary_only(disk, &primary,
							logical_last, block);
		if (primary_only < 0)
			primary_only = 0;
	}

	/* Checks the operation status. */
	if (primary_error != 0 && backup_error != 0) {
		/*
		 * The primary header supplied a structurally valid named
		 * extent, but neither named copy survived full table/entry
		 * validation.  Fall back only to the two independently bounded
		 * conventional locations; never search for a third header.
		 */

		/* Checks the operation status. */
		error = recover_backup_candidates(
			disk, protective_blocks, physical_last, primary_error,
			&backup, backup_entries, capacity, &logical_last);
		if (error != 0)
			goto out;
		selected = &backup;
		selected_entries = backup_entries;
		goto selected_copy;
	}

	/* Checks the operation status. */
	if (primary_error == 0 && backup_error == 0) {
		/* Checks the copy headers equal result. */
		if (!copy_headers_equal(&primary, &backup)) {
			hal_printf("gpt: %s rejected: contradictory headers\n",
				   disk->d_name);
			error = -EINVAL;
			goto out;
		}

		/* Handles the equal condition. */
		equal = copy_tables_equal(disk, &primary, &backup);
		if (equal < 0) {
			error = equal;
			goto out;
		}

		/* Handles the equal condition. */
		if (equal == 0) {
			hal_printf("gpt: %s rejected: contradictory entry "
				   "arrays\n",
				   disk->d_name);
			error = -EINVAL;
			goto out;
		}

		selected = &primary;
		selected_entries = primary_entries;
	} else if (primary_error == 0) {
		/* Handles the primary only condition. */
		if (primary_only != 0) {
			hal_printf(
				"gpt: %s intentional primary-only GPT accepted "
				"read-only\n",
				disk->d_name);
		} else {
			hal_printf("gpt: %s backup damaged (%d), using primary "
				   "read-only\n",
				   disk->d_name, -backup_error);
		}

		selected = &primary;
		selected_entries = primary_entries;
	} else {
		hal_printf("gpt: %s primary damaged (%d), using backup "
			   "read-only\n",
			   disk->d_name, -primary_error);
		selected = &backup;
		selected_entries = backup_entries;
	}

selected_copy:
	bounded = logical_last < physical_last;

	/* Checks the protective mbr matches extent result. */
	if (protective_mbr_matches_extent(protective_blocks, logical_last) != 0)
		protective_mismatch = 1;

	/* Handles the selected condition. */
	if (selected->active_count > capacity) {
		error = -ENOSPC;
		goto out;
	}

	/* Handles the protective mismatch condition. */
	if (protective_mismatch) {
		decimal_u64(advertised_last_text, protective_blocks);
		decimal_u64(gpt_last_text, logical_last);
		hal_printf("gpt: %s protective MBR extent mismatch: "
			   "advertised-last=%s gpt-last=%s; using GPT\n",
			   disk->d_name, advertised_last_text, gpt_last_text);
	}

	memcpy(entries, selected_entries,
	       selected->active_count * sizeof(*entries));

	/* Handles the bounded condition. */
	if (bounded) {
		decimal_u64(logical_last_text, logical_last);
		decimal_u64(physical_last_text, physical_last);
		decimal_u64(declared_sectors_text, logical_last + 1U);
		decimal_u64(physical_sectors_text, physical_last + 1U);
		decimal_u64(ignored_tail_text, physical_last - logical_last);
		hal_printf("gpt: %s bounded extent accepted: logical-last=%s "
			   "physical-last=%s declared-sectors=%s "
			   "physical-sectors=%s "
			   "ignored-tail-sectors=%s\n",
			   disk->d_name, logical_last_text, physical_last_text,
			   declared_sectors_text, physical_sectors_text,
			   ignored_tail_text);
	}

	error = (int)selected->active_count;
out:
	kern_free(block);
	kern_free(primary_entries);
	kern_free(backup_entries);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
