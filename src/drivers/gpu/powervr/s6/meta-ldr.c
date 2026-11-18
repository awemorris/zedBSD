/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The META loader stream of a Series6 firmware image, its trailer, and
 * the bootloader's table of register writes.
 */

#include "drivers/gpu/powervr/s6/meta-ldr.h"
#include <uapi/errno.h>

/*
 * One segment of the firmware's memory.
 *
 * The table of segments is constant; a segment is either in the code
 * allocation or in the data allocation, at an offset from its start.
 */
struct fw_segment {
	uint32_t identity;
	uint32_t start;
	uint32_t bytes;
	uint32_t offset;
	int code;
};

/*
 * The segments of the firmware's memory, in the order the bootloader's
 * table sets them up after the bootloader page, which the core maps by
 * itself and which comes last here.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgxfwimageutils.c (asRGXFWCodeSegments:
 * the bootloader at 0x40000000, 0x1000 bytes, offset 0, and the text at
 * 0x18880000, 0x30000 bytes, offset 0x1000; asRGXFWDataSegments: the
 * shared data at 0x38880000, 0x5000 bytes, offset 0, and the data at
 * 0x78880000, 0x12000 bytes, offset 0x5000).
 */
static const struct fw_segment fw_segments[POWERVR_S6_SEGMENT_COUNT] = {
	{ POWERVR_S6_SEGMENT_TEXT, 0x18880000U, 0x30000U, 0x1000U, 1 },
	{ POWERVR_S6_SEGMENT_SHARED, 0x38880000U, 0x5000U, 0x0U, 0 },
	{ POWERVR_S6_SEGMENT_DATA, 0x78880000U, 0x12000U, 0x5000U, 0 },
	{ POWERVR_S6_SEGMENT_BOOTLOADER, 0x40000000U, 0x1000U, 0x0U, 1 }
};

static uint16_t get16(const uint8_t *bytes);
static uint32_t get32(const uint8_t *bytes);
static uint32_t get32_big(const uint8_t *bytes);
static int range_fits(size_t size, uint32_t offset, uint32_t bytes);
static void read_core_trailer(const uint8_t *tail, struct powervr_s6_trailer *trailer);
static void read_signature_trailer(const uint8_t *tail, struct powervr_s6_trailer *trailer);
static int data_length(const uint8_t *image, size_t limit, uint32_t data, uint32_t *length);
static int walk_load(const uint8_t *image, size_t limit, uint32_t block, struct powervr_s6_ldr_target *target, struct powervr_s6_ldr_summary *summary);
static int walk_zero(const uint8_t *image, uint32_t block, struct powervr_s6_ldr_target *target, struct powervr_s6_ldr_summary *summary);
static int walk_config(const uint8_t *image, size_t limit, uint32_t block, struct powervr_s6_ldr_target *target, struct powervr_s6_ldr_summary *summary);
static int place_range(uint32_t address, uint32_t bytes, struct powervr_s6_ldr_step *step);
static uint8_t *range_view(struct powervr_s6_ldr_target *target, const struct powervr_s6_ldr_step *step);
static int table_put(struct powervr_s6_ldr_target *target, uint32_t reg, uint32_t value);
static void tell_step(struct powervr_s6_ldr_target *target, const struct powervr_s6_ldr_step *step);

/*
 * Reads the loader stream's header.
 */
int
powervr_s6_ldr_header(
	const uint8_t *image,
	size_t size,
	struct powervr_s6_ldr_header *header)
{
	/* Refuses an image shorter than the header. */
	if (size < POWERVR_S6_LDR_HEADER_BYTES)
		return EINVAL;

	/* Decodes the five fields, all little-endian. */
	header->device = get32(image);
	header->loader_code = get32(image + 4);
	header->first_block = get32(image + 8);
	header->control = get16(image + 12);
	header->check = get16(image + 14);

	/* Succeeded: the header is filled. */
	return 0;
}

