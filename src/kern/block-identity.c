/* Block device filesystem and partition identity.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/block-identity.h>
#include <kern/partition.h>

#include <errno.h>
#include <string.h>

#define UFS1_SUPER_OFFSET 8192U
#define UFS2_SUPER_OFFSET 65536U
#define UFS_SUPER_SIZE 8192U
#define UFS_FS_ID 144U
#define UFS_FS_VOLNAME 680U
#define UFS_FS_VOLNAME_SIZE 32U
#define UFS_FS_MAGIC 1372U
#define UFS1_MAGIC 0x00011954U
#define UFS2_MAGIC 0x19540119U

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static char hex(unsigned value)
{
	return (char)(value < 10U ? '0' + value : 'A' + value - 10U);
}

static void hex32(char *output, uint32_t value)
{
	unsigned i;
	for (i = 0; i < 8U; i++)
		output[i] = hex((value >> (28U - i * 4U)) & 15U);
}

static void copy_trimmed(char *output, size_t capacity, const uint8_t *input,
	size_t length)
{
	size_t end = length, i;
	while (end != 0 && (input[end - 1U] == ' ' || input[end - 1U] == 0)) end--;
	if (end >= capacity) end = capacity - 1U;
	for (i = 0; i < end; i++)
		output[i] = input[i] >= 0x20U && input[i] <= 0x7eU ?
		    (char)input[i] : '_';
	output[end] = '\0';
}

static int read_bytes(struct disk *disk, uint64_t offset, size_t length,
	uint8_t *output)
{
	uint8_t sector[512];
	if (disk->d_block_size != sizeof(sector)) return EOPNOTSUPP;
	while (length != 0) {
		size_t within = (size_t)(offset % sizeof(sector));
		size_t amount = sizeof(sector) - within;
		if (amount > length) amount = length;
		/* Identification is synchronous metadata probing.  Bypass bufcache:
		 * a loop device may itself be backed by a file on the cached disk. */
		if (disk_read_direct(disk, offset / sizeof(sector), 1, sector) != 0)
			return EIO;
		memcpy(output, sector + within, amount);
		output += amount; offset += amount; length -= amount;
	}
	return 0;
}

static int probe_fat(struct disk *disk, struct block_identity *id)
{
	uint8_t boot[512];
	uint32_t clusters, sectors, fatsz, root_sectors, data_sectors;
	unsigned serial_offset, label_offset;
	if (read_bytes(disk, 0, sizeof(boot), boot) != 0) return 0;
	if (boot[510] != 0x55U || boot[511] != 0xaaU || le16(boot + 11) != 512U ||
	    boot[13] == 0 || boot[16] == 0) return 0;
	sectors = le16(boot + 19);
	if (sectors == 0) sectors = le32(boot + 32);
	fatsz = le16(boot + 22);
	if (fatsz == 0) fatsz = le32(boot + 36);
	root_sectors = ((uint32_t)le16(boot + 17) * 32U + 511U) / 512U;
	if (sectors <= (uint32_t)le16(boot + 14) +
	    (uint32_t)boot[16] * fatsz + root_sectors) return 0;
	data_sectors = sectors - ((uint32_t)le16(boot + 14) +
	    (uint32_t)boot[16] * fatsz + root_sectors);
	clusters = data_sectors / boot[13];
	if (clusters < 4085U) strcpy(id->type, "vfat");
	else if (clusters < 65525U) strcpy(id->type, "vfat");
	else strcpy(id->type, "vfat");
	id->flags |= ZEDBSD_BLKID_TYPE;
	serial_offset = clusters < 65525U ? 39U : 67U;
	label_offset = clusters < 65525U ? 43U : 71U;
	if (boot[serial_offset - 1U] == 0x29U) {
		uint32_t serial = le32(boot + serial_offset);
		hex32(id->uuid, serial);
		memmove(id->uuid + 5, id->uuid + 4, 4);
		id->uuid[4] = '-'; id->uuid[9] = '\0';
		id->flags |= ZEDBSD_BLKID_UUID;
		copy_trimmed(id->label, sizeof(id->label), boot + label_offset, 11U);
		if (id->label[0] != '\0' && strcmp(id->label, "NO NAME") != 0)
			id->flags |= ZEDBSD_BLKID_LABEL;
	}
	return 1;
}

static int probe_ufs_at(struct disk *disk, uint64_t offset,
	struct block_identity *id)
{
	uint8_t super[UFS_SUPER_SIZE];
	uint32_t magic, first, second;
	int swapped;
	if (read_bytes(disk, offset, sizeof(super), super) != 0) return 0;
	magic = le32(super + UFS_FS_MAGIC);
	swapped = 0;
	if (magic != UFS1_MAGIC && magic != UFS2_MAGIC) {
		magic = be32(super + UFS_FS_MAGIC); swapped = 1;
	}
	if (magic != UFS1_MAGIC && magic != UFS2_MAGIC) return 0;
	strcpy(id->type, magic == UFS1_MAGIC ? "ufs1" : "ufs2");
	id->flags |= ZEDBSD_BLKID_TYPE;
	first = swapped ? be32(super + UFS_FS_ID) : le32(super + UFS_FS_ID);
	second = swapped ? be32(super + UFS_FS_ID + 4U) :
	    le32(super + UFS_FS_ID + 4U);
	if (first != 0 || second != 0) {
		hex32(id->uuid, first); hex32(id->uuid + 8, second);
		id->uuid[16] = '\0'; id->flags |= ZEDBSD_BLKID_UUID;
	}
	copy_trimmed(id->label, sizeof(id->label), super + UFS_FS_VOLNAME,
	    UFS_FS_VOLNAME_SIZE);
	if (id->label[0] != '\0') id->flags |= ZEDBSD_BLKID_LABEL;
	return 1;
}

