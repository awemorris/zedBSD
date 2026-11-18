/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot loaders' read-only UFS reader.
 *
 * It reads the on-disk format the kernel's UFS driver and mkfs write:
 * UFS2 with 64-bit block pointers, 256-byte inodes and the classic
 * directory records.  It reads only native (little-endian) volumes, does
 * not follow symbolic links and does not read the journal: a file is read
 * as its home blocks hold it, which is what a volume that was unmounted or
 * synced holds.  Every number it takes from the disk is checked against
 * the volume's bounds before it is used, so a damaged volume stops the
 * read instead of steering it.
 */

#include "bootloader/common/ufs-read.h"

/* Where the superblock is, and how many of its bytes the reader reads. */
#define UFS_SUPERBLOCK_OFFSET 65536U
#define UFS_SUPERBLOCK_BYTES 8192U

/* The superblock's magic number, as a native volume stores it. */
#define UFS_MAGIC 0x19540119U

/* The same number as a volume of the other byte order stores it. */
#define UFS_MAGIC_SWAPPED 0x19011954U

/* The superblock's fields the reader uses, by their byte offsets. */
#define UFS_FS_IBLKNO 16U
#define UFS_FS_NCG 44U
#define UFS_FS_BSIZE 48U
#define UFS_FS_FSIZE 52U
#define UFS_FS_FRAG 56U
#define UFS_FS_FRAGSHIFT 96U
#define UFS_FS_NINDIR 116U
#define UFS_FS_INOPB 120U
#define UFS_FS_IPG 184U
#define UFS_FS_FPG 188U
#define UFS_FS_SIZE 1080U
#define UFS_FS_MAGIC 1372U

/* The inode's fields the reader uses, by their byte offsets. */
#define UFS_DI_MODE 0U
#define UFS_DI_SIZE 16U
#define UFS_DI_DB 112U
#define UFS_DI_IB 208U

/* How many direct and indirect block pointers an inode holds. */
#define UFS_DIRECT_BLOCKS 12U
#define UFS_INDIRECT_LEVELS 3U

/* The root directory's inode number. */
#define UFS_ROOT_INODE 2U

/* The size of one directory block, which no record crosses. */
#define UFS_DIRECTORY_BLOCK 512U

/* The longest name a directory record holds. */
#define UFS_NAME_MAX 255U

/* The longest path a lookup takes. */
#define UFS_PATH_MAX 1024U

/* The file types in an inode's mode. */
#define UFS_MODE_TYPE 0170000U
#define UFS_MODE_REGULAR 0100000U
#define UFS_MODE_DIRECTORY 0040000U
#define UFS_MODE_SYMLINK 0120000U

static uint16_t get16(const uint8_t *bytes);
static uint32_t get32(const uint8_t *bytes);
static uint64_t get64(const uint8_t *bytes);
static int power_of_two(uint32_t value);
static void bytes_copy(uint8_t *destination, const uint8_t *source, size_t length);
static void bytes_zero(uint8_t *destination, size_t length);
static enum zbl_ufs_result read_fragments(struct zbl_ufs_volume *volume, uint64_t fragment, uint32_t bytes, uint8_t *buffer);
static enum zbl_ufs_result check_superblock(struct zbl_ufs_volume *volume, const uint8_t *super);
static enum zbl_ufs_result read_inode(struct zbl_ufs_volume *volume, uint32_t number, struct zbl_ufs_file *file);
static enum zbl_ufs_result read_indirect(struct zbl_ufs_volume *volume, unsigned depth, uint64_t fragment, const uint8_t **block);
static enum zbl_ufs_result block_address(struct zbl_ufs_volume *volume, const struct zbl_ufs_file *file, uint64_t logical, uint64_t *fragment);
static enum zbl_ufs_result find_entry(struct zbl_ufs_volume *volume, const struct zbl_ufs_file *directory, const char *name, size_t length, uint32_t *number);

/*
 * Checks a partition's UFS superblock and prepares the volume for reads.
 */