/*
 * Reads the trailer at the end of an image.
 *
 * The trailer is told apart by its first bytes: a signature header has
 * an X.509 key identifier type in its third byte and zero pad bytes, a
 * core trailer the record layout of a later interface as its first
 * big-endian word.  Nothing past the trailer's header is read; the
 * signature itself is not looked at.
 */
int
powervr_s6_trailer_read(
	const uint8_t *image,
	size_t size,
	struct powervr_s6_trailer *trailer)
{
	const uint8_t *tail;
	uint32_t layout;
	unsigned index;

	/* Refuses an image that is not whole blocks with a trailer after the stream. */
	if (size < 2U * POWERVR_S6_IMAGE_BLOCK)
		return EINVAL;
	if (size % POWERVR_S6_IMAGE_BLOCK != 0)
		return EINVAL;

	/* Starts with a trailer of no known form. */
	for (index = 0; index < sizeof(*trailer); index++)
		((uint8_t *)trailer)[index] = 0;

	/* The trailer is the image's last block. */
	trailer->kind = POWERVR_S6_TRAILER_UNKNOWN;
	tail = image + size - POWERVR_S6_IMAGE_BLOCK;

	/* A core trailer names its record layout first; a signature header does not. */
	layout = get32_big(tail);
	if (layout == POWERVR_S6_CORE_TRAILER_LAYOUT) {
		read_core_trailer(tail, trailer);
	} else {
		read_signature_trailer(tail, trailer);
	}

	/* Succeeded: the trailer's form is known or marked unknown. */
	return 0;
}

/*
 * Walks the loader stream of an image.
 *
 * The chain is followed from the header's first block until the end
 * marker; a chain that runs longer than the image could hold blocks is
 * a loop and is refused.
 *
 * Procedure referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgxfwimageutils.c (ProcessLDRCommandStream:
 * comment blocks are skipped; a load copies its data block's bytes; core
 * and register loads are refused; the start of threads is not carried
 * out; a zeroing in the core memory's data is skipped and any other is
 * carried out; a configuration's register writes go to the bootloader's
 * table and its other entries are refused; the global bit of an address
 * is dropped before the segment is found).
 */
int
powervr_s6_ldr_walk(
	const uint8_t *image,
	size_t size,
	struct powervr_s6_ldr_target *target,
	struct powervr_s6_ldr_summary *summary)
{
	struct powervr_s6_ldr_step step;
	struct powervr_s6_ldr_header header;
	size_t limit;
	uint32_t block;
	uint32_t command;
	uint32_t length;
	uint32_t next;
	uint32_t blocks_max;
	uint32_t needed;
	int fits;
	int error;
	unsigned index;

	/* Starts the counts at zero. */
	for (index = 0; index < sizeof(*summary); index++)
		((uint8_t *)summary)[index] = 0;

	/* Reads the header that names the first block. */
	error = powervr_s6_ldr_header(image, size, &header);
	if (error != 0)
		return error;

	/* The stream lives before the trailer. */
	if (size < 2U * POWERVR_S6_IMAGE_BLOCK)
		return EINVAL;
	limit = size - POWERVR_S6_IMAGE_BLOCK;

