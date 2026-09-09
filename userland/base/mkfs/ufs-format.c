/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The streaming initializer for the maintained UFS overlay data image.
 */

#include "ufs-format.h"
#include "ufs-super.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define FORMAT_BLOCK 8192U
#define FORMAT_FRAGMENT 1024U
#define FORMAT_IPG 256U
#define FORMAT_DATA_FRAGMENT 144U
#define FORMAT_CG0_USED 440U
#define FORMAT_JOURNAL_BYTES (128U * 1024U)
#define FORMAT_ROOT_MARKER "zedBSD ufs root v1\n"
#define FORMAT_LOG_SECTORS 256U
#define FORMAT_SNAPSHOT_SECTORS 2049U
#define FORMAT_TAIL_BYTES ((2U + FORMAT_LOG_SECTORS + FORMAT_SNAPSHOT_SECTORS) * 512U)
#define FORMAT_REGULAR 0100000U
#define FORMAT_DIRECTORY 0040000U
#define FORMAT_NATIVE 2
#define FORMAT_NATIVE_USED 152U
#define FORMAT_MAX_FPG ((FORMAT_BLOCK - 200U) * 8U)

/* One exact byte interval already compared with the generator's final bytes. */
struct format_extent {
	uint64_t start;
	uint64_t end;
};

/* Geometry and optional bounded coverage owned by one formatter invocation. */
struct format_context {
	int fd;
	int verify;
	int profile;
	uint64_t medium_bytes;
	uint64_t fragments;
	uint32_t ncg;
	uint32_t fpg;
	uint32_t cgsize;
	struct format_extent *extents;
	size_t extent_count;
	size_t extent_capacity;
};

static int format_run(int fd, uint64_t bytes, int verify, int profile);
static int record_extent(struct format_context *context, uint64_t offset, size_t length);
static int verify_gaps(struct format_context *context);
static int format_tail(struct format_context *context);
static uint32_t locator_digest(const uint8_t *buffer, size_t length);
static void put16(uint8_t *buffer, size_t offset, uint16_t value);
static void put32(uint8_t *buffer, size_t offset, uint32_t value);
static void put64(uint8_t *buffer, size_t offset, uint64_t value);
static uint32_t record_crc(const uint8_t *buffer, size_t length);
static int initialize_context(struct format_context *context, int fd, uint64_t bytes, int verify, int profile);
static int read_exact(int fd, void *buffer, size_t length, uint64_t offset);
static int transfer(struct format_context *context, uint64_t offset, const uint8_t *buffer, size_t length);
static int zero_region(struct format_context *context, uint64_t offset, uint64_t length);
static int format_metadata(struct format_context *context);
static int format_journal(struct format_context *context, unsigned slot);
static int format_directories(struct format_context *context);
static void directory_entry(uint8_t *buffer, size_t offset, uint32_t inode, uint16_t length, uint8_t type, const char *name);
static void format_inode(uint8_t *buffer, uint32_t number, uint16_t mode, uint16_t links, uint64_t bytes, uint32_t first, unsigned blocks, uint32_t indirect);
static int format_inodes(struct format_context *context);
static void make_cg(struct format_context *context, unsigned index, uint8_t *buffer);
static void make_super(struct format_context *context, uint8_t *buffer);

/*
 * Validates the exact geometry supported by the maintained image backend.
 */
int
ufs_format_validate_size(
	uint64_t bytes)
{
	/* Reject sizes outside the supported formatter and file-offset range. */
	if (bytes < UFS_FORMAT_MIN_BYTES || bytes > UFS_FORMAT_MAX_BYTES)
		return EINVAL;

	/* Require an integral count of filesystem fragments. */
	if (bytes % FORMAT_FRAGMENT != 0)
		return EINVAL;

	/* Report a representable data-image size. */
	return 0;
}

/*
 * Writes the ordinary overlay-ready UFS image under the caller's reservation.
 */
int
ufs_format_write(
	int fd,
	uint64_t bytes)
{
	int error;

	/* Generate the ordinary profile without a persistence tail. */
	error = format_run(fd, bytes, 0, 0);
	return error;
}

/*
 * Verifies the ordinary profile after the caller reopens its descriptor.
 */
int
ufs_format_verify(
	int fd,
	uint64_t bytes)
{
	int error;

	/* Compare the entire initialized metadata set. */
	error = format_run(fd, bytes, 1, 0);
	return error;
}

/*
 * Writes the explicit journal and snapshot profile under one reservation.
 */
int
ufs_format_feature_write(
	int fd,
	uint64_t bytes)
{
	int error;

	/* Select the profile in this call's private context. */
	error = format_run(fd, bytes, 0, 1);
	return error;
}

/*
 * Verifies the journal and inactive snapshot regions with their filesystem.
 */
