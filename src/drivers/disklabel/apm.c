/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Apple Partition Map scanner, and the PowerPC Mac's choice between it
 * and the PC/AT tables.
 *
 * An Apple-partitioned disk starts with the Driver Descriptor Map, whose
 * signature is "ER" and which names the block size the map counts in.
 * The partition map follows from the next block on, one entry a block,
 * each signed "PM"; the first entry describes the map itself and every
 * entry repeats how many entries the map has.  Each entry gives its
 * partition's first block and length in the map's blocks, a name, a type
 * and status bits.  The partitions are numbered from 1 in map order, the
 * map's own entry first, as Open Firmware numbers them in a device path
 * ("ud:2" is the second entry); entry n is published with index n - 1.
 * The map's own entry and free space are not published.
 *
 * Values referenced: Apple, Inside Macintosh: Devices (1994), chapter 3
 * "SCSI Manager", the Driver Descriptor Record (sbSig 0x4552, sbBlkSize
 * at offset 2, sbBlkCount at offset 4) and the partition map entry
 * (pmSig 0x504D, pmMapBlkCnt at 4, pmPyPartStart at 8, pmPartBlkCnt at
 * 12, pmPartName at 16 and pmParType at 48, 32 bytes each, pmPartStatus
 * at 88, the status bit 0x8 that marks valid boot information); the
 * partition types "Apple_partition_map" and "Apple_Free".  The Linux
 * kernel's block/partitions/mac.c reads entry n at n times the block
 * size, and scales the start and the length the same way.
 */

#include <drivers/disklabel/disklabel.h>

/* The signatures of the Driver Descriptor Map and of a partition map entry. */
#define APM_DDM_SIGNATURE		0x4552U
#define APM_ENTRY_SIGNATURE		0x504dU

/* The disk block size the scanner reads, and the largest map block size it takes. */
#define APM_SECTOR_SIZE			512U
#define APM_BLOCK_SIZE_MAX		4096U

/* Where the Driver Descriptor Map keeps the map's block size. */
#define APM_DDM_BLOCK_SIZE		2U

/* Where a partition map entry keeps its fields. */
#define APM_ENTRY_MAP_COUNT		4U
#define APM_ENTRY_START			8U
#define APM_ENTRY_COUNT			12U
#define APM_ENTRY_NAME			16U
#define APM_ENTRY_TYPE			48U
#define APM_ENTRY_STATUS		88U
#define APM_ENTRY_TEXT_SIZE		32U

/* The status bit that says the partition carries valid boot information. */
#define APM_STATUS_BOOT_VALID		0x00000008U

/* The label a partition without a printable name gets: "apm" and its number. */
#define APM_FALLBACK_LABEL_SIZE		6U

static int apm_scan(const struct partition_scheme *scheme, struct disk *disk, struct partition *entries, unsigned capacity);
static int mac_auto_scan(const struct partition_scheme *scheme, struct disk *disk, struct partition *entries, unsigned capacity);
static void apm_entry_decode(const uint8_t *raw, unsigned index, unsigned scale, struct disk *disk, struct partition *entry);
static int apm_type_is(const uint8_t *raw, const char *type);
static int apm_name_copy(const uint8_t *raw, char label[PARTITION_LABEL_MAX]);
static int apm_read_entry(struct disk *disk, unsigned index, unsigned map_block_size, uint8_t sector[APM_SECTOR_SIZE], const uint8_t **raw);
static uint16_t be16(const uint8_t *p);
static uint32_t be32(const uint8_t *p);

/*
 * The Apple Partition Map scheme, constant for the life of the system.
 */
const struct partition_scheme drv_partition_scheme_apm = {
	.name = "apm",
	.scan = apm_scan,
};

/*
 * The PowerPC Mac's scheme: the Apple Partition Map when the disk has a
 * Driver Descriptor Map, otherwise GPT or MBR as on a PC.
 */
const struct partition_scheme drv_partition_scheme_mac_auto = {
	.name = "mac-auto",
	.scan = mac_auto_scan,
};

/*
 * Scans an Apple-partitioned disk into partition entries.
 *
 * Returns the number of entries, the map's own first; -1 when the disk
 * has no Apple Partition Map the scanner can read.
 */