	/* Follows the chain of command blocks. */
	block = header.first_block;
	blocks_max = (uint32_t)(limit / POWERVR_S6_LDR_BLOCK_BYTES);
	while (block != POWERVR_S6_LDR_END) {
		/* Refuses a chain longer than the image could hold. */
		if (summary->blocks >= blocks_max)
			return EINVAL;

		/* Refuses a block whose fixed part is outside the stream. */
		fits = range_fits(limit, block, POWERVR_S6_LDR_BLOCK_BYTES);
		if (!fits)
			return EINVAL;
		if (block % 4U != 0)
			return EINVAL;

		/* Reads the block's command, length and successor. */
		command = get16(image + block);
		length = get16(image + block + 2);
		next = get32(image + block + 4);
		summary->blocks++;

		/* The block, with its own length, has to be in the stream. */
		if (length < POWERVR_S6_LDR_BLOCK_BYTES)
			return EINVAL;
		fits = range_fits(limit, block, length);
		if (!fits)
			return EINVAL;

		/* Finds how many argument words the command reads. */
		needed = 0;
		if ((command & POWERVR_S6_LDR_COMMENT) == 0) {
			if ((command & POWERVR_S6_LDR_COMMAND_MASK) == POWERVR_S6_LDR_LOAD) {
				needed = 2;
			} else if ((command & POWERVR_S6_LDR_COMMAND_MASK) == POWERVR_S6_LDR_ZERO) {
				needed = 2;
			} else if ((command & POWERVR_S6_LDR_COMMAND_MASK) == POWERVR_S6_LDR_CONFIG) {
				needed = 1;
			}
		}

		/* Refuses a block too short for its arguments. */
		if (length < POWERVR_S6_LDR_BLOCK_BYTES + 4U * needed)
			return EINVAL;

		/* Carries out the block's command. */
		error = 0;
		if ((command & POWERVR_S6_LDR_COMMENT) != 0) {
			/* A comment carries nothing for the host. */
			summary->comments++;
			step.block = block;
			step.command = command;
			step.address = 0;
			step.bytes = 0;
			step.source = 0;
			step.segment = -1;
			step.target = 0;
			tell_step(target, &step);
		} else {
			/* Picks the command's handling. */
			switch (command & POWERVR_S6_LDR_COMMAND_MASK) {
			case POWERVR_S6_LDR_LOAD:
				error = walk_load(image, limit, block, target, summary);
				break;
			case POWERVR_S6_LDR_ZERO:
				error = walk_zero(image, block, target, summary);
				break;
			case POWERVR_S6_LDR_CONFIG:
				error = walk_config(image, limit, block, target, summary);
				break;
			case POWERVR_S6_LDR_START_THREADS:
				/* The host starts the processor itself; the command is noted only. */
				summary->start_threads++;
				step.block = block;
				step.command = command;
				step.address = 0;
				step.bytes = 0;
				step.source = 0;
				step.segment = -1;
				step.target = 0;
				tell_step(target, &step);
				break;
			default:
				/* Core and register loads, and unknown commands, cannot be carried out. */
				error = ENOEXEC;
				break;
			}
		}

		/* Propagates a command that failed. */
		if (error != 0)
			return error;

		/* Moves to the next block, which records where the stream ended. */
		if (block + length > summary->end)
			summary->end = block + length;
		block = next;
	}

	/* Succeeded: every command was checked and, with a target, carried out. */
	return 0;
}

/*
 * Finds where a META address lands in the firmware's memory.
 */
int
powervr_s6_segment_find(
	uint32_t address,
	uint32_t bytes,
	uint32_t *offset)
{
	const struct fw_segment *segment;
	uint32_t local;
	unsigned index;

	/* Drops the global view's bit; both views reach the same memory. */
	local = address & ~POWERVR_S6_META_GLOBAL;

	/* Looks for the segment the whole range is inside. */
	for (index = 0; index < POWERVR_S6_SEGMENT_COUNT; index++) {
		segment = &fw_segments[index];

		/* Skips a segment that starts after the range or ends before its end. */
		if (local < segment->start)
			continue;
		if (local - segment->start > segment->bytes)
			continue;
		if (bytes > segment->bytes - (local - segment->start))
			continue;

		/* Reports the segment and the range's offset in its allocation. */
		*offset = segment->offset + (local - segment->start);
		return (int)segment->identity;
	}

	/* No segment holds the whole range. */
	return -1;
}

/*
 * Tells whether a segment lives in the code allocation.
 */
int
powervr_s6_segment_is_code(
	int segment)
{
	/* The text and the bootloader page are code; the rest is data. */
	if (segment == (int)POWERVR_S6_SEGMENT_TEXT)
		return 1;
	if (segment == (int)POWERVR_S6_SEGMENT_BOOTLOADER)
		return 1;

	/* Shared and private data. */
	return 0;
}

