/* Strict, read-only GUID Partition Table parser.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
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

static uint32_t
get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
get64(const uint8_t *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4U) << 32);
}

static int
all_zero(const uint8_t *p, size_t size)
{
	while (size-- != 0U)
		if (*p++ != 0U)
			return 0;
	return 1;
}

static uint32_t
crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
	while (size-- != 0U) {
		unsigned bit;

		crc ^= *data++;
		for (bit = 0; bit < 8U; bit++)
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

static void
decimal_u64(char output[21U], uint64_t value)
{
	char reverse[20U];
	unsigned count = 0U, index;

	do {
		reverse[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0U);
	for (index = 0U; index < count; index++)
		output[index] = reverse[count - index - 1U];
	output[count] = '\0';
}

static int
canonical_protective_mbr(struct disk *disk, uint8_t *block,
	uint32_t *advertised_blocks)
{
	unsigned protective_count = 0U, slot;

	if (disk_read(disk, 0U, 1U, block) != 0)
		return -EIO;
	if (block[510U] != 0x55U || block[511U] != 0xaaU)
		return -EINVAL;
	if (disk->d_block_count < 2U)
		return -EINVAL;
	/* The shared BIOS/UEFI system image also has a BIOS boot entry.  GPT is
	 * authoritative: require exactly one canonical EE entry and ignore every
	 * non-EE compatibility entry rather than reconciling or publishing it.
	 * CHS is ignored because creators use both geometry and ff/ff/ff. */
	for (slot = 0U; slot < 4U; slot++) {
		const uint8_t *entry = block + GPT_MBR_TABLE +
		    slot * GPT_MBR_ENTRY_SIZE;

		if (entry[4U] != 0xeeU)
			continue;
		protective_count++;
		if (entry[0U] != 0U || get32(entry + 8U) != 1U ||
		    get32(entry + 12U) == 0U)
			return -EINVAL;
		*advertised_blocks = get32(entry + 12U);
	}
	if (protective_count != 1U)
		return -EINVAL;
	return 0;
}

static int
protective_mbr_covers(uint32_t advertised_blocks, uint64_t logical_last)
{
	uint32_t expected = logical_last > UINT32_MAX ? UINT32_MAX :
	    (uint32_t)logical_last;

	return advertised_blocks == expected ? 0 : -EINVAL;
}

static int
read_table_bytes(struct disk *disk, uint64_t table_lba, uint64_t offset,
	uint8_t *output, size_t size, uint8_t *block)
{
	uint32_t block_size = disk->d_block_size;

	while (size != 0U) {
		uint64_t lba = table_lba + offset / block_size;
		size_t within = (size_t)(offset % block_size);
		size_t amount = block_size - within;

		if (amount > size)
			amount = size;
		if (disk_read(disk, lba, 1U, block) != 0)
			return -EIO;
		memcpy(output, block + within, amount);
		output += amount;
		offset += amount;
		size -= amount;
	}
	return 0;
}

static int
table_crc(struct disk *disk, const struct gpt_copy *copy, uint8_t *block,
	uint32_t *result)
{
	uint64_t remaining = copy->table_bytes;
	uint64_t lba = copy->table_lba;
	uint32_t crc = UINT32_MAX;

	while (remaining != 0U) {
		size_t amount = disk->d_block_size;

		if ((uint64_t)amount > remaining)
			amount = (size_t)remaining;
		if (disk_read(disk, lba++, 1U, block) != 0)
			return -EIO;
		crc = crc32_update(crc, block, amount);
		remaining -= amount;
	}
	*result = ~crc;
	return 0;
}

static char
hex(unsigned value)
{
	return (char)(value < 10U ? '0' + value : 'a' + value - 10U);
}

static void
guid_text(char output[PARTITION_UUID_MAX], const uint8_t guid[16U])
{
	static const uint8_t order[16U] = {
		3U, 2U, 1U, 0U, 5U, 4U, 7U, 6U,
		8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U
	};
	unsigned at = 0U, i;

	for (i = 0U; i < 16U; i++) {
		if (i == 4U || i == 6U || i == 8U || i == 10U)
			output[at++] = '-';
		output[at++] = hex(guid[order[i]] >> 4);
		output[at++] = hex(guid[order[i]] & 15U);
	}
	output[at] = '\0';
}