enum zbl_ufs_result
zbl_ufs_mount(
	struct zbl_ufs_volume *volume,
	const struct zbl_ufs_device *device,
	struct zbl_ufs_work *work)
{
	enum zbl_ufs_result result;
	uint64_t block;
	uint32_t count;
	unsigned level;
	int sized;
	int read_error;

	/* Refuses a call without its objects. */
	if (volume == 0 || device == 0 || work == 0)
		return ZBL_UFS_INVALID_ARGUMENT;
	if (device->read == 0)
		return ZBL_UFS_INVALID_ARGUMENT;

	/* Refuses a medium block size the reader does not convert. */
	sized = power_of_two(device->block_size);
	if (!sized || device->block_size < 512U || device->block_size > 4096U)
		return ZBL_UFS_INVALID_ARGUMENT;

	/* Starts with an empty volume and no indirect block cached. */
	bytes_zero((uint8_t *)volume, sizeof(*volume));
	bytes_copy((uint8_t *)&volume->device, (const uint8_t *)device, sizeof(*device));
	volume->work = work;
	for (level = 0; level < UFS_INDIRECT_LEVELS; level++)
		work->indirect_number[level] = 0;

	/* Refuses a partition too small to hold the superblock. */
	if (device->block_count < (UFS_SUPERBLOCK_OFFSET + UFS_SUPERBLOCK_BYTES) / device->block_size)
		return ZBL_UFS_NOT_UFS;

	/* Reads the superblock. */
	block = UFS_SUPERBLOCK_OFFSET / device->block_size;
	count = UFS_SUPERBLOCK_BYTES / device->block_size;
	read_error = device->read(device->context, block, count, work->data);
	if (read_error != 0)
		return ZBL_UFS_IO;

	/* Checks it and keeps the geometry. */
	result = check_superblock(volume, work->data);
	if (result != ZBL_UFS_OK)
		return result;

	/* Succeeded: the volume is ready for lookups. */
	return ZBL_UFS_OK;
}

/*
 * Finds the regular file at an absolute path.
 *
 * Every component but the last must be a directory, and none may be a
 * symbolic link.
 */
enum zbl_ufs_result
zbl_ufs_lookup(
	struct zbl_ufs_volume *volume,
	const char *path,
	struct zbl_ufs_file *file)
{
	struct zbl_ufs_file directory;
	enum zbl_ufs_result result;
	uint32_t number;
	size_t position;
	size_t start;
	size_t length;

	/* Refuses a call without its objects. */
	if (volume == 0 || path == 0 || file == 0)
		return ZBL_UFS_INVALID_ARGUMENT;

	/* Refuses a path that is not absolute. */
	if (path[0] != '/')
		return ZBL_UFS_BAD_PATH;

	/* Measures the path, refusing a long one. */
	length = 0;
	while (path[length] != '\0') {
		length++;
		if (length > UFS_PATH_MAX)
			return ZBL_UFS_BAD_PATH;
	}

	/* Starts at the root directory. */
	result = read_inode(volume, UFS_ROOT_INODE, &directory);
	if (result != ZBL_UFS_OK)
		return result;
	if ((directory.mode & UFS_MODE_TYPE) != UFS_MODE_DIRECTORY)
		return ZBL_UFS_BAD_INODE;

	/* Walks the path one component at a time. */
	position = 1;
	for (;;) {
		/* Finds the end of this component. */
		start = position;
		while (path[position] != '\0' && path[position] != '/')
			position++;

		/* Refuses an empty component and a name no record holds. */
		if (position == start)
			return ZBL_UFS_BAD_PATH;
		if (position - start > UFS_NAME_MAX)
			return ZBL_UFS_BAD_PATH;

		/* Looks the component up in the directory. */
		result = find_entry(volume, &directory, path + start, position - start, &number);
		if (result != ZBL_UFS_OK)
			return result;
		result = read_inode(volume, number, file);
		if (result != ZBL_UFS_OK)
			return result;

		/* Refuses a symbolic link anywhere on the path. */
		if ((file->mode & UFS_MODE_TYPE) == UFS_MODE_SYMLINK)
			return ZBL_UFS_SYMLINK;

		/* The last component is the file itself. */
		if (path[position] == '\0')
			break;

		/* Any other component has to be a directory to go on. */
		if ((file->mode & UFS_MODE_TYPE) != UFS_MODE_DIRECTORY)
			return ZBL_UFS_NOT_DIRECTORY;
		bytes_copy((uint8_t *)&directory, (const uint8_t *)file, sizeof(directory));
		position++;
	}