/*
 * Starts the bootloader's table.
 *
 * Procedure referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgxfwimageutils.c (RGXProcessFWImage: the
 * debug port privilege first; RGXFWConfigureSegMMU and
 * RGXFWConfigureSegID: every segment but the bootloader's, its base with
 * the all-threads writable bits, its limit as the size less one with a
 * 4 KiB minimum, and the two halves of its output address).
 */
int
powervr_s6_boot_table_begin(
	struct powervr_s6_ldr_target *target,
	uint64_t code_device,
	uint64_t data_device)
{
	const struct fw_segment *segment;
	uint64_t output;
	uint64_t top;
	uint32_t base;
	uint32_t limit;
	unsigned index;
	int error;

	/* Starts an empty table. */
	target->used = 0;

	/* Makes the slave port and the debug port privileged. */
	error = table_put(target, POWERVR_S6_META_JTAG_THREAD, POWERVR_S6_META_JTAG_PRIVILEGED);
	if (error != 0)
		return error;

	/* The firmware's page catalogue and the META data master, carried by every segment. */
	top = ((uint64_t)0 << POWERVR_S6_SEGMENT_CATALOGUE_SHIFT) |
	    ((uint64_t)POWERVR_S6_META_DATA_MASTER << POWERVR_S6_SEGMENT_MASTER_SHIFT);

	/* Sets every segment but the bootloader's, which the core maps itself. */
	for (index = 0; index < POWERVR_S6_SEGMENT_COUNT; index++) {
		segment = &fw_segments[index];

		/* The bootloader's segment is the core's own and is not in the table. */
		if (segment->identity == POWERVR_S6_SEGMENT_BOOTLOADER)
			continue;

		/* The output address is the allocation's device address and the segment's offset in it. */
		if (segment->code) {
			output = (code_device | top) + segment->offset;
		} else {
			output = (data_device | top) + segment->offset;
		}

		/* The limit is the last offset of the segment, at least one 4 KiB page. */
		limit = segment->bytes;
		if (limit < POWERVR_S6_META_SEGMENT_MINIMUM)
			limit = POWERVR_S6_META_SEGMENT_MINIMUM;
		limit -= 1;
		base = segment->start | POWERVR_S6_META_SEGMENT_SHARED;

		/* Writes the base, the limit and the two halves of the output address. */
		error = table_put(target, POWERVR_S6_META_SEGMENT_BASE + segment->identity * POWERVR_S6_META_SEGMENT_STRIDE, base);
		if (error != 0)
			return error;
		error = table_put(target, POWERVR_S6_META_SEGMENT_BASE + segment->identity * POWERVR_S6_META_SEGMENT_STRIDE + POWERVR_S6_META_SEGMENT_LIMIT, limit);
		if (error != 0)
			return error;
		error = table_put(target, POWERVR_S6_META_SEGMENT_BASE + segment->identity * POWERVR_S6_META_SEGMENT_STRIDE + POWERVR_S6_META_SEGMENT_OUT_LOW, (uint32_t)output);
		if (error != 0)
			return error;
		error = table_put(target, POWERVR_S6_META_SEGMENT_BASE + segment->identity * POWERVR_S6_META_SEGMENT_STRIDE + POWERVR_S6_META_SEGMENT_OUT_HIGH, (uint32_t)(output >> 32));
		if (error != 0)
			return error;
	}

	/* Succeeded: the table holds the privilege and the segments. */
	return 0;
}

/*
 * Ends the bootloader's table.
 *
 * Only thread 0 runs, so it is given the whole of both caches; the
 * other threads keep the cached-write bit of the data cache and nothing
 * else.
 *
 * Procedure referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgxfwimageutils.c
 * (RGXFWConfigureMetaCaches for one thread with thread 0 the main one,
 * then RGXProcessFWImage's zero pair and the init record's address).
 */
