/*
 * Block device filesystem and partition identity.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include <kern/block-identity.h>
#include <kern/kmem.h>
#include <kern/mount.h>
#include <kern/partition.h>
#include <kern/swap.h>

#include <errno.h>
#include <string.h>

static int
read_bytes(struct disk *disk, uint64_t offset, size_t length, uint8_t *output)
{
	uint8_t *block;
	uint64_t bytes;
	size_t block_size;
	int error = 0;

	if (disk == NULL || output == NULL || disk->d_block_size == 0U ||
	    disk->d_block_count > UINT64_MAX / disk->d_block_size)
		return EINVAL;
	block_size = disk->d_block_size;
	bytes = disk->d_block_count * disk->d_block_size;
	if (offset > bytes || length > bytes - offset)
		return EIO;
	block = kern_malloc(block_size);
	if (block == NULL)
		return ENOMEM;
	while (length != 0U) {
		size_t within = (size_t)(offset % block_size);
		size_t amount = block_size - within;

		if (amount > length)
			amount = length;
		/*
		 * Identity reads bypass bufcache.  A loop disk may be backed by a
		 * file on the disk whose identity is being composed.
		 */
		if (disk_read_direct(disk, offset / block_size, 1U, block) != 0) {
			error = EIO;
			break;
		}
		memcpy(output, block + within, amount);
		output += amount;
		offset += amount;
		length -= amount;
	}
	kern_free(block);
	return error;
}

static void
partition_identity_fill(struct disk *disk, struct block_identity *identity)
{
	const struct partition *part = disk->d_data;

	if ((disk->d_flags & DISK_PARTITION) == 0U || part == NULL)
		return;
	/* The opened/referenced disk pins its immutable partition record. Avoid
	 * traversing another disk's table while that disk is being reloaded. */
	if ((part->p_flags & PARTITION_HAS_UUID) != 0U) {
		memcpy(identity->partuuid, part->p_uuid, sizeof(identity->partuuid));
		identity->partuuid[sizeof(identity->partuuid) - 1U] = '\0';
		identity->flags |= ZEDBSD_BLKID_PARTUUID;
	}
	if ((part->p_flags & PARTITION_HAS_LABEL) != 0U) {
		memcpy(identity->partlabel, part->p_label, sizeof(identity->partlabel));
		identity->partlabel[sizeof(identity->partlabel) - 1U] = '\0';
		identity->flags |= ZEDBSD_BLKID_PARTLABEL;
	}
}

static int
swap_identify(struct disk *disk, struct block_identity *identity)
{
	uint8_t header[ZEDBSD_SWAP_HEADER_SIZE];
	struct swap_header_info info;
	uint64_t bytes;
	int error;

	if (disk == NULL || identity == NULL)
		return EINVAL;
	if (disk->d_block_size == 0U ||
	    disk->d_block_count > UINT64_MAX / disk->d_block_size)
		return EOPNOTSUPP;
	bytes = disk->d_block_count * disk->d_block_size;
	if (bytes < SWAP_PAGE_SIZE * 2ULL ||
	    bytes % SWAP_PAGE_SIZE != 0U ||
	    bytes < sizeof(header))
		return EOPNOTSUPP;
	error = read_bytes(disk, 0U, sizeof(header), header);
	if (error != 0)
		return error;
	if (memcmp(header, "ZEDSWAP1", 8U) != 0 &&
	    memcmp(header, "ZEDSWAP2", 8U) != 0)
		return EOPNOTSUPP;
	error = swap_header_parse(header, bytes, &info);
	if (error != 0)
		return error;
	memset(identity, 0, sizeof(*identity));
	strcpy(identity->type, "swap");
	identity->flags = ZEDBSD_BLKID_TYPE;
	if (swap_header_uuid_format(&info, identity->uuid,
	    sizeof(identity->uuid)) == 0)
		identity->flags |= ZEDBSD_BLKID_UUID;
	if (info.label[0] != '\0') {
		strcpy(identity->label, info.label);
		identity->flags |= ZEDBSD_BLKID_LABEL;
	}
	return 0;
}

static void
identity_merge_filesystem(struct block_identity *identity,
	const struct block_identity *filesystem)
{
	if ((filesystem->flags & ZEDBSD_BLKID_TYPE) != 0U) {
		memcpy(identity->type, filesystem->type, sizeof(identity->type));
		identity->flags |= ZEDBSD_BLKID_TYPE;
	}
	if ((filesystem->flags & ZEDBSD_BLKID_UUID) != 0U) {
		memcpy(identity->uuid, filesystem->uuid, sizeof(identity->uuid));
		identity->flags |= ZEDBSD_BLKID_UUID;
	}
	if ((filesystem->flags & ZEDBSD_BLKID_LABEL) != 0U) {
		memcpy(identity->label, filesystem->label, sizeof(identity->label));
		identity->flags |= ZEDBSD_BLKID_LABEL;
	}
}

static void
identity_load_cached(const struct disk *disk, struct block_identity *identity)
{
	memset(identity, 0, sizeof(*identity));
	identity->flags = disk->d_identity_flags;
	memcpy(identity->type, disk->d_identity_type, sizeof(identity->type));
	memcpy(identity->uuid, disk->d_identity_uuid, sizeof(identity->uuid));
	memcpy(identity->label, disk->d_identity_label, sizeof(identity->label));
	memcpy(identity->partuuid, disk->d_identity_partuuid,
	    sizeof(identity->partuuid));
	memcpy(identity->partlabel, disk->d_identity_partlabel,
	    sizeof(identity->partlabel));
}