	/* Refuses a path that names anything but a regular file. */
	if ((file->mode & UFS_MODE_TYPE) != UFS_MODE_REGULAR)
		return ZBL_UFS_NOT_REGULAR;

	/* Succeeded: the file is found. */
	return ZBL_UFS_OK;
}

/*
 * Reads bytes of a file from an offset.
 *
 * A hole reads as zeros.  The range must lie within the file.
 */
enum zbl_ufs_result
zbl_ufs_read(
	struct zbl_ufs_volume *volume,
	const struct zbl_ufs_file *file,
	uint64_t offset,
	void *buffer,
	size_t length)
{
	enum zbl_ufs_result result;
	uint8_t *output;
	uint64_t logical;
	uint64_t fragment;
	uint32_t within;
	uint32_t chunk;
	uint32_t needed;

	/* Refuses a call without its objects. */
	if (volume == 0 || file == 0)
		return ZBL_UFS_INVALID_ARGUMENT;
	if (buffer == 0 && length != 0)
		return ZBL_UFS_INVALID_ARGUMENT;

	/* Refuses a range past the file's end. */
	if (offset > file->size || length > file->size - offset)
		return ZBL_UFS_PAST_END;

	/* Copies the range one file system block at a time. */
	output = buffer;
	while (length != 0) {
		/* Finds the block, the place in it and how much of it is wanted. */
		logical = offset / volume->block_size;
		within = (uint32_t)(offset % volume->block_size);
		chunk = volume->block_size - within;
		if (chunk > length)
			chunk = (uint32_t)length;

		/* Finds where the block is on the disk. */
		result = block_address(volume, file, logical, &fragment);
		if (result != ZBL_UFS_OK)
			return result;

		/* A hole reads as zeros; a block reads the fragments that hold the bytes. */
		if (fragment == 0) {
			bytes_zero(output, chunk);
		} else {
			needed = (within + chunk + volume->fragment_size - 1U) /
			    volume->fragment_size * volume->fragment_size;
			result = read_fragments(volume, fragment, needed, volume->work->data);
			if (result != ZBL_UFS_OK)
				return result;
			bytes_copy(output, volume->work->data + within, chunk);
		}

		/* Moves past the bytes copied. */
		output += chunk;
		offset += chunk;
		length -= chunk;
	}

	/* Succeeded: the buffer holds the range. */
	return ZBL_UFS_OK;
}

/* Names a reader result for a loader message. */
const char *
zbl_ufs_result_name(
	enum zbl_ufs_result result)
{
	/* Names each result by what went wrong. */
	switch (result) {
	case ZBL_UFS_OK:
		return "ok";
	case ZBL_UFS_INVALID_ARGUMENT:
		return "invalid argument";
	case ZBL_UFS_IO:
		return "read error";
	case ZBL_UFS_NOT_UFS:
		return "not UFS";
	case ZBL_UFS_SWAPPED:
		return "byte-swapped volume";
	case ZBL_UFS_BAD_SUPERBLOCK:
		return "bad superblock";
	case ZBL_UFS_FRAGMENT_TOO_SMALL:
		return "fragment smaller than the medium's block";
	case ZBL_UFS_LARGER_THAN_PARTITION:
		return "volume larger than its partition";
	case ZBL_UFS_BAD_INODE:
		return "bad inode";
	case ZBL_UFS_BAD_BLOCK:
		return "bad block number";
	case ZBL_UFS_BAD_DIRECTORY:
		return "bad directory";
	case ZBL_UFS_BAD_PATH:
		return "bad path";
	case ZBL_UFS_NOT_FOUND:
		return "not found";
	case ZBL_UFS_NOT_DIRECTORY:
		return "not a directory";
	case ZBL_UFS_NOT_REGULAR:
		return "not a regular file";
	case ZBL_UFS_SYMLINK:
		return "symbolic link";
	case ZBL_UFS_PAST_END:
		return "past the end of the file";
	default:
		return "unknown";
	}
}