int
powervr_s6_boot_table_end(
	struct powervr_s6_ldr_target *target,
	uint32_t init_record)
{
	uint32_t data_partition;
	uint32_t code_partition;
	unsigned thread;
	int error;

	/* Lets both caches bypass the local region's MMU in window 3. */
	error = table_put(target, POWERVR_S6_META_BYPASS_CONTROL, POWERVR_S6_META_BYPASS_WINDOWS);
	if (error != 0)
		return error;

	/* Partitions the data cache: all of it to thread 0, writes cached for every thread. */
	for (thread = 0; thread < POWERVR_S6_META_THREADS; thread++) {
		data_partition = POWERVR_S6_META_PARTITION_WRITE;

		/* Only the running thread gets cache lines. */
		if (thread == 0)
			data_partition |= POWERVR_S6_META_PARTITION_WHOLE;

		/* Writes the thread's partition. */
		error = table_put(target, POWERVR_S6_META_DATA_PARTITION + thread * POWERVR_S6_META_PARTITION_STRIDE, data_partition);
		if (error != 0)
			return error;
	}

	/* Turns on hits in the data cache. */
	error = table_put(target, POWERVR_S6_META_DATA_CACHE, POWERVR_S6_META_CACHE_HITS);
	if (error != 0)
		return error;

	/* Partitions the code cache: all of it to thread 0. */
	for (thread = 0; thread < POWERVR_S6_META_THREADS; thread++) {
		code_partition = 0;

		/* Only the running thread gets cache lines. */
		if (thread == 0)
			code_partition = POWERVR_S6_META_PARTITION_WHOLE;

		/* Writes the thread's partition. */
		error = table_put(target, POWERVR_S6_META_CODE_PARTITION + thread * POWERVR_S6_META_PARTITION_STRIDE, code_partition);
		if (error != 0)
			return error;
	}

	/* Turns on hits in the code cache, and writes the closing set-up register. */
	error = table_put(target, POWERVR_S6_META_CODE_CACHE, POWERVR_S6_META_CACHE_HITS);
	if (error != 0)
		return error;
	error = table_put(target, POWERVR_S6_META_LAST_SETUP, 0);
	if (error != 0)
		return error;

	/* Ends the run of writes with a zero pair. */
	error = table_put(target, 0, 0);
	if (error != 0)
		return error;

	/* Leaves the firmware's argument: where its init record is. */
	if (target->used >= target->words)
		return ENOSPC;
	target->table[target->used] = init_record;
	target->used++;

	/* Succeeded: the table is complete. */
	return 0;
}

/*
 * Converts a device address in the firmware heap to a META address.
 */
int
powervr_s6_fw_address(
	uint64_t device,
	int cached,
	uint32_t *address)
{
	uint64_t offset;

	/* Refuses an address outside the heap. */
	if (device < POWERVR_S6_FW_HEAP_BASE)
		return ERANGE;
	offset = device - POWERVR_S6_FW_HEAP_BASE;
	if (offset >= POWERVR_S6_FW_HEAP_BYTES)
		return ERANGE;

	/* Picks the view through the cache or the direct map around it. */
	if (cached) {
		*address = (uint32_t)offset | POWERVR_S6_FW_CACHED_BASE;
	} else {
		*address = (uint32_t)offset | POWERVR_S6_FW_UNCACHED_BASE;
	}

	/* Succeeded: the address is META's view of the device address. */
	return 0;
}

/* Reads a core trailer of a later interface. */
static void
read_core_trailer(
	const uint8_t *tail,
	struct powervr_s6_trailer *trailer)
{
	uint64_t packed;
	unsigned index;