static int
apm_scan(
	const struct partition_scheme *scheme,
	struct disk *disk,
	struct partition *entries,
	unsigned capacity)
{
	const uint8_t *raw;
	uint8_t sector[APM_SECTOR_SIZE];
	uint32_t map_count;
	unsigned map_block_size;
	unsigned count;
	unsigned index;
	int error;

	(void)scheme;

	/* Reads the Driver Descriptor Map from a disk of 512-byte blocks. */
	if (disk == NULL || entries == NULL || capacity == 0U)
		return -1;
	if (disk->d_block_size != APM_SECTOR_SIZE)
		return -1;
	error = disk_read(disk, 0, 1, sector);
	if (error != 0)
		return -1;

	/* Refuses a disk without the map's signature. */
	if (be16(sector) != APM_DDM_SIGNATURE)
		return -1;

	/* Takes the map's block size, a whole number of disk blocks up to a page. */
	map_block_size = be16(sector + APM_DDM_BLOCK_SIZE);
	if (map_block_size < APM_SECTOR_SIZE ||
	    map_block_size > APM_BLOCK_SIZE_MAX ||
	    map_block_size % APM_SECTOR_SIZE != 0U)
		return -1;

	/* Reads the map's own entry, which says how many entries the map has. */
	error = apm_read_entry(disk, 0, map_block_size, sector, &raw);
	if (error != 0)
		return -1;
	map_count = be32(raw + APM_ENTRY_MAP_COUNT);
	if (map_count == 0U)
		return -1;

	/* Keeps as many entries as the caller has room for. */
	count = capacity;
	if (map_count < count)
		count = (unsigned)map_count;

	/* Decodes each entry in map order; the map ends at the first unsigned block. */
	for (index = 0; index < count; index++) {
		/* Reads the entry's block. */
		error = apm_read_entry(disk, index, map_block_size, sector, &raw);
		if (error != 0)
			break;

		/* Records the entry, published only when it describes a partition on the disk. */
		apm_entry_decode(raw, index, map_block_size / APM_SECTOR_SIZE, disk, &entries[index]);
	}

	/* Refuses a map whose own entry is all there is to read. */
	if (index == 0U)
		return -1;

	/* Succeeded: the entries read. */
	return (int)index;
}

/*
 * Scans a disk with the Apple Partition Map when it has one, else with
 * the PC/AT choice between GPT and MBR.
 *
 * A disk signed as Apple-partitioned is read only as one, so a hybrid
 * disk that also carries an MBR is taken the way the Mac's firmware
 * takes it.
 */
static int
mac_auto_scan(
	const struct partition_scheme *scheme,
	struct disk *disk,
	struct partition *entries,
	unsigned capacity)
{
	uint8_t sector[APM_SECTOR_SIZE];
	int count;
	int error;

	(void)scheme;

	/* Reads the first block, where an Apple disk keeps its Driver Descriptor Map. */
	if (disk == NULL || entries == NULL || capacity == 0U)
		return -1;
	if (disk->d_block_size != APM_SECTOR_SIZE)
		return -1;
	error = disk_read(disk, 0, 1, sector);
	if (error != 0)
		return -1;

	/* A disk signed as Apple-partitioned is scanned as one. */
	if (be16(sector) == APM_DDM_SIGNATURE) {
		count = drv_partition_scheme_apm.scan(&drv_partition_scheme_apm, disk, entries, capacity);
		if (count < 0)
			return count;

		/* Succeeded: the Apple partitions. */
		return count;
	}

	/* Any other disk is a PC disk, GPT or MBR. */
	count = drv_partition_scheme_pcat_auto.scan(&drv_partition_scheme_pcat_auto, disk, entries, capacity);
	if (count < 0)
		return count;

	/* Succeeded: the PC partitions. */
	return count;
}

/* Decodes one map entry into a partition entry. */
static void
apm_entry_decode(
	const uint8_t *raw,
	unsigned index,
	unsigned scale,
	struct disk *disk,
	struct partition *entry)
{
	uint64_t start;
	uint64_t blocks;
	uint32_t status;
	int named;

	/* Places the entry; it has no blocks until its range proves usable. */
	entry->p_parent = disk;
	entry->p_disk = NULL;
	entry->p_index = index;
	entry->p_block_count = 0;
	entry->p_flags = 0;
	entry->p_uuid[0] = '\0';

