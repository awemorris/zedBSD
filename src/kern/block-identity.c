/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Block device filesystem and partition identity.
 *
 * A disk's identity combines the partition table's UUID and label with the
 * type, UUID, and label read from a filesystem or swap header.  Once
 * composed it is cached on the disk, and selectors of the UUID=, LABEL=,
 * PARTUUID=, and PARTLABEL= forms resolve against it.
 */

#include <kern/block-identity.h>
#include <kern/kmem.h>
#include <kern/mount.h>
#include <kern/partition.h>
#include <kern/swap.h>

#include <errno.h>
#include <string.h>

static int read_bytes(struct disk *disk, uint64_t offset, size_t length, uint8_t *output);
static void partition_identity_fill(struct disk *disk, struct block_identity *identity);
static int swap_identify(struct disk *disk, struct block_identity *identity);
static void identity_merge_filesystem(struct block_identity *identity, const struct block_identity *filesystem);
static void identity_load_cached(const struct disk *disk, struct block_identity *identity);
static void identity_store_cached(struct disk *disk, const struct block_identity *identity);
static int equal_fold(const char *left, const char *right);
static const char *identity_field(const struct block_identity *identity, unsigned required);

/*
 * Composes the identity of a disk.
 *
 * A disk that carries both a filesystem and a swap header is ambiguous
 * and reported as EEXIST.  A disk with neither and no partition identity
 * is reported as ENOENT.
 */
int
block_identity_get(
	struct disk *disk,
	struct block_identity *identity)
{
	struct block_identity filesystem;
	struct block_identity swap;
	int filesystem_error;
	int swap_error;

	/* Rejects a missing disk or result. */
	if (disk == NULL || identity == NULL)
		return EINVAL;

	/* Serves a cached identity. */
	if (disk->d_identity_valid != 0U) {
		identity_load_cached(disk, identity);
		return 0;
	}

	/* Starts from the partition identity. */
	memset(identity, 0, sizeof(*identity));
	partition_identity_fill(disk, identity);

	/* Probes for a filesystem and for a swap header. */
	memset(&filesystem, 0, sizeof(filesystem));
	memset(&swap, 0, sizeof(swap));
	filesystem_error = filesystem_identify(disk, &filesystem);
	swap_error = swap_identify(disk, &swap);
	if (filesystem_error == 0 && swap_error == 0)
		return EEXIST;

	/* Merges whichever was found, or reports why neither was. */
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

	/* Caches the composed identity on the disk. */
	identity_store_cached(disk, identity);

	/* Reports the composed identity. */
	return 0;
}

/*
 * Resolves a device selector to a referenced disk.
 *
 * A plain name looks up the disk directly.  A UUID=, LABEL=, PARTUUID=, or
 * PARTLABEL= selector scans every disk and must match exactly one.
 */
int
block_identity_resolve(
	const char *selector,
	struct disk **result)
{
	struct block_identity identity;
	struct disk *match;
	struct disk *candidate;
	const char *value;
	unsigned required;
	unsigned i;
	int first_error;
	int error;

	match = NULL;
	first_error = 0;

	/* Rejects a missing selector or result. */
	if (selector == NULL || result == NULL)
		return EINVAL;

	*result = NULL;

	/* A plain device name is looked up directly. */
	if (strncmp(selector, "/dev/", 5U) == 0)
		selector += 5U;
	if (strchr(selector, '=') == NULL) {
		match = disk_find(selector);
		if (match == NULL)
			return ENOENT;
		*result = match;
		return 0;
	}

	/* Splits an identity selector into the field and the value. */
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