int block_identity_get(struct disk *disk, struct block_identity *id)
{
	unsigned i;
	if (disk == NULL || id == NULL) return EINVAL;
	if (disk->d_identity_valid) {
		memset(id, 0, sizeof(*id));
		id->flags = disk->d_identity_flags;
		strcpy(id->type, disk->d_identity_type);
		strcpy(id->uuid, disk->d_identity_uuid);
		strcpy(id->label, disk->d_identity_label);
		strcpy(id->partuuid, disk->d_identity_partuuid);
		strcpy(id->partlabel, disk->d_identity_partlabel);
		return 0;
	}
	memset(id, 0, sizeof(*id));
	if (disk->d_flags & DISK_PARTITION) {
		for (i = 0; i < partition_count(); i++) {
			const struct partition *part = partition_at(i);
			if (part == NULL || part->p_disk != disk) continue;
			if (part->p_flags & PARTITION_HAS_UUID) {
				memcpy(id->partuuid, part->p_uuid,
				    sizeof(id->partuuid));
				id->partuuid[sizeof(id->partuuid) - 1U] = '\0';
				id->flags |= ZEDBSD_BLKID_PARTUUID;
			}
			if (part->p_flags & PARTITION_HAS_LABEL) {
				memcpy(id->partlabel, part->p_label,
				    sizeof(id->partlabel));
				id->partlabel[sizeof(id->partlabel) - 1U] = '\0';
				id->flags |= ZEDBSD_BLKID_PARTLABEL;
			}
			break;
		}
	}
	if (!probe_fat(disk, id) && !probe_ufs_at(disk, UFS1_SUPER_OFFSET, id))
		(void)probe_ufs_at(disk, UFS2_SUPER_OFFSET, id);
	if (id->flags == 0) return ENOENT;
	disk->d_identity_flags = id->flags;
	strcpy(disk->d_identity_type, id->type);
	strcpy(disk->d_identity_uuid, id->uuid);
	strcpy(disk->d_identity_label, id->label);
	strcpy(disk->d_identity_partuuid, id->partuuid);
	strcpy(disk->d_identity_partlabel, id->partlabel);
	disk->d_identity_valid = 1;
	return 0;
}

static int equal_fold(const char *left, const char *right)
{
	for (;;) {
		char a = *left++, b = *right++;
		if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
		if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
		if (a != b) return 0;
		if (a == '\0') return 1;
	}
}

int block_identity_resolve(const char *selector, struct disk **result)
{
	const char *value, *field = NULL;
	struct disk *match = NULL;
	unsigned required = 0, i;
	if (selector == NULL || result == NULL) return EINVAL;
	*result = NULL;
	if (strncmp(selector, "/dev/", 5) == 0) selector += 5;
	if (strchr(selector, '=') == NULL) {
		match = disk_find(selector);
		if (match == NULL) return ENOENT;
		*result = match; return 0;
	}
	if (strncmp(selector, "UUID=", 5) == 0)
		required = ZEDBSD_BLKID_UUID, field = "uuid", value = selector + 5;
	else if (strncmp(selector, "LABEL=", 6) == 0)
		required = ZEDBSD_BLKID_LABEL, field = "label", value = selector + 6;
	else if (strncmp(selector, "PARTUUID=", 9) == 0)
		required = ZEDBSD_BLKID_PARTUUID, field = "partuuid", value = selector + 9;
	else if (strncmp(selector, "PARTLABEL=", 10) == 0)
		required = ZEDBSD_BLKID_PARTLABEL, field = "partlabel", value = selector + 10;
	else return EINVAL;
	if (*value == '\0') return EINVAL;
	for (i = 0; i < disk_count(); i++) {
		struct block_identity id;
		struct disk *candidate = disk_at(i);
		const char *candidate_value;
		if (candidate == NULL || block_identity_get(candidate, &id) != 0) {
			if (candidate != NULL) disk_release(candidate);
			continue;
		}
		if (strcmp(field, "uuid") == 0) candidate_value = id.uuid;
		else if (strcmp(field, "label") == 0) candidate_value = id.label;
		else if (strcmp(field, "partuuid") == 0) candidate_value = id.partuuid;
		else candidate_value = id.partlabel;
		if ((id.flags & required) && equal_fold(candidate_value, value)) {
			if (match != NULL) {
				disk_release(candidate); disk_release(match); return EEXIST;
			}
			match = candidate;
		} else disk_release(candidate);
	}
	if (match == NULL) return ENOENT;
	*result = match; return 0;
}