	/* The record's layout, its longest version, and the packed branch, unit count and configuration. */
	trailer->layout = get32_big(tail);
	trailer->version_max = get32_big(tail + 4);
	packed = ((uint64_t)get32_big(tail + 8) << 32) | get32_big(tail + 12);
	trailer->branch = (uint32_t)(packed >> 32) & 0xffffU;
	trailer->units = (uint32_t)(packed >> 16) & 0xffffU;
	trailer->config = (uint32_t)packed & 0xffffU;

	/* Copies the version string, always ended within the copy. */
	for (index = 0; index < POWERVR_S6_CORE_TRAILER_VERSION; index++)
		trailer->version[index] = (char)tail[16 + index];
	trailer->version[POWERVR_S6_CORE_TRAILER_VERSION] = 0;

	/* A version longer than its field means the block is something else. */
	if (trailer->version_max >= POWERVR_S6_CORE_TRAILER_VERSION)
		return;

	/* The block is a core trailer. */
	trailer->kind = POWERVR_S6_TRAILER_CORE;
}

/*
 * Reads a signature header.
 *
 * Only the header's fields are read; the signer, the key identifier and
 * the signature after it are not looked at.
 */
static void
read_signature_trailer(
	const uint8_t *tail,
	struct powervr_s6_trailer *trailer)
{
	/* A signature header has an X.509 key identifier and zero pad bytes. */
	if (tail[2] != POWERVR_S6_SIGNATURE_X509)
		return;
	if (tail[5] != 0 || tail[6] != 0 || tail[7] != 0)
		return;

	/* The five one-byte fields and the big-endian signature length. */
	trailer->algorithm = tail[0];
	trailer->digest = tail[1];
	trailer->key_type = tail[2];
	trailer->signer_bytes = tail[3];
	trailer->key_bytes = tail[4];
	trailer->signature_bytes = get32_big(tail + 8);

	/* A signature that is empty or would not fit the block is not one. */
	if (trailer->signature_bytes == 0)
		return;
	if (trailer->signature_bytes > POWERVR_S6_IMAGE_BLOCK)
		return;
	if (12U + trailer->signer_bytes + trailer->key_bytes + trailer->signature_bytes > POWERVR_S6_IMAGE_BLOCK)
		return;

	/* The block is a signature header. */
	trailer->kind = POWERVR_S6_TRAILER_SIGNATURE;
}

/* Reads a little-endian half-word. */
static uint16_t
get16(
	const uint8_t *bytes)
{
	/* The low byte first. */
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

/* Reads a little-endian word. */
static uint32_t
get32(
	const uint8_t *bytes)
{
	/* The low byte first. */
	return (uint32_t)bytes[0] |
	    ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) |
	    ((uint32_t)bytes[3] << 24);
}

/* Reads a big-endian word. */
static uint32_t
get32_big(
	const uint8_t *bytes)
{
	/* The high byte first. */
	return ((uint32_t)bytes[0] << 24) |
	    ((uint32_t)bytes[1] << 16) |
	    ((uint32_t)bytes[2] << 8) |
	    (uint32_t)bytes[3];
}

/* Tells whether a range of bytes is inside a stream of a size. */
static int
range_fits(
	size_t size,
	uint32_t offset,
	uint32_t bytes)
{
	/* The start has to be inside. */
	if (offset > size)
		return 0;

	/* The rest has to fit after it. */
	if (bytes > size - offset)
		return 0;

	/* The whole range is inside. */
	return 1;
}

/* Reads the length of a data block after checking it lies in the stream. */
static int
data_length(
	const uint8_t *image,
	size_t limit,
	uint32_t data,
	uint32_t *length)
{
	int fits;

	/* Refuses a data block whose head is outside the stream. */
	fits = range_fits(limit, data, POWERVR_S6_LDR_DATA_HEAD);
	if (!fits)
		return EINVAL;

	/* Refuses a length shorter than the block's own head and check. */
	*length = get16(image + data + 2);
	if (*length < POWERVR_S6_LDR_DATA_OVERHEAD)
		return EINVAL;

	/* Refuses a block whose bytes run past the stream. */
	fits = range_fits(limit, data, *length);
	if (!fits)
		return EINVAL;

	/* Succeeded: the whole block is in the stream. */
	return 0;
}