/* Decodes a little-endian 16-bit field. */
static uint16_t
get16(
	const uint8_t *bytes)
{
	uint16_t value;

	/* Puts the low byte first. */
	value = (uint16_t)bytes[0];
	value |= (uint16_t)((uint16_t)bytes[1] << 8);

	/* Reports the decoded field. */
	return value;
}

/* Decodes a little-endian 32-bit field. */
static uint32_t
get32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* Puts the low byte first. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* Reports the decoded field. */
	return value;
}

/* Decodes a little-endian 64-bit field. */
static uint64_t
get64(
	const uint8_t *bytes)
{
	uint64_t value;

	/* Puts the low word first. */
	value = (uint64_t)get32(bytes);
	value |= (uint64_t)get32(bytes + 4) << 32;

	/* Reports the decoded field. */
	return value;
}

/* Reports whether a value is a power of two. */
static int
power_of_two(
	uint32_t value)
{
	/* Zero is no power of two. */
	if (value == 0)
		return 0;

	/* A power of two has one bit set. */
	if ((value & (value - 1U)) != 0)
		return 0;

	/* Reports that exactly one bit is set. */
	return 1;
}

/* Copies bytes without the C library. */
static void
bytes_copy(
	uint8_t *destination,
	const uint8_t *source,
	size_t length)
{
	size_t index;

	/* Copies each byte in order. */
	for (index = 0; index < length; index++)
		destination[index] = source[index];
}

/* Clears bytes without the C library. */
static void
bytes_zero(
	uint8_t *destination,
	size_t length)
{
	size_t index;

	/* Clears each byte. */
	for (index = 0; index < length; index++)
		destination[index] = 0;
}

/*
 * Reads whole fragments from a fragment number on.
 *
 * The fragment number counts from the start of the partition in
 * fragment-sized units; the read converts it to the medium's blocks, which
 * are never larger than a fragment.
 */
static enum zbl_ufs_result
read_fragments(
	struct zbl_ufs_volume *volume,
	uint64_t fragment,
	uint32_t bytes,
	uint8_t *buffer)
{
	uint64_t fragment_count;
	uint64_t block;
	uint32_t count;
	int read_error;

	/* Refuses a run outside the volume. */
	fragment_count = bytes / volume->fragment_size;
	if (fragment >= volume->fragments)
		return ZBL_UFS_BAD_BLOCK;
	if (fragment_count > volume->fragments - fragment)
		return ZBL_UFS_BAD_BLOCK;

	/* Converts the run to the medium's blocks. */
	block = fragment * (volume->fragment_size / volume->device.block_size);
	count = bytes / volume->device.block_size;

	/* Reads the blocks. */
	read_error = volume->device.read(volume->device.context, block, count, buffer);
	if (read_error != 0)
		return ZBL_UFS_IO;

	/* Succeeded: the buffer holds the fragments. */
	return ZBL_UFS_OK;
}

