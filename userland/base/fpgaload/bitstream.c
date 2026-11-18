/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Puts a .bit or .bin file of a Xilinx 7-series logic in the order the
 * Zynq's configuration DMA takes.
 *
 * The .bit header is a 16-bit big-endian length (9) and that many bytes,
 * a 16-bit 1, then fields each named by one letter: 'a' to 'd' carry a
 * 16-bit big-endian length and a string, 'e' a 32-bit big-endian length
 * and the configuration data.  Format referenced: U-Boot v2025.07
 * drivers/fpga/xilinx.c, fpga_loadbitstream (the first field, the 16-bit
 * value after it and the fields 0x61 to 0x65), and drivers/fpga/zynqpl.c,
 * check_header (a .bin in either byte order).
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "bitstream.h"

/* The first field of a .bit header: its length, and the length of the 1 after it. */
#define BIT_HEADER_LENGTH	9U
#define BIT_FIELD_LENGTH_BYTES	2U
#define BIT_DATA_LENGTH_BYTES	4U

/* The field that holds the configuration data, and the first and last string fields. */
#define BIT_FIELD_DATA		'e'
#define BIT_FIELD_FIRST		'a'
#define BIT_FIELD_LAST		'd'

/* The size of a configuration word. */
#define WORD_BYTES		4U

/* The order the sync word is found in. */
enum sync_order {
	SYNC_NONE,
	SYNC_BIG_ENDIAN,
	SYNC_DMA
};

static uint32_t read_be16(const uint8_t *bytes);
static uint32_t read_be32(const uint8_t *bytes);
static int find_bit_data(uint8_t *file, size_t file_size, uint8_t **data, size_t *size);
static enum sync_order find_sync(const uint8_t *data, size_t size);
static void reverse_words(uint8_t *data, size_t size);

/*
 * Finds the configuration data of a .bit or .bin file and puts it in the
 * DMA's order.
 */
int
bitstream_prepare(
	uint8_t **data,
	size_t *size)
{
	uint8_t *found;
	size_t found_size;
	uint32_t first_field;
	enum sync_order order;
	int error;

	/* Nothing is not a bitstream. */
	if (data == NULL || size == NULL || *data == NULL)
		return EINVAL;

	/* A .bit file starts with its header's length; anything else is taken as data alone. */
	found = *data;
	found_size = *size;
	first_field = 0;
	if (found_size >= BIT_FIELD_LENGTH_BYTES)
		first_field = read_be16(found);
	if (first_field == BIT_HEADER_LENGTH) {
		error = find_bit_data(*data, *size, &found, &found_size);
		if (error != 0)
			return error;
	}

	/* The DMA counts whole words. */
	if (found_size == 0 || (found_size % WORD_BYTES) != 0)
		return EINVAL;

	/* Finds the order of the data from its sync word. */
	order = find_sync(found, found_size);
	if (order == SYNC_NONE)
		return EINVAL;

	/* Big-endian words, as a .bit holds them, are turned round for the DMA. */
	if (order == SYNC_BIG_ENDIAN)
		reverse_words(found, found_size);

	/* Succeeded: the data, in the DMA's order. */
	*data = found;
	*size = found_size;
	return 0;
}

/* Reads a 16-bit big-endian number. */
static uint32_t
read_be16(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The high byte first. */
	value = (uint32_t)bytes[0] << 8;
	value |= (uint32_t)bytes[1];

	/* Succeeded: the number. */
	return value;
}

/* Reads a 32-bit big-endian number. */
static uint32_t
read_be32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The high byte first. */
	value = (uint32_t)bytes[0] << 24;
	value |= (uint32_t)bytes[1] << 16;
	value |= (uint32_t)bytes[2] << 8;
	value |= (uint32_t)bytes[3];

	/* Succeeded: the number. */
	return value;
}

/*
 * Walks the fields of a .bit header to the data.  Returns 0, or EINVAL
 * when a field runs past the file or an unknown field comes first.
 */
static int
find_bit_data(
	uint8_t *file,
	size_t file_size,
	uint8_t **data,
	size_t *size)
{
	size_t position;
	uint32_t length;
	uint8_t field;

	/* Steps over the first field and the 1 that follows it. */
	position = BIT_FIELD_LENGTH_BYTES + BIT_HEADER_LENGTH + BIT_FIELD_LENGTH_BYTES;
	if (position > file_size)
		return EINVAL;

	/* Each named field, up to the data. */
	while (position < file_size) {
		field = file[position];
		position++;

		/* The data field: its length and the data, which must fit in the file. */
		if (field == BIT_FIELD_DATA) {
			if (file_size - position < BIT_DATA_LENGTH_BYTES)
				return EINVAL;
			length = read_be32(&file[position]);
			position += BIT_DATA_LENGTH_BYTES;
			if ((size_t)length > file_size - position)
				return EINVAL;

			/* Succeeded: the data field. */
			*data = &file[position];
			*size = (size_t)length;
			return 0;
		}

		/* Only the string fields come before the data. */
		if (field < BIT_FIELD_FIRST || field > BIT_FIELD_LAST)
			return EINVAL;

		/* Steps over the string. */
		if (file_size - position < BIT_FIELD_LENGTH_BYTES)
			return EINVAL;
		length = read_be16(&file[position]);
		position += BIT_FIELD_LENGTH_BYTES;
		if ((size_t)length > file_size - position)
			return EINVAL;
		position += length;
	}

	/* The header ended without the data. */
	return EINVAL;
}

/* Finds the sync word at a word boundary near the start, and the order it is in. */
static enum sync_order
find_sync(
	const uint8_t *data,
	size_t size)
{
	size_t limit;
	size_t offset;
	uint32_t word;

	/* Looks only at the start, as the kernel does. */
	limit = size;
	if (limit > BITSTREAM_SYNC_SEARCH)
		limit = BITSTREAM_SYNC_SEARCH;

	/* Each word in reach, read big endian. */
	for (offset = 0; offset + WORD_BYTES <= limit; offset += WORD_BYTES) {
		word = read_be32(&data[offset]);

		/* The sync word as a .bit holds it. */
		if (word == 0xaa995566U)
			return SYNC_BIG_ENDIAN;

		/* The sync word already in the DMA's order. */
		if (word == 0x665599aaU)
			return SYNC_DMA;
	}

	/* No word in reach is the sync word. */
	return SYNC_NONE;
}

/* Reverses the bytes of every word. */
static void
reverse_words(
	uint8_t *data,
	size_t size)
{
	size_t offset;
	uint8_t byte;

	/* Each word: its first and last bytes, then the middle two, swap places. */
	for (offset = 0; offset + WORD_BYTES <= size; offset += WORD_BYTES) {
		byte = data[offset];
		data[offset] = data[offset + 3U];
		data[offset + 3U] = byte;
		byte = data[offset + 1U];
		data[offset + 1U] = data[offset + 2U];
		data[offset + 2U] = byte;
	}
}