/* Carries out one load command. */
static int
walk_load(
	const uint8_t *image,
	size_t limit,
	uint32_t block,
	struct powervr_s6_ldr_target *target,
	struct powervr_s6_ldr_summary *summary)
{
	struct powervr_s6_ldr_step step;
	uint32_t data;
	uint32_t length;
	uint8_t *view;
	uint32_t index;
	int error;

	/* Reads the META address and the offset of the data block. */
	step.block = block;
	step.command = POWERVR_S6_LDR_LOAD;
	step.address = get32(image + block + 8);
	data = get32(image + block + 12);

	/* Refuses a data block whose head or bytes are outside the stream. */
	error = data_length(image, limit, data, &length);
	if (error != 0)
		return error;
	step.bytes = length - POWERVR_S6_LDR_DATA_OVERHEAD;
	step.source = data + POWERVR_S6_LDR_DATA_HEAD;

	/* Finds where the bytes land; refuses a range outside the firmware's memory. */
	error = place_range(step.address, step.bytes, &step);
	if (error != 0)
		return error;

	/* Copies the bytes when there is somewhere to put them. */
	view = range_view(target, &step);
	if (view != NULL) {
		for (index = 0; index < step.bytes; index++)
			view[index] = image[step.source + index];
	}

	/* Counts the load. */
	summary->loads++;
	summary->loaded_bytes[step.segment] += step.bytes;
	tell_step(target, &step);

	/* Succeeded: the load is checked and carried out. */
	return 0;
}

/* Carries out one zeroing command. */
static int
walk_zero(
	const uint8_t *image,
	uint32_t block,
	struct powervr_s6_ldr_target *target,
	struct powervr_s6_ldr_summary *summary)
{
	struct powervr_s6_ldr_step step;
	uint8_t *view;
	uint32_t index;
	int error;

	/* Reads the META address and the byte count. */
	step.block = block;
	step.command = POWERVR_S6_LDR_ZERO;
	step.address = get32(image + block + 8);
	step.bytes = get32(image + block + 12);
	step.source = 0;

	/* A zeroing of the core memory's data is the firmware's own business. */
	if (step.address >= POWERVR_S6_CORE_DATA_START &&
	    step.address - POWERVR_S6_CORE_DATA_START < POWERVR_S6_CORE_DATA_BYTES) {
		summary->skipped_zeroings++;
		step.segment = -1;
		step.target = 0;
		tell_step(target, &step);
		return 0;
	}

	/* Finds where the range lands; refuses a range outside the firmware's memory. */
	error = place_range(step.address, step.bytes, &step);
	if (error != 0)
		return error;

	/* Zeroes the range when there is somewhere to do it. */
	view = range_view(target, &step);
	if (view != NULL) {
		for (index = 0; index < step.bytes; index++)
			view[index] = 0;
	}

	/* Counts the zeroing. */
	summary->zeroings++;
	summary->zeroed_bytes[step.segment] += step.bytes;
	tell_step(target, &step);

	/* Succeeded: the zeroing is checked and carried out. */
	return 0;
}

/* Carries out one configuration command. */
static int
walk_config(
	const uint8_t *image,
	size_t limit,
	uint32_t block,
	struct powervr_s6_ldr_target *target,
	struct powervr_s6_ldr_summary *summary)
{
	struct powervr_s6_ldr_step step;
	uint32_t data;
	uint32_t length;
	uint32_t entry;
	uint32_t left;
	uint32_t type;
	int error;

	/* Reads the offset of the data block that holds the entries. */
	data = get32(image + block + 8);

	/* Refuses a data block whose head or entries are outside the stream. */
	error = data_length(image, limit, data, &length);
	if (error != 0)
		return error;
	left = length - POWERVR_S6_LDR_DATA_OVERHEAD;