static void
utf8_emit(char output[PARTITION_LABEL_MAX], unsigned *at, uint32_t value)
{
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

static int
gpt_name(char output[PARTITION_LABEL_MAX], const uint8_t raw[72U])
{
	unsigned at = 0U, unit = 0U;

	memset(output, 0, PARTITION_LABEL_MAX);
	while (unit < 36U) {
		uint16_t first = (uint16_t)raw[unit * 2U] |
		    (uint16_t)((uint16_t)raw[unit * 2U + 1U] << 8);
		uint32_t value;

		unit++;
		if (first == 0U)
			break;
		if (first >= 0xd800U && first <= 0xdbffU) {
			uint16_t second;

			if (unit == 36U)
				return -EINVAL;
			second = (uint16_t)raw[unit * 2U] |
			    (uint16_t)((uint16_t)raw[unit * 2U + 1U] << 8);
			if (second < 0xdc00U || second > 0xdfffU)
				return -EINVAL;
			unit++;
			value = 0x10000U + ((uint32_t)(first - 0xd800U) << 10) +
			    (uint32_t)(second - 0xdc00U);
		} else {
			if (first >= 0xdc00U && first <= 0xdfffU)
				return -EINVAL;
			value = first;
		}
		/* 36 code units occupy at most 108 UTF-8 bytes. */
		utf8_emit(output, &at, value);
	}
	output[at] = '\0';
	return 0;
}

static int
header_layout(struct disk *disk, struct gpt_copy *copy, int primary,
	uint64_t logical_last)
{
	uint64_t reserve_blocks;
	uint64_t table_blocks;
	uint64_t table_end;

	if (copy->entry_count == 0U ||
	    copy->entry_count > GPT_ENTRY_COUNT_LIMIT ||
	    copy->entry_size < GPT_ENTRY_MIN_SIZE ||
	    copy->entry_size > GPT_ENTRY_MAX_SIZE ||
	    copy->entry_size % GPT_ENTRY_MIN_SIZE != 0U ||
	    ((copy->entry_size / GPT_ENTRY_MIN_SIZE) &
	    (copy->entry_size / GPT_ENTRY_MIN_SIZE - 1U)) != 0U)
		return -EINVAL;
	if ((uint64_t)copy->entry_count >
	    UINT64_MAX / (uint64_t)copy->entry_size)
		return -EOVERFLOW;
	copy->table_bytes = (uint64_t)copy->entry_count * copy->entry_size;
	table_blocks = copy->table_bytes / disk->d_block_size;
	if (copy->table_bytes % disk->d_block_size != 0U)
		table_blocks++;
	reserve_blocks = GPT_ENTRY_ARRAY_RESERVE / disk->d_block_size;
	if (GPT_ENTRY_ARRAY_RESERVE % disk->d_block_size != 0U)
		reserve_blocks++;
	if (copy->table_lba > logical_last ||
	    table_blocks > logical_last - copy->table_lba + 1U)
		return -EINVAL;
	table_end = copy->table_lba + table_blocks;
	if (copy->first_usable < 2U ||
	    copy->first_usable > copy->last_usable ||
	    copy->last_usable >= logical_last)
		return -EINVAL;
	if (primary) {
		if (copy->table_lba < 2U || table_end > copy->first_usable ||
		    copy->first_usable - copy->table_lba < reserve_blocks)
			return -EINVAL;
	} else if (copy->table_lba <= copy->last_usable ||
	    table_end > copy->header_lba ||
	    copy->header_lba - copy->table_lba < reserve_blocks) {
		return -EINVAL;
	}
	return 0;
}

static int
validate_entries(struct disk *disk, struct gpt_copy *copy,
	struct partition *entries, unsigned capacity, uint8_t *block)
{
	struct gpt_record *records;
	uint8_t *raw;
	unsigned active = 0U, index;
	int error = 0;

	records = kern_calloc(copy->entry_count, sizeof(*records));
	raw = kern_malloc(copy->entry_size);
	if (records == NULL || raw == NULL) {
		kern_free(records);
		kern_free(raw);
		return -ENOMEM;
	}
	for (index = 0U; index < copy->entry_count; index++) {
		struct gpt_record *record;
		uint64_t offset = (uint64_t)index * copy->entry_size;
		uint64_t first, last;
		char label[PARTITION_LABEL_MAX];
		unsigned prior;

		error = read_table_bytes(disk, copy->table_lba, offset, raw,
		    copy->entry_size, block);
		if (error != 0)
			break;
		if (!all_zero(raw + GPT_ENTRY_MIN_SIZE,
		    copy->entry_size - GPT_ENTRY_MIN_SIZE)) {
			error = -EINVAL;
			break;
		}
		if (all_zero(raw, 16U))
			continue;
		if (all_zero(raw + 16U, 16U)) {
			error = -EINVAL;
			break;
		}
		first = get64(raw + 32U);
		last = get64(raw + 40U);
		if ((get64(raw + 48U) & UINT64_C(0x0000fffffffffff8)) != 0U ||
		    first < copy->first_usable || last > copy->last_usable ||
		    first > last || gpt_name(label, raw + 56U) != 0) {
			error = -EINVAL;
			break;
		}
		for (prior = 0U; prior < active; prior++) {
			if (memcmp(records[prior].unique_guid, raw + 16U, 16U) == 0 ||
			    (first <= records[prior].last &&
			    records[prior].first <= last)) {
				error = -EINVAL;
				break;
			}
		}
		if (error != 0)
			break;
		record = &records[active];
		memcpy(record->unique_guid, raw + 16U, 16U);
		record->first = first;
		record->last = last;
		if (active < capacity) {
			struct partition *entry = &entries[active];

			memset(entry, 0, sizeof(*entry));
			entry->p_parent = disk;
			entry->p_index = index;
			entry->p_start_block = first;
			entry->p_data_block = first;
			entry->p_block_count = last - first + 1U;
			guid_text(entry->p_uuid, raw + 16U);
			entry->p_flags = PARTITION_HAS_UUID;
			if (label[0] != '\0') {
				memcpy(entry->p_label, label, sizeof(entry->p_label));
				entry->p_flags |= PARTITION_HAS_LABEL;
			}
		}
		active++;
	}
	copy->active_count = active;
	kern_free(records);
	kern_free(raw);
	return error;
}

static int
read_header(struct disk *disk, uint64_t header_lba, struct gpt_copy *copy,
	uint32_t *expected_table_crc, uint8_t *block)
{
	uint32_t expected_header_crc;
	uint32_t header_size;

	memset(copy, 0, sizeof(*copy));
	if (disk_read(disk, header_lba, 1U, block) != 0)
		return -EIO;
	if (memcmp(block, "EFI PART", 8U) != 0 ||
	    get32(block + 8U) != 0x00010000U)
		return -EINVAL;
	header_size = get32(block + 12U);
	if (header_size < GPT_HEADER_MIN_SIZE ||
	    header_size > disk->d_block_size || get32(block + 20U) != 0U ||
	    !all_zero(block + GPT_HEADER_MIN_SIZE,
	    disk->d_block_size - GPT_HEADER_MIN_SIZE))
		return -EINVAL;
	expected_header_crc = get32(block + 16U);
	memset(block + 16U, 0, 4U);
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
	if (copy->header_lba != header_lba ||
	    all_zero(copy->disk_guid, sizeof(copy->disk_guid)))
		return -EINVAL;
	return 0;
}

static int
primary_header_extent(struct disk *disk, uint64_t *logical_last)
{
	struct gpt_copy primary;
	uint8_t *block;
	uint32_t ignored_table_crc;
	int error;

	block = kern_malloc(disk->d_block_size);
	if (block == NULL)
		return -ENOMEM;
	error = read_header(disk, 1U, &primary, &ignored_table_crc, block);
	if (error == 0 && (primary.alternate_lba <= 1U ||
	    primary.alternate_lba >= disk->d_block_count))
		error = -EINVAL;
	if (error == 0)
		*logical_last = primary.alternate_lba;
	kern_free(block);
	return error;
}

static int
validate_copy(struct disk *disk, uint64_t header_lba, int primary,
	uint64_t logical_last,
	struct gpt_copy *copy, struct partition *entries, unsigned capacity)
{
	uint8_t *block;
	uint32_t expected_table_crc, actual_table_crc;
	int error;

	block = kern_malloc(disk->d_block_size);
	if (block == NULL)
		return -ENOMEM;
	error = read_header(disk, header_lba, copy, &expected_table_crc, block);
	if (error != 0)
		goto out;
	if (copy->alternate_lba != (primary ? logical_last : 1U)) {
		error = -EINVAL;
		goto out;
	}
	error = header_layout(disk, copy, primary, logical_last);
	if (error != 0)
		goto out;
	error = table_crc(disk, copy, block, &actual_table_crc);
	if (error != 0)
		goto out;
	if (actual_table_crc != expected_table_crc) {
		error = -EINVAL;
		goto out;
	}
	error = validate_entries(disk, copy, entries, capacity, block);
out:
	kern_free(block);
	return error;
}

static int
copy_headers_equal(const struct gpt_copy *left, const struct gpt_copy *right)
{
	return left->first_usable == right->first_usable &&
	    left->last_usable == right->last_usable &&
	    left->header_size == right->header_size &&
	    left->entry_count == right->entry_count &&
	    left->entry_size == right->entry_size &&
	    left->table_bytes == right->table_bytes &&
	    left->active_count == right->active_count &&
	    memcmp(left->disk_guid, right->disk_guid,
	    sizeof(left->disk_guid)) == 0;
}

static int
copy_tables_equal(struct disk *disk, const struct gpt_copy *left,
	const struct gpt_copy *right)
{
	uint8_t *left_block, *right_block;
	uint64_t remaining = left->table_bytes;
	uint64_t left_lba = left->table_lba;
	uint64_t right_lba = right->table_lba;
	int equal = 0;

	left_block = kern_malloc(disk->d_block_size);
	right_block = kern_malloc(disk->d_block_size);
	if (left_block == NULL || right_block == NULL) {
		equal = -ENOMEM;
		goto out;
	}
	while (remaining != 0U) {
		size_t amount = disk->d_block_size;

		if ((uint64_t)amount > remaining)
			amount = (size_t)remaining;
		if (disk_read(disk, left_lba++, 1U, left_block) != 0 ||
		    disk_read(disk, right_lba++, 1U, right_block) != 0) {
			equal = -EIO;
			goto out;
		}
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
	return equal;
}

/* The UEFI-only image deliberately omits the conventional backup array and
 * header while reserving their logical final blocks as zero.  This strict
 * shape remains self-contained when copied to a larger physical medium.
 * Keep the probe independent of attacker-controlled usable-range values: it
 * reads only the MBR and the 16-KiB array reservation plus one header block.
 * A matching shape with nonzero reserved blocks is not classified as an
 * intentional omission; exact-media degraded-copy recovery remains separate.
 */
static int
intentional_primary_only(struct disk *disk, const struct gpt_copy *primary,
	uint64_t logical_last, uint8_t *block)
{
	const uint8_t *entry;
	uint64_t reserve_blocks, first, lba;
	uint32_t advertised;

	reserve_blocks = GPT_ENTRY_ARRAY_RESERVE / disk->d_block_size;
	if (GPT_ENTRY_ARRAY_RESERVE % disk->d_block_size != 0U)
		reserve_blocks++;
	if (logical_last <= reserve_blocks + 1U ||
	    primary->header_lba != 1U || primary->alternate_lba != logical_last ||
	    primary->table_lba != 2U ||
	    primary->entry_count != 128U || primary->entry_size != 128U ||
	    primary->table_bytes != GPT_ENTRY_ARRAY_RESERVE ||
	    primary->first_usable != 2U + reserve_blocks ||
	    primary->last_usable != logical_last - reserve_blocks - 1U)
		return 0;
	if (disk_read(disk, 0U, 1U, block) != 0)
		return -EIO;
	entry = block + GPT_MBR_TABLE;
	advertised = logical_last > UINT32_MAX ? UINT32_MAX :
	    (uint32_t)logical_last;
	if (!all_zero(block, GPT_MBR_TABLE) ||
	    entry[0U] != 0U || entry[1U] != 0U || entry[2U] != 2U ||
	    entry[3U] != 0U || entry[4U] != 0xeeU || entry[5U] != 0xffU ||
	    entry[6U] != 0xffU || entry[7U] != 0xffU ||
	    get32(entry + 8U) != 1U || get32(entry + 12U) != advertised ||
	    !all_zero(entry + GPT_MBR_ENTRY_SIZE,
	    3U * GPT_MBR_ENTRY_SIZE) || block[510U] != 0x55U ||
	    block[511U] != 0xaaU)
		return 0;
	first = logical_last - reserve_blocks;
	for (lba = first; lba <= logical_last; lba++) {
		if (disk_read(disk, lba, 1U, block) != 0)
			return -EIO;
		if (!all_zero(block, disk->d_block_size))
			return 0;
	}
	return 1;
}

static int
gpt_scan(const struct partition_scheme *scheme, struct disk *disk,
	struct partition *entries, unsigned capacity)
{
	struct gpt_copy primary, backup;
	struct partition *primary_entries = NULL, *backup_entries = NULL;
	const struct gpt_copy *selected;
	const struct partition *selected_entries;
	uint8_t *block;
	uint64_t logical_last = 0U, physical_last;
	uint32_t protective_blocks = 0U;
	int bounded = 0;
	int primary_only = 0;
	int primary_error, backup_error, equal;
	int error = -EINVAL;

	(void)scheme;
	if (disk == NULL || entries == NULL || capacity == 0U ||
	    capacity > PARTITION_POOL_MAX ||
	    (disk->d_block_size != 512U && disk->d_block_size != 4096U) ||
	    disk->d_block_count < 4U)
		return -EINVAL;
	block = kern_malloc(disk->d_block_size);
	primary_entries = kern_calloc(capacity, sizeof(*primary_entries));
	backup_entries = kern_calloc(capacity, sizeof(*backup_entries));
	if (block == NULL || primary_entries == NULL || backup_entries == NULL) {
		error = -ENOMEM;
		goto out;
	}
	error = canonical_protective_mbr(disk, block, &protective_blocks);
	if (error != 0) {
		hal_printf("gpt: %s rejected: invalid protective MBR (%d)\n",
		    disk->d_name, -error);
		goto out;
	}
	physical_last = disk->d_block_count - 1U;
	if (protective_blocks < UINT32_MAX) {
		logical_last = protective_blocks;
		if (logical_last > physical_last) {
			hal_printf("gpt: %s rejected: invalid protective MBR (%d)\n",
			    disk->d_name, EINVAL);
			error = -EINVAL;
			goto out;
		}
	} else if (physical_last < UINT32_MAX) {
		hal_printf("gpt: %s rejected: invalid protective MBR (%d)\n",
		    disk->d_name, EINVAL);
		error = -EINVAL;
		goto out;
	} else if (physical_last == UINT32_MAX) {
		logical_last = physical_last;
	} else {
		error = primary_header_extent(disk, &logical_last);
		if (error != 0) {
			if (error != -ENOMEM)
				hal_printf("gpt: %s rejected: saturated protective "
				    "MBR has no valid primary extent (%d)\n",
				    disk->d_name, -error);
			goto out;
		}
	}
	if (protective_mbr_covers(protective_blocks, logical_last) != 0) {
		hal_printf("gpt: %s rejected: invalid protective MBR (%d)\n",
		    disk->d_name, EINVAL);
		error = -EINVAL;
		goto out;
	}
	bounded = logical_last < physical_last;
	primary_error = validate_copy(disk, 1U, 1, logical_last, &primary,
	    primary_entries, capacity);
	if (primary_error == -ENOMEM) {
		error = -ENOMEM;
		goto out;
	}
	backup_error = validate_copy(disk, logical_last, 0, logical_last, &backup,
	    backup_entries, capacity);
	if (backup_error == -ENOMEM) {
		error = -ENOMEM;
		goto out;
	}
	if (primary_error == 0 && backup_error != 0 && backup_error != -EIO) {
		primary_only = intentional_primary_only(disk, &primary,
		    logical_last, block);
		if (primary_only < 0) {
			if (bounded) {
				error = -EINVAL;
				goto out;
			}
			primary_only = 0;
		}
	}
	if (bounded && (primary_error != 0 ||
	    (backup_error != 0 && primary_only == 0))) {
		hal_printf("gpt: %s rejected: bounded extent requires both copies "
		    "primary=%d backup=%d\n", disk->d_name, -primary_error,
		    -backup_error);
		error = -EINVAL;
		goto out;
	}
	if (primary_error != 0 && backup_error != 0) {
		hal_printf("gpt: %s rejected: primary=%d backup=%d\n",
		    disk->d_name, -primary_error, -backup_error);
		error = primary_error == -ENOSPC || backup_error == -ENOSPC ?
		    -ENOSPC : -EINVAL;
		goto out;
	}
	if (primary_error == 0 && backup_error == 0) {
		if (!copy_headers_equal(&primary, &backup)) {
			hal_printf("gpt: %s rejected: contradictory headers\n",
			    disk->d_name);
			error = -EINVAL;
			goto out;
		}
		equal = copy_tables_equal(disk, &primary, &backup);
		if (equal < 0) {
			error = equal;
			goto out;
		}
		if (equal == 0) {
			hal_printf("gpt: %s rejected: contradictory entry arrays\n",
			    disk->d_name);
			error = -EINVAL;
			goto out;
		}
		selected = &primary;
		selected_entries = primary_entries;
	} else if (primary_error == 0) {
		if (primary_only != 0)
			hal_printf("gpt: %s intentional primary-only GPT accepted "
			    "read-only\n", disk->d_name);
		else
			hal_printf("gpt: %s backup damaged (%d), using primary "
			    "read-only\n", disk->d_name, -backup_error);
		selected = &primary;
		selected_entries = primary_entries;
	} else {
		hal_printf("gpt: %s primary damaged (%d), using backup read-only\n",
		    disk->d_name, -primary_error);
		selected = &backup;
		selected_entries = backup_entries;
	}
	if (selected->active_count > capacity) {
		error = -ENOSPC;
		goto out;
	}
	memcpy(entries, selected_entries,
	    selected->active_count * sizeof(*entries));
	if (bounded) {
		char logical_last_text[21U], physical_last_text[21U];
		char declared_sectors_text[21U], physical_sectors_text[21U];
		char ignored_tail_text[21U];

		decimal_u64(logical_last_text, logical_last);
		decimal_u64(physical_last_text, physical_last);
		decimal_u64(declared_sectors_text, logical_last + 1U);
		decimal_u64(physical_sectors_text, physical_last + 1U);
		decimal_u64(ignored_tail_text, physical_last - logical_last);
		hal_printf("gpt: %s bounded extent accepted: logical-last=%s "
		    "physical-last=%s declared-sectors=%s physical-sectors=%s "
		    "ignored-tail-sectors=%s\n", disk->d_name,
		    logical_last_text, physical_last_text, declared_sectors_text,
		    physical_sectors_text, ignored_tail_text);
	}
	error = (int)selected->active_count;
out:
	kern_free(block);
	kern_free(primary_entries);
	kern_free(backup_entries);
	return error;
}

const struct partition_scheme partition_scheme_gpt = {
	.name = "gpt",
	.scan = gpt_scan,
};