/* Checks the superblock's geometry and keeps what the reads need. */
static enum zbl_ufs_result
check_superblock(
	struct zbl_ufs_volume *volume,
	const uint8_t *super)
{
	uint64_t partition_bytes;
	uint64_t inode_fragments;
	uint32_t magic;
	uint32_t fragment_shift;
	uint32_t shifted_fragments;
	int sized;

	/* Tells a volume of the other byte order from something that is not UFS. */
	magic = get32(super + UFS_FS_MAGIC);
	if (magic == UFS_MAGIC_SWAPPED)
		return ZBL_UFS_SWAPPED;
	if (magic != UFS_MAGIC)
		return ZBL_UFS_NOT_UFS;

	/* Takes the geometry. */
	volume->block_size = get32(super + UFS_FS_BSIZE);
	volume->fragment_size = get32(super + UFS_FS_FSIZE);
	volume->fragments_per_block = get32(super + UFS_FS_FRAG);
	fragment_shift = get32(super + UFS_FS_FRAGSHIFT);
	volume->groups = get32(super + UFS_FS_NCG);
	volume->inodes_per_group = get32(super + UFS_FS_IPG);
	volume->fragments_per_group = get32(super + UFS_FS_FPG);
	volume->inode_block = get32(super + UFS_FS_IBLKNO);
	volume->inodes_per_block = get32(super + UFS_FS_INOPB);
	volume->pointers_per_block = get32(super + UFS_FS_NINDIR);
	volume->fragments = get64(super + UFS_FS_SIZE);

	/* Requires a block size the kernel also reads. */
	sized = power_of_two(volume->block_size);
	if (!sized || volume->block_size < 4096U || volume->block_size > ZBL_UFS_BLOCK_MAX)
		return ZBL_UFS_BAD_SUPERBLOCK;

	/* Requires a fragment that is a power-of-two part of a block. */
	sized = power_of_two(volume->fragment_size);
	if (!sized || volume->fragment_size < 512U || volume->fragment_size > volume->block_size)
		return ZBL_UFS_BAD_SUPERBLOCK;
	if (volume->block_size / volume->fragment_size != volume->fragments_per_block)
		return ZBL_UFS_BAD_SUPERBLOCK;
	if (fragment_shift >= 32U)
		return ZBL_UFS_BAD_SUPERBLOCK;
	shifted_fragments = UINT32_C(1) << fragment_shift;
	if (shifted_fragments != volume->fragments_per_block)
		return ZBL_UFS_BAD_SUPERBLOCK;

	/* Refuses a fragment the medium's blocks cannot address alone. */
	if (volume->fragment_size < volume->device.block_size)
		return ZBL_UFS_FRAGMENT_TOO_SMALL;

	/* Requires the per-block counts that follow from the block size. */
	if (volume->pointers_per_block != volume->block_size / 8U)
		return ZBL_UFS_BAD_SUPERBLOCK;
	if (volume->inodes_per_block != volume->block_size / ZBL_UFS_INODE_SIZE)
		return ZBL_UFS_BAD_SUPERBLOCK;

	/* Requires groups that exist and hold their inode tables. */
	if (volume->groups == 0 || volume->inodes_per_group < 3U || volume->fragments_per_group == 0)
		return ZBL_UFS_BAD_SUPERBLOCK;
	if ((uint64_t)volume->groups * volume->inodes_per_group > UINT32_MAX)
		return ZBL_UFS_BAD_SUPERBLOCK;
	inode_fragments = ((uint64_t)volume->inodes_per_group + volume->inodes_per_block - 1U) /
	    volume->inodes_per_block * volume->fragments_per_block;
	if ((uint64_t)volume->inode_block + inode_fragments > volume->fragments_per_group)
		return ZBL_UFS_BAD_SUPERBLOCK;

	/* Requires a size whose last group starts inside it. */
	if (volume->fragments == 0)
		return ZBL_UFS_BAD_SUPERBLOCK;
	if ((uint64_t)(volume->groups - 1U) * volume->fragments_per_group >= volume->fragments)
		return ZBL_UFS_BAD_SUPERBLOCK;

	/* Refuses a volume that claims more than its partition holds. */
	partition_bytes = volume->device.block_count * volume->device.block_size;
	if (volume->fragments > partition_bytes / volume->fragment_size)
		return ZBL_UFS_LARGER_THAN_PARTITION;

	/* Succeeded: the geometry is consistent. */
	return ZBL_UFS_OK;
}