	/* Tells of the configuration as a whole first. */
	step.block = block;
	step.command = POWERVR_S6_LDR_CONFIG;
	step.address = 0;
	step.bytes = left;
	step.source = data + POWERVR_S6_LDR_DATA_HEAD;
	step.segment = -1;
	step.target = 0;
	tell_step(target, &step);

	/* Carries out each entry; only register writes are possible before the firmware runs. */
	entry = data + POWERVR_S6_LDR_DATA_HEAD;
	while (left != 0) {
		/* Refuses an entry cut off by the end of the block. */
		if (left < 4U)
			return EINVAL;
		type = get32(image + entry);
		if (type != POWERVR_S6_LDR_CONFIG_WRITE)
			return ENOEXEC;
		if (left < POWERVR_S6_LDR_CONFIG_WRITE_BYTES)
			return EINVAL;

		/* Appends the write to the bootloader's table when there is one. */
		if (target != NULL && target->table != NULL) {
			error = table_put(target, get32(image + entry + 4), get32(image + entry + 8));
			if (error != 0)
				return error;
		}

		/* Counts the write and moves past it. */
		summary->config_writes++;
		entry += POWERVR_S6_LDR_CONFIG_WRITE_BYTES;
		left -= POWERVR_S6_LDR_CONFIG_WRITE_BYTES;
	}

	/* Counts the configuration. */
	summary->configs++;

	/* Succeeded: every entry was a register write. */
	return 0;
}

/*
 * Finds where a load's or a zeroing's range lands.
 *
 * Besides being inside a segment, the range must stay clear of the
 * bootloader's table, which the host writes before the stream is walked.
 */
static int
place_range(
	uint32_t address,
	uint32_t bytes,
	struct powervr_s6_ldr_step *step)
{
	uint32_t offset;
	int segment;

	/* Finds the segment that holds the whole range. */
	segment = powervr_s6_segment_find(address, bytes, &offset);
	if (segment < 0)
		return ERANGE;

	/* Refuses a range in the bootloader page that reaches its table. */
	if (segment == (int)POWERVR_S6_SEGMENT_BOOTLOADER && offset + bytes > POWERVR_S6_BOOT_TABLE_OFFSET)
		return ERANGE;

	/* The range lands at this offset of its allocation. */
	step->segment = segment;
	step->target = offset;

	/* Succeeded: the range has a place. */
	return 0;
}

/* Returns the host's view of a step's range, or NULL when the walk only checks. */
static uint8_t *
range_view(
	struct powervr_s6_ldr_target *target,
	const struct powervr_s6_ldr_step *step)
{
	int code;

	/* A walk without a target, or without memory, has nowhere to write. */
	if (target == NULL)
		return NULL;

	/* Picks the allocation the segment is in. */
	code = powervr_s6_segment_is_code(step->segment);
	if (code) {
		/* A target without code memory only checks. */
		if (target->code == NULL)
			return NULL;
		return target->code + step->target;
	}

	/* A target without data memory only checks. */
	if (target->data == NULL)
		return NULL;

	/* The data allocation's bytes at the range. */
	return target->data + step->target;
}

/* Appends one register write to the bootloader's table. */
static int
table_put(
	struct powervr_s6_ldr_target *target,
	uint32_t reg,
	uint32_t value)
{
	/* Refuses a write the table has no room for. */
	if (target->words - target->used < 2U)
		return ENOSPC;

	/* Stores the register and its value. */
	target->table[target->used] = reg;
	target->table[target->used + 1] = value;
	target->used += 2;

	/* Succeeded: the write is in the table. */
	return 0;
}

/* Tells the target's observer of one command. */
static void
tell_step(
	struct powervr_s6_ldr_target *target,
	const struct powervr_s6_ldr_step *step)
{
	/* Only a target with an observer is told. */
	if (target == NULL)
		return;
	if (target->step == NULL)
		return;

	/* Hands the step over. */
	target->step(target->context, step);
}