int
ufs_format_feature_verify(
	int fd,
	uint64_t bytes)
{
	int error;

	/* Verify the same fixed profile selected by the write callback. */
	error = format_run(fd, bytes, 1, 1);
	return error;
}

/*
 * Checks the ordinary image including every unused and trailing byte.
 */
int
ufs_format_pristine(
	int fd,
	uint64_t bytes)
{
	int error;

	/* Enables exact coverage in addition to metadata comparison. */
	error = format_run(fd, bytes, 2, 0);
	return error;
}

/*
 * Checks the persistence profile including all unused filesystem bytes.
 */
int
ufs_format_feature_pristine(
	int fd,
	uint64_t bytes)
{
	int error;

	/* Selects the same deterministic profile with full-byte verification. */
	error = format_run(fd, bytes, 2, 1);
	return error;
}

/* Native geometry is bounded by the actual format and host offset types. */
int
ufs_format_native_validate_size(uint64_t bytes)
{
	struct format_context context;

	return initialize_context(&context, -1, bytes, 0, FORMAT_NATIVE);
}

/* Reports allocatable full blocks and inodes without opening or writing media. */
int
ufs_format_native_capacity(uint64_t bytes, struct ufs_format_capacity *capacity)
{
	struct format_context context;
	uint64_t free_fragments;
	int error;

	if (capacity == NULL)
		return EINVAL;
	memset(capacity, 0, sizeof(*capacity));
	error = initialize_context(&context, -1, bytes, 0, FORMAT_NATIVE);
	if (error != 0)
		return error;

	/* Only the final group has a partial block; every other boundary is aligned. */
	free_fragments = context.fragments - (uint64_t)context.ncg * FORMAT_DATA_FRAGMENT -
	    (FORMAT_NATIVE_USED - FORMAT_DATA_FRAGMENT);
	capacity->free_bytes = free_fragments / (FORMAT_BLOCK / FORMAT_FRAGMENT) * FORMAT_BLOCK;
	capacity->free_inodes = (uint64_t)context.ncg * FORMAT_IPG - 3U;
	capacity->allocation_size = FORMAT_BLOCK;
	return 0;
}

int
ufs_format_native_write(int fd, uint64_t bytes)
{
	return format_run(fd, bytes, 0, FORMAT_NATIVE);
}

int
ufs_format_native_verify(int fd, uint64_t bytes)
{
	return format_run(fd, bytes, 1, FORMAT_NATIVE);
}

/* Generates or checks one explicit profile without mutable global state. */
static int
format_run(
	int fd,
	uint64_t bytes,
	int verify,
	int profile)
{
	struct format_context context;
	struct ufs_super super;
	uint8_t buffer[FORMAT_BLOCK];
	int error;

	/* Reject unsupported geometry before touching the descriptor. */
	if (profile != FORMAT_NATIVE) {
		error = ufs_format_validate_size(bytes);
		if (error != 0)
			return error;
	}
	error = initialize_context(&context, fd, bytes, verify, profile);
	if (error != 0)
		return error;

	/* Decode a reopened image or invalidate its previous primary first. */
	if (verify) {
		error = read_exact(fd, buffer, sizeof(buffer), UFS_SBLOCK_OFFSET);
		if (error != 0)
			return error;
		error = ufs_super_decode(buffer, sizeof(buffer), bytes / 512U, &super);
		if (error != 0)
			return error;
	} else {
		error = zero_region(&context, UFS_SBLOCK_OFFSET, FORMAT_BLOCK);
		if (error != 0)
			return error;
		if (profile == FORMAT_NATIVE && fsync(fd) < 0)
			return errno;
	}

	/* Allocates only geometry-bounded coverage for pristine verification. */
	if (verify == 2) {
		context.extent_capacity = (size_t)context.ncg * 4U + 16U;
		context.extents = calloc(context.extent_capacity, sizeof(*context.extents));
		if (context.extents == NULL)
			return ENOMEM;
	}

	/* Install or verify every referenced region before the primary. */
	error = format_metadata(&context);

	/* Checks the complement only after every deterministic region matches. */
	if (error == 0 && verify == 2)
		error = verify_gaps(&context);

	/* Releases per-call coverage even when a comparison or read failed. */
	free(context.extents);
	return error;
}