/* Reads one inode by its number. */
static enum zbl_ufs_result
read_inode(
	struct zbl_ufs_volume *volume,
	uint32_t number,
	struct zbl_ufs_file *file)
{
	enum zbl_ufs_result result;
	uint64_t fragment;
	uint64_t largest;
	uint64_t pointers;
	uint32_t group;
	uint32_t index;
	uint16_t type;

	/* Refuses a number outside the volume's inodes. */
	if (number < UFS_ROOT_INODE)
		return ZBL_UFS_BAD_INODE;
	if (number >= volume->groups * volume->inodes_per_group)
		return ZBL_UFS_BAD_INODE;

	/* Finds the block of the group's inode table that holds the inode. */
	group = number / volume->inodes_per_group;
	index = number % volume->inodes_per_group;
	fragment = (uint64_t)group * volume->fragments_per_group + volume->inode_block +
	    (uint64_t)(index / volume->inodes_per_block) * volume->fragments_per_block;

	/* Reads the block and copies the inode out of it. */
	result = read_fragments(volume, fragment, volume->block_size, volume->work->data);
	if (result != ZBL_UFS_OK)
		return result;
	bytes_copy(file->inode, volume->work->data + (index % volume->inodes_per_block) * ZBL_UFS_INODE_SIZE,
	    ZBL_UFS_INODE_SIZE);
	file->number = number;
	file->mode = get16(file->inode + UFS_DI_MODE);
	file->size = get64(file->inode + UFS_DI_SIZE);

	/* Refuses a type the reader has no use for, and a free inode. */
	type = (uint16_t)(file->mode & UFS_MODE_TYPE);
	if (type != UFS_MODE_REGULAR && type != UFS_MODE_DIRECTORY && type != UFS_MODE_SYMLINK)
		return ZBL_UFS_BAD_INODE;

	/* Refuses a size more blocks than the pointers can name would hold. */
	pointers = volume->pointers_per_block;
	largest = UFS_DIRECT_BLOCKS + pointers + pointers * pointers + pointers * pointers * pointers;
	if (file->size / volume->block_size >= largest)
		return ZBL_UFS_BAD_INODE;

	/* Succeeded: the file holds the inode. */
	return ZBL_UFS_OK;
}

/*
 * Reads one indirect block, keeping it for the next read at its depth.
 *
 * The depth is the number of indirect levels below this block: 0 for a
 * block whose pointers name data.
 */
static enum zbl_ufs_result
read_indirect(
	struct zbl_ufs_volume *volume,
	unsigned depth,
	uint64_t fragment,
	const uint8_t **block)
{
	struct zbl_ufs_work *work;
	enum zbl_ufs_result result;

	/* Uses the block already read at this depth. */
	work = volume->work;
	if (work->indirect_number[depth] == fragment) {
		*block = work->indirect[depth];
		return ZBL_UFS_OK;
	}

	/* Reads the whole block, forgetting the cached one first. */
	work->indirect_number[depth] = 0;
	result = read_fragments(volume, fragment, volume->block_size, work->indirect[depth]);
	if (result != ZBL_UFS_OK)
		return result;

	/* Succeeded: the block is cached at its depth. */
	work->indirect_number[depth] = fragment;
	*block = work->indirect[depth];
	return ZBL_UFS_OK;
}

/*
 * Finds the fragment a file's logical block starts at.
 *
 * Zero means a hole.  An indirect block's number must name a whole block
 * inside the volume.
 */