	/* Scans every disk for the field value. */
	for (i = 0; i < disk_count(); i++) {
		candidate = disk_at(i);
		if (candidate == NULL)
			continue;

		/* A partition field needs no header read. */
		if (required == ZEDBSD_BLKID_PARTUUID ||
		    required == ZEDBSD_BLKID_PARTLABEL) {
			memset(&identity, 0, sizeof(identity));
			partition_identity_fill(candidate, &identity);
			if (identity.flags != 0U)
				error = 0;
			else
				error = ENOENT;
		} else {
			error = block_identity_get(candidate, &identity);
		}

		/* Remembers the first real failure and moves on. */
		if (error != 0) {
			if (first_error == 0 &&
			    error != ENOENT &&
			    error != EOPNOTSUPP)
				first_error = error;
			disk_release(candidate);
			continue;
		}

		/* Keeps a match, refusing a second one. */
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

	/* Without a match, reports the first failure or a missing disk. */
	if (match == NULL) {
		if (first_error != 0)
			return first_error;
		return ENOENT;
	}

	*result = match;

	/* Reports the matched disk. */
	return 0;
}

/* Reads bytes from a disk at any byte offset, bypassing the buffer cache. */
static int
read_bytes(
	struct disk *disk,
	uint64_t offset,
	size_t length,
	uint8_t *output)
{
	uint8_t *block;
	uint64_t bytes;
	size_t block_size;
	size_t within;
	size_t amount;
	int error;

	error = 0;

	/* Rejects a missing operand or a disk whose size overflows. */
	if (disk == NULL ||
	    output == NULL ||
	    disk->d_block_size == 0U ||
	    disk->d_block_count > UINT64_MAX / disk->d_block_size)
		return EINVAL;

	/* Rejects a range past the end of the disk. */
	block_size = disk->d_block_size;
	bytes = disk->d_block_count * disk->d_block_size;
	if (offset > bytes || length > bytes - offset)
		return EIO;

	/* Reads one block at a time through a bounce buffer. */
	block = kern_malloc(block_size);
	if (block == NULL)
		return ENOMEM;
	while (length != 0U) {
		within = (size_t)(offset % block_size);
		amount = block_size - within;
		if (amount > length)
			amount = length;

		/*
		 * Identity reads bypass bufcache.  A loop disk may be backed
		 * by a file on the disk whose identity is being composed.
		 */
		if (disk_read_direct(disk, offset / block_size, 1U, block) != 0) {
			error = EIO;
			break;
		}

		/* Copies the overlapping bytes and advances the cursor. */
		memcpy(output, block + within, amount);
		output += amount;
		offset += amount;
		length -= amount;
	}
	kern_free(block);

	/* Reports why the read failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Copies the partition table's UUID and label into an identity. */
static void
partition_identity_fill(
	struct disk *disk,
	struct block_identity *identity)
{
	const struct partition *part;

	/* Uses the immutable record pinned by the referenced partition disk. */
	part = disk->d_data;
	if ((disk->d_flags & DISK_PARTITION) == 0U || part == NULL)
		return;

	/* Copies the UUID without traversing tables another disk may reload. */
	if ((part->p_flags & PARTITION_HAS_UUID) != 0U) {
		memcpy(identity->partuuid, part->p_uuid, sizeof(identity->partuuid));
		identity->partuuid[sizeof(identity->partuuid) - 1U] = '\0';
		identity->flags |= ZEDBSD_BLKID_PARTUUID;
	}

	/* Copies the label supplied by the same pinned record. */
	if ((part->p_flags & PARTITION_HAS_LABEL) != 0U) {
		memcpy(identity->partlabel, part->p_label, sizeof(identity->partlabel));
		identity->partlabel[sizeof(identity->partlabel) - 1U] = '\0';
		identity->flags |= ZEDBSD_BLKID_PARTLABEL;
	}
}

/* Identifies a disk that carries a swap header. */
static int
swap_identify(
	struct disk *disk,
	struct block_identity *identity)
{
	uint8_t header[ZEDBSD_SWAP_HEADER_SIZE];
	struct swap_header_info info;
	uint64_t bytes;
	int error;

	/* Rejects a missing disk or result. */
	if (disk == NULL || identity == NULL)
		return EINVAL;

	/* A disk too small or oddly sized for swap cannot carry a header. */
	if (disk->d_block_size == 0U ||
	    disk->d_block_count > UINT64_MAX / disk->d_block_size)
		return EOPNOTSUPP;
	bytes = disk->d_block_count * disk->d_block_size;
	if (bytes < SWAP_PAGE_SIZE * 2ULL ||
	    bytes % SWAP_PAGE_SIZE != 0U ||
	    bytes < sizeof(header))
		return EOPNOTSUPP;

	/* Reads and recognizes the header. */
	error = read_bytes(disk, 0U, sizeof(header), header);
	if (error != 0)
		return error;
	if (memcmp(header, "ZEDSWAP1", 8U) != 0 &&
	    memcmp(header, "ZEDSWAP2", 8U) != 0)
		return EOPNOTSUPP;
	error = swap_header_parse(header, bytes, &info);
	if (error != 0)
		return error;

	/* Describes the swap area by type, UUID, and label. */
	memset(identity, 0, sizeof(*identity));
	strcpy(identity->type, "swap");
	identity->flags = ZEDBSD_BLKID_TYPE;
	if (swap_header_uuid_format(&info, identity->uuid, sizeof(identity->uuid)) == 0)
		identity->flags |= ZEDBSD_BLKID_UUID;
	if (info.label[0] != '\0') {
		strcpy(identity->label, info.label);
		identity->flags |= ZEDBSD_BLKID_LABEL;
	}

	/* Reports the swap identity. */
	return 0;
}

/* Merges the type, UUID, and label of a filesystem identity. */
static void
identity_merge_filesystem(
	struct block_identity *identity,
	const struct block_identity *filesystem)
{
	/* Copies each field the filesystem provides. */
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

/* Loads the identity cached on a disk. */
static void
identity_load_cached(
	const struct disk *disk,
	struct block_identity *identity)
{
	memset(identity, 0, sizeof(*identity));
	identity->flags = disk->d_identity_flags;
	memcpy(identity->type, disk->d_identity_type, sizeof(identity->type));
	memcpy(identity->uuid, disk->d_identity_uuid, sizeof(identity->uuid));
	memcpy(identity->label, disk->d_identity_label, sizeof(identity->label));
	memcpy(identity->partuuid, disk->d_identity_partuuid, sizeof(identity->partuuid));
	memcpy(identity->partlabel, disk->d_identity_partlabel, sizeof(identity->partlabel));
}

/* Caches an identity on a disk. */
static void
identity_store_cached(
	struct disk *disk,
	const struct block_identity *identity)
{
	disk->d_identity_flags = identity->flags;
	memcpy(disk->d_identity_type, identity->type, sizeof(disk->d_identity_type));
	memcpy(disk->d_identity_uuid, identity->uuid, sizeof(disk->d_identity_uuid));
	memcpy(disk->d_identity_label, identity->label, sizeof(disk->d_identity_label));
	memcpy(disk->d_identity_partuuid, identity->partuuid, sizeof(disk->d_identity_partuuid));
	memcpy(disk->d_identity_partlabel, identity->partlabel, sizeof(disk->d_identity_partlabel));
	disk->d_identity_valid = 1U;
}

/* Compares two strings ignoring ASCII case. */
static int
equal_fold(
	const char *left,
	const char *right)
{
	char a;
	char b;

	/* Compares byte by byte in upper case until a difference or the end. */
	for (;;) {
		a = *left++;
		b = *right++;
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

/* Selects the identity field a selector kind compares against. */
static const char *
identity_field(
	const struct block_identity *identity,
	unsigned required)
{
	/* Maps each selector kind onto its field. */
	if (required == ZEDBSD_BLKID_UUID)
		return identity->uuid;
	if (required == ZEDBSD_BLKID_LABEL)
		return identity->label;
	if (required == ZEDBSD_BLKID_PARTUUID)
		return identity->partuuid;

	/* The remaining kind is the partition label. */
	return identity->partlabel;
}