/* Records disjoint exact extents, coalescing only adjacent intervals. */
static int
record_extent(
	struct format_context *context,
	uint64_t offset,
	size_t length)
{
	struct format_extent *extents;
	uint64_t end;
	size_t index;
	size_t position;

	/* Rejects empty, overflowing or out-of-image programming errors. */
	if (length == 0 || offset > context->medium_bytes)
		return EINVAL;

	/* Subtracts before adding so an invalid extent cannot wrap. */
	if ((uint64_t)length > context->medium_bytes - offset)
		return EINVAL;

	/* Finds the insertion point by exact byte order, not allocation maps. */
	end = offset + length;
	extents = context->extents;
	position = 0;
	while (position < context->extent_count && extents[position].start < offset)
		position++;

	/* Rejects overlapping comparisons rather than hiding generator mistakes. */
	if (position > 0 && extents[position - 1U].end > offset)
		return EINVAL;

	/* Checks the following interval independently. */
	if (position < context->extent_count && end > extents[position].start)
		return EINVAL;

	/* Extends the predecessor when this range touches its end. */
	if (position > 0 && extents[position - 1U].end == offset) {
		extents[position - 1U].end = end;

		/* Joins both neighbors when this range exactly bridges their gap. */
		if (position < context->extent_count && end == extents[position].start) {
			extents[position - 1U].end = extents[position].end;
			for (index = position; index + 1U < context->extent_count; index++)
				extents[index] = extents[index + 1U];
			context->extent_count--;
		}

		return 0;
	}

	/* Extends a successor without consuming an additional slot. */
	if (position < context->extent_count && end == extents[position].start) {
		extents[position].start = offset;
		return 0;
	}

	/* Refuses to exceed the explicit geometry-derived metadata budget. */
	if (context->extent_count == context->extent_capacity)
		return EOVERFLOW;

	/* Inserts a new isolated interval while preserving sorted order. */
	for (index = context->extent_count; index > position; index--)
		extents[index] = extents[index - 1U];
	extents[position].start = offset;
	extents[position].end = end;
	context->extent_count++;
	return 0;
}

/* Checks every byte outside the deterministic extents against zero. */
static int
verify_gaps(
	struct format_context *context)
{
	uint8_t buffer[FORMAT_BLOCK];
	uint64_t cursor;
	uint64_t end;
	size_t index;
	size_t count;
	size_t byte;
	int error;

	/* Includes the trailing gap with a sentinel at the exact medium end. */
	cursor = 0;
	for (index = 0; index <= context->extent_count; index++) {
		end = context->medium_bytes;
		if (index < context->extent_count)
			end = context->extents[index].start;

		/* Reads the complete gap, including a partial final buffer. */
		while (cursor < end) {
			count = sizeof(buffer);
			if (end - cursor < count)
				count = (size_t)(end - cursor);
			error = read_exact(context->fd, buffer, count, cursor);
			if (error != 0)
				return error;

			/* Rejects any previous payload hidden in currently unused space. */
			for (byte = 0; byte < count; byte++) {
				if (buffer[byte] != 0)
					return EIO;
			}

			cursor += count;
		}

		/* Skips only bytes already compared successfully with the generator. */
		if (index < context->extent_count)
			cursor = context->extents[index].end;
	}

	return 0;
}

/* Stores a little-endian sixteen-bit field. */
static void
put16(
	uint8_t *buffer,
	size_t offset,
	uint16_t value)
{
	/* Write the low byte before the high byte. */
	buffer[offset] = (uint8_t)value;
	buffer[offset + 1U] = (uint8_t)(value >> 8);
}

/* Stores a little-endian thirty-two-bit field. */
static void
put32(
	uint8_t *buffer,
	size_t offset,
	uint32_t value)
{
	/* Write the two constituent little-endian halves. */
	put16(buffer, offset, (uint16_t)value);
	put16(buffer, offset + 2U, (uint16_t)(value >> 16));
}

/* Stores a little-endian sixty-four-bit field. */
static void
put64(
	uint8_t *buffer,
	size_t offset,
	uint64_t value)
{
	/* Write the two constituent little-endian words. */
	put32(buffer, offset, (uint32_t)value);
	put32(buffer, offset + 4U, (uint32_t)(value >> 32));
}

/* Computes the overlay record CRC32 without a lookup table. */
static uint32_t
record_crc(
	const uint8_t *buffer,
	size_t length)
{
	uint32_t crc;
	size_t index;
	unsigned bit;

	/* Fold each byte into the standard reflected CRC32 polynomial. */
	crc = UINT32_C(0xffffffff);
	for (index = 0; index < length; index++) {
		crc ^= buffer[index];

		/* Advance the polynomial by one byte. */
		for (bit = 0; bit < 8U; bit++) {
			/* Apply the polynomial when the outgoing bit is set. */
			if ((crc & 1U) != 0)
				crc = (crc >> 1) ^ UINT32_C(0xedb88320);
			else
				crc >>= 1;
		}
	}

	/* Return the conventional complemented checksum. */
	return crc ^ UINT32_C(0xffffffff);
}