static enum zbl_ufs_result
block_address(
	struct zbl_ufs_volume *volume,
	const struct zbl_ufs_file *file,
	uint64_t logical,
	uint64_t *fragment)
{
	enum zbl_ufs_result result;
	const uint8_t *block;
	uint64_t pointers;
	uint64_t span;
	uint64_t scale;
	uint64_t pointer;
	unsigned level;
	unsigned depth;
	unsigned step;

	/* A direct block's number is in the inode itself. */
	if (logical < UFS_DIRECT_BLOCKS) {
		*fragment = get64(file->inode + UFS_DI_DB + logical * 8U);
		return ZBL_UFS_OK;
	}

	/* Finds the indirect level whose span holds the block. */
	pointers = volume->pointers_per_block;
	logical -= UFS_DIRECT_BLOCKS;
	span = pointers;
	for (level = 0; level < UFS_INDIRECT_LEVELS; level++) {
		if (logical < span)
			break;
		logical -= span;
		span *= pointers;
	}

	/* Refuses a block beyond what the deepest level names. */
	if (level == UFS_INDIRECT_LEVELS)
		return ZBL_UFS_BAD_INODE;

	/* Walks down from the inode's pointer of that level to the data. */
	pointer = get64(file->inode + UFS_DI_IB + level * 8U);
	scale = span / pointers;
	for (step = 0; step <= level; step++) {
		/* A hole anywhere on the way is a hole for the block. */
		if (pointer == 0) {
			*fragment = 0;
			return ZBL_UFS_OK;
		}

		/* Reads the indirect block, which has to fit in the volume. */
		depth = level - step;
		result = read_indirect(volume, depth, pointer, &block);
		if (result != ZBL_UFS_OK)
			return result;

		/* Takes the pointer to the next level. */
		pointer = get64(block + (logical / scale) % pointers * 8U);
		scale /= pointers;
		if (scale == 0)
			scale = 1;
	}

	/* Succeeded: the pointer names the data block, or 0 for a hole. */
	*fragment = pointer;
	return ZBL_UFS_OK;
}

/*
 * Finds a name in a directory and reports its inode number.
 */
static enum zbl_ufs_result
find_entry(
	struct zbl_ufs_volume *volume,
	const struct zbl_ufs_file *directory,
	const char *name,
	size_t length,
	uint32_t *number)
{
	uint8_t chunk[UFS_DIRECTORY_BLOCK];
	enum zbl_ufs_result result;
	uint64_t position;
	uint32_t entry_number;
	uint32_t at;
	uint16_t record_length;
	uint8_t name_length;
	size_t index;
	int same;

	/* Refuses a directory that is not whole directory blocks, or too large. */
	if (directory->size % UFS_DIRECTORY_BLOCK != 0)
		return ZBL_UFS_BAD_DIRECTORY;
	if (directory->size > ZBL_UFS_DIRECTORY_MAX)
		return ZBL_UFS_BAD_DIRECTORY;

	/* Reads the directory one directory block at a time. */
	for (position = 0; position < directory->size; position += UFS_DIRECTORY_BLOCK) {
		result = zbl_ufs_read(volume, directory, position, chunk, sizeof(chunk));
		if (result != ZBL_UFS_OK)
			return result;

		/* Walks the records of the block, none of which may cross its end. */
		at = 0;
		while (at < UFS_DIRECTORY_BLOCK) {
			/* Takes the record's header. */
			if (at + 8U > UFS_DIRECTORY_BLOCK)
				return ZBL_UFS_BAD_DIRECTORY;
			entry_number = get32(chunk + at);
			record_length = get16(chunk + at + 4U);
			name_length = chunk[at + 7U];

			/* Refuses a record of an impossible length. */
			if (record_length < 8U || (record_length & 3U) != 0)
				return ZBL_UFS_BAD_DIRECTORY;
			if (at + record_length > UFS_DIRECTORY_BLOCK)
				return ZBL_UFS_BAD_DIRECTORY;
			if (8U + (uint32_t)name_length > record_length)
				return ZBL_UFS_BAD_DIRECTORY;

			/* Compares a used record's name with the one sought. */
			if (entry_number != 0 && name_length == length) {
				same = 1;
				for (index = 0; index < length; index++) {
					if (chunk[at + 8U + index] != (uint8_t)name[index]) {
						same = 0;
						break;
					}
				}

				/* Succeeded: the name is in the directory. */
				if (same) {
					*number = entry_number;
					return ZBL_UFS_OK;
				}
			}

			/* Steps to the next record. */
			at += record_length;
		}
	}

	/* The directory has no such name. */
	return ZBL_UFS_NOT_FOUND;
}