static void
identity_store_cached(struct disk *disk, const struct block_identity *identity)
{
	disk->d_identity_flags = identity->flags;
	memcpy(disk->d_identity_type, identity->type,
	    sizeof(disk->d_identity_type));
	memcpy(disk->d_identity_uuid, identity->uuid,
	    sizeof(disk->d_identity_uuid));
	memcpy(disk->d_identity_label, identity->label,
	    sizeof(disk->d_identity_label));
	memcpy(disk->d_identity_partuuid, identity->partuuid,
	    sizeof(disk->d_identity_partuuid));
	memcpy(disk->d_identity_partlabel, identity->partlabel,
	    sizeof(disk->d_identity_partlabel));
	disk->d_identity_valid = 1U;
}

int
block_identity_get(struct disk *disk, struct block_identity *identity)
{
	struct block_identity filesystem;
	struct block_identity swap;
	int filesystem_error;
	int swap_error;

	if (disk == NULL || identity == NULL)
		return EINVAL;
	if (disk->d_identity_valid != 0U) {
		identity_load_cached(disk, identity);
		return 0;
	}
	memset(identity, 0, sizeof(*identity));
	partition_identity_fill(disk, identity);
	memset(&filesystem, 0, sizeof(filesystem));
	memset(&swap, 0, sizeof(swap));
	filesystem_error = filesystem_identify(disk, &filesystem);
	swap_error = swap_identify(disk, &swap);
	if (filesystem_error == 0 && swap_error == 0)
		return EEXIST;
	if (filesystem_error == 0)
		identity_merge_filesystem(identity, &filesystem);
	else if (swap_error == 0)
		identity_merge_filesystem(identity, &swap);
	else if (filesystem_error != EOPNOTSUPP)
		return filesystem_error;
	else if (swap_error != EOPNOTSUPP)
		return swap_error;
	else if (identity->flags == 0U)
		return ENOENT;
	identity_store_cached(disk, identity);
	return 0;
}

static int
equal_fold(const char *left, const char *right)
{
	for (;;) {
		char a = *left++;
		char b = *right++;

		if (a >= 'a' && a <= 'z')
			a = (char)(a - 'a' + 'A');
		if (b >= 'a' && b <= 'z')
			b = (char)(b - 'a' + 'A');
		if (a != b)
			return 0;
		if (a == '\0')
			return 1;
	}
}

static const char *
identity_field(const struct block_identity *identity, unsigned required)
{
	if (required == ZEDBSD_BLKID_UUID)
		return identity->uuid;
	if (required == ZEDBSD_BLKID_LABEL)
		return identity->label;
	if (required == ZEDBSD_BLKID_PARTUUID)
		return identity->partuuid;
	return identity->partlabel;
}

int
block_identity_resolve(const char *selector, struct disk **result)
{
	const char *value;
	struct disk *match = NULL;
	unsigned required;
	unsigned i;
	int first_error = 0;

	if (selector == NULL || result == NULL)
		return EINVAL;
	*result = NULL;
	if (strncmp(selector, "/dev/", 5U) == 0)
		selector += 5U;
	if (strchr(selector, '=') == NULL) {
		match = disk_find(selector);
		if (match == NULL)
			return ENOENT;
		*result = match;
		return 0;
	}
	if (strncmp(selector, "UUID=", 5U) == 0) {
		required = ZEDBSD_BLKID_UUID;
		value = selector + 5U;
	} else if (strncmp(selector, "LABEL=", 6U) == 0) {
		required = ZEDBSD_BLKID_LABEL;
		value = selector + 6U;
	} else if (strncmp(selector, "PARTUUID=", 9U) == 0) {
		required = ZEDBSD_BLKID_PARTUUID;
		value = selector + 9U;
	} else if (strncmp(selector, "PARTLABEL=", 10U) == 0) {
		required = ZEDBSD_BLKID_PARTLABEL;
		value = selector + 10U;
	} else {
		return EINVAL;
	}
	if (*value == '\0')
		return EINVAL;
	for (i = 0; i < disk_count(); i++) {
		struct block_identity identity;
		struct disk *candidate = disk_at(i);
		int error;

		if (candidate == NULL)
			continue;
		if (required == ZEDBSD_BLKID_PARTUUID ||
		    required == ZEDBSD_BLKID_PARTLABEL) {
			memset(&identity, 0, sizeof(identity));
			partition_identity_fill(candidate, &identity);
			error = identity.flags != 0U ? 0 : ENOENT;
		} else {
			error = block_identity_get(candidate, &identity);
		}
		if (error != 0) {
			if (first_error == 0 && error != ENOENT &&
			    error != EOPNOTSUPP)
				first_error = error;
			disk_release(candidate);
			continue;
		}
		if ((identity.flags & required) != 0U &&
		    equal_fold(identity_field(&identity, required), value)) {
			if (match != NULL) {
				disk_release(candidate);
				disk_release(match);
				return EEXIST;
			}
			match = candidate;
		} else {
			disk_release(candidate);
		}
	}
	if (match == NULL)
		return first_error != 0 ? first_error : ENOENT;
	*result = match;
	return 0;
}