/* Derives the bounded group geometry from a validated file size. */
static int
initialize_context(
	struct format_context *context,
	int fd,
	uint64_t bytes,
	int verify,
	int profile)
{
	uint64_t groups, last;
	off_t final_offset;
	uint32_t fpg;

	/* Reject unsupported offsets before subtraction or any I/O. */
	if (bytes < UFS_FORMAT_MIN_BYTES || bytes % FORMAT_FRAGMENT != 0)
		return EINVAL;
	final_offset = (off_t)(bytes - 1);
	if (final_offset < 0 || (uint64_t)final_offset != bytes - 1)
		return EOVERFLOW;

	/* Keep geometry identical to the maintained host backend. */
	memset(context, 0, sizeof(*context));
	context->fd = fd;
	context->verify = verify;
	context->profile = profile;
	context->medium_bytes = bytes;
	if (profile)
		bytes = (bytes - FORMAT_TAIL_BYTES) / FORMAT_FRAGMENT * FORMAT_FRAGMENT;
	context->fragments = bytes / FORMAT_FRAGMENT;
	if (profile == FORMAT_NATIVE) {
		/* A bounded search keeps even the final group's metadata in range. */
		for (fpg = FORMAT_MAX_FPG; fpg >= 1024U; fpg -= 8U) {
			groups = (context->fragments + fpg - 1U) / fpg;
			if (groups > UINT32_MAX / FORMAT_IPG)
				return EOVERFLOW;
			last = context->fragments - (groups - 1U) * fpg;
			if (last <= FORMAT_DATA_FRAGMENT)
				continue;
			context->ncg = (uint32_t)groups;
			context->fpg = fpg;
			context->cgsize = 200U + fpg / 8U;
			return 0;
		}
		return EINVAL;
	}
	context->ncg = 2U;

	/* Add groups until each bitmap fits one filesystem block. */
	do {
		context->fpg = ((context->fragments + context->ncg - 1U) /
		    context->ncg + 7U) & ~7U;
		context->cgsize = 200U + (context->fpg + 7U) / 8U;
		if (context->cgsize <= FORMAT_BLOCK)
			break;
		context->ncg++;
	} while (1);
	return 0;
}

/* Reads one complete record and refuses a truncated transfer. */
static int
read_exact(
	int fd,
	void *buffer,
	size_t length,
	uint64_t offset)
{
	ssize_t count;

	/* Retry only interruptions that transferred no bytes. */
	do {
		count = pread(fd, buffer, length, (off_t)offset);
	} while (count < 0 && errno == EINTR);

	/* Preserve the operating system's read error. */
	if (count < 0)
		return errno;

	/* Refuse a short read instead of accepting incomplete metadata. */
	if ((size_t)count != length)
		return EIO;

	/* Report the complete record. */
	return 0;
}

/* Writes or verifies one bounded deterministic region. */
static int
transfer(
	struct format_context *context,
	uint64_t offset,
	const uint8_t *buffer,
	size_t length)
{
	uint8_t actual[FORMAT_BLOCK];
	ssize_t count;
	int error;

	/* Guard the bounded comparison buffer against a programming error. */
	if (length > sizeof(actual))
		return EINVAL;
	if (offset > context->medium_bytes || length > context->medium_bytes - offset)
		return EINVAL;

	/* Compare a reopened record with its deterministic initial contents. */
	if (context->verify) {
		error = read_exact(context->fd, actual, length, offset);
		if (error != 0)
			return error;

		/* Refuse a mismatched metadata or journal record. */
		if (memcmp(actual, buffer, length) != 0)
			return EIO;

		/* Records exact final-byte coverage only in the pristine mode. */
		if (context->verify == 2) {
			error = record_extent(context, offset, length);
			return error;
		}

		/* Report an exact match. */
		return 0;
	}

	/* Retry interruptions while preserving strict short-write failure. */
	do {
		count = pwrite(context->fd, buffer, length, (off_t)offset);
	} while (count < 0 && errno == EINTR);

	/* Preserve a failed write's error. */
	if (count < 0)
		return errno;

	/* Refuse an incomplete write before publishing further structures. */
	if ((size_t)count != length)
		return EIO;

	/* Report the complete write. */
	return 0;
}

/* Clears or checks a region using one filesystem-block buffer. */
static int
zero_region(
	struct format_context *context,
	uint64_t offset,
	uint64_t length)
{
	uint8_t buffer[FORMAT_BLOCK];
	size_t count;
	int error;

	/* Process every byte without allocating an image-sized buffer. */
	memset(buffer, 0, sizeof(buffer));
	while (length != 0) {
		count = length < sizeof(buffer) ? (size_t)length : sizeof(buffer);
		error = transfer(context, offset, buffer, count);
		if (error != 0)
			return error;

		/* Advance by the completed bounded transfer. */
		offset += count;
		length -= count;
	}

	/* Report the completed zero region. */
	return 0;
}

/* Installs all deterministic metadata before the primary superblock. */
static int
format_metadata(
	struct format_context *context)
{
	uint8_t buffer[FORMAT_BLOCK];
	unsigned group;
	uint64_t offset;
	int error;

