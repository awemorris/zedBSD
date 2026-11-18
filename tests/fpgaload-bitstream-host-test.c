/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of fpgaload's bitstream preparation
 * (userland/base/fpgaload/bitstream.c): a .bit made here (the header's
 * fields 'a' to 'd' and the data in 'e') comes out as its data with the
 * bytes of each word reversed; a .bin in .bit order is reversed, one in
 * the DMA's order is kept; malformed headers, a missing sync word and a
 * partial word are refused.  Given a real .bit (the plio block's), it
 * also checks that its data starts with the sync word in the DMA's order
 * within the kernel's search.
 *
 * cc -std=c11 -Wall -Wextra -Werror -Iuserland/base/fpgaload \
 *    tests/fpgaload-bitstream-host-test.c \
 *    userland/base/fpgaload/bitstream.c -o <out> && <out> [FILE.bit]
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitstream.h"

/* The data of the made bitstream: pad, the sync word, and a word to tell the order by. */
static const uint8_t data_bit_order[] = {
	0xff, 0xff, 0xff, 0xff,
	0xaa, 0x99, 0x55, 0x66,
	0x20, 0x00, 0x00, 0x00,
	0x01, 0x02, 0x03, 0x04
};
static const uint8_t data_dma_order[] = {
	0xff, 0xff, 0xff, 0xff,
	0x66, 0x55, 0x99, 0xaa,
	0x00, 0x00, 0x00, 0x20,
	0x04, 0x03, 0x02, 0x01
};

/* The number of failed checks. */
static int failures;

static void check(int condition, const char *name);
static size_t make_bit(uint8_t *out, const uint8_t *data, size_t data_size);
static void check_file(const char *path);

/*
 * Runs the checks and reports PASS or the failures.
 */
int
main(
	int argc,
	char **argv)
{
	uint8_t file[256];
	uint8_t *data;
	size_t size;
	size_t file_size;
	int error;

	/* A .bit: the data, turned round. */
	file_size = make_bit(file, data_bit_order, sizeof(data_bit_order));
	data = file;
	size = file_size;
	error = bitstream_prepare(&data, &size);
	check(error == 0, ".bit accepted");
	check(size == sizeof(data_dma_order), ".bit data size");
	check(error == 0 && memcmp(data, data_dma_order, sizeof(data_dma_order)) == 0, ".bit data reversed");

	/* A .bin in .bit order: turned round. */
	memcpy(file, data_bit_order, sizeof(data_bit_order));
	data = file;
	size = sizeof(data_bit_order);
	error = bitstream_prepare(&data, &size);
	check(error == 0 && memcmp(data, data_dma_order, sizeof(data_dma_order)) == 0, ".bin big endian reversed");

	/* A .bin in the DMA's order: kept. */
	memcpy(file, data_dma_order, sizeof(data_dma_order));
	data = file;
	size = sizeof(data_dma_order);
	error = bitstream_prepare(&data, &size);
	check(error == 0 && memcmp(data, data_dma_order, sizeof(data_dma_order)) == 0, ".bin DMA order kept");

	/* A partial word, no sync word, a data length past the file, an unknown field. */
	memcpy(file, data_dma_order, sizeof(data_dma_order));
	data = file;
	size = sizeof(data_dma_order) - 2U;
	check(bitstream_prepare(&data, &size) == EINVAL, "partial word refused");
	memset(file, 0xff, 16);
	data = file;
	size = 16;
	check(bitstream_prepare(&data, &size) == EINVAL, "no sync refused");
	file_size = make_bit(file, data_bit_order, sizeof(data_bit_order));
	file[file_size - sizeof(data_bit_order) - 1U] = 0x40;
	data = file;
	size = file_size;
	check(bitstream_prepare(&data, &size) == EINVAL, "data past the file refused");
	file_size = make_bit(file, data_bit_order, sizeof(data_bit_order));
	file[13] = 'z';
	data = file;
	size = file_size;
	check(bitstream_prepare(&data, &size) == EINVAL, "unknown field refused");

	/* A real bitstream, when one is given. */
	if (argc > 1)
		check_file(argv[1]);

	/* Reports the result. */
	if (failures != 0) {
		printf("FAIL bitstream-host-test (%d)\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("PASS bitstream-host-test\n");
	return 0;
}

/* Makes a .bit with the four string fields and the data. */
static size_t
make_bit(
	uint8_t *out,
	const uint8_t *data,
	size_t data_size)
{
	static const uint8_t header[] = {
		0x00, 0x09, 0x0f, 0xf0, 0x0f, 0xf0, 0x0f, 0xf0, 0x0f, 0xf0, 0x00, 0x00, 0x01,
		'a', 0x00, 0x05, 'p', 'l', 'i', 'o', 0x00,
		'b', 0x00, 0x0d, '7', 'z', '0', '1', '0', 'c', 'l', 'g', '4', '0', '0', 0x00, 0x00,
		'c', 0x00, 0x03, '2', '6', 0x00,
		'd', 0x00, 0x03, '1', '4', 0x00,
		'e'
	};
	size_t position;

	/* The header, then the data's length and the data. */
	memcpy(out, header, sizeof(header));
	position = sizeof(header);
	out[position++] = (uint8_t)(data_size >> 24);
	out[position++] = (uint8_t)(data_size >> 16);
	out[position++] = (uint8_t)(data_size >> 8);
	out[position++] = (uint8_t)data_size;
	memcpy(&out[position], data, data_size);
	position += data_size;

	/* Succeeded: the length of the made file. */
	return position;
}

/* Prepares a real bitstream and checks its data begins as the kernel expects. */
static void
check_file(
	const char *path)
{
	FILE *stream;
	uint8_t *file;
	uint8_t *data;
	size_t size;
	size_t offset;
	int found;
	int error;

	/* Reads the file whole. */
	stream = fopen(path, "rb");
	check(stream != NULL, "real file opened");
	if (stream == NULL)
		return;
	file = malloc(8U * 1024U * 1024U);
	size = fread(file, 1, 8U * 1024U * 1024U, stream);
	fclose(stream);

	/* Prepares it and finds the sync word in the DMA's order within the search. */
	data = file;
	error = bitstream_prepare(&data, &size);
	check(error == 0, "real file accepted");
	found = 0;
	for (offset = 0; error == 0 && offset + 4U <= size && offset < BITSTREAM_SYNC_SEARCH; offset += 4U) {
		if (data[offset] == 0x66 && data[offset + 1U] == 0x55 &&
		    data[offset + 2U] == 0x99 && data[offset + 3U] == 0xaa) {
			found = 1;
			break;
		}
	}

	/* The data must start as the kernel looks for it, in whole words. */
	check(found, "real file sync word in the DMA's order");
	check((size % 4U) == 0, "real file whole words");
	printf("real file: %zu bytes of data, sync at %zu\n", size, offset);
	free(file);
}

/* Counts and names a failed check. */
static void
check(
	int condition,
	const char *name)
{
	/* A check that holds says nothing. */
	if (condition)
		return;

	/* Names the check that failed. */
	printf("not ok %s\n", name);
	failures++;
}