	/* The range, from the map's blocks into the disk's. */
	start = (uint64_t)be32(raw + APM_ENTRY_START) * scale;
	blocks = (uint64_t)be32(raw + APM_ENTRY_COUNT) * scale;
	entry->p_start_block = start;
	entry->p_data_block = start;

	/* The name is the partition's label; one without a printable name is "apm" and its number. */
	named = apm_name_copy(raw, entry->p_label);
	if (named) {
		entry->p_flags |= PARTITION_HAS_LABEL;
	} else {
		entry->p_label[0] = 'a';
		entry->p_label[1] = 'p';
		entry->p_label[2] = 'm';
		entry->p_label[3] = (char)('0' + (index + 1U) / 10U);
		entry->p_label[4] = (char)('0' + (index + 1U) % 10U);
		entry->p_label[APM_FALLBACK_LABEL_SIZE - 1U] = '\0';
	}

	/* The map itself and free space hold no file system. */
	if (apm_type_is(raw, "Apple_partition_map"))
		return;
	if (apm_type_is(raw, "Apple_Free"))
		return;

	/* A range that is empty or runs past the disk is not published. */
	if (blocks == 0U || start >= disk->d_block_count)
		return;
	if (blocks > disk->d_block_count - start)
		return;
	entry->p_block_count = blocks;

	/* The firmware may boot a partition whose boot information is valid. */
	status = be32(raw + APM_ENTRY_STATUS);
	if ((status & APM_STATUS_BOOT_VALID) != 0U)
		entry->p_flags |= PARTITION_BOOTABLE;
}

/* Tells whether an entry's type is the one named, compared in full. */
static int
apm_type_is(
	const uint8_t *raw,
	const char *type)
{
	unsigned at;

	/* Compares up to the name's end; the field is NUL-padded. */
	for (at = 0; type[at] != '\0'; at++) {
		if (at == APM_ENTRY_TEXT_SIZE)
			return 0;
		if (raw[APM_ENTRY_TYPE + at] != (uint8_t)type[at])
			return 0;
	}

	/* The field must end where the name does. */
	if (at < APM_ENTRY_TEXT_SIZE && raw[APM_ENTRY_TYPE + at] != 0U)
		return 0;

	/* Succeeded: the type is the one named. */
	return 1;
}

/*
 * Copies an entry's name into a label.  Returns 1 for a name that is not
 * empty and holds only printable ASCII, 0 otherwise (the label is then
 * left empty).
 */
static int
apm_name_copy(
	const uint8_t *raw,
	char label[PARTITION_LABEL_MAX])
{
	unsigned at;
	uint8_t character;

	/* Copies up to the NUL that ends the name, or the whole field. */
	for (at = 0; at < APM_ENTRY_TEXT_SIZE; at++) {
		character = raw[APM_ENTRY_NAME + at];
		if (character == 0U)
			break;
		if (character < 0x20U || character > 0x7eU) {
			label[0] = '\0';
			return 0;
		}
		label[at] = (char)character;
	}

	/* An empty name is no label. */
	label[at] = '\0';
	if (at == 0U)
		return 0;

	/* Succeeded: the label is the name. */
	return 1;
}

/*
 * Reads map entry index (0 is the map's own) and checks its signature.
 * The entry lies index + 1 map blocks into the disk.
 */
static int
apm_read_entry(
	struct disk *disk,
	unsigned index,
	unsigned map_block_size,
	uint8_t sector[APM_SECTOR_SIZE],
	const uint8_t **raw)
{
	uint64_t offset;
	int error;

	/* Reads the disk block that holds the entry. */
	offset = (uint64_t)(index + 1U) * map_block_size;
	if (offset / APM_SECTOR_SIZE >= disk->d_block_count)
		return -1;
	error = disk_read(disk, offset / APM_SECTOR_SIZE, 1, sector);
	if (error != 0)
		return -1;

	/* Refuses a block that is not a map entry. */
	if (be16(sector) != APM_ENTRY_SIGNATURE)
		return -1;

	/* Succeeded: the entry starts the block. */
	*raw = sector;
	return 0;
}

/* Reads a big-endian 16-bit field. */
static uint16_t
be16(
	const uint8_t *p)
{
	/* The high byte first. */
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* Reads a big-endian 32-bit field. */
static uint32_t
be32(
	const uint8_t *p)
{
	/* The high byte first. */
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}