	/* Initialize the reserved boot area of every cylinder group. */
	for (group = 0; group < context->ncg; group++) {
		offset = (uint64_t)group * context->fpg * FORMAT_FRAGMENT;
		error = zero_region(context, offset, UFS_SBLOCK_OFFSET);
		if (error != 0)
			return error;
	}

	/* Install the two journal files and their indirect blocks. */
	if (context->profile != FORMAT_NATIVE) {
		error = format_journal(context, 0);
		if (error != 0)
			return error;
		error = format_journal(context, 1);
		if (error != 0)
			return error;
	}

	/* Install the root marker and two initial directories. */
	error = format_directories(context);
	if (error != 0)
		return error;

	/* Install the initial inode table entries. */
	error = format_inodes(context);
	if (error != 0)
		return error;

	/* Install all allocation maps and their accounting summaries. */
	for (group = 0; group < context->ncg; group++) {
		make_cg(context, group, buffer);
		offset = ((uint64_t)group * context->fpg + 72U) * FORMAT_FRAGMENT;
		error = transfer(context, offset, buffer, sizeof(buffer));
		if (error != 0)
			return error;
	}

	/* Initialize optional persistence records before publishing the volume. */
	error = format_tail(context);
	if (error != 0)
		return error;

	/* Write alternate superblocks before publishing the primary copy. */
	make_super(context, buffer);
	for (group = 1; group < context->ncg; group++) {
		offset = ((uint64_t)group * context->fpg + 64U) * FORMAT_FRAGMENT;
		error = transfer(context, offset, buffer, sizeof(buffer));
		if (error != 0)
			return error;
	}

	/* Publish the primary superblock only after the complete tree exists. */
	if (context->profile == FORMAT_NATIVE && !context->verify && fsync(context->fd) < 0)
		return errno;
	error = transfer(context, UFS_SBLOCK_OFFSET, buffer, sizeof(buffer));
	if (error != 0)
		return error;
	if (context->profile == FORMAT_NATIVE && !context->verify && fsync(context->fd) < 0)
		return errno;

	/* Report all initialized records. */
	return 0;
}

/* Builds a journal slot and its single-indirect block. */
static int
format_journal(
	struct format_context *context,
	unsigned slot)
{
	uint8_t buffer[FORMAT_BLOCK];
	uint32_t first;
	uint32_t checksum;
	unsigned block;
	int error;

	/* Initialize every data block in the selected journal. */
	first = slot == 0 ? 144U : 280U;
	for (block = 0; block < 16U; block++) {
		memset(buffer, 0, sizeof(buffer));

		/* Seed the active journal with an empty committed epoch. */
		if (slot == 0 && block == 0) {
			memcpy(buffer, "ZOVLSLT", 7U);
			put16(buffer, 8U, 1U);
			put16(buffer, 10U, 48U);
			put32(buffer, 12U, 512U);
			memcpy(buffer + 16U, "ZOVL", 4U);
			put64(buffer, 24U, 1U);
			put32(buffer, 36U, 1U);
			checksum = record_crc(buffer, 508U);
			put32(buffer, 508U, checksum);

			/* Seal the empty snapshot's commit record and header digest. */
			memcpy(buffer + 512U, "ZOVLCMT", 7U);
			put16(buffer, 520U, 1U);
			memcpy(buffer + 524U, "ZOVL", 4U);
			put64(buffer, 528U, 1U);
			put32(buffer, 540U, 1U);
			checksum = record_crc(buffer, 512U);
			put32(buffer, 552U, checksum);
			checksum = record_crc(buffer + 512U, 508U);
			put32(buffer, 1020U, checksum);
		}

		/* Transfer the complete journal data block. */
		error = transfer(context, (first + block * 8U) * FORMAT_FRAGMENT, buffer, sizeof(buffer));
		if (error != 0)
			return error;
	}

	/* Map the four data blocks beyond the twelve direct inode pointers. */
	memset(buffer, 0, sizeof(buffer));
	for (block = 0; block < 4U; block++)
		put64(buffer, block * 8U, first + (12U + block) * 8U);

	/* Install the journal's single-indirect block. */
	error = transfer(context, (first + 128U) * FORMAT_FRAGMENT, buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Report a complete initial journal slot. */
	return 0;
}

/* Encodes one BSD directory record with its caller-selected record length. */
static void
directory_entry(
	uint8_t *buffer,
	size_t offset,
	uint32_t inode,
	uint16_t length,
	uint8_t type,
	const char *name)
{
	size_t name_length;

	/* Fill the directory entry without relying on native structure padding. */
	name_length = strlen(name);
	put32(buffer, offset, inode);
	put16(buffer, offset + 4U, length);
	buffer[offset + 6U] = type;
	buffer[offset + 7U] = (uint8_t)name_length;
	memcpy(buffer + offset + 8U, name, name_length);
}

/* Builds the root marker and directory blocks in host allocation order. */
static int
format_directories(
	struct format_context *context)
{
	static const char marker[] = FORMAT_ROOT_MARKER;
	uint8_t buffer[FORMAT_BLOCK];
	int error;

	if (context->profile == FORMAT_NATIVE) {
		/* Native root starts with only its own dot entries. */
		memset(buffer, 0, sizeof(buffer));
		directory_entry(buffer, 0U, 2U, 12U, 4U, ".");
		directory_entry(buffer, 12U, 2U, 500U, 4U, "..");
		return transfer(context, FORMAT_DATA_FRAGMENT * FORMAT_FRAGMENT,
		    buffer, sizeof(buffer));
	}

	/* Install the maintained backend's root marker. */
	memset(buffer, 0, sizeof(buffer));
	memcpy(buffer, marker, sizeof(marker) - 1U);
	error = transfer(context, 416U * FORMAT_FRAGMENT, buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Install the root directory in the backend's sorted file order. */
	memset(buffer, 0, sizeof(buffer));
	directory_entry(buffer, 0U, 2U, 12U, 4U, ".");
	directory_entry(buffer, 12U, 2U, 12U, 4U, "..");
	directory_entry(buffer, 24U, 3U, 16U, 8U, ".zovl0");
	directory_entry(buffer, 40U, 4U, 16U, 8U, ".zovl1");
	directory_entry(buffer, 56U, 5U, 456U, 4U, "etc");
	error = transfer(context, 424U * FORMAT_FRAGMENT, buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Install the marker directory with the correct parent inode. */
	memset(buffer, 0, sizeof(buffer));
	directory_entry(buffer, 0U, 5U, 12U, 4U, ".");
	directory_entry(buffer, 12U, 2U, 12U, 4U, "..");
	directory_entry(buffer, 24U, 6U, 488U, 8U, "zedbsd-root");
	error = transfer(context, 432U * FORMAT_FRAGMENT, buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Report the completed initial namespace. */
	return 0;
}

/* Encodes one inode in the first cylinder group's first inode block. */
static void
format_inode(
	uint8_t *buffer,
	uint32_t number,
	uint16_t mode,
	uint16_t links,
	uint64_t bytes,
	uint32_t first,
	unsigned blocks,
	uint32_t indirect)
{
	uint8_t *inode;
	unsigned index;
	unsigned sectors;

	/* Initialize the inode's identity and ownership-neutral attributes. */
	inode = buffer + number * UFS_DINODE_SIZE;
	put16(inode, UFS_DI_MODE, mode);
	put16(inode, UFS_DI_NLINK, links);
	put64(inode, UFS_DI_SIZE, bytes);
	put32(inode, UFS_DI_BLKSIZE, FORMAT_BLOCK);
	put32(inode, UFS_DI_GEN, number);

	/* Populate the direct data addresses in allocation order. */
	for (index = 0; index < blocks && index < UFS_NDADDR; index++)
		put64(inode, UFS_DI_DB + index * 8U, first + index * 8U);

	/* Include the indirect block in the sector accounting when present. */
	sectors = blocks * 16U;
	if (indirect != 0) {
		put64(inode, UFS_DI_IB, indirect);
		sectors += 16U;
	}

	/* Store the total allocation in 512-byte sectors. */
	put64(inode, UFS_DI_BLOCKS, sectors);
}

/* Builds the fixed tree's inodes and verifies unused inode table space. */
static int
format_inodes(
	struct format_context *context)
{
	uint8_t buffer[FORMAT_BLOCK];
	uint64_t offset;
	unsigned group;
	int error;

	/* Encode the two directories and three regular files. */
	memset(buffer, 0, sizeof(buffer));
	if (context->profile == FORMAT_NATIVE) {
		format_inode(buffer, 2U, FORMAT_DIRECTORY | 0755U, 2U, 512U,
		    FORMAT_DATA_FRAGMENT, 1U, 0U);
	} else {
		format_inode(buffer, 2U, FORMAT_DIRECTORY | 0755U, 3U, 512U, 424U, 1U, 0U);
		format_inode(buffer, 3U, FORMAT_REGULAR | 0644U, 1U, FORMAT_JOURNAL_BYTES, 144U, 16U, 272U);
		format_inode(buffer, 4U, FORMAT_REGULAR | 0644U, 1U, FORMAT_JOURNAL_BYTES, 280U, 16U, 408U);
		format_inode(buffer, 5U, FORMAT_DIRECTORY | 0755U, 2U, 512U, 432U, 1U, 0U);
		format_inode(buffer, 6U, FORMAT_REGULAR | 0644U, 1U, sizeof(FORMAT_ROOT_MARKER) - 1U, 416U, 1U, 0U);
	}
	error = transfer(context, 80U * FORMAT_FRAGMENT, buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Preserve zeroed unused entries in the first cylinder group. */
	error = zero_region(context, 88U * FORMAT_FRAGMENT, 56U * FORMAT_FRAGMENT);
	if (error != 0)
		return error;

	/* Preserve the entirely free inode tables in every later group. */
	for (group = 1; group < context->ncg; group++) {
		offset = ((uint64_t)group * context->fpg + 80U) * FORMAT_FRAGMENT;
		error = zero_region(context, offset, 64U * FORMAT_FRAGMENT);
		if (error != 0)
			return error;
	}

	/* Report the completed inode tables. */
	return 0;
}

/* Encodes a cylinder group's bitmaps and exact free-space counters. */
static void
make_cg(
	struct format_context *context,
	unsigned index,
	uint8_t *buffer)
{
	uint32_t fragments;
	uint32_t used;
	uint32_t fragment;
	uint32_t free_fragments;
	uint64_t remaining;
	unsigned allocated_inodes, directories;

	/* Initialize the group header and fixed bitmap locations. */
	memset(buffer, 0, FORMAT_BLOCK);
	remaining = context->fragments - (uint64_t)index * context->fpg;
	fragments = remaining > context->fpg ? context->fpg : (uint32_t)remaining;
	used = index == 0 ? FORMAT_CG0_USED : FORMAT_DATA_FRAGMENT;
	allocated_inodes = 7U;
	directories = 2U;
	if (context->profile == FORMAT_NATIVE) {
		if (index == 0)
			used = FORMAT_NATIVE_USED;
		allocated_inodes = 3U;
		directories = 1U;
	}
	free_fragments = fragments - used;
	put32(buffer, UFS_CG_MAGIC, UFS_CG_MAGIC_VALUE);
	put32(buffer, UFS_CG_CGX, index);
	put32(buffer, UFS_CG_NDBLK, fragments);
	put32(buffer, UFS_CG_NDIR, index == 0 ? directories : 0U);
	put32(buffer, UFS_CG_NBFREE, free_fragments / 8U);
	put32(buffer, UFS_CG_NIFREE, index == 0 ? FORMAT_IPG - allocated_inodes : FORMAT_IPG);
	put32(buffer, UFS_CG_NFFREE, free_fragments % 8U);
	put32(buffer, UFS_CG_IUSEDOFF, 168U);
	put32(buffer, UFS_CG_FREEOFF, 200U);
	put32(buffer, UFS_CG_NEXTFREEOFF, context->cgsize);
	put32(buffer, 116U, FORMAT_IPG);
	put32(buffer, 120U, FORMAT_IPG);

	/* Reserve the first seven inodes only in the first group. */
	if (index == 0)
		buffer[168U] = (uint8_t)((1U << allocated_inodes) - 1U);

	/* Mark each unallocated fragment as available. */
	for (fragment = used; fragment < fragments; fragment++)
		buffer[200U + fragment / 8U] |= (uint8_t)(1U << (fragment & 7U));
}

/* Encodes the maintained superblock including all summaries. */
static void
make_super(
	struct format_context *context,
	uint8_t *buffer)
{
	uint32_t free_fragments;
	uint64_t free_blocks;
	uint64_t partial_fragments;
	uint32_t group;
	uint32_t fragments;
	uint64_t remaining;
	uint32_t first_used, allocated_inodes, directories;

	/* Initialize the canonical fixed geometry and filesystem identity. */
	memset(buffer, 0, FORMAT_BLOCK);
	put32(buffer, UFS_FS_SBLKNO, 64U);
	put32(buffer, UFS_FS_CBLKNO, 72U);
	put32(buffer, UFS_FS_IBLKNO, 80U);
	put32(buffer, UFS_FS_DBLKNO, FORMAT_DATA_FRAGMENT);
	put64(buffer, UFS_FS_SIZE, context->fragments);
	put64(buffer, UFS_FS_DSIZE, context->fragments - context->ncg * FORMAT_DATA_FRAGMENT);
	put64(buffer, UFS_FS_SBLOCKLOC, UFS_SBLOCK_OFFSET);
	put64(buffer, UFS_FS_CSADDR, FORMAT_DATA_FRAGMENT);
	put32(buffer, UFS_FS_NCG, context->ncg);
	put32(buffer, UFS_FS_BSIZE, FORMAT_BLOCK);
	put32(buffer, UFS_FS_FSIZE, FORMAT_FRAGMENT);
	put32(buffer, UFS_FS_FRAG, 8U);
	put32(buffer, UFS_FS_BSHIFT, 13U);
	put32(buffer, UFS_FS_FSHIFT, 10U);
	put32(buffer, UFS_FS_FRAGSHIFT, 3U);
	put32(buffer, UFS_FS_FSBTODB, 1U);
	put32(buffer, UFS_FS_SBSIZE, UFS_FS_STRUCT_SIZE);
	put32(buffer, UFS_FS_NINDIR, FORMAT_BLOCK / 8U);
	put32(buffer, UFS_FS_INOPB, FORMAT_BLOCK / UFS_DINODE_SIZE);
	memcpy(buffer + UFS_FS_ID, "zedBSD\001", 8U);
	put32(buffer, UFS_FS_CGSIZE, context->cgsize);
	put32(buffer, UFS_FS_IPG, FORMAT_IPG);
	put32(buffer, UFS_FS_FPG, context->fpg);
	buffer[UFS_FS_CLEAN] = 1U;
	put32(buffer, UFS_FS_MAXSYMLINKLEN, 120U);
	put64(buffer, UFS_FS_MAXFILESIZE, UINT64_C(0x7fffffffffffffff));
	put32(buffer, UFS_FS_MAGIC, UFS_MAGIC);

	/* Sum full and partial free blocks across all cylinder groups. */
	free_blocks = 0;
	partial_fragments = 0;
	first_used = context->profile == FORMAT_NATIVE ? FORMAT_NATIVE_USED : FORMAT_CG0_USED;
	allocated_inodes = context->profile == FORMAT_NATIVE ? 3U : 7U;
	directories = context->profile == FORMAT_NATIVE ? 1U : 2U;
	for (group = 0; group < context->ncg; group++) {
		remaining = context->fragments - (uint64_t)group * context->fpg;
		fragments = remaining > context->fpg ? context->fpg : (uint32_t)remaining;
		free_fragments = fragments - (group == 0 ?
		    first_used : FORMAT_DATA_FRAGMENT);
		free_blocks += free_fragments / 8U;
		partial_fragments += free_fragments % 8U;
	}
	put64(buffer, UFS_FS_CSTOTAL_NDIR, directories);
	put64(buffer, UFS_FS_CSTOTAL_NBFREE, free_blocks);
	put64(buffer, UFS_FS_CSTOTAL_NIFREE, (uint64_t)context->ncg * FORMAT_IPG - allocated_inodes);
	put64(buffer, UFS_FS_CSTOTAL_NFFREE, partial_fragments);
}

/* Computes the existing version-one persistence locator checksum. */
static uint32_t
locator_digest(
	const uint8_t *buffer,
	size_t length)
{
	uint32_t value;
	size_t index;

	/* Fold each byte into the version-one FNV digest. */
	value = UINT32_C(2166136261);
	for (index = 0; index < length; index++) {
		value ^= buffer[index];
		value *= UINT32_C(16777619);
	}

	/* Return the stored unsigned digest. */
	return value;
}

/* Initializes disjoint journal and inactive snapshot storage after the volume. */
static int
format_tail(
	struct format_context *context)
{
	uint8_t buffer[512];
	uint64_t end;
	uint64_t cursor;
	uint32_t checksum;
	int error;

	/* Leave the ordinary profile's full file extent to the filesystem. */
	if (!context->profile)
		return 0;

	/* Clear old recovery records and unallocated final alignment padding. */
	end = (uint64_t)context->fragments * 2U;
	cursor = end + 1U + FORMAT_LOG_SECTORS;
	error = zero_region(context, (end + 1U) * 512U, FORMAT_LOG_SECTORS * 512U);
	if (error != 0)
		return error;
	error = zero_region(context, (cursor + 2U) * 512U,
	    context->medium_bytes - (cursor + 2U) * 512U);
	if (error != 0)
		return error;

	/* Publish the bounded journal locator. */
	memset(buffer, 0, sizeof(buffer));
	memcpy(buffer, "ZUJ2", 4U);
	put32(buffer, 4U, 2U);
	put32(buffer, 8U, FORMAT_LOG_SECTORS);
	put64(buffer, 12U, end);
	checksum = locator_digest(buffer, 24U);
	put32(buffer, 24U, checksum);
	error = transfer(context, end * 512U, buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Publish a separate snapshot locator referencing the same volume end. */
	memset(buffer, 0, sizeof(buffer));
	memcpy(buffer, "ZSL1", 4U);
	put32(buffer, 4U, 1U);
	put32(buffer, 8U, FORMAT_SNAPSHOT_SECTORS);
	put64(buffer, 16U, cursor);
	put64(buffer, 24U, end);
	checksum = locator_digest(buffer, 32U);
	put32(buffer, 32U, checksum);
	error = transfer(context, cursor * 512U, buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Initialize an explicitly inactive snapshot control record. */
	memset(buffer, 0, sizeof(buffer));
	memcpy(buffer, "ZSN1", 4U);
	put32(buffer, 4U, 1U);
	put32(buffer, 16U, (FORMAT_SNAPSHOT_SECTORS - 1U) / 2U);
	put64(buffer, 24U, end);
	checksum = locator_digest(buffer, 32U);
	put32(buffer, 32U, checksum);
	error = transfer(context, (cursor + 1U) * 512U, buffer, sizeof(buffer));
	return error;
}
